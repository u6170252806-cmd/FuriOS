#include "gic.h"
#include "config.h"

#define GICD_CTLR            0x000
#define GICD_IGROUPR(n)      (0x080 + ((n) * 4))
#define GICD_ISENABLER(n)    (0x100 + ((n) * 4))
#define GICD_ICPENDR(n)      (0x280 + ((n) * 4))
#define GICD_IPRIORITYR(n)   (0x400 + (n))
#define GICD_ITARGETSR(n)    (0x800 + (n))

#define GICC_CTLR            0x000
#define GICC_PMR             0x004
#define GICC_IAR             0x00C
#define GICC_EOIR            0x010

#define GIC_SPURIOUS_IRQ     1023U

static volatile uint32_t *const gicd = (volatile uint32_t *)GICD_BASE;
static volatile uint32_t *const gicc = (volatile uint32_t *)GICC_BASE;

static inline void mmio_write32(volatile uint32_t *base, uint32_t off, uint32_t value) {
    base[off / 4] = value;
}

static inline uint32_t mmio_read32(volatile uint32_t *base, uint32_t off) {
    return base[off / 4];
}

static inline void mmio_write8(volatile uint32_t *base, uint32_t off, uint8_t value) {
    volatile uint8_t *b = (volatile uint8_t *)base;
    b[off] = value;
}

void gic_enable_irq(uint32_t intid, uint8_t priority) {
    uint32_t bank = intid / 32U;
    uint32_t bit = intid % 32U;
    uint32_t mask = (1U << bit);

    uint32_t ig = mmio_read32(gicd, GICD_IGROUPR(bank));
    ig |= mask;
    mmio_write32(gicd, GICD_IGROUPR(bank), ig);

    mmio_write32(gicd, GICD_ICPENDR(bank), mask);
    mmio_write32(gicd, GICD_ISENABLER(bank), mask);

    mmio_write8(gicd, GICD_IPRIORITYR(intid), priority);
    if (intid >= 32U) {
        mmio_write8(gicd, GICD_ITARGETSR(intid), 0x01);
    }
}

void gic_init(void) {
    mmio_write32(gicd, GICD_CTLR, 0);
    mmio_write32(gicc, GICC_CTLR, 0);

    mmio_write32(gicc, GICC_PMR, 0xFF);
    gic_enable_irq(TIMER_IRQ, 0x80);

    mmio_write32(gicd, GICD_CTLR, 1);
    mmio_write32(gicc, GICC_CTLR, 1);
}

uint32_t gic_ack_irq(void) {
    uint32_t iar = mmio_read32(gicc, GICC_IAR);
    uint32_t id = iar & 0x3FFU;
    if (id == GIC_SPURIOUS_IRQ) {
        return GIC_SPURIOUS_IRQ;
    }
    return iar;
}

void gic_eoi_irq(uint32_t iar) {
    uint32_t id = iar & 0x3FFU;
    if (id == GIC_SPURIOUS_IRQ) {
        return;
    }
    mmio_write32(gicc, GICC_EOIR, iar);
}
