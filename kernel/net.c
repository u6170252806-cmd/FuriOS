#include "net.h"
#include "netdev.h"
#include "rtl8139.h"
#include "rtl8125.h"
#include "config.h"
#include "timer.h"
#include "string.h"

#define ETH_ADDR_LEN            6U
#define ETH_TYPE_ARP            0x0806U
#define ETH_TYPE_IPV4           0x0800U

#define ARP_HTYPE_ETH           1U
#define ARP_PTYPE_IPV4          0x0800U
#define ARP_HLEN_ETH            6U
#define ARP_PLEN_IPV4           4U
#define ARP_OP_REQUEST          1U
#define ARP_OP_REPLY            2U

#define IP_PROTO_ICMP           1U
#define IP_PROTO_TCP            6U
#define IP_PROTO_UDP            17U

#define ICMP_ECHO_REPLY         0U
#define ICMP_ECHO_REQUEST       8U

#define TCP_FIN                 0x01U
#define TCP_SYN                 0x02U
#define TCP_RST                 0x04U
#define TCP_PSH                 0x08U
#define TCP_ACK                 0x10U

#define NET_IF_MAX              4U
#define NET_ROUTE_MAX           8U
#define NET_ARP_MAX             8U
#define NET_MTU                 1500U
#define NET_FRAME_MAX           1518U
#define NET_CMD_MAX             128U
#define NET_RESP_MAX            512U
#define NET_RXQ_MAX             64U
#define NET_RX_BUDGET           32U
#define NET_UDP_PAYLOAD_MAX     1472U
#define NET_SOCK_MAX            32U
#define NET_SOCK_RXQ            8U
#define NET_EPHEMERAL_BASE      49152U
#define NET_EPHEMERAL_LAST      65535U
#define NET_LOOPBACK_IP_BE      0x7F000001U
#define NET_LOOPBACK_MASK_BE    0xFF000000U
#define NET_DEFAULT_IP_BE       0x0A00020FU /* 10.0.2.15 */
#define NET_DEFAULT_MASK_BE     0xFFFFFF00U /* 255.255.255.0 */
#define NET_DEFAULT_GW_BE       0x0A000202U /* 10.0.2.2 */
#define NET_ARP_TTL_TICKS       (TIMER_HZ * 60U)
#define NET_TCP_BUF_MAX         2048U
#define NET_TCP_ACCEPTQ         4U
#define NET_TCP_RTO_TICKS       (TIMER_HZ / 2U + 1U)
#define NET_TCP_TIMEWAIT_TICKS  (TIMER_HZ * 2U)
#define NET_TCP_MAX_RETRIES     6U

#define NETERR_EACCES           13
#define NETERR_EINVAL           22
#define NETERR_EPIPE            32
#define NETERR_EOPNOTSUPP       95
#define NETERR_ENETUNREACH      101
#define NETERR_ECONNRESET       104
#define NETERR_ENOBUFS          105
#define NETERR_EISCONN          106
#define NETERR_ENOTCONN         107
#define NETERR_ETIMEDOUT        110
#define NETERR_ECONNREFUSED     111
#define NETERR_EALREADY         114
#define NETERR_EINPROGRESS      115

typedef enum {
    NET_SOCK_PROTO_NONE = 0,
    NET_SOCK_PROTO_UDP,
    NET_SOCK_PROTO_TCP,
} net_sock_proto_t;

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECV,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT1,
    TCP_STATE_FIN_WAIT2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_CLOSING,
    TCP_STATE_TIME_WAIT,
} tcp_state_t;

typedef struct {
    bool up;
    bool loopback;
    int dev_index;
    char name[8];
    uint32_t addr_be;
    uint32_t mask_be;
    uint32_t gw_be;
    uint8_t mac[6];
} net_if_t;

typedef struct {
    bool used;
    uint32_t prefix_be;
    uint32_t mask_be;
    uint32_t gw_be;
    int if_index;
} net_route_t;

typedef struct {
    bool valid;
    uint32_t ip_be;
    int if_index;
    uint8_t mac[6];
    uint64_t updated_tick;
} arp_entry_t;

typedef struct {
    bool active;
    uint16_t id;
    uint16_t seq;
    uint64_t tx_tick;
    uint64_t rx_tick;
    bool replied;
} ping_wait_t;

typedef struct {
    bool used;
    int dev_index;
    size_t len;
    uint8_t frame[NET_FRAME_MAX];
} net_rx_item_t;

typedef struct {
    bool valid;
    uint16_t flags;
    uint32_t seq;
    size_t len;
    uint8_t data[NET_MTU];
    uint64_t sent_tick;
    uint8_t retries;
} tcp_tx_seg_t;

typedef struct net_socket net_socket_impl_t;

struct net_socket {
    bool used;
    uint16_t refs;
    net_sock_proto_t proto;
    bool reuseaddr;
    bool broadcast;
    bool bound;
    int last_error;
    uint32_t bind_ip_be; /* 0 means INADDR_ANY */
    uint16_t bind_port;
    uint32_t peer_ip_be;
    uint16_t peer_port;
    union {
        struct {
            uint8_t q_head;
            uint8_t q_tail;
            uint8_t q_count;
            struct {
                size_t len;
                uint32_t src_ip_be;
                uint16_t src_port;
                uint8_t data[NET_UDP_PAYLOAD_MAX];
            } rxq[NET_SOCK_RXQ];
        } udp;
        struct {
            tcp_state_t state;
            uint8_t backlog;
            uint8_t pending_count;
            net_socket_impl_t *pending[NET_TCP_ACCEPTQ];
            net_socket_impl_t *listener;
            uint32_t iss;
            uint32_t snd_una;
            uint32_t snd_nxt;
            uint32_t irs;
            uint32_t rcv_nxt;
            uint16_t peer_wnd;
            uint8_t rx_buf[NET_TCP_BUF_MAX];
            size_t rx_head;
            size_t rx_tail;
            size_t rx_len;
            bool rx_closed;
            bool app_shut_rd;
            bool app_shut_wr;
            bool fin_pending;
            bool detached;
            bool error;
            tcp_tx_seg_t tx_seg;
            uint64_t timewait_deadline;
        } tcp;
    };
};

typedef struct {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t csum;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t off_flags;
    uint16_t window;
    uint16_t csum;
    uint16_t urg_ptr;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    bool ok;
    bool local;
    int if_index;
    int dev_index;
    uint32_t src_ip_be;
    uint32_t next_hop_be;
} route_result_t;

static bool g_net_ready;
static net_if_t g_ifaces[NET_IF_MAX];
static uint32_t g_if_count;
static net_route_t g_routes[NET_ROUTE_MAX];
static arp_entry_t g_arp[NET_ARP_MAX];
static ping_wait_t g_ping;
static uint16_t g_ping_seq;
static uint16_t g_next_ephemeral;
static char g_resp[NET_RESP_MAX];
static size_t g_resp_len;
static size_t g_resp_off;
static uint8_t g_frame[NET_FRAME_MAX];
static net_rx_item_t g_rxq[NET_RXQ_MAX];
static uint32_t g_rxq_head;
static uint32_t g_rxq_tail;
static uint32_t g_rxq_count;
static struct net_socket g_socks[NET_SOCK_MAX];

static uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t be32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8) |
           ((v & 0x00FF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static uint16_t min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}

static uint32_t mask_prefix_len(uint32_t mask_be) {
    uint32_t n = 0U;
    while (mask_be & 0x80000000U) {
        n++;
        mask_be <<= 1;
    }
    return n;
}

static uint32_t net_rand32(void) {
    static uint64_t seed = 0x5A17D3C4B8E91F23ULL;
    seed ^= timer_ticks() + 0x9E3779B97F4A7C15ULL;
    seed = seed * 6364136223846793005ULL + 1ULL;
    return (uint32_t)(seed >> 16);
}

static bool tcp_seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool tcp_seq_gt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static uint32_t tcp_tx_seg_end_seq(const struct net_socket *sock) {
    uint32_t end;
    if (!sock || !sock->tcp.tx_seg.valid) {
        return 0U;
    }
    end = sock->tcp.tx_seg.seq + (uint32_t)sock->tcp.tx_seg.len;
    if ((sock->tcp.tx_seg.flags & TCP_SYN) != 0U) {
        end++;
    }
    if ((sock->tcp.tx_seg.flags & TCP_FIN) != 0U) {
        end++;
    }
    return end;
}

static void socket_set_error(struct net_socket *s, int err) {
    if (!s || err <= 0 || s->last_error != 0) {
        return;
    }
    s->last_error = err;
}

static int socket_take_error(struct net_socket *s) {
    if (!s) {
        return NETERR_EINVAL;
    }
    int err = s->last_error;
    s->last_error = 0;
    return err;
}

static int socket_error_or(const struct net_socket *s, int fallback) {
    if (!s) {
        return fallback;
    }
    return s->last_error != 0 ? s->last_error : fallback;
}

static bool ip_in_subnet(uint32_t ip_be, uint32_t prefix_be, uint32_t mask_be) {
    return (ip_be & mask_be) == (prefix_be & mask_be);
}

static bool parse_ipv4_be(const char *s, uint32_t *ip_be_out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    uint32_t idx = 0;
    uint32_t cur = 0;
    bool have = false;

    if (!s || !ip_be_out) {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            have = true;
            cur = cur * 10U + (uint32_t)(c - '0');
            if (cur > 255U) {
                return false;
            }
            continue;
        }
        if (c == '.') {
            if (!have || idx >= 3U) {
                return false;
            }
            parts[idx++] = cur;
            cur = 0U;
            have = false;
            continue;
        }
        return false;
    }
    if (!have || idx != 3U) {
        return false;
    }
    parts[3] = cur;
    *ip_be_out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static void append_text(char *dst, size_t *off, size_t cap, const char *src) {
    if (!dst || !off || !src || *off >= cap) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - *off - 1U) {
        n = cap - *off - 1U;
    }
    memcpy(dst + *off, src, n);
    *off += n;
    dst[*off] = '\0';
}

static void append_u32(char *dst, size_t *off, size_t cap, uint32_t v) {
    char tmp[16];
    size_t n = 0U;
    if (v == 0U) {
        tmp[n++] = '0';
    } else {
        char rev[16];
        while (v > 0U && n < sizeof(rev)) {
            rev[n++] = (char)('0' + (v % 10U));
            v /= 10U;
        }
        for (size_t i = 0; i < n; i++) {
            tmp[i] = rev[n - 1U - i];
        }
    }
    tmp[n] = '\0';
    append_text(dst, off, cap, tmp);
}

static void ip_be_to_text(uint32_t ip_be, char *out, size_t outsz) {
    if (!out || outsz < 16U) {
        return;
    }
    uint32_t b0 = (ip_be >> 24) & 0xFFU;
    uint32_t b1 = (ip_be >> 16) & 0xFFU;
    uint32_t b2 = (ip_be >> 8) & 0xFFU;
    uint32_t b3 = ip_be & 0xFFU;
    size_t off = 0U;
    out[0] = '\0';
    append_u32(out, &off, outsz, b0);
    append_text(out, &off, outsz, ".");
    append_u32(out, &off, outsz, b1);
    append_text(out, &off, outsz, ".");
    append_u32(out, &off, outsz, b2);
    append_text(out, &off, outsz, ".");
    append_u32(out, &off, outsz, b3);
}

static uint16_t checksum16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0U;
    while (len >= 2U) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t ipv4_l4_checksum(uint32_t src_ip_be, uint32_t dst_ip_be, uint8_t proto,
                                 const void *payload, size_t len) {
    uint32_t sum = 0U;
    const uint8_t *p = (const uint8_t *)payload;
    uint8_t pseudo[12];

    pseudo[0] = (uint8_t)((src_ip_be >> 24) & 0xFFU);
    pseudo[1] = (uint8_t)((src_ip_be >> 16) & 0xFFU);
    pseudo[2] = (uint8_t)((src_ip_be >> 8) & 0xFFU);
    pseudo[3] = (uint8_t)(src_ip_be & 0xFFU);
    pseudo[4] = (uint8_t)((dst_ip_be >> 24) & 0xFFU);
    pseudo[5] = (uint8_t)((dst_ip_be >> 16) & 0xFFU);
    pseudo[6] = (uint8_t)((dst_ip_be >> 8) & 0xFFU);
    pseudo[7] = (uint8_t)(dst_ip_be & 0xFFU);
    pseudo[8] = 0U;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)((len >> 8) & 0xFFU);
    pseudo[11] = (uint8_t)(len & 0xFFU);

    for (size_t i = 0; i < sizeof(pseudo); i += 2U) {
        sum += ((uint32_t)pseudo[i] << 8) | (uint32_t)pseudo[i + 1U];
    }
    while (len >= 2U) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void resp_set(const char *s) {
    if (!s) {
        g_resp_len = 0U;
        g_resp_off = 0U;
        return;
    }
    size_t n = strlen(s);
    if (n > NET_RESP_MAX - 1U) {
        n = NET_RESP_MAX - 1U;
    }
    memcpy(g_resp, s, n);
    g_resp[n] = '\0';
    g_resp_len = n;
    g_resp_off = 0U;
}

