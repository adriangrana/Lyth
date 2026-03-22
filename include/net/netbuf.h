#ifndef NET_NETBUF_H
#define NET_NETBUF_H

#include <stdint.h>

#define NETBUF_MAX_SIZE  2048

typedef struct netbuf {
	uint8_t  data[NETBUF_MAX_SIZE];
	uint16_t len;      /* payload length from data[0] */
	struct netbuf* next;
} netbuf_t;

netbuf_t* netbuf_alloc(void);
void      netbuf_free(netbuf_t* nb);

#endif
