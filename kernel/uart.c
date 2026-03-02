#include "uart.h"
#include "config.h"

#define UART_DR    0x00
#define UART_FR    0x18
#define FR_TXFF    (1u << 5)
#define FR_RXFE    (1u << 4)

static volatile uint32_t *const uart = (volatile uint32_t *)UART0_BASE;

void uart_init(void) {
    (void)uart;
}

void uart_putc(char c) {
    while (uart[UART_FR / 4] & FR_TXFF) {
    }
    uart[UART_DR / 4] = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

int uart_getc(void) {
    while (uart[UART_FR / 4] & FR_RXFE) {
    }
    return (int)(uart[UART_DR / 4] & 0xFFu);
}

int uart_rx_ready(void) {
    return (uart[UART_FR / 4] & FR_RXFE) == 0;
}
