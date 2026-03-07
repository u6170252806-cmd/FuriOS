#include "user.h"

#define DHCP_CLIENT_PORT 68U
#define DHCP_SERVER_PORT 67U
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_FLAG_BROADCAST 0x8000U
#define DHCP_HTYPE_ETH 1U
#define DHCP_HLEN_ETH 6U
#define DHCP_OP_BOOTREQUEST 1U
#define DHCP_OP_BOOTREPLY 2U
#define DHCP_DISCOVER 1U
#define DHCP_OFFER 2U
#define DHCP_REQUEST 3U
#define DHCP_ACK 5U
#define DHCP_OPTION_PAD 0U
#define DHCP_OPTION_SUBNET_MASK 1U
#define DHCP_OPTION_ROUTER 3U
#define DHCP_OPTION_DNS 6U
#define DHCP_OPTION_REQUESTED_IP 50U
#define DHCP_OPTION_LEASE_TIME 51U
#define DHCP_OPTION_MSGTYPE 53U
#define DHCP_OPTION_SERVER_ID 54U
#define DHCP_OPTION_PARAM_REQ 55U
#define DHCP_OPTION_END 255U
#define DHCP_EAGAIN 11
#define DHCP_TIMEOUT_POLLS 30U
#define DHCP_POLL_TICKS 10U
#define DHCP_MAX_ATTEMPTS 3U
#define DHCP_FIXED_LEN 240U

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} sockaddr_in;

typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;
    uint8_t options[312];
} __attribute__((packed)) dhcp_packet_t;

typedef struct {
    uint32_t yiaddr_be;
    uint32_t server_id_be;
    uint32_t mask_be;
    uint32_t router_be;
    uint32_t dns_be;
    uint8_t msg_type;
} dhcp_offer_t;

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

static uint32_t ntohl32(uint32_t v) {
    return bswap32(v);
}

static int net0_request(const char *cmd, char *resp, size_t resp_cap) {
    int fd = open("/dev/net0", O_RDWR);
    long n;
    size_t off = 0U;

    if (fd < 0) {
        return -1;
    }
    if (write(fd, cmd, (unsigned long)strlen(cmd)) < 0) {
        close(fd);
        return -1;
    }
    if (!resp || resp_cap == 0U) {
        close(fd);
        return 0;
    }
    while ((n = read(fd, resp + off, resp_cap - off - 1U)) > 0) {
        off += (size_t)n;
        if (off + 1U >= resp_cap) {
            break;
        }
    }
    resp[off] = '\0';
    close(fd);
    return 0;
}

static int parse_hex_byte(const char *s, uint8_t *out) {
    uint8_t v = 0U;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        uint8_t d;
        if (c >= '0' && c <= '9') {
            d = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint8_t)(10 + c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            d = (uint8_t)(10 + c - 'A');
        } else {
            return -1;
        }
        v = (uint8_t)((v << 4) | d);
    }
    *out = v;
    return 0;
}

static char *find_substr(char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (!haystack || !needle || needle_len == 0U) {
        return haystack;
    }
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }
    return 0;
}

static int get_iface_mac(const char *ifname, uint8_t mac[6]) {
    char resp[512];
    char *line = resp;
    size_t name_len = strlen(ifname);

    if (net0_request("ifconfig\n", resp, sizeof(resp)) != 0) {
        return -1;
    }
    while (line && *line) {
        char *next = strchr(line, '\n');
        char *mac_pos;
        if (next) {
            *next = '\0';
        }
        if (strncmp(line, ifname, name_len) == 0 &&
            (line[name_len] == ' ' || line[name_len] == '\t')) {
            mac_pos = find_substr(line, "mac=");
            if (!mac_pos) {
                return -1;
            }
            mac_pos += 4;
            for (int i = 0; i < 6; i++) {
                if (parse_hex_byte(mac_pos, &mac[i]) != 0) {
                    return -1;
                }
                mac_pos += 2;
                if (i != 5) {
                    if (*mac_pos != ':') {
                        return -1;
                    }
                    mac_pos++;
                }
            }
            return 0;
        }
        if (!next) {
            break;
        }
        line = next + 1;
    }
    return -1;
}

static void fill_addr(sockaddr_in *addr, uint32_t ip_be, uint16_t port_host) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons16(port_host);
    addr->sin_addr = htonl32(ip_be);
}

static size_t dhcp_option_u32(uint8_t *opts, size_t off, uint8_t code, uint32_t value_be) {
    opts[off++] = code;
    opts[off++] = 4U;
    opts[off++] = (uint8_t)((value_be >> 24) & 0xFFU);
    opts[off++] = (uint8_t)((value_be >> 16) & 0xFFU);
    opts[off++] = (uint8_t)((value_be >> 8) & 0xFFU);
    opts[off++] = (uint8_t)(value_be & 0xFFU);
    return off;
}

