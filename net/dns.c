#include "dns.h"
#include "udp.h"
#include "netif.h"
#include "e1000.h"
#include "endian.h"
#include "string.h"
#include "timer.h"

static dns_cache_entry_t cache[DNS_CACHE_SIZE];
static uint32_t dns_server_ip;
static uint16_t dns_next_id;

/* Pending query state (single outstanding) */
static uint16_t pending_id;
static volatile uint32_t pending_result;
static volatile int      pending_done;

void dns_init(void) {
	memset(cache, 0, sizeof(cache));
	dns_server_ip = 0;
	dns_next_id = 1;
	pending_done = 0;
	pending_result = 0;
}

void dns_set_server(uint32_t server_ip) {
	dns_server_ip = server_ip;
}

/* ── Encode DNS name ────────────────────────────────────────────── */

static int dns_encode_name(const char* name, uint8_t* buf, int max) {
	int pos = 0;
	while (*name) {
		const char* dot = name;
		while (*dot && *dot != '.') dot++;
		int label_len = (int)(dot - name);
		if (label_len <= 0 || label_len > 63) return -1;
		if (pos + 1 + label_len >= max) return -1;
		buf[pos++] = (uint8_t)label_len;
		memcpy(&buf[pos], name, (uint32_t)label_len);
		pos += label_len;
		name = *dot ? dot + 1 : dot;
	}
	if (pos + 1 >= max) return -1;
	buf[pos++] = 0; /* root label */
	return pos;
}

/* ── Send DNS query ─────────────────────────────────────────────── */

uint32_t dns_resolve(const char* hostname) {
	if (!dns_server_ip) return 0;

	/* Check cache */
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		if (cache[i].valid && strcmp(cache[i].name, hostname) == 0)
			return cache[i].ip;
	}

	/* Build query packet */
	uint8_t buf[512];
	memset(buf, 0, sizeof(buf));
	dns_header_t* hdr = (dns_header_t*)buf;
	pending_id = dns_next_id++;
	hdr->id = htons(pending_id);
	hdr->flags = htons(0x0100); /* RD=1 (recursion desired) */
	hdr->qdcount = htons(1);

	int pos = sizeof(dns_header_t);
	int name_len = dns_encode_name(hostname, &buf[pos], (int)(512 - (uint32_t)pos));
	if (name_len < 0) return 0;
	pos += name_len;

	/* QTYPE = A (1), QCLASS = IN (1) */
	buf[pos++] = 0; buf[pos++] = DNS_TYPE_A;
	buf[pos++] = 0; buf[pos++] = DNS_CLASS_IN;

	pending_done = 0;
	pending_result = 0;

	netif_t* iface = netif_get_default();
	udp_send(iface, dns_server_ip, 53, DNS_PORT,
			 buf, (uint16_t)pos);

	/* Wait for response */
	uint32_t start = timer_get_ticks();
	while (!pending_done && (timer_get_ticks() - start) < 300)
		e1000_poll_rx();

	if (pending_done && pending_result) {
		/* Cache result */
		for (int i = 0; i < DNS_CACHE_SIZE; i++) {
			if (!cache[i].valid) {
				int k = 0;
				while (hostname[k] && k < DNS_MAX_NAME - 1) {
					cache[i].name[k] = hostname[k]; k++;
				}
				cache[i].name[k] = '\0';
				cache[i].ip = pending_result;
				cache[i].timestamp = timer_get_ticks();
				cache[i].valid = 1;
				break;
			}
		}
		return pending_result;
	}

	return 0;
}

/* ── Handle DNS response ────────────────────────────────────────── */

static int dns_skip_name(const uint8_t* data, int pos, int max) {
	while (pos < max) {
		if (data[pos] == 0) return pos + 1;
		if ((data[pos] & 0xC0) == 0xC0) return pos + 2; /* pointer */
		pos += 1 + data[pos];
	}
	return max;
}

void dns_rx(const uint8_t* data, uint16_t len) {
	if (len < sizeof(dns_header_t)) return;

	const dns_header_t* hdr = (const dns_header_t*)data;
	if (ntohs(hdr->id) != pending_id) return;
	if (!(ntohs(hdr->flags) & DNS_QR)) return; /* not a response */

	uint16_t ancount = ntohs(hdr->ancount);
	uint16_t qdcount = ntohs(hdr->qdcount);

	/* Skip question section */
	int pos = sizeof(dns_header_t);
	for (uint16_t q = 0; q < qdcount; q++) {
		pos = dns_skip_name(data, pos, len);
		pos += 4; /* QTYPE + QCLASS */
	}

	/* Parse answers */
	for (uint16_t a = 0; a < ancount && pos < len; a++) {
		pos = dns_skip_name(data, pos, len);
		if (pos + 10 > len) break;

		uint16_t rtype = (uint16_t)((data[pos] << 8) | data[pos + 1]);
		/* uint16_t rclass = (data[pos+2]<<8)|data[pos+3]; */
		/* uint32_t ttl = ...;  */
		uint16_t rdlen = (uint16_t)((data[pos + 8] << 8) | data[pos + 9]);
		pos += 10;

		if (rtype == DNS_TYPE_A && rdlen == 4 && pos + 4 <= len) {
			uint32_t ip;
			memcpy(&ip, &data[pos], 4);
			pending_result = ip;
			pending_done = 1;
			return;
		}

		pos += rdlen;
	}
}
