#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <stdint.h>
#include "netif.h"

/* DHCP message types */
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_DECLINE   4
#define DHCP_ACK       5
#define DHCP_NAK       6
#define DHCP_RELEASE   7

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_MAGIC_COOKIE 0x63825363

typedef struct __attribute__((packed)) {
	uint8_t  op;
	uint8_t  htype;
	uint8_t  hlen;
	uint8_t  hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t  chaddr[16];
	uint8_t  sname[64];
	uint8_t  file[128];
	uint32_t magic;
	uint8_t  options[312];
} dhcp_packet_t;

/* Results from DHCP */
typedef struct {
	uint32_t ip_addr;
	uint32_t netmask;
	uint32_t gateway;
	uint32_t dns_server;
	uint32_t lease_time;
	volatile int ok;
} dhcp_result_t;

int  dhcp_discover(netif_t* iface);
void dhcp_handle_reply(netif_t* iface, const uint8_t* data, uint16_t len);
const dhcp_result_t* dhcp_get_result(void);

#endif