static int dhcp_send(int fd, uint32_t xid, const uint8_t mac[6], uint8_t msg_type,
                     uint32_t request_ip_be, uint32_t server_id_be) {
    dhcp_packet_t pkt;
    sockaddr_in dst;
    size_t off = 0U;

    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_OP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = DHCP_HLEN_ETH;
    pkt.xid = htonl32(xid);
    pkt.flags = htons16(DHCP_FLAG_BROADCAST);
    memcpy(pkt.chaddr, mac, 6U);
    pkt.cookie = htonl32(DHCP_MAGIC_COOKIE);

    off = 0U;
    pkt.options[off++] = DHCP_OPTION_MSGTYPE;
    pkt.options[off++] = 1U;
    pkt.options[off++] = msg_type;
    if (msg_type == DHCP_REQUEST) {
        off = dhcp_option_u32(pkt.options, off, DHCP_OPTION_REQUESTED_IP, request_ip_be);
        off = dhcp_option_u32(pkt.options, off, DHCP_OPTION_SERVER_ID, server_id_be);
    }
    pkt.options[off++] = DHCP_OPTION_PARAM_REQ;
    pkt.options[off++] = 4U;
    pkt.options[off++] = DHCP_OPTION_SUBNET_MASK;
    pkt.options[off++] = DHCP_OPTION_ROUTER;
    pkt.options[off++] = DHCP_OPTION_DNS;
    pkt.options[off++] = DHCP_OPTION_LEASE_TIME;
    pkt.options[off++] = DHCP_OPTION_END;

    fill_addr(&dst, 0xFFFFFFFFU, DHCP_SERVER_PORT);
    return sendto(fd, &pkt, (unsigned long)(DHCP_FIXED_LEN + off), 0,
                  (const fu_sockaddr_t *)&dst, sizeof(dst)) < 0 ? -1 : 0;
}

static int dhcp_parse_offer(const uint8_t *buf, size_t len, uint32_t xid, const uint8_t mac[6], dhcp_offer_t *offer) {
    const dhcp_packet_t *pkt = (const dhcp_packet_t *)buf;
    size_t off = 0U;
    size_t opts_len;

    if (!buf || len < DHCP_FIXED_LEN || !offer) {
        return -1;
    }
    if (pkt->op != DHCP_OP_BOOTREPLY ||
        pkt->htype != DHCP_HTYPE_ETH ||
        pkt->hlen != DHCP_HLEN_ETH ||
        ntohl32(pkt->xid) != xid ||
        memcmp(pkt->chaddr, mac, 6U) != 0 ||
        ntohl32(pkt->cookie) != DHCP_MAGIC_COOKIE) {
        return -1;
    }

    memset(offer, 0, sizeof(*offer));
    offer->yiaddr_be = ntohl32(pkt->yiaddr);
    opts_len = len - DHCP_FIXED_LEN;

    while (off < opts_len) {
        uint8_t code = pkt->options[off++];
        uint8_t opt_len;
        if (code == DHCP_OPTION_PAD) {
            continue;
        }
        if (code == DHCP_OPTION_END) {
            break;
        }
        if (off >= opts_len) {
            return -1;
        }
        opt_len = pkt->options[off++];
        if (off + opt_len > opts_len) {
            return -1;
        }
        switch (code) {
            case DHCP_OPTION_MSGTYPE:
                if (opt_len >= 1U) {
                    offer->msg_type = pkt->options[off];
                }
                break;
            case DHCP_OPTION_SUBNET_MASK:
                if (opt_len == 4U) {
                    offer->mask_be = ((uint32_t)pkt->options[off] << 24) |
                                     ((uint32_t)pkt->options[off + 1U] << 16) |
                                     ((uint32_t)pkt->options[off + 2U] << 8) |
                                     (uint32_t)pkt->options[off + 3U];
                }
                break;
            case DHCP_OPTION_ROUTER:
                if (opt_len >= 4U) {
                    offer->router_be = ((uint32_t)pkt->options[off] << 24) |
                                       ((uint32_t)pkt->options[off + 1U] << 16) |
                                       ((uint32_t)pkt->options[off + 2U] << 8) |
                                       (uint32_t)pkt->options[off + 3U];
                }
                break;
            case DHCP_OPTION_DNS:
                if (opt_len >= 4U) {
                    offer->dns_be = ((uint32_t)pkt->options[off] << 24) |
                                    ((uint32_t)pkt->options[off + 1U] << 16) |
                                    ((uint32_t)pkt->options[off + 2U] << 8) |
                                    (uint32_t)pkt->options[off + 3U];
                }
                break;
            case DHCP_OPTION_SERVER_ID:
                if (opt_len == 4U) {
                    offer->server_id_be = ((uint32_t)pkt->options[off] << 24) |
                                          ((uint32_t)pkt->options[off + 1U] << 16) |
                                          ((uint32_t)pkt->options[off + 2U] << 8) |
                                          (uint32_t)pkt->options[off + 3U];
                }
                break;
            default:
                break;
        }
        off += opt_len;
    }
    return offer->msg_type != 0U ? 0 : -1;
}

