#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include "netif.h"

#define UDP_HDR_LEN 8

typedef struct __attribute__((packed)) {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t length;
	uint16_t checksum;
} udp_header_t;

/* Simple UDP socket */
#define UDP_MAX_SOCKETS 16
#define UDP_RX_BUF_SIZE 4096

typedef struct {
	int      used;
	uint16_t local_port;
	uint32_t remote_ip;     /* filter, 0 = any */
	uint16_t remote_port;   /* filter, 0 = any */
	uint8_t  rx_buf[UDP_RX_BUF_SIZE];
	uint16_t rx_len;
	uint32_t rx_from_ip;
	uint16_t rx_from_port;
	int      rx_ready;
} udp_socket_t;

void udp_init(void);
void udp_rx(netif_t* iface, uint32_t src_ip, uint32_t dst_ip,
			const uint8_t* data, uint16_t len);
int  udp_send(netif_t* iface, uint32_t dst_ip,
			  uint16_t src_port, uint16_t dst_port,
			  const uint8_t* payload, uint16_t payload_len);

/* Socket-level API */
int  udp_socket_open(uint16_t local_port);
int  udp_socket_close(int sock);
int  udp_socket_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
					   const uint8_t* data, uint16_t len);
int  udp_socket_recvfrom(int sock, uint8_t* buf, uint16_t buf_len,
						 uint32_t* from_ip, uint16_t* from_port);
int  udp_socket_bind(int sock, uint32_t remote_ip, uint16_t remote_port);

#endif
