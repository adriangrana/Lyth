#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdint.h>

/* Address families */
#define AF_INET 2

/* Socket types */
#define SOCK_STREAM 1  /* TCP */
#define SOCK_DGRAM  2  /* UDP */
#define SOCK_RAW    3  /* ICMP/raw */

/* socket address (IPv4) */
typedef struct {
	uint16_t sin_family;
	uint16_t sin_port;     /* network byte order */
	uint32_t sin_addr;     /* network byte order */
} sockaddr_in_t;

#define NET_MAX_SOCKETS 32

/* Kernel socket handle */
typedef struct {
	int      used;
	int      domain;    /* AF_INET */
	int      type;      /* SOCK_STREAM, SOCK_DGRAM */
	int      proto_fd;  /* underlying tcp/udp socket index */
} ksocket_t;

void net_init(void);

/* BSD-like syscall layer */
int  net_socket(int domain, int type, int protocol);
int  net_bind(int sockfd, uint32_t addr, uint16_t port);
int  net_connect(int sockfd, uint32_t addr, uint16_t port);
int  net_listen(int sockfd, int backlog);
int  net_accept(int sockfd);
int  net_send(int sockfd, const void* buf, uint32_t len);
int  net_recv(int sockfd, void* buf, uint32_t len);
int  net_sendto(int sockfd, const void* buf, uint32_t len,
				uint32_t dst_addr, uint16_t dst_port);
int  net_recvfrom(int sockfd, void* buf, uint32_t len,
				  uint32_t* from_addr, uint16_t* from_port);
int  net_close(int sockfd);

#endif
