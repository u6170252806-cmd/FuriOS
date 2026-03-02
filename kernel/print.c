#include "uart.h"
#include "string.h"
#include <stdint.h>

static void print_hex_nibble(uint8_t v) {
    v &= 0xF;
    uart_putc((char)(v < 10 ? ('0' + v) : ('A' + v - 10)));
}

void print_hex64(uint64_t v) {
    uart_puts("0x");
    for (int i = 15; i >= 0; i--) {
        print_hex_nibble((uint8_t)(v >> (i * 4)));
    }
}

void print_dec(int v) {
    char buf[16];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    if (v < 0) {
        uart_putc('-');
        v = -v;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        uart_putc(buf[i]);
    }
}