static void arp_age(uint64_t now) {
    for (uint32_t i = 0; i < NET_ARP_MAX; i++) {
        if (!g_arp[i].valid) {
            continue;
        }
        if (now - g_arp[i].updated_tick >= NET_ARP_TTL_TICKS) {
            memset(&g_arp[i], 0, sizeof(g_arp[i]));
        }
    }
}

static void arp_flush(void) {
    memset(g_arp, 0, sizeof(g_arp));
}

static int iface_lookup(const char *name) {
    if (!name) {
        return -1;
    }
    for (uint32_t i = 0; i < g_if_count; i++) {
        if (strcmp(g_ifaces[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool ip_is_any_local(uint32_t ip_be) {
    if ((ip_be & NET_LOOPBACK_MASK_BE) == (NET_LOOPBACK_IP_BE & NET_LOOPBACK_MASK_BE)) {
        return true;
    }
    for (uint32_t i = 0; i < g_if_count; i++) {
        if (g_ifaces[i].up && g_ifaces[i].addr_be != 0U && g_ifaces[i].addr_be == ip_be) {
            return true;
        }
    }
    return false;
}

static uint32_t iface_src_ip_for_dst(const net_if_t *iface, uint32_t dst_ip_be) {
    if (!iface) {
        return 0U;
    }
    if (iface->loopback || (dst_ip_be & NET_LOOPBACK_MASK_BE) == (NET_LOOPBACK_IP_BE & NET_LOOPBACK_MASK_BE)) {
        return NET_LOOPBACK_IP_BE;
    }
    return iface->addr_be;
}

static void routes_clear_for_iface(int if_index) {
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        if (g_routes[i].used && g_routes[i].if_index == if_index) {
            memset(&g_routes[i], 0, sizeof(g_routes[i]));
        }
    }
}

static bool route_add(uint32_t prefix_be, uint32_t mask_be, uint32_t gw_be, int if_index) {
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        if (g_routes[i].used &&
            g_routes[i].prefix_be == prefix_be &&
            g_routes[i].mask_be == mask_be) {
            g_routes[i].gw_be = gw_be;
            g_routes[i].if_index = if_index;
            return true;
        }
    }
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        if (!g_routes[i].used) {
            g_routes[i].used = true;
            g_routes[i].prefix_be = prefix_be;
            g_routes[i].mask_be = mask_be;
            g_routes[i].gw_be = gw_be;
            g_routes[i].if_index = if_index;
            return true;
        }
    }
    return false;
}

static bool route_del(uint32_t prefix_be, uint32_t mask_be) {
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        if (g_routes[i].used &&
            g_routes[i].prefix_be == prefix_be &&
            g_routes[i].mask_be == mask_be) {
            memset(&g_routes[i], 0, sizeof(g_routes[i]));
            return true;
        }
    }
    return false;
}

static bool route_del_default(int if_index) {
    bool removed = false;
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        if (!g_routes[i].used) {
            continue;
        }
        if (g_routes[i].prefix_be != 0U || g_routes[i].mask_be != 0U) {
            continue;
        }
        if (if_index >= 0 && g_routes[i].if_index != if_index) {
            continue;
        }
        memset(&g_routes[i], 0, sizeof(g_routes[i]));
        removed = true;
    }
    return removed;
}

static void iface_refresh_routes(int if_index) {
    if (if_index < 0 || (uint32_t)if_index >= g_if_count) {
        return;
    }
    net_if_t *iface = &g_ifaces[if_index];
    routes_clear_for_iface(if_index);
    if (!iface->up) {
        return;
    }
    if (iface->loopback) {
        (void)route_add(NET_LOOPBACK_IP_BE & NET_LOOPBACK_MASK_BE, NET_LOOPBACK_MASK_BE, 0U, if_index);
        return;
    }
    if (iface->mask_be != 0U) {
        (void)route_add(iface->addr_be & iface->mask_be, iface->mask_be, 0U, if_index);
    }
    if (iface->gw_be != 0U) {
        (void)route_add(0U, 0U, iface->gw_be, if_index);
    }
}

static route_result_t route_lookup(uint32_t dst_ip_be) {
    route_result_t res;
    memset(&res, 0, sizeof(res));

    if (ip_is_any_local(dst_ip_be) ||
        (dst_ip_be & NET_LOOPBACK_MASK_BE) == (NET_LOOPBACK_IP_BE & NET_LOOPBACK_MASK_BE)) {
        for (uint32_t i = 0; i < g_if_count; i++) {
            if (g_ifaces[i].loopback) {
                res.ok = true;
                res.local = true;
                res.if_index = (int)i;
                res.dev_index = -1;
                res.src_ip_be = iface_src_ip_for_dst(&g_ifaces[i], dst_ip_be);
                res.next_hop_be = dst_ip_be;
                return res;
            }
        }
    }

    uint32_t best_prefix = 0U;
    const net_route_t *best = 0;
    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        const net_route_t *r = &g_routes[i];
        if (!r->used || r->if_index < 0 || (uint32_t)r->if_index >= g_if_count) {
            continue;
        }
        if (!g_ifaces[r->if_index].up) {
            continue;
        }
        if (!ip_in_subnet(dst_ip_be, r->prefix_be, r->mask_be)) {
            continue;
        }
        uint32_t prefix = mask_prefix_len(r->mask_be);
        if (!best || prefix > best_prefix) {
            best = r;
            best_prefix = prefix;
        }
    }

    if (!best) {
        return res;
    }

    const net_if_t *iface = &g_ifaces[best->if_index];
    res.ok = true;
    res.local = iface->loopback;
    res.if_index = best->if_index;
    res.dev_index = iface->dev_index;
    res.src_ip_be = iface_src_ip_for_dst(iface, dst_ip_be);
    res.next_hop_be = best->gw_be ? best->gw_be : dst_ip_be;
    return res;
}

static bool pick_broadcast_iface(uint32_t bind_ip_be, route_result_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < g_if_count; i++) {
        const net_if_t *iface = &g_ifaces[i];
        if (!iface->up || iface->loopback || iface->dev_index < 0) {
            continue;
        }
        if (bind_ip_be != 0U && iface->addr_be != bind_ip_be) {
            continue;
        }
        out->ok = true;
        out->local = false;
        out->if_index = (int)i;
        out->dev_index = iface->dev_index;
        out->src_ip_be = iface->addr_be;
        out->next_hop_be = 0xFFFFFFFFU;
        return true;
    }
    return false;
}

static int arp_lookup(uint32_t ip_be, int if_index, uint8_t mac_out[6]) {
    for (uint32_t i = 0; i < NET_ARP_MAX; i++) {
        if (g_arp[i].valid && g_arp[i].ip_be == ip_be && g_arp[i].if_index == if_index) {
            memcpy(mac_out, g_arp[i].mac, 6U);
            return (int)i;
        }
    }
    return -1;
}

static void arp_update(uint32_t ip_be, int if_index, const uint8_t *mac) {
    if (!mac) {
        return;
    }
    int slot = arp_lookup(ip_be, if_index, (uint8_t [6]){0});
    if (slot < 0) {
        uint32_t victim = 0U;
        uint64_t oldest = ~0ULL;
        for (uint32_t i = 0; i < NET_ARP_MAX; i++) {
            if (!g_arp[i].valid) {
                victim = i;
                oldest = 0U;
                break;
            }
            if (g_arp[i].updated_tick < oldest) {
                oldest = g_arp[i].updated_tick;
                victim = i;
            }
        }
        slot = (int)victim;
    }
    g_arp[slot].valid = true;
    g_arp[slot].ip_be = ip_be;
    g_arp[slot].if_index = if_index;
    memcpy(g_arp[slot].mac, mac, 6U);
    g_arp[slot].updated_tick = timer_ticks();
}

static bool socket_udp_queue_push(struct net_socket *s, const void *data, size_t len,
                                  uint32_t src_ip_be, uint16_t src_port) {
    if (!s || !data || len > NET_UDP_PAYLOAD_MAX || s->udp.q_count >= NET_SOCK_RXQ) {
        return false;
    }
    uint8_t idx = s->udp.q_tail;
    s->udp.rxq[idx].len = len;
    s->udp.rxq[idx].src_ip_be = src_ip_be;
    s->udp.rxq[idx].src_port = src_port;
    memcpy(s->udp.rxq[idx].data, data, len);
    s->udp.q_tail = (uint8_t)((s->udp.q_tail + 1U) % NET_SOCK_RXQ);
    s->udp.q_count++;
    return true;
}

static bool socket_udp_queue_pop(struct net_socket *s, void *buf, size_t len,
                                 size_t *copied_out, uint32_t *src_ip_be,
                                 uint16_t *src_port) {
    if (!s || s->udp.q_count == 0U || !buf || !copied_out) {
        return false;
    }
    uint8_t idx = s->udp.q_head;
    size_t n = s->udp.rxq[idx].len;
    if (n > len) {
        n = len;
    }
    memcpy(buf, s->udp.rxq[idx].data, n);
    *copied_out = n;
    if (src_ip_be) {
        *src_ip_be = s->udp.rxq[idx].src_ip_be;
    }
    if (src_port) {
        *src_port = s->udp.rxq[idx].src_port;
    }
    s->udp.q_head = (uint8_t)((s->udp.q_head + 1U) % NET_SOCK_RXQ);
    s->udp.q_count--;
    return true;
}

static size_t tcp_rx_write(struct net_socket *s, const uint8_t *data, size_t len) {
    if (!s || !data || len == 0U) {
        return 0U;
    }
    size_t space = NET_TCP_BUF_MAX - s->tcp.rx_len;
    if (len > space) {
        len = space;
    }
    for (size_t i = 0; i < len; i++) {
        s->tcp.rx_buf[s->tcp.rx_tail] = data[i];
        s->tcp.rx_tail = (s->tcp.rx_tail + 1U) % NET_TCP_BUF_MAX;
    }
    s->tcp.rx_len += len;
    return len;
}

static size_t tcp_rx_read(struct net_socket *s, uint8_t *buf, size_t len) {
    if (!s || !buf || len == 0U) {
        return 0U;
    }
    if (len > s->tcp.rx_len) {
        len = s->tcp.rx_len;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = s->tcp.rx_buf[s->tcp.rx_head];
        s->tcp.rx_head = (s->tcp.rx_head + 1U) % NET_TCP_BUF_MAX;
    }
    s->tcp.rx_len -= len;
    return len;
}

static void listener_queue_prune(struct net_socket *listener) {
    if (!listener) {
        return;
    }
    for (uint8_t i = 0U; i < listener->tcp.pending_count;) {
        struct net_socket *child = listener->tcp.pending[i];
        bool drop = false;
        if (!child || !child->used) {
            drop = true;
        } else if (child->tcp.listener != listener) {
            drop = true;
        } else if (child->tcp.state == TCP_STATE_CLOSED || child->tcp.error) {
            drop = true;
        }
        if (!drop) {
            i++;
            continue;
        }
        if (child && child->used) {
            child->tcp.listener = 0;
            memset(child, 0, sizeof(*child));
        }
        for (uint8_t j = (uint8_t)(i + 1U); j < listener->tcp.pending_count; j++) {
            listener->tcp.pending[j - 1U] = listener->tcp.pending[j];
        }
        listener->tcp.pending_count--;
        listener->tcp.pending[listener->tcp.pending_count] = 0;
    }
}

static bool listener_queue_add(struct net_socket *listener, struct net_socket *child) {
    if (!listener || !child) {
        return false;
    }
    listener_queue_prune(listener);
    if (listener->tcp.pending_count >= NET_TCP_ACCEPTQ) {
        return false;
    }
    listener->tcp.pending[listener->tcp.pending_count++] = child;
    child->tcp.listener = listener;
    return true;
}

static void listener_queue_remove(struct net_socket *listener, struct net_socket *child) {
    if (!listener || !child) {
        return;
    }
    for (uint8_t i = 0; i < listener->tcp.pending_count; i++) {
        if (listener->tcp.pending[i] != child) {
            continue;
        }
        for (uint8_t j = (uint8_t)(i + 1U); j < listener->tcp.pending_count; j++) {
            listener->tcp.pending[j - 1U] = listener->tcp.pending[j];
        }
        listener->tcp.pending_count--;
        listener->tcp.pending[listener->tcp.pending_count] = 0;
        child->tcp.listener = 0;
        return;
    }
}

static struct net_socket *listener_queue_take_established(struct net_socket *listener) {
    if (!listener) {
        return 0;
    }
    listener_queue_prune(listener);
    for (uint8_t i = 0; i < listener->tcp.pending_count; i++) {
        struct net_socket *child = listener->tcp.pending[i];
        if (!child || !child->used) {
            continue;
        }
        if (child->tcp.state != TCP_STATE_ESTABLISHED &&
            child->tcp.state != TCP_STATE_CLOSE_WAIT) {
            continue;
        }
        listener_queue_remove(listener, child);
        return child;
    }
    return 0;
}

