#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <stdint.h>
#include "netif.h"

#define ETH_ADDR_LEN   6
#define ETH_HDR_LEN   14
#define ETH_MTU      1500
#define ETH_FRAME_MAX (ETH_HDR_LEN + ETH_MTU)

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD

typedef struct __attribute__((packed)) {
	uint8_t  dst[ETH_ADDR_LEN];
	uint8_t  src[ETH_ADDR_LEN];
	uint16_t ethertype;
} eth_header_t;

void eth_rx(netif_t* iface, const uint8_t* frame, uint16_t len);
int  eth_send(netif_t* iface, const uint8_t dst_mac[6],
			  uint16_t ethertype, const uint8_t* payload, uint16_t payload_len);

extern const uint8_t ETH_BROADCAST[6];

#endif
