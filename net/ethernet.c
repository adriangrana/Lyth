#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "endian.h"
#include "string.h"

const uint8_t ETH_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void eth_rx(netif_t* iface, const uint8_t* frame, uint16_t len) {
	if (len < ETH_HDR_LEN) return;

	const eth_header_t* hdr = (const eth_header_t*)frame;
	uint16_t ethertype = ntohs(hdr->ethertype);
	const uint8_t* payload = frame + ETH_HDR_LEN;
	uint16_t payload_len = len - ETH_HDR_LEN;

	switch (ethertype) {
	case ETHERTYPE_ARP:
		arp_rx(iface, payload, payload_len);
		break;
	case ETHERTYPE_IPV4:
		ipv4_rx(iface, payload, payload_len);
		break;
	default:
		break;
	}
}

int eth_send(netif_t* iface, const uint8_t dst_mac[6],
			 uint16_t ethertype, const uint8_t* payload, uint16_t payload_len)
{
	if (!iface || !iface->send) return -1;
	if (payload_len > ETH_MTU) return -1;

	uint8_t frame[ETH_FRAME_MAX];
	eth_header_t* hdr = (eth_header_t*)frame;

	memcpy(hdr->dst, dst_mac, 6);
	memcpy(hdr->src, iface->mac, 6);
	hdr->ethertype = htons(ethertype);

	memcpy(frame + ETH_HDR_LEN, payload, payload_len);

	return iface->send(iface, frame, (uint16_t)(ETH_HDR_LEN + payload_len));
}
