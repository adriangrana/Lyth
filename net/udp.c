#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "dhcp.h"
#include "dns.h"
#include "endian.h"
#include "string.h"

static udp_socket_t sockets[UDP_MAX_SOCKETS];

void udp_init(void) {
	memset(sockets, 0, sizeof(sockets));
}

void udp_rx(netif_t* iface, uint32_t src_ip, uint32_t dst_ip,
			const uint8_t* data, uint16_t len)
{
	if (len < UDP_HDR_LEN) return;
	const udp_header_t* hdr = (const udp_header_t*)data;
	uint16_t src_port = ntohs(hdr->src_port);
	uint16_t dst_port = ntohs(hdr->dst_port);
	uint16_t udp_len  = ntohs(hdr->length);
	uint16_t payload_len = udp_len - UDP_HDR_LEN;
	const uint8_t* payload = data + UDP_HDR_LEN;

	(void)dst_ip;

	/* DHCP reply handling (port 68) */
	if (dst_port == DHCP_CLIENT_PORT) {
		dhcp_handle_reply(iface, payload, payload_len);
		return;
	}

	/* DNS reply handling (from port 53) */
	if (src_port == DNS_PORT) {
		dns_rx(payload, payload_len);
		return;
	}

	/* Deliver to bound socket */
	for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
		if (!sockets[i].used) continue;
		if (sockets[i].local_port != dst_port) continue;
		if (sockets[i].remote_ip && sockets[i].remote_ip != src_ip) continue;
		if (sockets[i].remote_port && sockets[i].remote_port != src_port) continue;

		uint16_t copy_len = payload_len;
		if (copy_len > UDP_RX_BUF_SIZE) copy_len = UDP_RX_BUF_SIZE;
		memcpy(sockets[i].rx_buf, payload, copy_len);
		sockets[i].rx_len = copy_len;
		sockets[i].rx_from_ip = src_ip;
		sockets[i].rx_from_port = src_port;
		sockets[i].rx_ready = 1;
		return;
	}
}

int udp_send(netif_t* iface, uint32_t dst_ip,
			 uint16_t src_port, uint16_t dst_port,
			 const uint8_t* payload, uint16_t payload_len)
{
	uint16_t udp_total = UDP_HDR_LEN + payload_len;
	uint8_t buf[ETH_MTU];
	if (udp_total > ETH_MTU - 20) return -1;

	udp_header_t* hdr = (udp_header_t*)buf;
	hdr->src_port = htons(src_port);
	hdr->dst_port = htons(dst_port);
	hdr->length   = htons(udp_total);
	hdr->checksum = 0; /* checksum optional in UDP over IPv4 */

	memcpy(buf + UDP_HDR_LEN, payload, payload_len);

	return ipv4_send(iface, dst_ip, IP_PROTO_UDP, buf, udp_total);
}

/* ── Socket API ─────────────────────────────────────────────────── */

int udp_socket_open(uint16_t local_port) {
	for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
		if (!sockets[i].used) {
			memset(&sockets[i], 0, sizeof(udp_socket_t));
			sockets[i].used = 1;
			sockets[i].local_port = local_port;
			return i;
		}
	}
	return -1;
}

int udp_socket_close(int sock) {
	if (sock < 0 || sock >= UDP_MAX_SOCKETS) return -1;
	sockets[sock].used = 0;
	return 0;
}

int udp_socket_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
					  const uint8_t* data, uint16_t len)
{
	if (sock < 0 || sock >= UDP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	return udp_send(netif_get_default(), dst_ip,
					sockets[sock].local_port, dst_port, data, len);
}

int udp_socket_recvfrom(int sock, uint8_t* buf, uint16_t buf_len,
						uint32_t* from_ip, uint16_t* from_port)
{
	if (sock < 0 || sock >= UDP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	if (!sockets[sock].rx_ready)
		return 0; /* no data */

	uint16_t copy = sockets[sock].rx_len;
	if (copy > buf_len) copy = buf_len;
	memcpy(buf, sockets[sock].rx_buf, copy);
	if (from_ip) *from_ip = sockets[sock].rx_from_ip;
	if (from_port) *from_port = sockets[sock].rx_from_port;
	sockets[sock].rx_ready = 0;
	return (int)copy;
}

int udp_socket_bind(int sock, uint32_t remote_ip, uint16_t remote_port) {
	if (sock < 0 || sock >= UDP_MAX_SOCKETS || !sockets[sock].used)
		return -1;
	sockets[sock].remote_ip = remote_ip;
	sockets[sock].remote_port = remote_port;
	return 0;
}
