#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <stdint.h>
#include "netif.h"

#define ICMP_ECHO_REPLY    0
#define ICMP_DEST_UNREACH  3
#define ICMP_ECHO_REQUEST  8

typedef struct __attribute__((packed)) {
	uint8_t  type;
	uint8_t  code;
	uint16_t checksum;
	uint16_t id;
	uint16_t seq;
} icmp_header_t;

void icmp_rx(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len);
int  icmp_send_echo(netif_t* iface, uint32_t dst_ip, uint16_t id, uint16_t seq,
					const uint8_t* payload, uint16_t payload_len);

/* Ping callback — set by shell ping command to receive replies */
typedef void (*icmp_reply_cb_t)(uint32_t src_ip, uint16_t id, uint16_t seq, uint16_t data_len);
void icmp_set_reply_callback(icmp_reply_cb_t cb);

#endif
