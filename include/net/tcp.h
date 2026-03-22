#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>
#include "netif.h"

#define TCP_HDR_MIN_LEN 20
#define TCP_MAX_SOCKETS 16
#define TCP_RX_BUF_SIZE 8192
#define TCP_TX_BUF_SIZE 8192
#define TCP_MSS         1460
#define TCP_WINDOW_SIZE 8192

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

typedef struct __attribute__((packed)) {
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq_num;
	uint32_t ack_num;
	uint8_t  data_off;   /* upper 4 bits = header length in 32-bit words */
	uint8_t  flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent;
} tcp_header_t;

/* TCP connection states */
typedef enum {
	TCP_CLOSED,
	TCP_LISTEN,
	TCP_SYN_SENT,
	TCP_SYN_RECEIVED,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSE_WAIT,
	TCP_CLOSING,
	TCP_LAST_ACK,
	TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
	int         used;
	tcp_state_t state;
	uint16_t    local_port;
	uint32_t    remote_ip;
	uint16_t    remote_port;
	uint32_t    snd_una;    /* oldest unacked */
	uint32_t    snd_nxt;    /* next to send */
	uint32_t    rcv_nxt;    /* next expected */
	uint32_t    rcv_wnd;
	uint8_t     rx_buf[TCP_RX_BUF_SIZE];
	uint16_t    rx_len;
	int         rx_ready;
	/* Retransmit state */
	uint32_t    last_ack_tick;
	int         retransmit_count;
} tcp_socket_t;

void tcp_init(void);
void tcp_rx(netif_t* iface, uint32_t src_ip, uint32_t dst_ip,
			const uint8_t* data, uint16_t len);
void tcp_tick(void);   /* called periodically for retransmits/timeouts */

/* Socket API */
int  tcp_socket_open(void);
int  tcp_socket_connect(int sock, uint32_t dst_ip, uint16_t dst_port);
int  tcp_socket_listen(int sock, uint16_t port);
int  tcp_socket_accept(int sock);
int  tcp_socket_send(int sock, const uint8_t* data, uint16_t len);
int  tcp_socket_recv(int sock, uint8_t* buf, uint16_t buf_len);
int  tcp_socket_close(int sock);
tcp_state_t tcp_socket_state(int sock);

#endif
