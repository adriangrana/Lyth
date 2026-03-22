#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>

#define DNS_PORT          53
#define DNS_MAX_NAME     128
#define DNS_CACHE_SIZE    16

/* DNS header */
typedef struct __attribute__((packed)) {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
} dns_header_t;

#define DNS_QR       (1U << 15)
#define DNS_TYPE_A   1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN 1

typedef struct {
	char     name[DNS_MAX_NAME];
	uint32_t ip;
	uint32_t timestamp;
	int      valid;
} dns_cache_entry_t;

void     dns_init(void);
uint32_t dns_resolve(const char* hostname);
void     dns_rx(const uint8_t* data, uint16_t len);
void     dns_set_server(uint32_t server_ip);

#endif
