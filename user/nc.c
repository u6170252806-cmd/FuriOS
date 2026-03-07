#include "user.h"

#define NC_EAGAIN 11
#define NC_EINPROGRESS 115
#define NC_POLL_TICKS 20
#define NC_UDP_IDLE_LIMIT 5

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} sockaddr_in;

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8) |
           ((v & 0x00FF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static uint16_t htons16(uint16_t v) {
    return bswap16(v);
}

static uint32_t htonl32(uint32_t v) {
    return bswap32(v);
}

static int parse_u16(const char *s, uint16_t *out) {
    uint32_t v = 0U;
    if (!s || !*s || !out) {
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10U + (uint32_t)(*s - '0');
        if (v > 65535U) {
            return -1;
        }
    }
    *out = (uint16_t)v;
    return 0;
}

static void fill_addr(sockaddr_in *addr, uint32_t ip_be, uint16_t port_host) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons16(port_host);
    addr->sin_addr = htonl32(ip_be);
}

static int await_connect_ready(int fd) {
    fu_pollfd_t pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT | POLLIN;
    pfd.revents = 0;
    for (;;) {
        int rc = poll(&pfd, 1, NC_POLL_TICKS);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        if (pfd.revents & (POLLOUT | POLLIN | POLLERR | POLLHUP)) {
            int so_error = 0;
            unsigned long optlen = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
                return -1;
            }
            return so_error == 0 ? 0 : -1;
        }
        if (pfd.revents & POLLNVAL) {
            return -1;
        }
    }
}

static int await_listener_ready(int fd) {
    fu_pollfd_t pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int rc = poll(&pfd, 1, NC_POLL_TICKS);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        if (pfd.revents & POLLIN) {
            return 0;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }
    }
}

