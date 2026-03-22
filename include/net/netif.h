#ifndef NET_NETIF_H
#define NET_NETIF_H

#include <stdint.h>

#define NETIF_NAME_MAX 8
#define NETIF_MAX      4

typedef struct netif {
	char     name[NETIF_NAME_MAX];
	uint8_t  mac[6];
	uint32_t ip_addr;     /* network byte order */
	uint32_t netmask;     /* network byte order */
	uint32_t gateway;     /* network byte order */
	uint32_t dns_server;  /* network byte order */
	int      up;
	int      (*send)(struct netif* iface, const uint8_t* data, uint16_t len);
} netif_t;

void     netif_init(void);
netif_t* netif_register(const char* name,
						const uint8_t mac[6],
						int (*send_fn)(struct netif*, const uint8_t*, uint16_t));
netif_t* netif_get_default(void);
netif_t* netif_find(const char* name);
int      netif_count(void);
netif_t* netif_get(int index);
void     netif_set_addr(netif_t* iface, uint32_t ip, uint32_t mask, uint32_t gw);
void     netif_rx(netif_t* iface, const uint8_t* data, uint16_t len);

#endif
