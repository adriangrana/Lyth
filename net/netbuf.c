#include "netbuf.h"
#include "heap.h"
#include "string.h"

netbuf_t* netbuf_alloc(void) {
	netbuf_t* nb = (netbuf_t*)kmalloc(sizeof(netbuf_t));
	if (!nb) return 0;
	nb->len  = 0;
	nb->next = 0;
	return nb;
}

void netbuf_free(netbuf_t* nb) {
	if (nb) kfree(nb);
}
