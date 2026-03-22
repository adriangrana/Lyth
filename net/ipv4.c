#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "ethernet.h"
#include "e1000.h"
#include "endian.h"
#include "string.h"
#include "klog.h"

static uint16_t ip_id_counter;
static route_entry_t routes[ROUTE_MAX];
static int route_count;

void ipv4_init(void) {
	ip_id_counter = 1;
	route_count = 0;
	memset(routes, 0, sizeof(routes));
}

uint16_t ipv4_checksum(const void* data, uint16_t len) {
	const uint16_t* ptr = (const uint16_t*)data;
	uint32_t sum = 0;
	while (len > 1) {
		sum += *ptr++;
		len -= 2;
	}
	if (len == 1)
		sum += *(const uint8_t*)ptr;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)(~sum);
}

void ipv4_rx(netif_t* iface, const uint8_t* data, uint16_t len) {
	if (len < IP_HDR_MIN_LEN) return;

	const ipv4_header_t* hdr = (const ipv4_header_t*)data;
	uint8_t version = (hdr->version_ihl >> 4) & 0x0F;
	uint8_t ihl = (hdr->version_ihl & 0x0F) * 4;

	if (version != 4 || ihl < IP_HDR_MIN_LEN) return;

	uint16_t total_len = ntohs(hdr->total_length);
	if (total_len > len) return;

	/* Verify checksum */
	if (ipv4_checksum(data, ihl) != 0) return;

	/* Is this packet for us? */
	uint32_t dst = hdr->dst_ip;
	if (dst != iface->ip_addr && dst != 0xFFFFFFFF &&
		(dst & ~iface->netmask) != ~iface->netmask) {
		/* Not for us and not broadcast */
		return;
	}

	const uint8_t* payload = data + ihl;
	uint16_t payload_len = total_len - ihl;

	switch (hdr->protocol) {
	case IP_PROTO_ICMP:
		icmp_rx(iface, hdr->src_ip, payload, payload_len);
		break;
	case IP_PROTO_UDP:
		udp_rx(iface, hdr->src_ip, hdr->dst_ip, payload, payload_len);
		break;
	case IP_PROTO_TCP:
		tcp_rx(iface, hdr->src_ip, hdr->dst_ip, payload, payload_len);
		break;
	default:
		break;
	}
}

int ipv4_send(netif_t* iface, uint32_t dst_ip, uint8_t protocol,
			  const uint8_t* payload, uint16_t payload_len)
{
	if (!iface) {
		uint32_t next_hop;
		iface = route_lookup(dst_ip, &next_hop);
		if (!iface) iface = netif_get_default();
		if (!iface) return -1;
	}

	uint16_t total_len = IP_HDR_MIN_LEN + payload_len;
	uint8_t packet[ETH_MTU];
	if (total_len > ETH_MTU) return -1;

	ipv4_header_t* hdr = (ipv4_header_t*)packet;
	hdr->version_ihl   = 0x45; /* v4, 5 words */
	hdr->tos           = 0;
	hdr->total_length  = htons(total_len);
	hdr->identification = htons(ip_id_counter++);
	hdr->flags_fragment = 0;
	hdr->ttl           = 64;
	hdr->protocol      = protocol;
	hdr->checksum      = 0;
	hdr->src_ip        = iface->ip_addr;
	hdr->dst_ip        = dst_ip;
	hdr->checksum      = ipv4_checksum(hdr, IP_HDR_MIN_LEN);

	memcpy(packet + IP_HDR_MIN_LEN, payload, payload_len);

	/* Determine next hop */
	uint32_t next_hop = dst_ip;
	if (iface->gateway != 0 &&
		(dst_ip & iface->netmask) != (iface->ip_addr & iface->netmask)) {
		next_hop = iface->gateway;
	}

	/* ARP resolve */
	uint8_t dst_mac[6];
	if (arp_resolve(iface, next_hop, dst_mac) < 0) {
		/* ARP request sent, retry a few times */
		for (int retry = 0; retry < 3; retry++) {
			for (volatile int d = 0; d < 500000; d++) {
				if ((d & 0xFFFF) == 0) e1000_poll_rx();
			}
			if (arp_resolve(iface, next_hop, dst_mac) == 0)
				goto send;
		}
		return -1;
	}

send:
	return eth_send(iface, dst_mac, ETHERTYPE_IPV4, packet, total_len);
}

/* ── Routing ────────────────────────────────────────────────────── */

void route_add(uint32_t network, uint32_t netmask, uint32_t gateway, netif_t* iface) {
	if (route_count >= ROUTE_MAX) return;
	routes[route_count].network = network;
	routes[route_count].netmask = netmask;
	routes[route_count].gateway = gateway;
	routes[route_count].iface   = iface;
	route_count++;
}

netif_t* route_lookup(uint32_t dst_ip, uint32_t* next_hop_out) {
	netif_t* best = 0;
	uint32_t best_mask = 0;
	uint32_t best_gw = 0;

	for (int i = 0; i < route_count; i++) {
		if ((dst_ip & routes[i].netmask) == routes[i].network) {
			if (routes[i].netmask >= best_mask) {
				best = routes[i].iface;
				best_mask = routes[i].netmask;
				best_gw = routes[i].gateway;
			}
		}
	}

	if (next_hop_out)
		*next_hop_out = best_gw ? best_gw : dst_ip;
	return best;
}

void ip4_to_str(uint32_t ip, char buf[16]) {
	uint8_t a = (uint8_t)(ip);
	uint8_t b = (uint8_t)(ip >> 8);
	uint8_t c = (uint8_t)(ip >> 16);
	uint8_t d = (uint8_t)(ip >> 24);
	int pos = 0;
	/* helper: write decimal byte */
	uint8_t parts[4] = {a, b, c, d};
	for (int p = 0; p < 4; p++) {
		uint8_t v = parts[p];
		if (v >= 100) buf[pos++] = (char)('0' + v / 100);
		if (v >= 10) buf[pos++] = (char)('0' + (v / 10) % 10);
		buf[pos++] = (char)('0' + v % 10);
		if (p < 3) buf[pos++] = '.';
	}
	buf[pos] = '\0';
}