static bool listener_queue_has_established(const struct net_socket *listener) {
    if (!listener) {
        return false;
    }
    listener_queue_prune((struct net_socket *)listener);
    for (uint8_t i = 0; i < listener->tcp.pending_count; i++) {
        const struct net_socket *child = listener->tcp.pending[i];
        if (!child || !child->used) {
            continue;
        }
        if (child->tcp.state == TCP_STATE_ESTABLISHED || child->tcp.state == TCP_STATE_CLOSE_WAIT) {
            return true;
        }
    }
    return false;
}

static bool socket_port_conflict(uint16_t port_host, uint32_t bind_ip_be,
                                 net_sock_proto_t proto, const struct net_socket *skip) {
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        const struct net_socket *s = &g_socks[i];
        if (!s->used || !s->bound || s == skip || s->proto != proto) {
            continue;
        }
        if (s->bind_port != port_host) {
            continue;
        }
        bool addr_overlap = (s->bind_ip_be == 0U || bind_ip_be == 0U || s->bind_ip_be == bind_ip_be);
        if (!addr_overlap) {
            continue;
        }
        if (!(s->reuseaddr && skip && skip->reuseaddr)) {
            return true;
        }
    }
    return false;
}

static uint16_t socket_alloc_ephemeral(struct net_socket *skip, uint32_t bind_ip_be,
                                       net_sock_proto_t proto) {
    uint16_t start = g_next_ephemeral;
    if (start < NET_EPHEMERAL_BASE || start > NET_EPHEMERAL_LAST) {
        start = NET_EPHEMERAL_BASE;
    }

    uint16_t p = start;
    do {
        if (!socket_port_conflict(p, bind_ip_be, proto, skip)) {
            g_next_ephemeral = (p == NET_EPHEMERAL_LAST) ? NET_EPHEMERAL_BASE : (uint16_t)(p + 1U);
            return p;
        }
        p = (p == NET_EPHEMERAL_LAST) ? NET_EPHEMERAL_BASE : (uint16_t)(p + 1U);
    } while (p != start);

    return 0U;
}

static struct net_socket *socket_alloc_internal(net_sock_proto_t proto) {
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            struct net_socket *s = &g_socks[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->refs = 1U;
            s->proto = proto;
            if (proto == NET_SOCK_PROTO_TCP) {
                s->tcp.peer_wnd = NET_TCP_BUF_MAX;
            }
            return s;
        }
    }
    return 0;
}

static void socket_reset(struct net_socket *s) {
    if (!s) {
        return;
    }
    if (s->proto == NET_SOCK_PROTO_TCP && s->tcp.listener) {
        listener_queue_remove(s->tcp.listener, s);
    }
    if (s->proto == NET_SOCK_PROTO_TCP && s->tcp.state == TCP_STATE_LISTEN) {
        while (s->tcp.pending_count > 0U) {
            struct net_socket *child = s->tcp.pending[s->tcp.pending_count - 1U];
            s->tcp.pending[s->tcp.pending_count - 1U] = 0;
            s->tcp.pending_count--;
            if (child) {
                memset(child, 0, sizeof(*child));
            }
        }
    }
    memset(s, 0, sizeof(*s));
}

static bool rxq_push(int dev_index, const void *frame, size_t len) {
    if (!frame || len > NET_FRAME_MAX || g_rxq_count >= NET_RXQ_MAX) {
        return false;
    }
    net_rx_item_t *it = &g_rxq[g_rxq_tail];
    memset(it, 0, sizeof(*it));
    it->used = true;
    it->dev_index = dev_index;
    it->len = len;
    memcpy(it->frame, frame, len);
    g_rxq_tail = (g_rxq_tail + 1U) % NET_RXQ_MAX;
    g_rxq_count++;
    return true;
}

static bool rxq_pop(net_rx_item_t *out) {
    if (!out || g_rxq_count == 0U) {
        return false;
    }
    *out = g_rxq[g_rxq_head];
    memset(&g_rxq[g_rxq_head], 0, sizeof(g_rxq[g_rxq_head]));
    g_rxq_head = (g_rxq_head + 1U) % NET_RXQ_MAX;
    g_rxq_count--;
    return true;
}

static bool send_ethernet(int dev_index, const uint8_t *src_mac, const uint8_t *dst_mac,
                          uint16_t ethertype, const void *payload, size_t payload_len) {
    if (dev_index < 0 || !src_mac || !dst_mac || !payload || payload_len > NET_MTU) {
        return false;
    }
    size_t frame_len = sizeof(eth_hdr_t) + payload_len;
    if (frame_len > sizeof(g_frame)) {
        return false;
    }

    eth_hdr_t *eh = (eth_hdr_t *)g_frame;
    memcpy(eh->dst, dst_mac, ETH_ADDR_LEN);
    memcpy(eh->src, src_mac, ETH_ADDR_LEN);
    eh->ethertype = be16(ethertype);
    memcpy(g_frame + sizeof(*eh), payload, payload_len);

    return netdev_send(dev_index, g_frame, frame_len);
}

static bool send_arp_request(int if_index, uint32_t target_ip_be) {
    if (if_index < 0 || (uint32_t)if_index >= g_if_count) {
        return false;
    }
    const net_if_t *iface = &g_ifaces[if_index];
    uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    arp_pkt_t arp;
    memset(&arp, 0, sizeof(arp));
    arp.htype = be16(ARP_HTYPE_ETH);
    arp.ptype = be16(ARP_PTYPE_IPV4);
    arp.hlen = ARP_HLEN_ETH;
    arp.plen = ARP_PLEN_IPV4;
    arp.oper = be16(ARP_OP_REQUEST);
    memcpy(arp.sha, iface->mac, ETH_ADDR_LEN);
    arp.spa = be32(iface->addr_be);
    arp.tpa = be32(target_ip_be);
    return send_ethernet(iface->dev_index, iface->mac, bcast, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static bool send_arp_reply(int if_index, const uint8_t *dst_mac, uint32_t dst_ip_be) {
    if (if_index < 0 || (uint32_t)if_index >= g_if_count || !dst_mac) {
        return false;
    }
    const net_if_t *iface = &g_ifaces[if_index];
    arp_pkt_t arp;
    memset(&arp, 0, sizeof(arp));
    arp.htype = be16(ARP_HTYPE_ETH);
    arp.ptype = be16(ARP_PTYPE_IPV4);
    arp.hlen = ARP_HLEN_ETH;
    arp.plen = ARP_PLEN_IPV4;
    arp.oper = be16(ARP_OP_REPLY);
    memcpy(arp.sha, iface->mac, ETH_ADDR_LEN);
    arp.spa = be32(iface->addr_be);
    memcpy(arp.tha, dst_mac, ETH_ADDR_LEN);
    arp.tpa = be32(dst_ip_be);
    return send_ethernet(iface->dev_index, iface->mac, dst_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static bool resolve_arp(int if_index, uint32_t ip_be, uint64_t timeout_ticks, uint8_t mac_out[6]);
static void process_udp(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, bool local_inject);
static void process_tcp(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, bool local_inject);

static void process_arp(int if_index, const uint8_t *src_mac, const uint8_t *payload, size_t len) {
    if (if_index < 0 || !src_mac || !payload || len < sizeof(arp_pkt_t)) {
        return;
    }
    const arp_pkt_t *arp = (const arp_pkt_t *)payload;
    if (be16(arp->htype) != ARP_HTYPE_ETH ||
        be16(arp->ptype) != ARP_PTYPE_IPV4 ||
        arp->hlen != ARP_HLEN_ETH ||
        arp->plen != ARP_PLEN_IPV4) {
        return;
    }
    uint16_t op = be16(arp->oper);
    uint32_t spa = be32(arp->spa);
    uint32_t tpa = be32(arp->tpa);

    if (op == ARP_OP_REQUEST) {
        arp_update(spa, if_index, arp->sha);
        if (g_ifaces[if_index].addr_be == tpa) {
            (void)send_arp_reply(if_index, src_mac, spa);
        }
    } else if (op == ARP_OP_REPLY) {
        arp_update(spa, if_index, arp->sha);
    }
}

static void process_ipv4(int if_index, const uint8_t *src_mac, const uint8_t *payload,
                         size_t len, bool local_inject) {
    if (!payload || len < sizeof(ipv4_hdr_t)) {
        return;
    }
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)payload;
    uint8_t ihl = (uint8_t)((ip->ver_ihl & 0x0FU) * 4U);
    if ((ip->ver_ihl >> 4) != 4U || ihl < sizeof(ipv4_hdr_t) || len < ihl) {
        return;
    }
    uint16_t tot_len = be16(ip->tot_len);
    if (tot_len < ihl || tot_len > len) {
        return;
    }

    uint32_t src_ip = be32(ip->src);
    uint32_t dst_ip = be32(ip->dst);
    bool limited_broadcast = (dst_ip == 0xFFFFFFFFU &&
                              if_index >= 0 &&
                              (uint32_t)if_index < g_if_count &&
                              g_ifaces[if_index].up &&
                              !g_ifaces[if_index].loopback);
    if (!ip_is_any_local(dst_ip) && !limited_broadcast) {
        return;
    }
    if (!local_inject && src_mac) {
        arp_update(src_ip, if_index, src_mac);
    }

    const uint8_t *l4 = payload + ihl;
    size_t l4_len = (size_t)(tot_len - ihl);

    if (ip->proto == IP_PROTO_ICMP) {
        if (l4_len < sizeof(icmp_hdr_t)) {
            return;
        }
        const icmp_hdr_t *icmp = (const icmp_hdr_t *)l4;
        uint16_t id = be16(icmp->id);
        uint16_t seq = be16(icmp->seq);

        if (icmp->type == ICMP_ECHO_REQUEST) {
            uint8_t reply[sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 8U];
            ipv4_hdr_t *rip = (ipv4_hdr_t *)reply;
            icmp_hdr_t *ricmp = (icmp_hdr_t *)(reply + sizeof(*rip));
            uint8_t *pl = reply + sizeof(*rip) + sizeof(*ricmp);
            for (uint32_t i = 0; i < 8U; i++) {
                pl[i] = (uint8_t)('A' + i);
            }
            memset(rip, 0, sizeof(*rip));
            rip->ver_ihl = 0x45U;
            rip->tot_len = be16((uint16_t)sizeof(reply));
            rip->id = be16(seq);
            rip->frag_off = be16(0x4000U);
            rip->ttl = 64U;
            rip->proto = IP_PROTO_ICMP;
            rip->src = be32(dst_ip);
            rip->dst = be32(src_ip);
            rip->csum = be16(checksum16(rip, sizeof(*rip)));
            memset(ricmp, 0, sizeof(*ricmp));
            ricmp->type = ICMP_ECHO_REPLY;
            ricmp->id = be16(id);
            ricmp->seq = be16(seq);
            ricmp->csum = be16(checksum16(ricmp, sizeof(*ricmp) + 8U));
            process_ipv4(if_index, 0, reply, sizeof(reply), true);
        } else if (icmp->type == ICMP_ECHO_REPLY && g_ping.active &&
                   id == g_ping.id && seq == g_ping.seq) {
            g_ping.replied = true;
            g_ping.rx_tick = timer_ticks();
        }
        return;
    }

    if (ip->proto == IP_PROTO_UDP) {
        process_udp(ip, l4, l4_len, local_inject);
    } else if (ip->proto == IP_PROTO_TCP) {
        process_tcp(ip, l4, l4_len, local_inject);
    }
}

static void inject_local_ipv4(uint32_t src_ip_be, uint32_t dst_ip_be, uint8_t proto,
                              const void *payload, size_t payload_len) {
    uint8_t pkt[sizeof(ipv4_hdr_t) + NET_MTU];
    if (!payload || payload_len > NET_MTU) {
        return;
    }
    ipv4_hdr_t *ip = (ipv4_hdr_t *)pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45U;
    ip->tot_len = be16((uint16_t)(sizeof(*ip) + payload_len));
    ip->id = be16((uint16_t)net_rand32());
    ip->frag_off = be16(0x4000U);
    ip->ttl = 64U;
    ip->proto = proto;
    ip->src = be32(src_ip_be);
    ip->dst = be32(dst_ip_be);
    ip->csum = be16(checksum16(ip, sizeof(*ip)));
    memcpy(pkt + sizeof(*ip), payload, payload_len);
    process_ipv4(0, 0, pkt, sizeof(*ip) + payload_len, true);
}

static bool send_ipv4_l4_ex(uint8_t proto, uint32_t src_ip_be, uint32_t dst_ip_be,
                            const void *payload, size_t payload_len, bool preserve_zero_src) {
    route_result_t route;
    memset(&route, 0, sizeof(route));
    if (!payload || payload_len > NET_MTU) {
        return false;
    }
    if (dst_ip_be == 0xFFFFFFFFU) {
        if (!pick_broadcast_iface(src_ip_be, &route)) {
            return false;
        }
    } else {
        route = route_lookup(dst_ip_be);
        if (!route.ok) {
            return false;
        }
    }
    if (src_ip_be == 0U && !preserve_zero_src) {
        src_ip_be = route.src_ip_be;
    }
    if (route.local) {
        inject_local_ipv4(src_ip_be, dst_ip_be, proto, payload, payload_len);
        return true;
    }
    if (route.if_index < 0 || (uint32_t)route.if_index >= g_if_count) {
        return false;
    }

    uint8_t dst_mac[6];
    if (dst_ip_be == 0xFFFFFFFFU) {
        memset(dst_mac, 0xFF, sizeof(dst_mac));
    } else if (!resolve_arp(route.if_index, route.next_hop_be, TIMER_HZ * 2U, dst_mac)) {
        return false;
    }

    const net_if_t *iface = &g_ifaces[route.if_index];
    uint8_t pkt[sizeof(ipv4_hdr_t) + NET_MTU];
    ipv4_hdr_t *ip = (ipv4_hdr_t *)pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45U;
    ip->tot_len = be16((uint16_t)(sizeof(*ip) + payload_len));
    ip->id = be16((uint16_t)net_rand32());
    ip->frag_off = be16(0x4000U);
    ip->ttl = 64U;
    ip->proto = proto;
    ip->src = be32(src_ip_be);
    ip->dst = be32(dst_ip_be);
    ip->csum = be16(checksum16(ip, sizeof(*ip)));
    memcpy(pkt + sizeof(*ip), payload, payload_len);
    return send_ethernet(route.dev_index, iface->mac, dst_mac, ETH_TYPE_IPV4,
                         pkt, sizeof(*ip) + payload_len);
}

static bool send_ipv4_l4(uint8_t proto, uint32_t src_ip_be, uint32_t dst_ip_be,
                         const void *payload, size_t payload_len) {
    return send_ipv4_l4_ex(proto, src_ip_be, dst_ip_be, payload, payload_len, false);
}

static void net_collect_rx(uint32_t budget) {
    uint8_t frame[NET_FRAME_MAX];
    int count = netdev_count();
    for (int d = 0; d < count; d++) {
        for (uint32_t i = 0; i < budget; i++) {
            int n = netdev_recv(d, frame, sizeof(frame));
            if (n <= 0) {
                break;
            }
            if (!rxq_push(d, frame, (size_t)n)) {
                netdev_note_rx_drop(d);
                break;
            }
        }
    }
}

static void net_process_deferred(uint32_t budget) {
    net_rx_item_t it;
    while (budget-- > 0U && rxq_pop(&it)) {
        if (!it.used || it.len < sizeof(eth_hdr_t)) {
            continue;
        }
        int if_index = -1;
        for (uint32_t i = 0; i < g_if_count; i++) {
            if (g_ifaces[i].dev_index == it.dev_index) {
                if_index = (int)i;
                break;
            }
        }
        const eth_hdr_t *eh = (const eth_hdr_t *)it.frame;
        uint16_t et = be16(eh->ethertype);
        const uint8_t *pl = it.frame + sizeof(*eh);
        size_t pl_len = it.len - sizeof(*eh);
        if (et == ETH_TYPE_ARP) {
            process_arp(if_index, eh->src, pl, pl_len);
        } else if (et == ETH_TYPE_IPV4) {
            process_ipv4(if_index, eh->src, pl, pl_len, false);
        }
    }
}

static bool send_udp_ipv4(uint32_t src_ip_be, uint32_t dst_ip_be, uint16_t dst_port,
                          uint16_t src_port, const uint8_t *payload, size_t payload_len) {
    if (!payload || payload_len > NET_UDP_PAYLOAD_MAX) {
        return false;
    }

    uint8_t pkt[sizeof(udp_hdr_t) + NET_UDP_PAYLOAD_MAX];
    udp_hdr_t *udp = (udp_hdr_t *)pkt;
    uint8_t *udp_pl = pkt + sizeof(*udp);
    memcpy(udp_pl, payload, payload_len);

    udp->src_port = be16(src_port);
    udp->dst_port = be16(dst_port);
    udp->len = be16((uint16_t)(sizeof(*udp) + payload_len));
    udp->csum = 0U;
    udp->csum = be16(ipv4_l4_checksum(src_ip_be, dst_ip_be, IP_PROTO_UDP,
                                      pkt, sizeof(*udp) + payload_len));
    if (udp->csum == 0U) {
        udp->csum = be16(0xFFFFU);
    }
    return send_ipv4_l4_ex(IP_PROTO_UDP, src_ip_be, dst_ip_be, pkt,
                           sizeof(*udp) + payload_len, src_ip_be == 0U);
}

static bool tcp_send_segment(struct net_socket *sock, uint16_t flags, uint32_t seq, uint32_t ack,
                             const uint8_t *payload, size_t payload_len, bool retransmit) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP || payload_len > NET_MTU - sizeof(tcp_hdr_t)) {
        return false;
    }
    bool track_tx = !retransmit && (((flags & (TCP_SYN | TCP_FIN)) != 0U) || payload_len > 0U);

    uint8_t pkt[sizeof(tcp_hdr_t) + NET_MTU];
    tcp_hdr_t *tcp = (tcp_hdr_t *)pkt;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = be16(sock->bind_port);
    tcp->dst_port = be16(sock->peer_port);
    tcp->seq = be32(seq);
    tcp->ack = be32(ack);
    tcp->off_flags = be16((uint16_t)((5U << 12) | flags));
    tcp->window = be16((uint16_t)(NET_TCP_BUF_MAX - sock->tcp.rx_len));
    tcp->urg_ptr = 0U;
    if (payload_len > 0U) {
        memcpy(pkt + sizeof(*tcp), payload, payload_len);
    }
    tcp->csum = 0U;
    tcp->csum = be16(ipv4_l4_checksum(sock->bind_ip_be ? sock->bind_ip_be : NET_LOOPBACK_IP_BE,
                                      sock->peer_ip_be, IP_PROTO_TCP,
                                      pkt, sizeof(*tcp) + payload_len));

    if (track_tx) {
        sock->tcp.tx_seg.valid = true;
        sock->tcp.tx_seg.flags = flags;
        sock->tcp.tx_seg.seq = seq;
        sock->tcp.tx_seg.len = payload_len;
        if (payload_len > 0U) {
            memcpy(sock->tcp.tx_seg.data, payload, payload_len);
        }
        sock->tcp.tx_seg.sent_tick = timer_ticks();
        sock->tcp.tx_seg.retries = 0U;
    }

    if (!send_ipv4_l4(IP_PROTO_TCP,
                      sock->bind_ip_be ? sock->bind_ip_be : 0U,
                      sock->peer_ip_be, pkt, sizeof(*tcp) + payload_len)) {
        if (track_tx) {
            memset(&sock->tcp.tx_seg, 0, sizeof(sock->tcp.tx_seg));
        }
        return false;
    }
    return true;
}

static bool tcp_send_ack(struct net_socket *sock) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP) {
        return false;
    }
    return tcp_send_segment(sock, TCP_ACK, sock->tcp.snd_nxt, sock->tcp.rcv_nxt, 0, 0U, true);
}

