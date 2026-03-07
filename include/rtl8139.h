#ifndef FUROS_RTL8139_H
#define FUROS_RTL8139_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void rtl8139_init(void);
bool rtl8139_ready(void);
bool rtl8139_handle_irq(uint32_t intid);
void rtl8139_poll(void);
int rtl8139_recv_frame(void *buf, size_t maxlen);
bool rtl8139_send_frame(const void *buf, size_t len);
const uint8_t *rtl8139_mac_addr(void);

#endif
