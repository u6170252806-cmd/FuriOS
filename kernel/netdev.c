#include "netdev.h"
#include "string.h"

typedef struct {
    bool used;
    char name[NETDEV_NAME_MAX];
    void *ctx;
    const netdev_ops_t *ops;
    netdev_stats_t stats;
} netdev_slot_t;

static netdev_slot_t g_netdev[NETDEV_MAX];

bool netdev_register(const netdev_desc_t *desc) {
    if (!desc || !desc->name || !desc->ops ||
        !desc->ops->recv_frame || !desc->ops->send_frame || !desc->ops->mac_addr) {
        return false;
    }

    for (int i = 0; i < NETDEV_MAX; i++) {
        if (!g_netdev[i].used) {
            netdev_slot_t *s = &g_netdev[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->ctx = desc->ctx;
            s->ops = desc->ops;
            size_t n = strlen(desc->name);
            if (n >= NETDEV_NAME_MAX) {
                n = NETDEV_NAME_MAX - 1U;
            }
            memcpy(s->name, desc->name, n);
            s->name[n] = '\0';
            return true;
        }
    }
    return false;
}

int netdev_count(void) {
    int c = 0;
    for (int i = 0; i < NETDEV_MAX; i++) {
        if (g_netdev[i].used) {
            c++;
        }
    }
    return c;
}

const char *netdev_get_name(int index) {
    if (index < 0 || index >= NETDEV_MAX || !g_netdev[index].used) {
        return 0;
    }
    return g_netdev[index].name;
}

bool netdev_get_mac(int index, uint8_t mac_out[6]) {
    if (!mac_out || index < 0 || index >= NETDEV_MAX || !g_netdev[index].used) {
        return false;
    }
    const uint8_t *mac = g_netdev[index].ops->mac_addr(g_netdev[index].ctx);
    if (!mac) {
        return false;
    }
    memcpy(mac_out, mac, 6U);
    return true;
}

void netdev_poll_all(void) {
    for (int i = 0; i < NETDEV_MAX; i++) {
        if (!g_netdev[i].used || !g_netdev[i].ops || !g_netdev[i].ops->poll) {
            continue;
        }
        g_netdev[i].ops->poll(g_netdev[i].ctx);
    }
}

int netdev_recv(int index, void *buf, size_t maxlen) {
    if (index < 0 || index >= NETDEV_MAX || !g_netdev[index].used || !buf || maxlen == 0U) {
        return -1;
    }
    int n = g_netdev[index].ops->recv_frame(g_netdev[index].ctx, buf, maxlen);
    if (n > 0) {
        g_netdev[index].stats.rx_packets++;
        g_netdev[index].stats.rx_bytes += (uint64_t)n;
    }
    return n;
}

bool netdev_send(int index, const void *buf, size_t len) {
    if (index < 0 || index >= NETDEV_MAX || !g_netdev[index].used || !buf || len == 0U) {
        return false;
    }
    bool ok = g_netdev[index].ops->send_frame(g_netdev[index].ctx, buf, len);
    if (ok) {
        g_netdev[index].stats.tx_packets++;
        g_netdev[index].stats.tx_bytes += (uint64_t)len;
    } else {
        g_netdev[index].stats.tx_drops++;
    }
    return ok;
}

void netdev_note_rx_drop(int index) {
    if (index >= 0 && index < NETDEV_MAX && g_netdev[index].used) {
        g_netdev[index].stats.rx_drops++;
    }
}

void netdev_note_tx_drop(int index) {
    if (index >= 0 && index < NETDEV_MAX && g_netdev[index].used) {
        g_netdev[index].stats.tx_drops++;
    }
}

bool netdev_stats_snapshot(int index, netdev_stats_t *out) {
    if (!out || index < 0 || index >= NETDEV_MAX || !g_netdev[index].used) {
        return false;
    }
    *out = g_netdev[index].stats;
    return true;
}
