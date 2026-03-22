#include "tcp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "endian.h"
#include "string.h"
#include "timer.h"

static tcp_socket_t sockets[TCP_MAX_SOCKETS];
static uint32_t tcp_isn;  /* initial sequence number counter */

/* ── Pseudo-header for TCP checksum ─────────────────────────────── */

typedef struct __attribute__((packed)) {
	uint32_t src;
	uint32_t dst;
	uint8_t  zero;
	uint8_t  protocol;
	uint16_t tcp_len;
} tcp_pseudo_t;

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
							 const uint8_t* tcp_seg, uint16_t tcp_len)
{
	tcp_pseudo_t pseudo;
	pseudo.src = src_ip;
	pseudo.dst = dst_ip;
	pseudo.zero = 0;
	pseudo.protocol = IP_PROTO_TCP;
	pseudo.tcp_len = htons(tcp_len);

	uint32_t sum = 0;

	/* Sum pseudo-header - copy to aligned buffer to avoid packed pointer warning */
	uint8_t pseudo_buf[12];
	memcpy(pseudo_buf, &pseudo, 12);
	const uint16_t* pw = (const uint16_t*)pseudo_buf;
	for (int i = 0; i < 6; i++) sum += pw[i];

	/* Sum TCP segment */
	const uint16_t* p = (const uint16_t*)tcp_seg;
	uint16_t rem = tcp_len;
	while (rem > 1) { sum += *p++; rem -= 2; }
	if (rem == 1) sum += *(const uint8_t*)p;

	while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)(~sum);
}

/* ── Send a TCP segment ─────────────────────────────────────────── */

static int tcp_send_segment(netif_t* iface, uint32_t dst_ip,
							uint16_t src_port, uint16_t dst_port,
							uint32_t seq, uint32_t ack,
							uint8_t flags, uint16_t window,
							const uint8_t* payload, uint16_t payload_len)
{
	uint16_t tcp_len = TCP_HDR_MIN_LEN + payload_len;
	uint8_t buf[ETH_MTU];
	if (tcp_len > ETH_MTU - 20) return -1;

	tcp_header_t* hdr = (tcp_header_t*)buf;
	memset(buf, 0, TCP_HDR_MIN_LEN);
	hdr->src_port = htons(src_port);
	hdr->dst_port = htons(dst_port);
	hdr->seq_num  = htonl(seq);
	hdr->ack_num  = htonl(ack);
	hdr->data_off = (TCP_HDR_MIN_LEN / 4) << 4;
	hdr->flags    = flags;
	hdr->window   = htons(window);
	hdr->checksum = 0;
	hdr->urgent   = 0;

	if (payload && payload_len > 0)
		memcpy(buf + TCP_HDR_MIN_LEN, payload, payload_len);

	uint32_t src_ip = 0;
	netif_t* out_iface = iface ? iface : netif_get_default();
	if (out_iface) src_ip = out_iface->ip_addr;

	hdr->checksum = tcp_checksum(src_ip, dst_ip, buf, tcp_len);

	return ipv4_send(out_iface, dst_ip, IP_PROTO_TCP, buf, tcp_len);
}

/* ── Init ───────────────────────────────────────────────────────── */

void tcp_init(void) {
	memset(sockets, 0, sizeof(sockets));
	tcp_isn = timer_get_ticks() * 12345 + 67890;
}

/* ── RX processing ──────────────────────────────────────────────── */