static void tcp_enter_time_wait(struct net_socket *sock) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP) {
        return;
    }
    sock->tcp.state = TCP_STATE_TIME_WAIT;
    sock->tcp.timewait_deadline = timer_ticks() + NET_TCP_TIMEWAIT_TICKS;
}

static bool tcp_send_fin_now(struct net_socket *sock) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP) {
        return false;
    }
    uint32_t seq = sock->tcp.snd_nxt;
    if (!tcp_send_segment(sock, TCP_ACK | TCP_FIN, seq, sock->tcp.rcv_nxt, 0, 0U, false)) {
        return false;
    }
    sock->tcp.snd_nxt = seq + 1U;
    if (sock->tcp.state == TCP_STATE_ESTABLISHED) {
        sock->tcp.state = TCP_STATE_FIN_WAIT1;
    } else if (sock->tcp.state == TCP_STATE_CLOSE_WAIT) {
        sock->tcp.state = TCP_STATE_LAST_ACK;
    }
    return true;
}

static bool tcp_send_reply_raw(uint32_t src_ip_be, uint32_t dst_ip_be,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint16_t flags) {
    uint8_t pkt[sizeof(tcp_hdr_t)];
    tcp_hdr_t *tcp = (tcp_hdr_t *)pkt;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = be16(src_port);
    tcp->dst_port = be16(dst_port);
    tcp->seq = be32(seq);
    tcp->ack = be32(ack);
    tcp->off_flags = be16((uint16_t)((5U << 12) | flags));
    tcp->window = be16(NET_TCP_BUF_MAX);
    tcp->csum = be16(ipv4_l4_checksum(src_ip_be, dst_ip_be, IP_PROTO_TCP, pkt, sizeof(pkt)));
    return send_ipv4_l4(IP_PROTO_TCP, src_ip_be, dst_ip_be, pkt, sizeof(pkt));
}

static void tcp_send_rst_for_segment(uint32_t local_ip_be, uint32_t remote_ip_be,
                                     uint16_t local_port, uint16_t remote_port,
                                     uint16_t flags, uint32_t seq, uint32_t ack_seq,
                                     size_t payload_len) {
    if (flags & TCP_RST) {
        return;
    }
    if (flags & TCP_ACK) {
        (void)tcp_send_reply_raw(local_ip_be, remote_ip_be, local_port, remote_port,
                                 ack_seq, 0U, TCP_RST);
        return;
    }
    uint32_t ack = seq + (uint32_t)payload_len;
    if (flags & TCP_SYN) {
        ack++;
    }
    if (flags & TCP_FIN) {
        ack++;
    }
    (void)tcp_send_reply_raw(local_ip_be, remote_ip_be, local_port, remote_port,
                             0U, ack, TCP_RST | TCP_ACK);
}

static bool tcp_peer_match(const struct net_socket *s, uint32_t dst_ip_be, uint16_t dst_port,
                           uint32_t src_ip_be, uint16_t src_port) {
    if (!s || !s->used || s->proto != NET_SOCK_PROTO_TCP || !s->bound) {
        return false;
    }
    if (s->bind_port != dst_port) {
        return false;
    }
    if (s->bind_ip_be != 0U && s->bind_ip_be != dst_ip_be) {
        return false;
    }
    if (s->peer_ip_be != src_ip_be || s->peer_port != src_port) {
        return false;
    }
    if (s->tcp.state == TCP_STATE_LISTEN) {
        return false;
    }
    return true;
}

static struct net_socket *tcp_find_listener(uint32_t dst_ip_be, uint16_t dst_port) {
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->proto != NET_SOCK_PROTO_TCP || s->tcp.state != TCP_STATE_LISTEN || !s->bound) {
            continue;
        }
        if (s->bind_port != dst_port) {
            continue;
        }
        if (s->bind_ip_be != 0U && s->bind_ip_be != dst_ip_be) {
            continue;
        }
        return s;
    }
    return 0;
}

static struct net_socket *tcp_find_socket(uint32_t dst_ip_be, uint16_t dst_port,
                                          uint32_t src_ip_be, uint16_t src_port) {
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        if (tcp_peer_match(&g_socks[i], dst_ip_be, dst_port, src_ip_be, src_port)) {
            return &g_socks[i];
        }
    }
    return tcp_find_listener(dst_ip_be, dst_port);
}

static bool tcp_ack_advance(struct net_socket *sock, uint32_t ack_seq) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP) {
        return false;
    }
    if (ack_seq == sock->tcp.snd_una) {
        return false;
    }
    if (tcp_seq_lt(ack_seq, sock->tcp.snd_una) || tcp_seq_gt(ack_seq, sock->tcp.snd_nxt)) {
        return false;
    }

    sock->tcp.snd_una = ack_seq;
    if (sock->tcp.tx_seg.valid) {
        uint32_t tx_end = tcp_tx_seg_end_seq(sock);
        if (!tcp_seq_lt(ack_seq, tx_end)) {
            memset(&sock->tcp.tx_seg, 0, sizeof(sock->tcp.tx_seg));
            if (sock->tcp.fin_pending &&
                (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT)) {
                if (tcp_send_fin_now(sock)) {
                    sock->tcp.fin_pending = false;
                }
            }
        }
    }
    return true;
}

static void tcp_on_rst(struct net_socket *sock) {
    if (!sock || sock->proto != NET_SOCK_PROTO_TCP) {
        return;
    }
    if (sock->tcp.state == TCP_STATE_SYN_SENT || sock->tcp.state == TCP_STATE_SYN_RECV) {
        socket_set_error(sock, NETERR_ECONNREFUSED);
    } else {
        socket_set_error(sock, NETERR_ECONNRESET);
    }
    sock->tcp.error = true;
    sock->tcp.rx_closed = true;
    sock->tcp.state = TCP_STATE_CLOSED;
    memset(&sock->tcp.tx_seg, 0, sizeof(sock->tcp.tx_seg));
    if (sock->tcp.listener && sock->refs <= 1U) {
        socket_reset(sock);
    }
}