static int shuttle_stream(int fd, int udp_mode, int has_peer, sockaddr_in *peer) {
    fu_pollfd_t pfds[2];
    int stdin_open = 1;
    int sock_open = 1;
    char inbuf[256];
    char netbuf[512];
    int udp_idle = 0;

    while (stdin_open || sock_open) {
        int nfds = 0;
        if (stdin_open) {
            pfds[nfds].fd = 0;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (sock_open) {
            pfds[nfds].fd = fd;
            pfds[nfds].events = POLLIN;
            if (stdin_open) {
                pfds[nfds].events |= POLLOUT;
            }
            pfds[nfds].revents = 0;
            nfds++;
        }
        int rc = poll(pfds, nfds, NC_POLL_TICKS);
        if (rc < 0) {
            return 1;
        }
        if (rc == 0) {
            if (udp_mode && !stdin_open && has_peer) {
                udp_idle++;
                if (udp_idle >= NC_UDP_IDLE_LIMIT) {
                    break;
                }
            }
            continue;
        }
        udp_idle = 0;

        int idx = 0;
        if (stdin_open) {
            if (pfds[idx].revents & POLLIN) {
                long n = read(0, inbuf, sizeof(inbuf));
                if (n < 0) {
                    return 1;
                }
                if (n == 0) {
                    stdin_open = 0;
                    if (!udp_mode) {
                        (void)shutdown(fd, SHUT_WR);
                    }
                } else if (udp_mode) {
                    if (!has_peer || !peer) {
                        return 1;
                    }
                    if (sendto(fd, inbuf, (unsigned long)n, 0,
                               (const fu_sockaddr_t *)peer, sizeof(*peer)) < 0) {
                        return 1;
                    }
                } else if (write(fd, inbuf, (unsigned long)n) < 0) {
                    return 1;
                }
            } else if (pfds[idx].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdin_open = 0;
            }
            idx++;
        }

        if (sock_open) {
            if (pfds[idx].revents & POLLIN) {
                if (udp_mode) {
                    sockaddr_in src;
                    unsigned long slen = sizeof(src);
                    long n = recvfrom(fd, netbuf, sizeof(netbuf), 0,
                                      (fu_sockaddr_t *)&src, &slen);
                    if (n < 0) {
                        if (errno == NC_EAGAIN) {
                            continue;
                        }
                        return 1;
                    }
                    if (!has_peer && slen >= sizeof(src)) {
                        *peer = src;
                        has_peer = 1;
                    }
                    if (n > 0 && write(1, netbuf, (unsigned long)n) < 0) {
                        return 1;
                    }
                } else {
                    long n = read(fd, netbuf, sizeof(netbuf));
                    if (n < 0) {
                        if (errno == NC_EAGAIN) {
                            continue;
                        }
                        return 1;
                    }
                    if (n == 0) {
                        sock_open = 0;
                    } else if (write(1, netbuf, (unsigned long)n) < 0) {
                        return 1;
                    }
                }
            }
            if (pfds[idx].revents & (POLLERR | POLLNVAL)) {
                return 1;
            }
            if ((pfds[idx].revents & POLLHUP) && !udp_mode) {
                sock_open = 0;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    int udp_mode = 0;
    int listen_mode = 0;
    int argi = 1;
    int fd = -1;
    uint16_t port = 0;
    uint32_t ip_be = 0U;
    sockaddr_in addr;
    sockaddr_in peer;

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-u") == 0) {
            udp_mode = 1;
        } else if (strcmp(argv[argi], "-l") == 0) {
            listen_mode = 1;
        } else {
            puts("usage: nc [-u] [-l] host port | nc [-u] -l port\n");
            return 1;
        }
        argi++;
    }

    if (listen_mode) {
        if (argc - argi != 1 || parse_u16(argv[argi], &port) != 0) {
            puts("usage: nc [-u] -l port\n");
            return 1;
        }
    } else {
        if (argc - argi != 2 ||
            fu_resolve_name_ipv4(argv[argi], &ip_be) != 0 ||
            parse_u16(argv[argi + 1], &port) != 0) {
            puts("usage: nc [-u] host port\n");
            return 1;
        }
    }

    fd = socket(AF_INET, (udp_mode ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK,
                udp_mode ? IPPROTO_UDP : IPPROTO_TCP);
    if (fd < 0) {
        puts("nc: socket failed\n");
        return 1;
    }

    if (listen_mode) {
        fill_addr(&addr, 0U, port);
        if (bind(fd, (const fu_sockaddr_t *)&addr, sizeof(addr)) != 0) {
            puts("nc: bind failed\n");
            close(fd);
            return 1;
        }
        if (!udp_mode) {
            if (listen(fd, 1) != 0) {
                puts("nc: listen failed\n");
                close(fd);
                return 1;
            }
            if (await_listener_ready(fd) != 0) {
                puts("nc: accept wait failed\n");
                close(fd);
                return 1;
            }
            unsigned long plen = sizeof(peer);
            int cfd = accept(fd, (fu_sockaddr_t *)&peer, &plen);
            if (cfd < 0) {
                puts("nc: accept failed\n");
                close(fd);
                return 1;
            }
            close(fd);
            fd = cfd;
            return shuttle_stream(fd, 0, 1, &peer);
        }
        memset(&peer, 0, sizeof(peer));
        return shuttle_stream(fd, 1, 0, &peer);
    }

    fill_addr(&addr, ip_be, port);
    if (udp_mode) {
        memset(&peer, 0, sizeof(peer));
        peer = addr;
        return shuttle_stream(fd, 1, 1, &peer);
    }

    if (connect(fd, (const fu_sockaddr_t *)&addr, sizeof(addr)) != 0 &&
        errno != NC_EAGAIN && errno != NC_EINPROGRESS) {
        puts("nc: connect failed\n");
        close(fd);
        return 1;
    }
    if (await_connect_ready(fd) != 0) {
        puts("nc: connect wait failed\n");
        close(fd);
        return 1;
    }
    return shuttle_stream(fd, 0, 1, &addr);
}
