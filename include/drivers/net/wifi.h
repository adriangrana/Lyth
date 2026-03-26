#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>

/* ── WiFi security types ────────────────────────────────────────── */
#define WIFI_SEC_OPEN    0
#define WIFI_SEC_WEP     1
#define WIFI_SEC_WPA     2
#define WIFI_SEC_WPA2    3

/* ── WiFi connection states ─────────────────────────────────────── */
#define WIFI_STATE_OFF          0   /* radio disabled or not present */
#define WIFI_STATE_IDLE         1   /* radio on, not connected */
#define WIFI_STATE_SCANNING     2   /* scanning for networks */
#define WIFI_STATE_CONNECTING   3   /* associating / authenticating */
#define WIFI_STATE_CONNECTED    4   /* associated and IP-configured */

/* ── Scan result entry ──────────────────────────────────────────── */
#define WIFI_SSID_MAX    33  /* 32 chars + NUL */
#define WIFI_SCAN_MAX    16  /* max visible networks */

typedef struct {
	char     ssid[WIFI_SSID_MAX];
	uint8_t  bssid[6];
	int      signal;        /* dBm estimate (-30 = excellent, -90 = weak) */
	int      channel;
	int      security;      /* WIFI_SEC_* */
} wifi_network_t;

/* ── Saved / known network ──────────────────────────────────────── */
#define WIFI_PASS_MAX    64
#define WIFI_KNOWN_MAX   8

typedef struct {
	char ssid[WIFI_SSID_MAX];
	char password[WIFI_PASS_MAX];
	int  security;
} wifi_saved_t;

/* ── Public API ─────────────────────────────────────────────────── */

int  wifi_init(void);                                    /* probe & init */
int  wifi_get_state(void);                               /* WIFI_STATE_* */
int  wifi_scan(void);                                    /* trigger scan */
int  wifi_scan_count(void);                              /* # results */
const wifi_network_t* wifi_scan_results(void);           /* array */
int  wifi_connect(const char* ssid, const char* pass);   /* start assoc */
void wifi_disconnect(void);
const wifi_network_t* wifi_current_network(void);        /* NULL if idle */
void wifi_poll(void);                                    /* RX poll tick */

/* saved (known) networks */
int  wifi_known_count(void);
const wifi_saved_t* wifi_known_list(void);
int  wifi_known_add(const char* ssid, const char* pass, int sec);
void wifi_known_remove(const char* ssid);

#endif