void tcp_rx(netif_t* iface, uint32_t src_ip, uint32_t dst_ip,
			const uint8_t* data, uint16_t len)
{
	if (len < TCP_HDR_MIN_LEN) return;

	(void)dst_ip;
	const tcp_header_t* hdr = (const tcp_header_t*)data;
	uint16_t src_port = ntohs(hdr->src_port);
	uint16_t dst_port = ntohs(hdr->dst_port);
	uint32_t seq = ntohl(hdr->seq_num);
	uint32_t ack = ntohl(hdr->ack_num);
	uint8_t flags = hdr->flags;
	uint8_t data_off = (hdr->data_off >> 4) * 4;
	uint16_t payload_len = len - data_off;
	const uint8_t* payload = data + data_off;

	/* Find matching socket */
	tcp_socket_t* sk = 0;
	int sk_idx = -1;

	/* First try exact match */
	for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
		if (!sockets[i].used) continue;
		if (sockets[i].local_port == dst_port &&
			sockets[i].remote_port == src_port &&
			sockets[i].remote_ip == src_ip) {
			sk = &sockets[i];
			sk_idx = i;
			break;
		}
	}

	/* Try listening socket */
	if (!sk) {
		for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
			if (!sockets[i].used) continue;
			if (sockets[i].state == TCP_LISTEN &&
				sockets[i].local_port == dst_port) {
				sk = &sockets[i];
				sk_idx = i;
				break;
			}
		}
	}

	/* Try SYN_SENT */
	if (!sk) {
		for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
			if (!sockets[i].used) continue;
			if (sockets[i].state == TCP_SYN_SENT &&
				sockets[i].remote_ip == src_ip &&
				sockets[i].remote_port == src_port) {
				sk = &sockets[i];
				sk_idx = i;
				break;
			}
		}
	}

	if (!sk) {
		/* Send RST for unmatched segments (except RST) */
		if (!(flags & TCP_RST)) {
			tcp_send_segment(iface, src_ip, dst_port, src_port,
							 0, seq + 1, TCP_RST | TCP_ACK,
							 0, 0, 0);
		}
		return;
	}

	(void)sk_idx;

	switch (sk->state) {
	case TCP_LISTEN:
		if (flags & TCP_SYN) {
			sk->remote_ip = src_ip;
			sk->remote_port = src_port;
			sk->rcv_nxt = seq + 1;
			sk->snd_nxt = tcp_isn++;
			sk->snd_una = sk->snd_nxt;
			sk->state = TCP_SYN_RECEIVED;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_SYN | TCP_ACK, TCP_WINDOW_SIZE,
							 0, 0);
			sk->snd_nxt++;
		}
		break;

	case TCP_SYN_SENT:
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
			sk->rcv_nxt = seq + 1;
			sk->snd_una = ack;
			sk->state = TCP_ESTABLISHED;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, TCP_WINDOW_SIZE,
							 0, 0);
		}
		break;

	case TCP_SYN_RECEIVED:
		if (flags & TCP_ACK) {
			sk->snd_una = ack;
			sk->state = TCP_ESTABLISHED;
		}
		break;

	case TCP_ESTABLISHED:
		if (flags & TCP_FIN) {
			sk->rcv_nxt = seq + payload_len + 1;
			sk->state = TCP_CLOSE_WAIT;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, TCP_WINDOW_SIZE,
							 0, 0);
		} else if (payload_len > 0) {
			if (seq == sk->rcv_nxt) {
				uint16_t space = TCP_RX_BUF_SIZE - sk->rx_len;
				uint16_t copy = payload_len < space ? payload_len : space;
				if (copy > 0) {
					memcpy(sk->rx_buf + sk->rx_len, payload, copy);
					sk->rx_len += copy;
					sk->rx_ready = 1;
				}
				sk->rcv_nxt += payload_len;
			}
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, TCP_WINDOW_SIZE,
							 0, 0);
		} else if (flags & TCP_ACK) {
			sk->snd_una = ack;
		}
		if (flags & TCP_RST) {
			sk->state = TCP_CLOSED;
			sk->used = 0;
		}
		break;

	case TCP_FIN_WAIT_1:
		if ((flags & TCP_ACK) && (flags & TCP_FIN)) {
			sk->rcv_nxt = seq + 1;
			sk->state = TCP_TIME_WAIT;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, 0, 0, 0);
		} else if (flags & TCP_ACK) {
			sk->snd_una = ack;
			sk->state = TCP_FIN_WAIT_2;
		} else if (flags & TCP_FIN) {
			sk->rcv_nxt = seq + 1;
			sk->state = TCP_CLOSING;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, 0, 0, 0);
		}
		break;

	case TCP_FIN_WAIT_2:
		if (flags & TCP_FIN) {
			sk->rcv_nxt = seq + 1;
			sk->state = TCP_TIME_WAIT;
			tcp_send_segment(iface, src_ip, sk->local_port, src_port,
							 sk->snd_nxt, sk->rcv_nxt,
							 TCP_ACK, 0, 0, 0);
		}
		break;

	case TCP_CLOSING:
		if (flags & TCP_ACK) {
			sk->state = TCP_TIME_WAIT;
		}
		break;

	case TCP_LAST_ACK:
		if (flags & TCP_ACK) {
			sk->state = TCP_CLOSED;
			sk->used = 0;
		}
		break;

	case TCP_CLOSE_WAIT:
		/* Application will call close */
		break;

	case TCP_TIME_WAIT:
		/* Wait then close */
		sk->last_ack_tick = timer_get_ticks();
		break;

	default:
		break;
	}
}

void tcp_tick(void) {
	uint32_t now = timer_get_ticks();
	for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
		if (!sockets[i].used) continue;
		if (sockets[i].state == TCP_TIME_WAIT &&
			(now - sockets[i].last_ack_tick) > 200) {
			sockets[i].state = TCP_CLOSED;
			sockets[i].used = 0;
		}
	}
}

/* ── Socket API ─────────────────────────────────────────────────── */

int tcp_socket_open(void) {
	for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
		if (!sockets[i].used) {
			memset(&sockets[i], 0, sizeof(tcp_socket_t));
			sockets[i].used = 1;
			sockets[i].state = TCP_CLOSED;
			sockets[i].rcv_wnd = TCP_WINDOW_SIZE;
			return i;
		}
	}
	return -1;
}

