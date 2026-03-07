#ifndef FUROS_NET_H
#define FUROS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

typedef struct net_socket net_socket_t;

void net_init(void);
void net_tick(uint64_t now_ticks);
void net_pump(void);
bool net_ready(void);
int net_dev_write(const void *buf, size_t len);
int net_dev_read(void *buf, size_t len);

int net_socket_create(int domain, int type, int protocol, net_socket_t **out_sock);
void net_socket_ref(net_socket_t *sock);
void net_socket_close(net_socket_t *sock);
int net_socket_bind(net_socket_t *sock, uint32_t addr_be, uint16_t port_be);
long net_socket_sendto(net_socket_t *sock, const void *buf, size_t len,
                       uint32_t addr_be, uint16_t port_be);
long net_socket_recvfrom(net_socket_t *sock, void *buf, size_t len,
                         uint32_t *src_addr_be, uint16_t *src_port_be,
                         bool nonblock);
int net_socket_connect(net_socket_t *sock, uint32_t addr_be, uint16_t port_be, bool nonblock);
int net_socket_listen(net_socket_t *sock, int backlog);
int net_socket_accept(net_socket_t *sock, net_socket_t **out_sock, uint32_t *addr_be,
                      uint16_t *port_be, bool nonblock);
int net_socket_getsockname(net_socket_t *sock, uint32_t *addr_be, uint16_t *port_be);
int net_socket_getpeername(net_socket_t *sock, uint32_t *addr_be, uint16_t *port_be);
int net_socket_shutdown(net_socket_t *sock, int how);
long net_socket_read(net_socket_t *sock, void *buf, size_t len, bool nonblock);
long net_socket_write(net_socket_t *sock, const void *buf, size_t len, bool nonblock);
int net_socket_setsockopt(net_socket_t *sock, int level, int optname,
                          const void *optval, size_t optlen);
int net_socket_getsockopt(net_socket_t *sock, int level, int optname,
                          void *optval, size_t *optlen);
int16_t net_socket_poll_revents(net_socket_t *sock, int16_t events);

#endif
