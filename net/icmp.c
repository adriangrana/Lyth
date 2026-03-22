#include "icmp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "endian.h"
#include "string.h"

static icmp_reply_cb_t reply_callback;

void icmp_set_reply_callback(icmp_reply_cb_t cb) {
	reply_callback = cb;
}

void icmp_rx(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len) {
	if (len < sizeof(icmp_header_t)) return;

	const icmp_header_t* hdr = (const icmp_header_t*)data;

	if (hdr->type == ICMP_ECHO_REQUEST) {
		/* Reply: swap src/dst, change type to reply, recalculate checksum */
		uint8_t reply[ETH_MTU];
		if (len > ETH_MTU - 20) return; /* safety */
		memcpy(reply, data, len);

		icmp_header_t* rhdr = (icmp_header_t*)reply;
		rhdr->type = ICMP_ECHO_REPLY;
		rhdr->checksum = 0;
		rhdr->checksum = ipv4_checksum(reply, len);

		ipv4_send(iface, src_ip, IP_PROTO_ICMP, reply, len);
	} else if (hdr->type == ICMP_ECHO_REPLY) {
		if (reply_callback) {
			reply_callback(src_ip, ntohs(hdr->id), ntohs(hdr->seq),
						   len - (uint16_t)sizeof(icmp_header_t));
		}
	}
}

int icmp_send_echo(netif_t* iface, uint32_t dst_ip, uint16_t id, uint16_t seq,
				   const uint8_t* payload, uint16_t payload_len)
{
	uint16_t total = (uint16_t)(sizeof(icmp_header_t) + payload_len);
	uint8_t buf[ETH_MTU];
	if (total > ETH_MTU - 20) return -1;

	icmp_header_t* hdr = (icmp_header_t*)buf;
	hdr->type     = ICMP_ECHO_REQUEST;
	hdr->code     = 0;
	hdr->checksum = 0;
	hdr->id       = htons(id);
	hdr->seq      = htons(seq);

	if (payload && payload_len > 0)
		memcpy(buf + sizeof(icmp_header_t), payload, payload_len);

	hdr->checksum = ipv4_checksum(buf, total);

	return ipv4_send(iface, dst_ip, IP_PROTO_ICMP, buf, total);
}
