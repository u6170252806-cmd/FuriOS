#ifndef FUROS_UART_H
#define FUROS_UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int uart_getc(void);
int uart_rx_ready(void);

#endif
