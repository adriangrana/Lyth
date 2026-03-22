#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <stdint.h>
#include "netif.h"

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP  17

#define IP_HDR_MIN_LEN 20

typedef struct __attribute__((packed)) {
	uint8_t  version_ihl;
	uint8_t  tos;
	uint16_t total_length;
	uint16_t identification;
	uint16_t flags_fragment;
	uint8_t  ttl;
	uint8_t  protocol;
	uint16_t checksum;
	uint32_t src_ip;
	uint32_t dst_ip;
} ipv4_header_t;

void     ipv4_init(void);
void     ipv4_rx(netif_t* iface, const uint8_t* data, uint16_t len);
int      ipv4_send(netif_t* iface, uint32_t dst_ip, uint8_t protocol,
				   const uint8_t* payload, uint16_t payload_len);
uint16_t ipv4_checksum(const void* data, uint16_t len);

/* Routing table (simple) */
#define ROUTE_MAX 8

typedef struct {
	uint32_t network;   /* network byte order */
	uint32_t netmask;   /* network byte order */
	uint32_t gateway;   /* network byte order, 0 = direct */
	netif_t* iface;
} route_entry_t;

void      route_add(uint32_t network, uint32_t netmask, uint32_t gateway, netif_t* iface);
netif_t*  route_lookup(uint32_t dst_ip, uint32_t* next_hop_out);

/* Helper: make IP from 4 octets (host byte order input → network byte order) */
static inline uint32_t ip4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
	return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

/* Pretty-print helpers */
void ip4_to_str(uint32_t ip, char buf[16]);

#endif
