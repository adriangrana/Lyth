#include "socket.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "ipv4.h"
#include "netif.h"
#include "dhcp.h"
#include "dns.h"
#include "string.h"

static ksocket_t sockets[NET_MAX_SOCKETS];

void net_init(void) {
	memset(sockets, 0, sizeof(sockets));
	netif_init();
	arp_init();
	ipv4_init();
	udp_init();
	tcp_init();
	dns_init();
}

int net_socket(int domain, int type, int protocol) {
	(void)protocol;
	if (domain != AF_INET) return -1;

	for (int i = 0; i < NET_MAX_SOCKETS; i++) {
		if (sockets[i].used) continue;

		int proto_fd = -1;
		if (type == SOCK_DGRAM) {
			proto_fd = udp_socket_open(0);
		} else if (type == SOCK_STREAM) {
			proto_fd = tcp_socket_open();
		}
		if (proto_fd < 0) return -1;

		sockets[i].used     = 1;
		sockets[i].domain   = domain;
		sockets[i].type     = type;
		sockets[i].proto_fd = proto_fd;
		return i;
	}
	return -1;
}

int net_bind(int sockfd, uint32_t addr, uint16_t port) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	(void)addr;
	if (sockets[sockfd].type == SOCK_DGRAM) {
		/* Re-open socket on specific port */
		udp_socket_close(sockets[sockfd].proto_fd);
		sockets[sockfd].proto_fd = udp_socket_open(port);
		return sockets[sockfd].proto_fd >= 0 ? 0 : -1;
	}
	return -1;
}

int net_connect(int sockfd, uint32_t addr, uint16_t port) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type == SOCK_STREAM) {
		return tcp_socket_connect(sockets[sockfd].proto_fd, addr, port);
	} else if (sockets[sockfd].type == SOCK_DGRAM) {
		return udp_socket_bind(sockets[sockfd].proto_fd, addr, port);
	}
	return -1;
}

int net_listen(int sockfd, int backlog) {
	(void)backlog;
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type != SOCK_STREAM) return -1;
	return tcp_socket_listen(sockets[sockfd].proto_fd, 0);
}

int net_accept(int sockfd) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type != SOCK_STREAM) return -1;
	return tcp_socket_accept(sockets[sockfd].proto_fd);
}

int net_send(int sockfd, const void* buf, uint32_t len) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type == SOCK_STREAM) {
		uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
		return tcp_socket_send(sockets[sockfd].proto_fd, (const uint8_t*)buf, chunk);
	}
	return -1;
}

int net_recv(int sockfd, void* buf, uint32_t len) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type == SOCK_STREAM) {
		uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
		return tcp_socket_recv(sockets[sockfd].proto_fd, (uint8_t*)buf, chunk);
	}
	return -1;
}

int net_sendto(int sockfd, const void* buf, uint32_t len,
			   uint32_t dst_addr, uint16_t dst_port)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type != SOCK_DGRAM) return -1;
	uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
	return udp_socket_sendto(sockets[sockfd].proto_fd, dst_addr, dst_port,
							 (const uint8_t*)buf, chunk);
}

int net_recvfrom(int sockfd, void* buf, uint32_t len,
				 uint32_t* from_addr, uint16_t* from_port)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type != SOCK_DGRAM) return -1;
	uint16_t chunk = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
	return udp_socket_recvfrom(sockets[sockfd].proto_fd, (uint8_t*)buf, chunk,
							   from_addr, from_port);
}

int net_close(int sockfd) {
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS || !sockets[sockfd].used)
		return -1;
	if (sockets[sockfd].type == SOCK_DGRAM)
		udp_socket_close(sockets[sockfd].proto_fd);
	else if (sockets[sockfd].type == SOCK_STREAM)
		tcp_socket_close(sockets[sockfd].proto_fd);
	sockets[sockfd].used = 0;
	return 0;
}