static void process_tcp_listener(struct net_socket *listener, uint32_t src_ip_be, uint16_t src_port,
                                 uint32_t dst_ip_be, const tcp_hdr_t *tcp, uint16_t flags,
                                 uint32_t seq) {
    if (!listener || !tcp || listener->tcp.pending_count >= listener->tcp.backlog) {
        if (listener && (flags & TCP_SYN) != 0U) {
            tcp_send_rst_for_segment(dst_ip_be, src_ip_be, listener->bind_port, src_port,
                                     flags, seq, be32(tcp->ack), 0U);
        }
        return;
    }
    if ((flags & TCP_SYN) == 0U) {
        return;
    }

    struct net_socket *child = socket_alloc_internal(NET_SOCK_PROTO_TCP);
    if (!child) {
        return;
    }
    child->bound = true;
    child->bind_ip_be = listener->bind_ip_be ? listener->bind_ip_be : dst_ip_be;
    child->bind_port = listener->bind_port;
    child->peer_ip_be = src_ip_be;
    child->peer_port = src_port;
    child->tcp.state = TCP_STATE_SYN_RECV;
    child->tcp.iss = net_rand32();
    child->tcp.snd_una = child->tcp.iss;
    child->tcp.snd_nxt = child->tcp.iss + 1U;
    child->tcp.irs = seq;
    child->tcp.rcv_nxt = seq + 1U;
    child->tcp.peer_wnd = NET_TCP_BUF_MAX;
    if (!listener_queue_add(listener, child)) {
        socket_reset(child);
        return;
    }
    (void)tcp_send_segment(child, TCP_SYN | TCP_ACK, child->tcp.iss, child->tcp.rcv_nxt,
                           0, 0U, false);
}

static void process_tcp_established(struct net_socket *sock, uint16_t flags, uint32_t seq,
                                    uint32_t ack_seq, uint16_t window,
                                    const uint8_t *payload, size_t payload_len) {
    if (!sock) {
        return;
    }

    sock->tcp.peer_wnd = window;

    if (flags & TCP_RST) {
        tcp_on_rst(sock);
        return;
    }

    if (flags & TCP_ACK) {
        if (tcp_seq_lt(ack_seq, sock->tcp.snd_una) || tcp_seq_gt(ack_seq, sock->tcp.snd_nxt)) {
            (void)tcp_send_ack(sock);
            return;
        }
        if (tcp_ack_advance(sock, ack_seq)) {
            if (sock->tcp.state == TCP_STATE_SYN_RECV && ack_seq == sock->tcp.snd_nxt) {
                sock->tcp.state = TCP_STATE_ESTABLISHED;
            } else if (sock->tcp.state == TCP_STATE_SYN_SENT && ack_seq == sock->tcp.snd_nxt) {
                sock->tcp.state = TCP_STATE_ESTABLISHED;
            } else if (sock->tcp.state == TCP_STATE_FIN_WAIT1 && ack_seq == sock->tcp.snd_nxt) {
                if (sock->tcp.rx_closed) {
                    tcp_enter_time_wait(sock);
                } else {
                    sock->tcp.state = TCP_STATE_FIN_WAIT2;
                }
            } else if (sock->tcp.state == TCP_STATE_CLOSING && ack_seq == sock->tcp.snd_nxt) {
                tcp_enter_time_wait(sock);
            } else if (sock->tcp.state == TCP_STATE_LAST_ACK && ack_seq == sock->tcp.snd_nxt) {
                sock->tcp.state = TCP_STATE_CLOSED;
                sock->tcp.rx_closed = true;
            }
        }
    }

    if (payload_len > 0U) {
        if (sock->tcp.rx_closed) {
            (void)tcp_send_ack(sock);
            return;
        }
        if (tcp_seq_lt(seq, sock->tcp.rcv_nxt)) {
            uint32_t trim = sock->tcp.rcv_nxt - seq;
            if ((size_t)trim >= payload_len) {
                (void)tcp_send_ack(sock);
                payload_len = 0U;
            } else {
                payload += trim;
                payload_len -= (size_t)trim;
                seq = sock->tcp.rcv_nxt;
            }
        }
        if (payload_len == 0U) {
            /* Pure duplicate data; ACK current receive edge and keep processing FIN. */
        } else if (seq == sock->tcp.rcv_nxt) {
            size_t wrote = tcp_rx_write(sock, payload, payload_len);
            sock->tcp.rcv_nxt += (uint32_t)wrote;
            (void)tcp_send_ack(sock);
        } else {
            (void)tcp_send_ack(sock);
            return;
        }
    }

    if (flags & TCP_FIN) {
        uint32_t fin_seq = seq + (uint32_t)payload_len;
        if (tcp_seq_lt(fin_seq, sock->tcp.rcv_nxt)) {
            (void)tcp_send_ack(sock);
            if (sock->tcp.state == TCP_STATE_TIME_WAIT) {
                tcp_enter_time_wait(sock);
            }
            return;
        }
        if (fin_seq == sock->tcp.rcv_nxt) {
            sock->tcp.rcv_nxt++;
            sock->tcp.rx_closed = true;
            (void)tcp_send_ack(sock);
            if (sock->tcp.state == TCP_STATE_ESTABLISHED) {
                sock->tcp.state = TCP_STATE_CLOSE_WAIT;
            } else if (sock->tcp.state == TCP_STATE_FIN_WAIT1) {
                if (sock->tcp.snd_una == sock->tcp.snd_nxt) {
                    tcp_enter_time_wait(sock);
                } else {
                    sock->tcp.state = TCP_STATE_CLOSING;
                }
            } else if (sock->tcp.state == TCP_STATE_FIN_WAIT2) {
                tcp_enter_time_wait(sock);
            } else if (sock->tcp.state == TCP_STATE_TIME_WAIT) {
                tcp_enter_time_wait(sock);
            }
        } else {
            (void)tcp_send_ack(sock);
        }
    }
}

static void process_tcp(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, bool local_inject) {
    (void)local_inject;
    if (!ip || !payload || len < sizeof(tcp_hdr_t)) {
        return;
    }
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)payload;
    uint16_t off_flags = be16(tcp->off_flags);
    uint8_t hdr_len = (uint8_t)(((off_flags >> 12) & 0xFU) * 4U);
    if (hdr_len < sizeof(*tcp) || hdr_len > len) {
        return;
    }
    uint16_t flags = off_flags & 0x01FFU;
    uint32_t src_ip_be = be32(ip->src);
    uint32_t dst_ip_be = be32(ip->dst);
    uint16_t src_port = be16(tcp->src_port);
    uint16_t dst_port = be16(tcp->dst_port);
    uint32_t seq = be32(tcp->seq);
    uint32_t ack_seq = be32(tcp->ack);
    uint16_t win = be16(tcp->window);
    const uint8_t *tcp_pl = payload + hdr_len;
    size_t tcp_pl_len = len - hdr_len;

    struct net_socket *sock = tcp_find_socket(dst_ip_be, dst_port, src_ip_be, src_port);
    if (!sock) {
        tcp_send_rst_for_segment(dst_ip_be, src_ip_be, dst_port, src_port, flags, seq, ack_seq, tcp_pl_len);
        return;
    }
    if (sock->tcp.state == TCP_STATE_LISTEN) {
        process_tcp_listener(sock, src_ip_be, src_port, dst_ip_be, tcp, flags, seq);
        return;
    }

    if (sock->tcp.state == TCP_STATE_SYN_SENT) {
        if ((flags & TCP_ACK) != 0U &&
            (tcp_seq_lt(ack_seq, sock->tcp.snd_una) || tcp_seq_gt(ack_seq, sock->tcp.snd_nxt))) {
            tcp_send_rst_for_segment(dst_ip_be, src_ip_be, dst_port, src_port, flags, seq, ack_seq, tcp_pl_len);
            return;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack_seq == sock->tcp.snd_nxt) {
            sock->tcp.irs = seq;
            sock->tcp.rcv_nxt = seq + 1U;
            sock->tcp.peer_wnd = win;
            (void)tcp_ack_advance(sock, ack_seq);
            sock->tcp.state = TCP_STATE_ESTABLISHED;
            (void)tcp_send_ack(sock);
            return;
        }
        if (flags & TCP_RST) {
            tcp_on_rst(sock);
        }
        return;
    }

    process_tcp_established(sock, flags, seq, ack_seq, win, tcp_pl, tcp_pl_len);
}

static void process_udp(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, bool local_inject) {
    (void)local_inject;
    if (!ip || !payload || len < sizeof(udp_hdr_t)) {
        return;
    }
    const udp_hdr_t *uh = (const udp_hdr_t *)payload;
    uint16_t ulen = be16(uh->len);
    if (ulen < sizeof(*uh) || ulen > len) {
        return;
    }

    uint32_t src_ip_be = be32(ip->src);
    uint32_t dst_ip_be = be32(ip->dst);
    uint16_t src_port = be16(uh->src_port);
    uint16_t dst_port = be16(uh->dst_port);
    const uint8_t *udp_payload = payload + sizeof(*uh);
    size_t udp_payload_len = (size_t)(ulen - sizeof(*uh));

    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->proto != NET_SOCK_PROTO_UDP || !s->bound) {
            continue;
        }
        if (s->bind_port != dst_port) {
            continue;
        }
        if (s->bind_ip_be != 0U && s->bind_ip_be != dst_ip_be) {
            continue;
        }
        (void)socket_udp_queue_push(s, udp_payload, udp_payload_len, src_ip_be, src_port);
    }
}

static void tcp_tick(uint64_t now) {
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->proto != NET_SOCK_PROTO_TCP) {
            continue;
        }
        if (s->tcp.detached && (s->tcp.state == TCP_STATE_CLOSED || s->tcp.error)) {
            socket_reset(s);
            continue;
        }
        if (s->tcp.state == TCP_STATE_TIME_WAIT &&
            s->tcp.timewait_deadline != 0U &&
            now >= s->tcp.timewait_deadline) {
            socket_reset(s);
            continue;
        }
        if (!s->tcp.tx_seg.valid) {
            continue;
        }
        uint64_t rto = NET_TCP_RTO_TICKS;
        uint8_t shift = s->tcp.tx_seg.retries;
        if (shift > 3U) {
            shift = 3U;
        }
        rto <<= shift;
        if (now - s->tcp.tx_seg.sent_tick < rto) {
            continue;
        }
        if (s->tcp.tx_seg.retries >= NET_TCP_MAX_RETRIES) {
            socket_set_error(s, NETERR_ETIMEDOUT);
            tcp_on_rst(s);
            continue;
        }
        uint16_t flags = s->tcp.tx_seg.flags;
        uint32_t ack = s->tcp.rcv_nxt;
        if ((flags & (TCP_SYN | TCP_FIN)) != 0U || s->tcp.tx_seg.len > 0U) {
            flags |= TCP_ACK;
        }
        if (!tcp_send_segment(s, flags, s->tcp.tx_seg.seq, ack,
                              s->tcp.tx_seg.data, s->tcp.tx_seg.len, true)) {
            continue;
        }
        s->tcp.tx_seg.sent_tick = now;
        s->tcp.tx_seg.retries++;
    }
}

void net_pump(void) {
    netdev_poll_all();
    net_collect_rx(8U);
    net_process_deferred(NET_RX_BUDGET);
    tcp_tick(timer_ticks());
}

static bool resolve_arp(int if_index, uint32_t ip_be, uint64_t timeout_ticks, uint8_t mac_out[6]) {
    if (!mac_out || ip_be == 0U || if_index < 0 || (uint32_t)if_index >= g_if_count) {
        return false;
    }
    if (arp_lookup(ip_be, if_index, mac_out) >= 0) {
        return true;
    }

    uint64_t t0 = timer_ticks();
    uint64_t last_req = 0U;
    while (timer_ticks() - t0 < timeout_ticks) {
        uint64_t now = timer_ticks();
        if (now == t0 || now - last_req >= (TIMER_HZ / 4U + 1U)) {
            (void)send_arp_request(if_index, ip_be);
            last_req = now;
        }
        net_pump();
        if (arp_lookup(ip_be, if_index, mac_out) >= 0) {
            return true;
        }
        __asm__ volatile("yield");
    }
    return false;
}

static bool net_ping4(uint32_t dst_ip_be, uint64_t timeout_ticks, uint32_t *rtt_ms_out) {
    if (!g_net_ready || dst_ip_be == 0U) {
        return false;
    }

    g_ping.active = true;
    g_ping.replied = false;
    g_ping.id = 0xF00DU;
    g_ping.seq = ++g_ping_seq;
    g_ping.tx_tick = timer_ticks();
    g_ping.rx_tick = 0U;

    uint8_t pkt[sizeof(icmp_hdr_t) + 8U];
    icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
    uint8_t *pl = pkt + sizeof(*icmp);
    for (uint32_t i = 0; i < 8U; i++) {
        pl[i] = (uint8_t)('A' + i);
    }
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->id = be16(g_ping.id);
    icmp->seq = be16(g_ping.seq);
    icmp->csum = be16(checksum16(icmp, sizeof(*icmp) + 8U));

    if (!send_ipv4_l4(IP_PROTO_ICMP, 0U, dst_ip_be, pkt, sizeof(pkt))) {
        g_ping.active = false;
        return false;
    }

    while (timer_ticks() - g_ping.tx_tick < timeout_ticks) {
        net_pump();
        if (g_ping.replied) {
            if (rtt_ms_out) {
                uint64_t dt = g_ping.rx_tick - g_ping.tx_tick;
                *rtt_ms_out = (uint32_t)((dt * 1000U) / TIMER_HZ);
            }
            g_ping.active = false;
            return true;
        }
        __asm__ volatile("yield");
    }

    g_ping.active = false;
    return false;
}