static int dhcp_wait(int fd, uint32_t xid, const uint8_t mac[6], uint8_t expect_type, dhcp_offer_t *offer) {
    fu_pollfd_t pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (uint32_t i = 0; i < DHCP_TIMEOUT_POLLS; i++) {
        int rc = poll(&pfd, 1, DHCP_POLL_TICKS);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        if (pfd.revents & POLLIN) {
            uint8_t buf[640];
            sockaddr_in src;
            unsigned long slen = sizeof(src);
            long n = recvfrom(fd, buf, sizeof(buf), 0, (fu_sockaddr_t *)&src, &slen);
            if (n < 0) {
                if (errno == DHCP_EAGAIN) {
                    continue;
                }
                return -1;
            }
            if (dhcp_parse_offer(buf, (size_t)n, xid, mac, offer) == 0 && offer->msg_type == expect_type) {
                return 0;
            }
        }
    }
    return -1;
}

static int write_resolv_conf(uint32_t dns_be) {
    char line[64];
    char ip[16];
    int fd;

    fu_format_ipv4(dns_be, ip, sizeof(ip));
    (void)snprintf(line, sizeof(line), "nameserver %s\n", ip);
    fd = open("/etc/resolv.conf", O_WRONLY | O_TRUNC | O_CREAT);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, line, (unsigned long)strlen(line)) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    uint8_t mac[6];
    char cmd[96];
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];
    int fd;
    int one = 1;
    sockaddr_in bind_addr;
    uint32_t xid;
    dhcp_offer_t offer;

    if (argc != 2) {
        puts("usage: dhcp <if>\n");
        return 1;
    }
    if (get_iface_mac(argv[1], mac) != 0) {
        puts("dhcp: interface not found\n");
        return 1;
    }

    (void)snprintf(cmd, sizeof(cmd), "ifconfig %s up\n", argv[1]);
    (void)net0_request(cmd, 0, 0);

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (fd < 0) {
        puts("dhcp: socket failed\n");
        return 1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) != 0) {
        puts("dhcp: broadcast setup failed\n");
        close(fd);
        return 1;
    }
    fill_addr(&bind_addr, 0U, DHCP_CLIENT_PORT);
    if (bind(fd, (const fu_sockaddr_t *)&bind_addr, sizeof(bind_addr)) != 0) {
        puts("dhcp: bind failed\n");
        close(fd);
        return 1;
    }

    xid = 0x44500000U ^ ((uint32_t)getpid() << 8) ^ (uint32_t)mac[5];
    memset(&offer, 0, sizeof(offer));

    for (uint32_t attempt = 0; attempt < DHCP_MAX_ATTEMPTS; attempt++) {
        if (dhcp_send(fd, xid, mac, DHCP_DISCOVER, 0U, 0U) == 0 &&
            dhcp_wait(fd, xid, mac, DHCP_OFFER, &offer) == 0) {
            break;
        }
        memset(&offer, 0, sizeof(offer));
    }
    if (offer.msg_type != DHCP_OFFER || offer.yiaddr_be == 0U || offer.server_id_be == 0U) {
        puts("dhcp: no offer\n");
        close(fd);
        return 1;
    }

    {
        dhcp_offer_t ack;
        memset(&ack, 0, sizeof(ack));
        for (uint32_t attempt = 0; attempt < DHCP_MAX_ATTEMPTS; attempt++) {
            if (dhcp_send(fd, xid, mac, DHCP_REQUEST, offer.yiaddr_be, offer.server_id_be) == 0 &&
                dhcp_wait(fd, xid, mac, DHCP_ACK, &ack) == 0) {
                offer = ack;
                break;
            }
            memset(&ack, 0, sizeof(ack));
        }
    }
    close(fd);

    if (offer.msg_type != DHCP_ACK) {
        puts("dhcp: no ack\n");
        return 1;
    }
    if (offer.mask_be == 0U) {
        offer.mask_be = 0xFFFFFF00U;
    }

    fu_format_ipv4(offer.yiaddr_be, ip, sizeof(ip));
    fu_format_ipv4(offer.mask_be, mask, sizeof(mask));
    fu_format_ipv4(offer.router_be, gw, sizeof(gw));
    fu_format_ipv4(offer.dns_be, dns, sizeof(dns));

    if (offer.router_be != 0U) {
        (void)snprintf(cmd, sizeof(cmd), "ifconfig %s %s %s %s\n", argv[1], ip, mask, gw);
    } else {
        (void)snprintf(cmd, sizeof(cmd), "ifconfig %s %s %s\n", argv[1], ip, mask);
    }
    if (net0_request(cmd, 0, 0) != 0) {
        puts("dhcp: lease apply failed\n");
        return 1;
    }
    if (offer.dns_be != 0U) {
        (void)write_resolv_conf(offer.dns_be);
    }

    printf("dhcp: lease %s mask %s", ip, mask);
    if (offer.router_be != 0U) {
        printf(" gw %s", gw);
    }
    if (offer.dns_be != 0U) {
        printf(" dns %s", dns);
    }
    putc('\n');
    return 0;
}
