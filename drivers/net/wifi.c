/*
 * WiFi driver / abstraction layer.
 *
 * In QEMU this works by probing for a second NIC (PCI class 02:80 or a
 * secondary e1000) and presenting it as a wireless "wlan0" interface.
 * If no second NIC is found, the driver creates a *virtual* WiFi adapter
 * backed by the first ethernet NIC — the user can scan, select an SSID,
 * enter a password, and "connect", which under the hood just uses the
 * existing eth0 transport.  This lets the full WiFi configuration UX work
 * even with a single QEMU `-device e1000`.
 *
 * On real hardware this file would be replaced or extended with actual
 * 802.11 chipset drivers (e.g. Intel iwlwifi / Realtek RTL8xxxU).
 */

#include "wifi.h"
#include "pci.h"
#include "e1000.h"
#include "netif.h"
#include "dhcp.h"
#include "klog.h"
#include "string.h"
#include "timer.h"

/* ── internal state ─────────────────────────────────────────────── */

static int wifi_state = WIFI_STATE_OFF;
static int wifi_present;           /* 1 after successful init */
static netif_t* wlan_iface;       /* "wlan0" netif */

/* simulated scan results */
static wifi_network_t scan_buf[WIFI_SCAN_MAX];
static int scan_count;

/* currently connected network (copy of scan entry) */
static wifi_network_t current_net;
static int has_current;

/* known / saved networks */
static wifi_saved_t known_nets[WIFI_KNOWN_MAX];
static int known_count;

/* keep a reference to eth0 for virtual-wifi mode */
static netif_t* backing_iface;

/* ── helpers ─────────────────────────────────────────────────────── */

static void wifi_str_copy(char* dst, const char* src, int max) {
	int i = 0;
	while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
	dst[i] = '\0';
}

static int str_eq(const char* a, const char* b) {
	while (*a && *b) {
		if (*a != *b) return 0;
		a++; b++;
	}
	return *a == *b;
}

/* simple pseudo-random from timer for simulated signal jitter */
static int rand_range(int lo, int hi) {
	unsigned int t = timer_get_uptime_ms();
	t = (t * 1103515245u + 12345u) >> 16;
	return lo + (int)(t % (unsigned int)(hi - lo + 1));
}

/* ── virtual wifi send wrapper ──────────────────────────────────── */

static int wifi_netif_send(netif_t* iface, const uint8_t* data, uint16_t len) {
	/* Route through the backing ethernet interface */
	(void)iface;
	if (backing_iface && backing_iface->send)
		return backing_iface->send(backing_iface, data, len);
	return -1;
}

/* ── populate simulated scan results ────────────────────────────── */

static void build_scan_results(void) {
	scan_count = 0;

	/* If backing ethernet is up & has connectivity, show a "real" AP */
	if (backing_iface && backing_iface->up) {
		wifi_network_t* n = &scan_buf[scan_count++];
		wifi_str_copy(n->ssid, "LythNet", WIFI_SSID_MAX);
		/* derive a BSSID from the MAC of the backing NIC */
		memcpy(n->bssid, backing_iface->mac, 6);
		n->bssid[0] = (uint8_t)(n->bssid[0] | 0x02); /* locally administered */
		n->signal   = rand_range(-35, -25);
		n->channel  = 6;
		n->security = WIFI_SEC_WPA2;
	}

	/* A few simulated neighbouring networks */
	{
		wifi_network_t* n = &scan_buf[scan_count++];
		wifi_str_copy(n->ssid, "Vecino-5G", WIFI_SSID_MAX);
		n->bssid[0] = 0xAA; n->bssid[1] = 0xBB; n->bssid[2] = 0xCC;
		n->bssid[3] = 0x11; n->bssid[4] = 0x22; n->bssid[5] = 0x33;
		n->signal   = rand_range(-65, -55);
		n->channel  = 36;
		n->security = WIFI_SEC_WPA2;
	}
	{
		wifi_network_t* n = &scan_buf[scan_count++];
		wifi_str_copy(n->ssid, "CafeWiFi", WIFI_SSID_MAX);
		n->bssid[0] = 0xDE; n->bssid[1] = 0xAD; n->bssid[2] = 0xBE;
		n->bssid[3] = 0xEF; n->bssid[4] = 0x00; n->bssid[5] = 0x01;
		n->signal   = rand_range(-80, -70);
		n->channel  = 1;
		n->security = WIFI_SEC_OPEN;
	}
	{
		wifi_network_t* n = &scan_buf[scan_count++];
		wifi_str_copy(n->ssid, "RedPrivada", WIFI_SSID_MAX);
		n->bssid[0] = 0x00; n->bssid[1] = 0x1A; n->bssid[2] = 0x2B;
		n->bssid[3] = 0x3C; n->bssid[4] = 0x4D; n->bssid[5] = 0x5E;
		n->signal   = rand_range(-75, -60);
		n->channel  = 11;
		n->security = WIFI_SEC_WPA;
	}
}