static bool rtl_wrap_send(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    return rtl8125_send_frame(buf, len);
}

static int rtl_wrap_recv(void *ctx, void *buf, size_t maxlen) {
    (void)ctx;
    return rtl8125_recv_frame(buf, maxlen);
}

static void rtl_wrap_poll(void *ctx) {
    (void)ctx;
    rtl8125_poll();
}

static const uint8_t *rtl_wrap_mac(void *ctx) {
    (void)ctx;
    return rtl8125_mac_addr();
}

static bool rtl8139_wrap_send(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    return rtl8139_send_frame(buf, len);
}

static int rtl8139_wrap_recv(void *ctx, void *buf, size_t maxlen) {
    (void)ctx;
    return rtl8139_recv_frame(buf, maxlen);
}

static void rtl8139_wrap_poll(void *ctx) {
    (void)ctx;
    rtl8139_poll();
}

static const uint8_t *rtl8139_wrap_mac(void *ctx) {
    (void)ctx;
    return rtl8139_mac_addr();
}

static void iface_init_loopback(void) {
    net_if_t *iface = &g_ifaces[g_if_count++];
    memset(iface, 0, sizeof(*iface));
    iface->up = true;
    iface->loopback = true;
    iface->dev_index = -1;
    strcpy(iface->name, "lo");
    iface->addr_be = NET_LOOPBACK_IP_BE;
    iface->mask_be = NET_LOOPBACK_MASK_BE;
    iface_refresh_routes((int)(g_if_count - 1U));
}

static void iface_init_phys_all(void) {
    int count = netdev_count();
    for (int dev = 0; dev < count && g_if_count < NET_IF_MAX; dev++) {
        net_if_t *iface = &g_ifaces[g_if_count++];
        memset(iface, 0, sizeof(*iface));
        iface->loopback = false;
        iface->dev_index = dev;
        iface->name[0] = 'e';
        iface->name[1] = 't';
        iface->name[2] = 'h';
        iface->name[3] = (char)('0' + dev);
        iface->name[4] = '\0';
        (void)netdev_get_mac(dev, iface->mac);
        if (dev == 0) {
            iface->up = true;
            iface->addr_be = NET_DEFAULT_IP_BE;
            iface->mask_be = NET_DEFAULT_MASK_BE;
            iface->gw_be = NET_DEFAULT_GW_BE;
        }
        iface_refresh_routes((int)(g_if_count - 1U));
    }
}

void net_init(void) {
    g_net_ready = false;
    memset(g_ifaces, 0, sizeof(g_ifaces));
    g_if_count = 0U;
    memset(g_routes, 0, sizeof(g_routes));
    memset(g_arp, 0, sizeof(g_arp));
    memset(&g_ping, 0, sizeof(g_ping));
    g_ping_seq = 0U;
    g_next_ephemeral = NET_EPHEMERAL_BASE;
    g_resp_len = 0U;
    g_resp_off = 0U;
    g_rxq_head = g_rxq_tail = g_rxq_count = 0U;
    memset(g_rxq, 0, sizeof(g_rxq));
    memset(g_socks, 0, sizeof(g_socks));

    if (rtl8125_ready()) {
        static const netdev_ops_t rtl_ops = {
            .poll = rtl_wrap_poll,
            .recv_frame = rtl_wrap_recv,
            .send_frame = rtl_wrap_send,
            .mac_addr = rtl_wrap_mac,
        };
        netdev_desc_t desc = {
            .name = "rtl0",
            .ctx = 0,
            .ops = &rtl_ops,
        };
        (void)netdev_register(&desc);
    }
    if (rtl8139_ready()) {
        static const netdev_ops_t rtl8139_ops = {
            .poll = rtl8139_wrap_poll,
            .recv_frame = rtl8139_wrap_recv,
            .send_frame = rtl8139_wrap_send,
            .mac_addr = rtl8139_wrap_mac,
        };
        netdev_desc_t desc = {
            .name = "rtl8139",
            .ctx = 0,
            .ops = &rtl8139_ops,
        };
        (void)netdev_register(&desc);
    }

    iface_init_loopback();
    iface_init_phys_all();
    g_net_ready = (g_if_count > 0U);
}

void net_tick(uint64_t now_ticks) {
    arp_age(now_ticks);
    net_pump();
}

bool net_ready(void) {
    return g_net_ready;
}

static void net_format_ifaces(char *out, size_t outsz) {
    size_t off = 0U;
    char ip[16];
    char mask[16];
    char gw[16];
    char mac[18];
    netdev_stats_t stats;
    out[0] = '\0';

    for (uint32_t i = 0; i < g_if_count; i++) {
        const net_if_t *iface = &g_ifaces[i];
        ip_be_to_text(iface->addr_be, ip, sizeof(ip));
        ip_be_to_text(iface->mask_be, mask, sizeof(mask));
        ip_be_to_text(iface->gw_be, gw, sizeof(gw));
        if (iface->loopback) {
            mac[0] = '\0';
        } else {
            size_t moff = 0U;
            mac[0] = '\0';
            for (uint32_t b = 0; b < 6U; b++) {
                uint8_t v = iface->mac[b];
                char hex[3];
                hex[0] = (char)((v >> 4) < 10U ? '0' + (v >> 4) : 'a' + ((v >> 4) - 10U));
                hex[1] = (char)((v & 0xFU) < 10U ? '0' + (v & 0xFU) : 'a' + ((v & 0xFU) - 10U));
                hex[2] = '\0';
                append_text(mac, &moff, sizeof(mac), hex);
                if (b != 5U) {
                    append_text(mac, &moff, sizeof(mac), ":");
                }
            }
        }

        append_text(out, &off, outsz, iface->name);
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, ip);
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, mask);
        if (!iface->loopback) {
            append_text(out, &off, outsz, " gw=");
            append_text(out, &off, outsz, gw);
            append_text(out, &off, outsz, " mac=");
            append_text(out, &off, outsz, mac);
            append_text(out, &off, outsz, " dev=");
            {
                const char *dev_name = netdev_get_name(iface->dev_index);
                append_text(out, &off, outsz, dev_name ? dev_name : "unknown");
            }
            if (netdev_stats_snapshot(iface->dev_index, &stats)) {
                append_text(out, &off, outsz, " rxp=");
                append_u32(out, &off, outsz, (uint32_t)stats.rx_packets);
                append_text(out, &off, outsz, " txp=");
                append_u32(out, &off, outsz, (uint32_t)stats.tx_packets);
                append_text(out, &off, outsz, " drop=");
                append_u32(out, &off, outsz, (uint32_t)(stats.rx_drops + stats.tx_drops));
            }
        } else {
            append_text(out, &off, outsz, " loopback");
        }
        append_text(out, &off, outsz, iface->up ? " up\n" : " down\n");
    }
}

static void net_format_routes(char *out, size_t outsz) {
    size_t off = 0U;
    char prefix[16];
    char mask[16];
    char gw[16];
    out[0] = '\0';

    for (uint32_t i = 0; i < NET_ROUTE_MAX; i++) {
        const net_route_t *r = &g_routes[i];
        if (!r->used || r->if_index < 0 || (uint32_t)r->if_index >= g_if_count) {
            continue;
        }
        ip_be_to_text(r->prefix_be, prefix, sizeof(prefix));
        ip_be_to_text(r->mask_be, mask, sizeof(mask));
        ip_be_to_text(r->gw_be, gw, sizeof(gw));
        if (r->prefix_be == 0U && r->mask_be == 0U) {
            append_text(out, &off, outsz, "default");
        } else {
            append_text(out, &off, outsz, prefix);
            append_text(out, &off, outsz, " ");
            append_text(out, &off, outsz, mask);
        }
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, gw);
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, g_ifaces[r->if_index].name);
        append_text(out, &off, outsz, "\n");
    }
}

static void net_format_arp(char *out, size_t outsz) {
    size_t off = 0U;
    char ip[16];
    char mac[18];
    out[0] = '\0';
    for (uint32_t i = 0; i < NET_ARP_MAX; i++) {
        if (!g_arp[i].valid || g_arp[i].if_index < 0 || (uint32_t)g_arp[i].if_index >= g_if_count) {
            continue;
        }
        ip_be_to_text(g_arp[i].ip_be, ip, sizeof(ip));
        size_t moff = 0U;
        mac[0] = '\0';
        for (uint32_t b = 0; b < 6U; b++) {
            uint8_t v = g_arp[i].mac[b];
            char hex[3];
            hex[0] = (char)((v >> 4) < 10U ? (int)('0' + (v >> 4)) : (int)('a' + ((v >> 4) - 10U)));
            hex[1] = (char)((v & 0xFU) < 10U ? (int)('0' + (v & 0xFU)) : (int)('a' + ((v & 0xFU) - 10U)));
            hex[2] = '\0';
            append_text(mac, &moff, sizeof(mac), hex);
            if (b != 5U) {
                append_text(mac, &moff, sizeof(mac), ":");
            }
        }
        append_text(out, &off, outsz, ip);
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, mac);
        append_text(out, &off, outsz, " ");
        append_text(out, &off, outsz, g_ifaces[g_arp[i].if_index].name);
        append_text(out, &off, outsz, " age=");
        append_u32(out, &off, outsz,
                   (uint32_t)((timer_ticks() - g_arp[i].updated_tick) / TIMER_HZ));
        append_text(out, &off, outsz, "s");
        append_text(out, &off, outsz, "\n");
    }
}

static bool net_ifconfig_set(const char *ifname, uint32_t ip_be, uint32_t mask_be, uint32_t gw_be, bool up) {
    int idx = iface_lookup(ifname);
    if (idx < 0 || (uint32_t)idx >= g_if_count) {
        return false;
    }
    net_if_t *iface = &g_ifaces[idx];
    iface->addr_be = ip_be;
    iface->mask_be = mask_be;
    iface->gw_be = gw_be;
    iface->up = up;
    iface_refresh_routes(idx);
    return true;
}

static bool net_ifconfig_updown(const char *ifname, bool up) {
    int idx = iface_lookup(ifname);
    if (idx < 0 || (uint32_t)idx >= g_if_count) {
        return false;
    }
    net_if_t *iface = &g_ifaces[idx];
    iface->up = up;
    iface_refresh_routes(idx);
    return true;
}

