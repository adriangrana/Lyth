#include "netif.h"
#include "ethernet.h"
#include "string.h"
#include "klog.h"

static netif_t interfaces[NETIF_MAX];
static int     iface_count;
static netif_t* default_iface;

void netif_init(void) {
	memset(interfaces, 0, sizeof(interfaces));
	iface_count  = 0;
	default_iface = 0;
}

netif_t* netif_register(const char* name,
						const uint8_t mac[6],
						int (*send_fn)(netif_t*, const uint8_t*, uint16_t))
{
	if (iface_count >= NETIF_MAX) return 0;
	netif_t* iface = &interfaces[iface_count++];
	memset(iface, 0, sizeof(*iface));
	int i = 0;
	while (name[i] && i < NETIF_NAME_MAX - 1) { iface->name[i] = name[i]; i++; }
	iface->name[i] = '\0';
	memcpy(iface->mac, mac, 6);
	iface->send = send_fn;
	iface->up   = 1;

	if (!default_iface)
		default_iface = iface;

	return iface;
}

netif_t* netif_get_default(void) { return default_iface; }

netif_t* netif_find(const char* name) {
	for (int i = 0; i < iface_count; i++) {
		if (strcmp(interfaces[i].name, name) == 0)
			return &interfaces[i];
	}
	return 0;
}

int netif_count(void) { return iface_count; }

netif_t* netif_get(int index) {
	if (index < 0 || index >= iface_count) return 0;
	return &interfaces[index];
}

void netif_set_addr(netif_t* iface, uint32_t ip, uint32_t mask, uint32_t gw) {
	if (!iface) return;
	iface->ip_addr = ip;
	iface->netmask = mask;
	iface->gateway = gw;
}

void netif_rx(netif_t* iface, const uint8_t* data, uint16_t len) {
	eth_rx(iface, data, len);
}
