#ifndef NET_ARP_H
#define NET_ARP_H

#include <stdint.h>
#include "netif.h"

#define ARP_HW_ETHERNET  1
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

#define ARP_CACHE_SIZE  32

typedef struct __attribute__((packed)) {
	uint16_t hw_type;
	uint16_t proto_type;
	uint8_t  hw_len;
	uint8_t  proto_len;
	uint16_t opcode;
	uint8_t  sender_mac[6];
	uint32_t sender_ip;
	uint8_t  target_mac[6];
	uint32_t target_ip;
} arp_packet_t;

typedef struct {
	uint32_t ip;
	uint8_t  mac[6];
	uint32_t timestamp; /* ticks when entry was added */
	int      valid;
} arp_entry_t;

void arp_init(void);
void arp_rx(netif_t* iface, const uint8_t* data, uint16_t len);
int  arp_resolve(netif_t* iface, uint32_t ip, uint8_t mac_out[6]);
void arp_request(netif_t* iface, uint32_t target_ip);
const arp_entry_t* arp_cache_get(int index);

#endif