int net_dev_write(const void *buf, size_t len) {
    if (!buf || len == 0U) {
        return -1;
    }

    char cmd[NET_CMD_MAX];
    size_t n = len;
    if (n >= sizeof(cmd)) {
        n = sizeof(cmd) - 1U;
    }
    memcpy(cmd, buf, n);
    cmd[n] = '\0';
    while (n > 0U && (cmd[n - 1U] == '\n' || cmd[n - 1U] == '\r' || cmd[n - 1U] == ' ')) {
        cmd[--n] = '\0';
    }

    if (strncmp(cmd, "ping ", 5U) == 0) {
        uint32_t ip_be = 0U;
        if (!parse_ipv4_be(cmd + 5U, &ip_be)) {
            resp_set("ping: bad ip\n");
            return (int)len;
        }
        uint32_t rtt = 0U;
        bool ok = net_ping4(ip_be, TIMER_HZ * 2U, &rtt);
        char ipbuf[16];
        char out[NET_RESP_MAX];
        size_t off = 0U;
        ip_be_to_text(ip_be, ipbuf, sizeof(ipbuf));
        memset(out, 0, sizeof(out));
        if (ok) {
            append_text(out, &off, sizeof(out), "pong ");
            append_text(out, &off, sizeof(out), ipbuf);
            append_text(out, &off, sizeof(out), " rtt=");
            append_u32(out, &off, sizeof(out), rtt);
            append_text(out, &off, sizeof(out), "ms\n");
        } else {
            append_text(out, &off, sizeof(out), "timeout ");
            append_text(out, &off, sizeof(out), ipbuf);
            append_text(out, &off, sizeof(out), "\n");
        }
        resp_set(out);
        return (int)len;
    }

    if (strcmp(cmd, "ifconfig") == 0) {
        char out[NET_RESP_MAX];
        net_format_ifaces(out, sizeof(out));
        resp_set(out);
        return (int)len;
    }

    if (strncmp(cmd, "ifconfig ", 9U) == 0) {
        char arg0[16];
        char arg1[16];
        char arg2[16];
        char arg3[16];
        int ai = 0;
        int wi = 0;
        const char *p = cmd + 9U;
        char *argv[4] = {arg0, arg1, arg2, arg3};
        size_t caps[4] = {sizeof(arg0), sizeof(arg1), sizeof(arg2), sizeof(arg3)};
        memset(arg0, 0, sizeof(arg0));
        memset(arg1, 0, sizeof(arg1));
        memset(arg2, 0, sizeof(arg2));
        memset(arg3, 0, sizeof(arg3));
        while (*p && ai < 4) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            wi = 0;
            while (*p && *p != ' ' && wi + 1 < (int)caps[ai]) {
                argv[ai][wi++] = *p++;
            }
            argv[ai][wi] = '\0';
            while (*p && *p != ' ') {
                p++;
            }
            ai++;
        }
        if (ai == 2 && (strcmp(arg1, "down") == 0 || strcmp(arg1, "up") == 0)) {
            if (!net_ifconfig_updown(arg0, strcmp(arg1, "up") == 0)) {
                resp_set("ifconfig: failed\n");
                return (int)len;
            }
            resp_set("ifconfig: ok\n");
            return (int)len;
        }
        if (ai < 3) {
            resp_set("usage: ifconfig <if> <ip> <mask> [gw] | ifconfig <if> up|down\n");
            return (int)len;
        }
        uint32_t ip_be;
        uint32_t mask_be;
        uint32_t gw_be = 0U;
        if (!parse_ipv4_be(arg1, &ip_be) || !parse_ipv4_be(arg2, &mask_be)) {
            resp_set("ifconfig: bad address\n");
            return (int)len;
        }
        if (ai >= 4 && !parse_ipv4_be(arg3, &gw_be)) {
            resp_set("ifconfig: bad gateway\n");
            return (int)len;
        }
        if (!net_ifconfig_set(arg0, ip_be, mask_be, gw_be, true)) {
            resp_set("ifconfig: failed\n");
            return (int)len;
        }
        resp_set("ifconfig: ok\n");
        return (int)len;
    }

    if (strcmp(cmd, "route") == 0) {
        char out[NET_RESP_MAX];
        net_format_routes(out, sizeof(out));
        resp_set(out);
        return (int)len;
    }

    if (strncmp(cmd, "route add ", 10U) == 0) {
        if (strncmp(cmd + 10U, "default ", 8U) == 0) {
            char gw_s[16];
            char if_s[16];
            const char *p = cmd + 18U;
            int ai = 0;
            char *argv[2] = {gw_s, if_s};
            size_t caps[2] = {sizeof(gw_s), sizeof(if_s)};
            memset(gw_s, 0, sizeof(gw_s));
            memset(if_s, 0, sizeof(if_s));
            while (*p && ai < 2) {
                while (*p == ' ') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                size_t wi = 0U;
                while (*p && *p != ' ' && wi + 1U < caps[ai]) {
                    argv[ai][wi++] = *p++;
                }
                argv[ai][wi] = '\0';
                while (*p && *p != ' ') {
                    p++;
                }
                ai++;
            }
            uint32_t gw_be;
            int if_index = iface_lookup(if_s);
            if (ai < 2 || if_index < 0 || !parse_ipv4_be(gw_s, &gw_be) ||
                !route_add(0U, 0U, gw_be, if_index)) {
                resp_set("usage: route add default <gw> <if>\n");
                return (int)len;
            }
            resp_set("route: ok\n");
            return (int)len;
        }
        char prefix_s[16];
        char mask_s[16];
        char gw_s[16];
        char if_s[16];
        int ai = 0;
        const char *p = cmd + 10U;
        char *argv[4] = {prefix_s, mask_s, gw_s, if_s};
        size_t caps[4] = {sizeof(prefix_s), sizeof(mask_s), sizeof(gw_s), sizeof(if_s)};
        memset(prefix_s, 0, sizeof(prefix_s));
        memset(mask_s, 0, sizeof(mask_s));
        memset(gw_s, 0, sizeof(gw_s));
        memset(if_s, 0, sizeof(if_s));
        while (*p && ai < 4) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            size_t wi = 0U;
            while (*p && *p != ' ' && wi + 1U < caps[ai]) {
                argv[ai][wi++] = *p++;
            }
            argv[ai][wi] = '\0';
            while (*p && *p != ' ') {
                p++;
            }
            ai++;
        }
        if (ai < 4) {
            resp_set("usage: route add <prefix> <mask> <gw> <if>\n");
            return (int)len;
        }
        uint32_t prefix_be;
        uint32_t mask_be;
        uint32_t gw_be;
        int if_index = iface_lookup(if_s);
        if (if_index < 0 ||
            !parse_ipv4_be(prefix_s, &prefix_be) ||
            !parse_ipv4_be(mask_s, &mask_be) ||
            !parse_ipv4_be(gw_s, &gw_be) ||
            !route_add(prefix_be, mask_be, gw_be, if_index)) {
            resp_set("route: failed\n");
            return (int)len;
        }
        resp_set("route: ok\n");
        return (int)len;
    }

    if (strncmp(cmd, "route del ", 10U) == 0) {
        if (strcmp(cmd + 10U, "default") == 0) {
            if (!route_del_default(-1)) {
                resp_set("route: failed\n");
                return (int)len;
            }
            resp_set("route: ok\n");
            return (int)len;
        }
        char prefix_s[16];
        char mask_s[16];
        int ai = 0;
        const char *p = cmd + 10U;
        char *argv[2] = {prefix_s, mask_s};
        size_t caps[2] = {sizeof(prefix_s), sizeof(mask_s)};
        memset(prefix_s, 0, sizeof(prefix_s));
        memset(mask_s, 0, sizeof(mask_s));
        while (*p && ai < 2) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            size_t wi = 0U;
            while (*p && *p != ' ' && wi + 1U < caps[ai]) {
                argv[ai][wi++] = *p++;
            }
            argv[ai][wi] = '\0';
            while (*p && *p != ' ') {
                p++;
            }
            ai++;
        }
        if (ai < 2) {
            resp_set("usage: route del <prefix> <mask>\n");
            return (int)len;
        }
        uint32_t prefix_be;
        uint32_t mask_be;
        if (!parse_ipv4_be(prefix_s, &prefix_be) ||
            !parse_ipv4_be(mask_s, &mask_be) ||
            !route_del(prefix_be, mask_be)) {
            resp_set("route: failed\n");
            return (int)len;
        }
        resp_set("route: ok\n");
        return (int)len;
    }

    if (strncmp(cmd, "route get ", 10U) == 0) {
        uint32_t dst_be = 0U;
        char out[NET_RESP_MAX];
        char dst[16];
        char src[16];
        char hop[16];
        size_t off = 0U;
        route_result_t route;
        if (!parse_ipv4_be(cmd + 10U, &dst_be)) {
            resp_set("route: bad address\n");
            return (int)len;
        }
        route = route_lookup(dst_be);
        if (!route.ok || route.if_index < 0 || (uint32_t)route.if_index >= g_if_count) {
            resp_set("route: unreachable\n");
            return (int)len;
        }
        out[0] = '\0';
        ip_be_to_text(dst_be, dst, sizeof(dst));
        ip_be_to_text(route.src_ip_be, src, sizeof(src));
        ip_be_to_text(route.next_hop_be, hop, sizeof(hop));
        append_text(out, &off, sizeof(out), "dst=");
        append_text(out, &off, sizeof(out), dst);
        append_text(out, &off, sizeof(out), " via=");
        append_text(out, &off, sizeof(out), route.local ? "local" : hop);
        append_text(out, &off, sizeof(out), " dev=");
        append_text(out, &off, sizeof(out), g_ifaces[route.if_index].name);
        append_text(out, &off, sizeof(out), " src=");
        append_text(out, &off, sizeof(out), src);
        append_text(out, &off, sizeof(out), "\n");
        resp_set(out);
        return (int)len;
    }

    if (strcmp(cmd, "arp") == 0) {
        char out[NET_RESP_MAX];
        net_format_arp(out, sizeof(out));
        resp_set(out);
        return (int)len;
    }

    if (strcmp(cmd, "arp flush") == 0) {
        arp_flush();
        resp_set("arp: ok\n");
        return (int)len;
    }

    resp_set("usage: ping|ifconfig|route|arp\n");
    return (int)len;
}

int net_dev_read(void *buf, size_t len) {
    if (!buf || len == 0U) {
        return -1;
    }
    if (g_resp_off >= g_resp_len) {
        return 0;
    }
    size_t n = g_resp_len - g_resp_off;
    if (n > len) {
        n = len;
    }
    memcpy(buf, g_resp + g_resp_off, n);
    g_resp_off += n;
    if (g_resp_off >= g_resp_len) {
        g_resp_off = 0U;
        g_resp_len = 0U;
    }
    return (int)n;
}

int net_socket_create(int domain, int type, int protocol, net_socket_t **out_sock) {
    if (!out_sock) {
        return -1;
    }
    *out_sock = 0;
    if (domain != AF_INET) {
        return -1;
    }

    int base_type = type & 0xFF;
    net_sock_proto_t proto = NET_SOCK_PROTO_NONE;
    if (base_type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) {
        proto = NET_SOCK_PROTO_UDP;
    } else if (base_type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) {
        proto = NET_SOCK_PROTO_TCP;
    } else {
        return -1;
    }

    struct net_socket *s = socket_alloc_internal(proto);
    if (!s) {
        return -1;
    }
    if (proto == NET_SOCK_PROTO_TCP) {
        s->tcp.state = TCP_STATE_CLOSED;
    }
    *out_sock = s;
    return 0;
}

void net_socket_ref(net_socket_t *sock) {
    if (!sock || !sock->used) {
        return;
    }
    if (sock->refs < 0xFFFFU) {
        sock->refs++;
    }
}

void net_socket_close(net_socket_t *sock) {
    if (!sock || !sock->used) {
        return;
    }
    if (sock->refs > 1U) {
        sock->refs--;
        return;
    }
    if (sock->proto == NET_SOCK_PROTO_TCP) {
        sock->refs = 0U;
        if (sock->tcp.state == TCP_STATE_LISTEN ||
            sock->tcp.state == TCP_STATE_CLOSED ||
            sock->tcp.state == TCP_STATE_TIME_WAIT ||
            sock->tcp.error) {
            socket_reset(sock);
            return;
        }
        sock->tcp.detached = true;
        sock->tcp.app_shut_rd = true;
        sock->tcp.app_shut_wr = true;
        if (sock->tcp.state == TCP_STATE_SYN_SENT || sock->tcp.state == TCP_STATE_SYN_RECV) {
            tcp_on_rst(sock);
            socket_reset(sock);
            return;
        }
        if (!sock->tcp.tx_seg.valid &&
            (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT)) {
            if (!tcp_send_fin_now(sock)) {
                tcp_on_rst(sock);
                socket_reset(sock);
            }
        } else if (sock->tcp.tx_seg.valid) {
            sock->tcp.fin_pending = true;
        }
        return;
    }
    socket_reset(sock);
}

int net_socket_bind(net_socket_t *sock, uint32_t addr_be, uint16_t port_be) {
    if (!sock || !sock->used) {
        return -1;
    }
    if (!(addr_be == 0U || ip_is_any_local(addr_be))) {
        return -1;
    }

    uint16_t port_host = be16(port_be);
    if (port_host == 0U) {
        port_host = socket_alloc_ephemeral(sock, addr_be, sock->proto);
        if (port_host == 0U) {
            return -1;
        }
    } else if (socket_port_conflict(port_host, addr_be, sock->proto, sock)) {
        return -1;
    }

    sock->bind_ip_be = addr_be;
    sock->bind_port = port_host;
    sock->bound = true;
    return 0;
}

long net_socket_sendto(net_socket_t *sock, const void *buf, size_t len,
                       uint32_t addr_be, uint16_t port_be) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_UDP ||
        !buf || len > NET_UDP_PAYLOAD_MAX) {
        return -NETERR_EINVAL;
    }

    uint16_t dst_port = be16(port_be);
    if (dst_port == 0U) {
        return -NETERR_EINVAL;
    }

    if (!sock->bound) {
        uint16_t eph = socket_alloc_ephemeral(sock, 0U, sock->proto);
        if (eph == 0U) {
            return -NETERR_ENOBUFS;
        }
        sock->bound = true;
        sock->bind_ip_be = 0U;
        sock->bind_port = eph;
    }

    if (addr_be == 0xFFFFFFFFU && !sock->broadcast) {
        return -NETERR_EACCES;
    }

    uint32_t src_ip = sock->bind_ip_be;
    bool dhcp_broadcast = (src_ip == 0U &&
                           addr_be == 0xFFFFFFFFU &&
                           sock->bind_port == 68U &&
                           dst_port == 67U);
    if (src_ip == 0U) {
        if (dhcp_broadcast) {
            src_ip = 0U;
        } else if (addr_be == 0xFFFFFFFFU) {
            route_result_t route;
            if (!pick_broadcast_iface(0U, &route)) {
                return -NETERR_ENETUNREACH;
            }
            src_ip = route.src_ip_be;
        } else {
            route_result_t route = route_lookup(addr_be);
            if (!route.ok) {
                return -NETERR_ENETUNREACH;
            }
            src_ip = route.src_ip_be;
        }
    }
    if (!send_udp_ipv4(src_ip, addr_be, dst_port, sock->bind_port, (const uint8_t *)buf, len)) {
        return -NETERR_ENETUNREACH;
    }
    return (long)len;
}

