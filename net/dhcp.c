#include "dhcp.h"
#include "udp.h"
#include "netif.h"
#include "ipv4.h"
#include "dns.h"
#include "endian.h"
#include "ethernet.h"
#include "string.h"
#include "klog.h"

static dhcp_result_t last_result;
static uint32_t dhcp_xid;
static int dhcp_state; /* 0=idle, 1=discover sent, 2=request sent, 3=done */

/* ── Option helpers ─────────────────────────────────────────────── */

static int dhcp_add_option(uint8_t* opts, int pos, uint8_t code, uint8_t len, const void* data) {
	opts[pos++] = code;
	opts[pos++] = len;
	memcpy(&opts[pos], data, len);
	return pos + len;
}

/* ── Send DHCP discover ─────────────────────────────────────────── */

int dhcp_discover(netif_t* iface) {
	if (!iface) return -1;

	dhcp_xid = 0x12345678; /* simple XID */
	dhcp_state = 1;
	memset(&last_result, 0, sizeof(last_result));

	dhcp_packet_t pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.op = 1; /* BOOTREQUEST */
	pkt.htype = 1;
	pkt.hlen = 6;
	pkt.xid = htonl(dhcp_xid);
	pkt.flags = htons(0x8000); /* broadcast */
	memcpy(pkt.chaddr, iface->mac, 6);
	pkt.magic = htonl(DHCP_MAGIC_COOKIE);

	int opt_pos = 0;
	uint8_t msg_type = DHCP_DISCOVER;
	opt_pos = dhcp_add_option(pkt.options, opt_pos, 53, 1, &msg_type);
	/* Request: subnet, router, DNS */
	uint8_t req_list[] = {1, 3, 6};
	opt_pos = dhcp_add_option(pkt.options, opt_pos, 55, sizeof(req_list), req_list);
	pkt.options[opt_pos++] = 255; /* end */

	/* Send via broadcast IP */
	uint32_t saved_ip = iface->ip_addr;
	iface->ip_addr = 0;

	uint16_t pkt_len = (uint16_t)(sizeof(dhcp_packet_t) - sizeof(pkt.options) + (uint16_t)opt_pos);

	/* Build UDP manually since we need 0.0.0.0 → 255.255.255.255 */
	udp_send(iface, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
			 (const uint8_t*)&pkt, pkt_len);

	iface->ip_addr = saved_ip;
	return 0;
}

/* ── Handle DHCP reply (offer or ack) ───────────────────────────── */

void dhcp_handle_reply(netif_t* iface, const uint8_t* data, uint16_t len) {
	if (len < sizeof(dhcp_packet_t) - 312) return;

	const dhcp_packet_t* pkt = (const dhcp_packet_t*)data;
	if (pkt->op != 2) return; /* BOOTREPLY */
	if (ntohl(pkt->xid) != dhcp_xid) return;

	uint32_t offered_ip = pkt->yiaddr;
	uint32_t server_ip  = pkt->siaddr;

	/* Parse options */
	uint8_t msg_type = 0;
	uint32_t subnet_mask = 0;
	uint32_t router = 0;
	uint32_t dns = 0;
	uint32_t lease = 0;

	int max_opts = (int)(len - (sizeof(dhcp_packet_t) - 312));
	if (max_opts > 312) max_opts = 312;

	const uint8_t* opts = pkt->options;
	int i = 0;
	while (i < max_opts && opts[i] != 255) {
		if (opts[i] == 0) { i++; continue; }
		uint8_t code = opts[i];
		uint8_t olen = opts[i + 1];
		const uint8_t* oval = &opts[i + 2];

		switch (code) {
		case 53: msg_type = oval[0]; break;
		case 1: if (olen >= 4) memcpy(&subnet_mask, oval, 4); break;
		case 3: if (olen >= 4) memcpy(&router, oval, 4); break;
		case 6: if (olen >= 4) memcpy(&dns, oval, 4); break;
		case 51: if (olen >= 4) memcpy(&lease, oval, 4); break;
		default: break;
		}
		i += 2 + olen;
	}

	if (msg_type == DHCP_OFFER && dhcp_state == 1) {
		/* Send DHCP REQUEST */
		dhcp_state = 2;
		dhcp_packet_t req;
		memset(&req, 0, sizeof(req));
		req.op = 1;
		req.htype = 1;
		req.hlen = 6;
		req.xid = htonl(dhcp_xid);
		req.flags = htons(0x8000);
		memcpy(req.chaddr, iface->mac, 6);
		req.magic = htonl(DHCP_MAGIC_COOKIE);

		int opt_pos = 0;
		uint8_t mt = DHCP_REQUEST;
		opt_pos = dhcp_add_option(req.options, opt_pos, 53, 1, &mt);
		opt_pos = dhcp_add_option(req.options, opt_pos, 50, 4, &offered_ip);
		opt_pos = dhcp_add_option(req.options, opt_pos, 54, 4, &server_ip);
		req.options[opt_pos++] = 255;

		uint16_t req_len = (uint16_t)(sizeof(dhcp_packet_t) - sizeof(req.options) + (uint16_t)opt_pos);

		udp_send(iface, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
				 (const uint8_t*)&req, req_len);

	} else if (msg_type == DHCP_ACK && dhcp_state == 2) {
		/* Apply configuration */
		last_result.ip_addr = offered_ip;
		last_result.netmask = subnet_mask;
		last_result.gateway = router;
		last_result.dns_server = dns;
		last_result.lease_time = ntohl(lease);
		last_result.ok = 1;
		dhcp_state = 3;

		netif_set_addr(iface, offered_ip, subnet_mask, router);
		iface->dns_server = dns;

		/* Add default route */
		if (router)
			route_add(0, 0, router, iface);
		/* Local subnet route */
		route_add(offered_ip & subnet_mask, subnet_mask, 0, iface);

		dns_set_server(dns);

		klog_write(KLOG_LEVEL_INFO, "dhcp", "Configuracion IP obtenida");
	}
}

const dhcp_result_t* dhcp_get_result(void) {
	return &last_result;
}