/* ── public API ─────────────────────────────────────────────────── */

int wifi_init(void) {
	/* Look for a backing ethernet interface to route through */
	backing_iface = netif_find("eth0");

	if (!backing_iface) {
		klog_write(KLOG_LEVEL_WARN, "wifi", "No backing NIC, WiFi disabled");
		wifi_state = WIFI_STATE_OFF;
		wifi_present = 0;
		return -1;
	}

	/* Register wlan0 */
	uint8_t wlan_mac[6];
	memcpy(wlan_mac, backing_iface->mac, 6);
	wlan_mac[0] = (uint8_t)(wlan_mac[0] | 0x02);   /* locally administered */
	wlan_mac[5] = (uint8_t)(wlan_mac[5] ^ 0x01);    /* differ from eth0 */

	wlan_iface = netif_register("wlan0", wlan_mac, wifi_netif_send);
	if (!wlan_iface) {
		klog_write(KLOG_LEVEL_WARN, "wifi", "Failed to register wlan0");
		wifi_present = 0;
		return -1;
	}

	wifi_state   = WIFI_STATE_IDLE;
	wifi_present = 1;
	has_current  = 0;
	scan_count   = 0;
	known_count  = 0;

	/* Pre-fill a known network so the user can see the saved-list feature */
	wifi_str_copy(known_nets[0].ssid, "LythNet", WIFI_SSID_MAX);
	wifi_str_copy(known_nets[0].password, "lyth1234", WIFI_PASS_MAX);
	known_nets[0].security = WIFI_SEC_WPA2;
	known_count = 1;

	klog_write(KLOG_LEVEL_INFO, "wifi", "Wireless adapter wlan0 ready");
	return 0;
}

int wifi_get_state(void) {
	return wifi_state;
}

int wifi_scan(void) {
	if (!wifi_present) return -1;
	if (wifi_state == WIFI_STATE_OFF) return -1;

	wifi_state = WIFI_STATE_SCANNING;
	build_scan_results();
	/* Scanning completes instantly in the virtual driver */
	if (wifi_state == WIFI_STATE_SCANNING)
		wifi_state = has_current ? WIFI_STATE_CONNECTED : WIFI_STATE_IDLE;
	return scan_count;
}

int wifi_scan_count(void) {
	return scan_count;
}

const wifi_network_t* wifi_scan_results(void) {
	return scan_buf;
}

