#ifndef FUROS_NETDEV_H
#define FUROS_NETDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETDEV_NAME_MAX 8
#define NETDEV_MAX 4

typedef struct {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_drops;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_drops;
} netdev_stats_t;

typedef struct {
    void (*poll)(void *ctx);
    int (*recv_frame)(void *ctx, void *buf, size_t maxlen);
    bool (*send_frame)(void *ctx, const void *buf, size_t len);
    const uint8_t *(*mac_addr)(void *ctx);
} netdev_ops_t;

typedef struct {
    const char *name;
    void *ctx;
    const netdev_ops_t *ops;
} netdev_desc_t;

bool netdev_register(const netdev_desc_t *desc);
int netdev_count(void);
bool netdev_get_mac(int index, uint8_t mac_out[6]);
const char *netdev_get_name(int index);
void netdev_poll_all(void);
int netdev_recv(int index, void *buf, size_t maxlen);
bool netdev_send(int index, const void *buf, size_t len);
void netdev_note_rx_drop(int index);
void netdev_note_tx_drop(int index);
bool netdev_stats_snapshot(int index, netdev_stats_t *out);

#endif