long net_socket_recvfrom(net_socket_t *sock, void *buf, size_t len,
                         uint32_t *src_addr_be, uint16_t *src_port_be,
                         bool nonblock) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_UDP || !buf) {
        return -1;
    }

    size_t copied = 0U;
    uint32_t src_ip_be = 0U;
    uint16_t src_port_host = 0U;
    if (!socket_udp_queue_pop(sock, buf, len, &copied, &src_ip_be, &src_port_host)) {
        return nonblock ? -11 : -2;
    }

    if (src_addr_be) {
        *src_addr_be = src_ip_be;
    }
    if (src_port_be) {
        *src_port_be = be16(src_port_host);
    }
    return (long)copied;
}

int net_socket_connect(net_socket_t *sock, uint32_t addr_be, uint16_t port_be, bool nonblock) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP) {
        return -NETERR_EINVAL;
    }
    uint16_t dst_port = be16(port_be);
    if (dst_port == 0U) {
        return -NETERR_EINVAL;
    }
    if (!ip_is_any_local(addr_be) && !route_lookup(addr_be).ok) {
        return -NETERR_ENETUNREACH;
    }

    if (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT) {
        return 0;
    }
    if (sock->tcp.error || (sock->tcp.state == TCP_STATE_CLOSED && sock->peer_port != 0U)) {
        return -socket_error_or(sock, NETERR_ECONNREFUSED);
    }
    if (sock->tcp.state == TCP_STATE_SYN_SENT || sock->tcp.state == TCP_STATE_SYN_RECV) {
        if (!nonblock) {
            net_pump();
            if (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT) {
                return 0;
            }
        }
        if (sock->tcp.error) {
            return -socket_error_or(sock, NETERR_ECONNREFUSED);
        }
        return nonblock ? -NETERR_EALREADY : -2;
    }
    if (sock->tcp.state != TCP_STATE_CLOSED) {
        return -NETERR_EISCONN;
    }

    if (!sock->bound) {
        route_result_t route = route_lookup(addr_be);
        uint32_t bind_ip = route.src_ip_be;
        uint16_t eph = socket_alloc_ephemeral(sock, bind_ip, sock->proto);
        if (eph == 0U) {
            return -NETERR_ENOBUFS;
        }
        sock->bound = true;
        sock->bind_ip_be = bind_ip;
        sock->bind_port = eph;
    }

    sock->last_error = 0;
    sock->tcp.error = false;
    sock->tcp.rx_closed = false;
    sock->peer_ip_be = addr_be;
    sock->peer_port = dst_port;
    sock->tcp.state = TCP_STATE_SYN_SENT;
    sock->tcp.iss = net_rand32();
    sock->tcp.snd_una = sock->tcp.iss;
    sock->tcp.snd_nxt = sock->tcp.iss + 1U;
    sock->tcp.rcv_nxt = 0U;
    sock->tcp.peer_wnd = NET_TCP_BUF_MAX;
    if (!tcp_send_segment(sock, TCP_SYN, sock->tcp.iss, 0U, 0, 0U, false)) {
        socket_set_error(sock, NETERR_ENETUNREACH);
        tcp_on_rst(sock);
        return -socket_error_or(sock, NETERR_ENETUNREACH);
    }
    return nonblock ? -NETERR_EINPROGRESS : -2;
}

int net_socket_listen(net_socket_t *sock, int backlog) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP) {
        return -1;
    }
    if (!sock->bound) {
        uint16_t eph = socket_alloc_ephemeral(sock, 0U, sock->proto);
        if (eph == 0U) {
            return -1;
        }
        sock->bound = true;
        sock->bind_ip_be = 0U;
        sock->bind_port = eph;
    }
    if (backlog < 1) {
        backlog = 1;
    }
    if (backlog > (int)NET_TCP_ACCEPTQ) {
        backlog = (int)NET_TCP_ACCEPTQ;
    }
    sock->tcp.state = TCP_STATE_LISTEN;
    sock->tcp.backlog = (uint8_t)backlog;
    return 0;
}

int net_socket_accept(net_socket_t *sock, net_socket_t **out_sock, uint32_t *addr_be,
                      uint16_t *port_be, bool nonblock) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP ||
        sock->tcp.state != TCP_STATE_LISTEN || !out_sock) {
        return -1;
    }
    *out_sock = 0;
    struct net_socket *child = listener_queue_take_established(sock);
    if (!child && !nonblock) {
        net_pump();
        child = listener_queue_take_established(sock);
    }
    if (!child) {
        return nonblock ? -11 : -2;
    }
    if (addr_be) {
        *addr_be = child->peer_ip_be;
    }
    if (port_be) {
        *port_be = be16(child->peer_port);
    }
    *out_sock = child;
    return 0;
}

int net_socket_getsockname(net_socket_t *sock, uint32_t *addr_be, uint16_t *port_be) {
    if (!sock || !sock->used || !sock->bound) {
        return -1;
    }
    if (addr_be) {
        *addr_be = sock->bind_ip_be;
    }
    if (port_be) {
        *port_be = be16(sock->bind_port);
    }
    return 0;
}

int net_socket_getpeername(net_socket_t *sock, uint32_t *addr_be, uint16_t *port_be) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP || sock->peer_port == 0U) {
        return -NETERR_ENOTCONN;
    }
    if (sock->tcp.state == TCP_STATE_LISTEN || sock->tcp.state == TCP_STATE_CLOSED) {
        return -NETERR_ENOTCONN;
    }
    if (addr_be) {
        *addr_be = sock->peer_ip_be;
    }
    if (port_be) {
        *port_be = be16(sock->peer_port);
    }
    return 0;
}

int net_socket_shutdown(net_socket_t *sock, int how) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP) {
        return -NETERR_EINVAL;
    }
    if (!(how == SHUT_RD || how == SHUT_WR || how == SHUT_RDWR)) {
        return -NETERR_EINVAL;
    }
    if (sock->tcp.state == TCP_STATE_LISTEN ||
        (sock->tcp.state == TCP_STATE_CLOSED && !sock->tcp.error && sock->peer_port == 0U)) {
        return -NETERR_ENOTCONN;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        sock->tcp.app_shut_rd = true;
        sock->tcp.rx_len = 0U;
        sock->tcp.rx_head = 0U;
        sock->tcp.rx_tail = 0U;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        sock->tcp.app_shut_wr = true;
        if (sock->tcp.tx_seg.valid) {
            sock->tcp.fin_pending = true;
        } else if (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT) {
            (void)tcp_send_fin_now(sock);
        }
    }
    return 0;
}

long net_socket_read(net_socket_t *sock, void *buf, size_t len, bool nonblock) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP || !buf) {
        return -NETERR_EINVAL;
    }
    if (sock->tcp.app_shut_rd) {
        return 0;
    }
    if (sock->tcp.rx_len == 0U) {
        if (!nonblock) {
            net_pump();
        }
        if (sock->tcp.error) {
            return -socket_error_or(sock, NETERR_ECONNRESET);
        }
        if (sock->tcp.rx_closed || sock->tcp.state == TCP_STATE_CLOSED) {
            return 0;
        }
        if (sock->tcp.rx_len > 0U) {
            return (long)tcp_rx_read(sock, (uint8_t *)buf, len);
        }
        return nonblock ? -11 : -2;
    }
    return (long)tcp_rx_read(sock, (uint8_t *)buf, len);
}

long net_socket_write(net_socket_t *sock, const void *buf, size_t len, bool nonblock) {
    if (!sock || !sock->used || sock->proto != NET_SOCK_PROTO_TCP ||
        !buf) {
        return (len == 0U) ? 0 : -NETERR_EINVAL;
    }
    if (sock->tcp.error) {
        return -socket_error_or(sock, NETERR_EPIPE);
    }
    if (sock->tcp.app_shut_wr) {
        return -NETERR_EPIPE;
    }
    if (!(sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT)) {
        return -NETERR_ENOTCONN;
    }
    if (sock->tcp.tx_seg.valid) {
        if (!nonblock) {
            net_pump();
            if (!sock->tcp.tx_seg.valid) {
                return net_socket_write(sock, buf, len, nonblock);
            }
        }
        return nonblock ? -11 : -2;
    }
    if (sock->tcp.peer_wnd == 0U) {
        if (!nonblock) {
            net_pump();
            if (sock->tcp.error) {
                return -socket_error_or(sock, NETERR_ECONNRESET);
            }
            if (sock->tcp.peer_wnd != 0U) {
                return net_socket_write(sock, buf, len, nonblock);
            }
        }
        return nonblock ? -11 : -2;
    }
    size_t chunk = len;
    size_t max_chunk = NET_MTU - sizeof(tcp_hdr_t);
    if (chunk > max_chunk) {
        chunk = max_chunk;
    }
    if (chunk > (size_t)sock->tcp.peer_wnd) {
        chunk = (size_t)sock->tcp.peer_wnd;
    }
    if (chunk == 0U) {
        return nonblock ? -11 : -2;
    }
    if (!tcp_send_segment(sock, TCP_ACK | TCP_PSH, sock->tcp.snd_nxt, sock->tcp.rcv_nxt,
                          (const uint8_t *)buf, chunk, false)) {
        return -NETERR_ENETUNREACH;
    }
    sock->tcp.snd_nxt += (uint32_t)chunk;
    return (long)chunk;
}

int net_socket_setsockopt(net_socket_t *sock, int level, int optname,
                          const void *optval, size_t optlen) {
    if (!sock || !sock->used || !optval || optlen < sizeof(int)) {
        return -NETERR_EINVAL;
    }
    if (level != SOL_SOCKET) {
        return -NETERR_EOPNOTSUPP;
    }
    if (optname == SO_REUSEADDR) {
        int v = *(const int *)optval;
        sock->reuseaddr = (v != 0);
        return 0;
    }
    if (optname == SO_BROADCAST) {
        int v = *(const int *)optval;
        sock->broadcast = (v != 0);
        return 0;
    }
    return -NETERR_EOPNOTSUPP;
}

int net_socket_getsockopt(net_socket_t *sock, int level, int optname,
                          void *optval, size_t *optlen) {
    if (!sock || !sock->used || !optval || !optlen || *optlen < sizeof(int)) {
        return -NETERR_EINVAL;
    }
    if (level != SOL_SOCKET) {
        return -NETERR_EOPNOTSUPP;
    }
    int value = 0;
    if (optname == SO_ERROR) {
        value = socket_take_error(sock);
    } else if (optname == SO_REUSEADDR) {
        value = sock->reuseaddr ? 1 : 0;
    } else if (optname == SO_BROADCAST) {
        value = sock->broadcast ? 1 : 0;
    } else {
        return -NETERR_EOPNOTSUPP;
    }
    memcpy(optval, &value, sizeof(value));
    *optlen = sizeof(value);
    return 0;
}

int16_t net_socket_poll_revents(net_socket_t *sock, int16_t events) {
    if (!sock || !sock->used) {
        return POLLERR;
    }

    int16_t revents = 0;
    if (sock->proto == NET_SOCK_PROTO_UDP) {
        if ((events & POLLIN) && sock->udp.q_count > 0U) {
            revents |= POLLIN;
        }
        if (events & POLLOUT) {
            revents |= POLLOUT;
        }
        return revents;
    }

    if (sock->proto != NET_SOCK_PROTO_TCP) {
        return POLLERR;
    }

    if (sock->last_error != 0) {
        revents |= POLLERR;
        if (events & POLLOUT) {
            revents |= POLLOUT;
        }
    }
    if (sock->tcp.error) {
        revents |= POLLHUP;
    }
    if (sock->tcp.state == TCP_STATE_LISTEN) {
        if ((events & POLLIN) && listener_queue_has_established(sock)) {
            revents |= POLLIN;
        }
        return revents;
    }
    if ((events & POLLIN) &&
        (sock->tcp.rx_len > 0U || sock->tcp.rx_closed || sock->tcp.state == TCP_STATE_CLOSED)) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && sock->tcp.state == TCP_STATE_SYN_SENT && sock->tcp.error) {
        revents |= POLLOUT;
    }
    if ((events & POLLOUT) &&
        (sock->tcp.state == TCP_STATE_ESTABLISHED || sock->tcp.state == TCP_STATE_CLOSE_WAIT) &&
        !sock->tcp.tx_seg.valid && !sock->tcp.app_shut_wr && sock->tcp.peer_wnd != 0U) {
        revents |= POLLOUT;
    }
    if (sock->tcp.rx_closed || sock->tcp.state == TCP_STATE_CLOSED) {
        revents |= POLLHUP;
    }
    return revents;
}