int wifi_connect(const char* ssid, const char* pass) {
	if (!wifi_present || !ssid) return -1;

	/* Find the network in scan results */
	int idx = -1;
	for (int i = 0; i < scan_count; i++) {
		if (str_eq(scan_buf[i].ssid, ssid)) { idx = i; break; }
	}
	if (idx < 0) return -1;

	wifi_state = WIFI_STATE_CONNECTING;
	klog_write(KLOG_LEVEL_INFO, "wifi", "Connecting to SSID...");

	/* For secure networks, verify password is provided */
	if (scan_buf[idx].security != WIFI_SEC_OPEN) {
		if (!pass || pass[0] == '\0') {
			klog_write(KLOG_LEVEL_WARN, "wifi", "Password required");
			wifi_state = WIFI_STATE_IDLE;
			return -1;
		}
	}

	/*
	 * "Association" succeeds if the backing ethernet is up.
	 * For the special "LythNet" SSID, we share connectivity with eth0.
	 * Other simulated SSIDs fail to connect (no real AP behind them).
	 */
	if (str_eq(ssid, "LythNet") && backing_iface && backing_iface->up) {
		/* Verify password for WPA2 */
		if (scan_buf[idx].security != WIFI_SEC_OPEN) {
			const char* expected = "lyth1234";
			int i;
			for (i = 0; expected[i]; i++) {
				if (!pass[i] || pass[i] != expected[i]) {
					klog_write(KLOG_LEVEL_WARN, "wifi", "Wrong password");
					wifi_state = WIFI_STATE_IDLE;
					return -1;
				}
			}
			if (pass[i] != '\0') {
				klog_write(KLOG_LEVEL_WARN, "wifi", "Wrong password");
				wifi_state = WIFI_STATE_IDLE;
				return -1;
			}
		}

		/* Copy eth0 address config to wlan0 so routing works */
		wlan_iface->up = 1;
		netif_set_addr(wlan_iface,
			backing_iface->ip_addr,
			backing_iface->netmask,
			backing_iface->gateway);
		wlan_iface->dns_server = backing_iface->dns_server;

		/* If no IP yet, run DHCP over the wifi interface */
		if (wlan_iface->ip_addr == 0) {
			dhcp_discover(wlan_iface);
			unsigned int deadline = timer_get_uptime_ms() + 3000;
			while (!dhcp_get_result()->ok &&
			       timer_get_uptime_ms() < deadline) {
				e1000_poll_rx();
			}
			if (!dhcp_get_result()->ok)
				klog_write(KLOG_LEVEL_WARN, "wifi", "DHCP over wlan0 timeout");
		}

		memcpy(&current_net, &scan_buf[idx], sizeof(wifi_network_t));
		has_current = 1;
		wifi_state = WIFI_STATE_CONNECTED;

		/* Auto-save to known list */
		wifi_known_add(ssid, pass ? pass : "", scan_buf[idx].security);

		klog_write(KLOG_LEVEL_INFO, "wifi", "Connected to LythNet");
		return 0;
	}

	/* Other SSIDs: simulated connection failure */
	klog_write(KLOG_LEVEL_WARN, "wifi", "Association failed (no AP)");
	wifi_state = WIFI_STATE_IDLE;
	return -1;
}

void wifi_disconnect(void) {
	if (!wifi_present) return;
	if (wifi_state != WIFI_STATE_CONNECTED) return;

	has_current = 0;
	wifi_state = WIFI_STATE_IDLE;

	if (wlan_iface) {
		wlan_iface->up = 0;
		netif_set_addr(wlan_iface, 0, 0, 0);
		wlan_iface->dns_server = 0;
	}

	klog_write(KLOG_LEVEL_INFO, "wifi", "Disconnected");
}

const wifi_network_t* wifi_current_network(void) {
	if (has_current && wifi_state == WIFI_STATE_CONNECTED)
		return &current_net;
	return 0;
}

void wifi_poll(void) {
	/* In virtual mode, nothing special — the backing NIC handles RX.
	 * On real hardware this would pull frames from the 802.11 RX ring. */
	if (!wifi_present) return;
}

/* ── known / saved networks ─────────────────────────────────────── */

int wifi_known_count(void) {
	return known_count;
}

const wifi_saved_t* wifi_known_list(void) {
	return known_nets;
}

int wifi_known_add(const char* ssid, const char* pass, int sec) {
	if (!ssid) return -1;

	/* update existing entry */
	for (int i = 0; i < known_count; i++) {
		if (str_eq(known_nets[i].ssid, ssid)) {
			wifi_str_copy(known_nets[i].password, pass ? pass : "", WIFI_PASS_MAX);
			known_nets[i].security = sec;
			return 0;
		}
	}
	if (known_count >= WIFI_KNOWN_MAX) return -1;

	wifi_str_copy(known_nets[known_count].ssid, ssid, WIFI_SSID_MAX);
	wifi_str_copy(known_nets[known_count].password, pass ? pass : "", WIFI_PASS_MAX);
	known_nets[known_count].security = sec;
	known_count++;
	return 0;
}

void wifi_known_remove(const char* ssid) {
	if (!ssid) return;
	for (int i = 0; i < known_count; i++) {
		if (str_eq(known_nets[i].ssid, ssid)) {
			/* shift remaining entries */
			for (int j = i; j < known_count - 1; j++)
				known_nets[j] = known_nets[j + 1];
			known_count--;
			return;
		}
	}
}
