#ifndef FUROS_RTL8125_H
#define FUROS_RTL8125_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void rtl8125_init(void);
bool rtl8125_ready(void);
bool rtl8125_handle_irq(uint32_t intid);
void rtl8125_poll(void);
int rtl8125_recv_frame(void *buf, size_t maxlen);
bool rtl8125_send_frame(const void *buf, size_t len);
const uint8_t *rtl8125_mac_addr(void);

#endif
