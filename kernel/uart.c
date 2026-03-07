#include "uart.h"
#include "config.h"
#include "gic.h"

#define UART_DR         0x00
#define UART_RSR_ECR    0x04
#define UART_FR         0x18
#define UART_IFLS       0x34
#define UART_IMSC       0x38
#define UART_MIS        0x40
#define UART_ICR        0x44

#define FR_TXFF         (1u << 5)
#define FR_RXFE         (1u << 4)

#define UART_IMSC_RXIM  (1u << 4)
#define UART_IMSC_RTIM  (1u << 6)
#define UART_ICR_RXIC   (1u << 4)
#define UART_ICR_RTIC   (1u << 6)
#define UART_ICR_ALL    0x07FFu

#define UART_RX_BUF_SIZE 131072u
#define UART_RX_BUF_MASK (UART_RX_BUF_SIZE - 1u)

static volatile uint32_t *const uart = (volatile uint32_t *)UART0_BASE;
static uint8_t rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

static inline uint32_t mmio_read32(uint32_t off) {
    return uart[off / 4];
}

static inline void mmio_write32(uint32_t off, uint32_t v) {
    uart[off / 4] = v;
}

static inline uint64_t irq_save(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}

static inline void irq_restore(uint64_t daif) {
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static void uart_drain_rx_locked(void) {
    while ((mmio_read32(UART_FR) & FR_RXFE) == 0u) {
        uint8_t c = (uint8_t)(mmio_read32(UART_DR) & 0xFFu);
        uint32_t next = (rx_head + 1u) & UART_RX_BUF_MASK;
        if (next == rx_tail) {
            break;
        }
        rx_buf[rx_head] = c;
        rx_head = next;
    }
    mmio_write32(UART_RSR_ECR, 0u);
    mmio_write32(UART_ICR, UART_ICR_RXIC | UART_ICR_RTIC);
}

static void uart_drain_rx(void) {
    uint64_t daif = irq_save();
    uart_drain_rx_locked();
    irq_restore(daif);
}

void uart_init(void) {
    rx_head = 0u;
    rx_tail = 0u;
    mmio_write32(UART_IMSC, 0u);
    mmio_write32(UART_IFLS, 0u);
    mmio_write32(UART_ICR, UART_ICR_ALL);
    mmio_write32(UART_RSR_ECR, 0u);
    uart_drain_rx();
}

void uart_enable_irq(void) {
    uint64_t daif = irq_save();
    mmio_write32(UART_ICR, UART_ICR_ALL);
    mmio_write32(UART_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);
    irq_restore(daif);
    gic_enable_irq(UART_IRQ, 0x70u);
}

void uart_putc(char c) {
    uart_drain_rx();
    while (mmio_read32(UART_FR) & FR_TXFF) {
        uart_drain_rx();
    }
    mmio_write32(UART_DR, (uint32_t)c);
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
    for (;;) {
        uint64_t daif = irq_save();
        uart_drain_rx_locked();
        if (rx_head != rx_tail) {
            uint8_t c = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1u) & UART_RX_BUF_MASK;
            irq_restore(daif);
            return (int)c;
        }
        irq_restore(daif);
    }
}

int uart_rx_ready(void) {
    uart_drain_rx();
    return rx_head != rx_tail;
}

int uart_handle_irq(uint32_t intid) {
    if (intid != UART_IRQ) {
        return 0;
    }
    uart_drain_rx();
    return 1;
}