int tcp_socket_connect(int sock, uint32_t dst_ip, uint16_t dst_port) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;

	tcp_socket_t* sk = &sockets[sock];
	sk->remote_ip = dst_ip;
	sk->remote_port = dst_port;
	sk->local_port = (uint16_t)(49152 + (tcp_isn++ % 16384));
	sk->snd_nxt = tcp_isn++;
	sk->snd_una = sk->snd_nxt;
	sk->state = TCP_SYN_SENT;

	tcp_send_segment(0, dst_ip, sk->local_port, dst_port,
					 sk->snd_nxt, 0, TCP_SYN, TCP_WINDOW_SIZE,
					 0, 0);
	sk->snd_nxt++;

	/* Wait for connection (simple busy-wait with timeout) */
	uint32_t start = timer_get_ticks();
	while (sk->state == TCP_SYN_SENT && (timer_get_ticks() - start) < 500);

	return (sk->state == TCP_ESTABLISHED) ? 0 : -1;
}

int tcp_socket_listen(int sock, uint16_t port) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	sockets[sock].local_port = port;
	sockets[sock].state = TCP_LISTEN;
	return 0;
}

int tcp_socket_accept(int sock) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	if (sockets[sock].state != TCP_LISTEN)
		return -1;

	/* Wait for incoming SYN → ESTABLISHED */
	uint32_t start = timer_get_ticks();
	while (sockets[sock].state != TCP_ESTABLISHED &&
		   sockets[sock].state != TCP_SYN_RECEIVED &&
		   (timer_get_ticks() - start) < 3000);

	if (sockets[sock].state == TCP_SYN_RECEIVED) {
		while (sockets[sock].state == TCP_SYN_RECEIVED &&
			   (timer_get_ticks() - start) < 3000);
	}

	return (sockets[sock].state == TCP_ESTABLISHED) ? sock : -1;
}

int tcp_socket_send(int sock, const uint8_t* data, uint16_t len) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	if (sockets[sock].state != TCP_ESTABLISHED &&
		sockets[sock].state != TCP_CLOSE_WAIT)
		return -1;

	tcp_socket_t* sk = &sockets[sock];
	uint16_t sent = 0;

	while (sent < len) {
		uint16_t chunk = len - sent;
		if (chunk > TCP_MSS) chunk = TCP_MSS;

		tcp_send_segment(0, sk->remote_ip,
						 sk->local_port, sk->remote_port,
						 sk->snd_nxt, sk->rcv_nxt,
						 TCP_ACK | TCP_PSH, TCP_WINDOW_SIZE,
						 data + sent, chunk);
		sk->snd_nxt += chunk;
		sent += chunk;
	}
	return (int)sent;
}

int tcp_socket_recv(int sock, uint8_t* buf, uint16_t buf_len) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;

	tcp_socket_t* sk = &sockets[sock];
	if (!sk->rx_ready && sk->state != TCP_ESTABLISHED &&
		sk->state != TCP_CLOSE_WAIT)
		return -1;

	if (!sk->rx_ready)
		return 0;

	uint16_t copy = sk->rx_len;
	if (copy > buf_len) copy = buf_len;
	memcpy(buf, sk->rx_buf, copy);

	/* Shift remaining data */
	uint16_t remaining = sk->rx_len - copy;
	if (remaining > 0)
		memmove(sk->rx_buf, sk->rx_buf + copy, remaining);
	sk->rx_len = remaining;
	if (remaining == 0)
		sk->rx_ready = 0;

	return (int)copy;
}

int tcp_socket_close(int sock) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return -1;

	tcp_socket_t* sk = &sockets[sock];

	if (sk->state == TCP_ESTABLISHED) {
		sk->state = TCP_FIN_WAIT_1;
		tcp_send_segment(0, sk->remote_ip,
						 sk->local_port, sk->remote_port,
						 sk->snd_nxt, sk->rcv_nxt,
						 TCP_FIN | TCP_ACK, 0, 0, 0);
		sk->snd_nxt++;
	} else if (sk->state == TCP_CLOSE_WAIT) {
		sk->state = TCP_LAST_ACK;
		tcp_send_segment(0, sk->remote_ip,
						 sk->local_port, sk->remote_port,
						 sk->snd_nxt, sk->rcv_nxt,
						 TCP_FIN | TCP_ACK, 0, 0, 0);
		sk->snd_nxt++;
	} else {
		sk->state = TCP_CLOSED;
		sk->used = 0;
	}

	return 0;
}

tcp_state_t tcp_socket_state(int sock) {
	if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].used)
		return TCP_CLOSED;
	return sockets[sock].state;
}
