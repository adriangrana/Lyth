#include "arp.h"
#include "ethernet.h"
#include "endian.h"
#include "string.h"
#include "timer.h"
#include "klog.h"

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

void arp_init(void) {
	memset(arp_cache, 0, sizeof(arp_cache));
}

static void arp_cache_add(uint32_t ip, const uint8_t mac[6]) {
	/* Update existing entry? */
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (arp_cache[i].valid && arp_cache[i].ip == ip) {
			memcpy(arp_cache[i].mac, mac, 6);
			arp_cache[i].timestamp = timer_get_ticks();
			return;
		}
	}
	/* Find free slot */
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!arp_cache[i].valid) {
			arp_cache[i].ip = ip;
			memcpy(arp_cache[i].mac, mac, 6);
			arp_cache[i].timestamp = timer_get_ticks();
			arp_cache[i].valid = 1;
			return;
		}
	}
	/* Evict oldest */
	int oldest = 0;
	uint32_t oldest_tick = arp_cache[0].timestamp;
	for (int i = 1; i < ARP_CACHE_SIZE; i++) {
		if (arp_cache[i].timestamp < oldest_tick) {
			oldest = i;
			oldest_tick = arp_cache[i].timestamp;
		}
	}
	arp_cache[oldest].ip = ip;
	memcpy(arp_cache[oldest].mac, mac, 6);
	arp_cache[oldest].timestamp = timer_get_ticks();
	arp_cache[oldest].valid = 1;
}

void arp_rx(netif_t* iface, const uint8_t* data, uint16_t len) {
	if (len < sizeof(arp_packet_t)) return;

	const arp_packet_t* pkt = (const arp_packet_t*)data;
	uint16_t op = ntohs(pkt->opcode);

	/* Learn sender's MAC/IP */
	arp_cache_add(pkt->sender_ip, pkt->sender_mac);

	if (op == ARP_OP_REQUEST) {
		/* Is the target us? */
		if (pkt->target_ip == iface->ip_addr && iface->ip_addr != 0) {
			/* Build ARP reply */
			arp_packet_t reply;
			reply.hw_type    = htons(ARP_HW_ETHERNET);
			reply.proto_type = htons(ETHERTYPE_IPV4);
			reply.hw_len     = 6;
			reply.proto_len  = 4;
			reply.opcode     = htons(ARP_OP_REPLY);
			memcpy(reply.sender_mac, iface->mac, 6);
			reply.sender_ip  = iface->ip_addr;
			memcpy(reply.target_mac, pkt->sender_mac, 6);
			reply.target_ip  = pkt->sender_ip;

			eth_send(iface, pkt->sender_mac, ETHERTYPE_ARP,
					 (const uint8_t*)&reply, sizeof(reply));
		}
	}
	/* ARP_OP_REPLY: already cached above */
}

int arp_resolve(netif_t* iface, uint32_t ip, uint8_t mac_out[6]) {
	/* Broadcast? */
	if (ip == 0xFFFFFFFF || (ip & ~iface->netmask) == ~iface->netmask) {
		memcpy(mac_out, ETH_BROADCAST, 6);
		return 0;
	}

	/* Cache lookup */
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (arp_cache[i].valid && arp_cache[i].ip == ip) {
			memcpy(mac_out, arp_cache[i].mac, 6);
			return 0;
		}
	}

	/* Not found — send request and fail (caller should retry) */
	arp_request(iface, ip);
	return -1;
}

void arp_request(netif_t* iface, uint32_t target_ip) {
	arp_packet_t req;
	memset(&req, 0, sizeof(req));
	req.hw_type    = htons(ARP_HW_ETHERNET);
	req.proto_type = htons(ETHERTYPE_IPV4);
	req.hw_len     = 6;
	req.proto_len  = 4;
	req.opcode     = htons(ARP_OP_REQUEST);
	memcpy(req.sender_mac, iface->mac, 6);
	req.sender_ip  = iface->ip_addr;
	memset(req.target_mac, 0, 6);
	req.target_ip  = target_ip;

	eth_send(iface, ETH_BROADCAST, ETHERTYPE_ARP,
			 (const uint8_t*)&req, sizeof(req));
}

const arp_entry_t* arp_cache_get(int index) {
	if (index < 0 || index >= ARP_CACHE_SIZE) return 0;
	return &arp_cache[index];
}
