#include "rtl8125.h"
#include "config.h"
#include "gic.h"
#include "pmm.h"
#include "string.h"
#include "uart.h"
#include "print.h"

#define RTL_VENDOR_ID_REALTEK   0x10ECU
#define RTL_DEV_8125            0x8125U
#define RTL_DEV_8126            0x8126U
#define RTL_DEV_8127            0x8127U

#define RTL_CLASS_NETWORK       0x02U

#define RTL_BAR0                0x10U
#define RTL_BAR2                0x18U

#define RTL_TX_DESC_COUNT       64U
#define RTL_RX_DESC_COUNT       64U
#define RTL_TX_BUF_SIZE         2048U
#define RTL_RX_BUF_SIZE         2048U

#define RTL_REG_MAC0            0x00U
#define RTL_REG_TX_DESC_LO      0x20U
#define RTL_REG_TX_DESC_HI      0x24U
#define RTL_REG_CHIPCMD         0x37U
#define RTL_REG_INTR_MASK_8125  0x38U
#define RTL_REG_INTR_STAT_8125  0x3CU
#define RTL_REG_TX_CONFIG       0x40U
#define RTL_REG_RX_CONFIG       0x44U
#define RTL_REG_RX_MAX_SIZE     0xDAU
#define RTL_REG_RX_DESC_LO      0xE4U
#define RTL_REG_RX_DESC_HI      0xE8U
#define RTL_REG_TX_POLL_8125    0x90U

#define RTL_CMD_RESET           0x10U
#define RTL_CMD_RX_EN           0x08U
#define RTL_CMD_TX_EN           0x04U

#define RTL_INT_TX_OK           0x0004U
#define RTL_INT_RX_OK           0x0001U
#define RTL_INT_TX_ERR          0x0008U
#define RTL_INT_RX_ERR          0x0002U
#define RTL_INT_RX_OVERFLOW     0x0010U
#define RTL_INT_LINK_CHG        0x0020U

#define RTL_RXCFG_DMA_SHIFT     8U
#define RTL_TXCFG_DMA_SHIFT     8U
#define RTL_TXCFG_IFG_SHIFT     24U
#define RTL_RX_FETCH_DFLT_8125  (8U << 27)
#define RTL_RX_DMA_BURST        (7U << RTL_RXCFG_DMA_SHIFT)
#define RTL_TX_DMA_BURST        7U
#define RTL_INTER_FRAME_GAP     0x03U

#define RTL_ACCEPT_BROADCAST    0x08U
#define RTL_ACCEPT_MY_PHYS      0x02U

#define RTL_DESC_OWN            (1U << 31)
#define RTL_DESC_RING_END       (1U << 30)
#define RTL_DESC_FIRST_FRAG     (1U << 29)
#define RTL_DESC_LAST_FRAG      (1U << 28)
#define RTL_RX_DESC_ERR         (1U << 21)
#define RTL_DESC_LEN_MASK       0x3FFFU

typedef struct {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed)) rtl_desc_t;

typedef struct {
    bool ready;
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint32_t irq;
    volatile uint8_t *mmio;
    uint8_t mac[6];

    rtl_desc_t *tx_desc;
    rtl_desc_t *rx_desc;
    uint8_t *tx_buf[RTL_TX_DESC_COUNT];
    uint8_t *rx_buf[RTL_RX_DESC_COUNT];
    bool tx_busy[RTL_TX_DESC_COUNT];

    uint32_t tx_prod;
    uint32_t tx_dirty;
    uint32_t rx_cons;
} rtl8125_dev_t;

static rtl8125_dev_t g_dev;
static uint64_t g_pci_mmio_next;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1ULL) & ~(a - 1ULL);
}

static inline void mb(void) {
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline uintptr_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    return (uintptr_t)(PCIE_ECAM_BASE +
        ((uint64_t)bus << 20) +
        ((uint64_t)dev << 15) +
        ((uint64_t)fn << 12) +
        (uint64_t)(off & 0xFFFU));
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    volatile uint32_t *p = (volatile uint32_t *)pci_cfg_addr(bus, dev, fn, off);
    return *p;
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v) {
    volatile uint32_t *p = (volatile uint32_t *)pci_cfg_addr(bus, dev, fn, off);
    *p = v;
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    uint32_t v = pci_read32(bus, dev, fn, (uint16_t)(off & ~3U));
    return (uint16_t)((v >> ((off & 2U) * 8U)) & 0xFFFFU);
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint16_t val) {
    uint16_t base = (uint16_t)(off & ~3U);
    uint32_t shift = (off & 2U) * 8U;
    uint32_t v = pci_read32(bus, dev, fn, base);
    v &= ~(0xFFFFU << shift);
    v |= ((uint32_t)val << shift);
    pci_write32(bus, dev, fn, base, v);
}

static bool pci_assign_bar32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t bar_off,
                             uint64_t *bar_out) {
    uint32_t bar = pci_read32(bus, dev, fn, bar_off);
    if ((bar & 0x1U) != 0U) {
        return false;
    }
    if (((bar >> 1) & 0x3U) == 0x2U) {
        return false;
    }
    if ((bar & ~0xFU) != 0U) {
        *bar_out = (uint64_t)(bar & ~0xFU);
        return true;
    }

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, (uint16_t)(cmd & ~0x3U));
    pci_write32(bus, dev, fn, bar_off, 0xFFFFFFFFU);
    uint32_t mask = pci_read32(bus, dev, fn, bar_off);
    pci_write32(bus, dev, fn, bar_off, bar);
    pci_write16(bus, dev, fn, 0x04, cmd);
    if (mask == 0U || mask == 0xFFFFFFFFU) {
        return false;
    }

    uint32_t size_mask = mask & ~0xFU;
    uint32_t size = (~size_mask) + 1U;
    if (size == 0U) {
        return false;
    }
    uint64_t addr = align_up_u64(g_pci_mmio_next, size);
    if (addr + size > (uint64_t)PCIE_MMIO_BASE + (uint64_t)PCIE_MMIO_SIZE) {
        return false;
    }
    g_pci_mmio_next = addr + size;
    pci_write32(bus, dev, fn, bar_off, (uint32_t)addr);
    *bar_out = addr;
    return true;
}

static uint32_t pci_irq_route(uint8_t dev, uint8_t pin) {
    if (pin < 1U || pin > 4U) {
        return 0U;
    }
    uint32_t swz = ((uint32_t)dev + (uint32_t)(pin - 1U)) % PCIE_INTX_IRQ_COUNT;
    return PCIE_INTX_IRQ_BASE + swz;
}

static uint32_t pci_irq_for_device(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t il = pci_read32(bus, dev, fn, 0x3C);
    uint8_t line = (uint8_t)(il & 0xFFU);
    uint8_t pin = (uint8_t)((il >> 8) & 0xFFU);
    if (line != 0xFFU && line != 0U) {
        return (uint32_t)line;
    }
    return pci_irq_route(dev, pin);
}

static inline uint8_t rtl_r8(uint32_t off) {
    return *(volatile uint8_t *)(g_dev.mmio + off);
}

static inline uint16_t rtl_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_dev.mmio + off);
}

static inline uint32_t rtl_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_dev.mmio + off);
}

static inline void rtl_w8(uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(g_dev.mmio + off) = v;
}

static inline void rtl_w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(g_dev.mmio + off) = v;
}

static inline void rtl_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_dev.mmio + off) = v;
}

static bool rtl_is_supported(uint16_t vendor, uint16_t device, uint8_t class_code) {
    if (vendor != RTL_VENDOR_ID_REALTEK || class_code != RTL_CLASS_NETWORK) {
        return false;
    }
    return device == RTL_DEV_8125 || device == RTL_DEV_8126 || device == RTL_DEV_8127;
}

static bool rtl_hw_reset(void) {
    rtl_w8(RTL_REG_CHIPCMD, RTL_CMD_RESET);
    for (uint32_t i = 0; i < 2000000U; i++) {
        if ((rtl_r8(RTL_REG_CHIPCMD) & RTL_CMD_RESET) == 0U) {
            return true;
        }
    }
    return false;
}

static void rtl_reap_tx(void) {
    while (g_dev.tx_dirty != g_dev.tx_prod) {
        uint32_t e = g_dev.tx_dirty;
        uint32_t st = g_dev.tx_desc[e].opts1;
        if ((st & RTL_DESC_OWN) != 0U) {
            break;
        }
        g_dev.tx_busy[e] = false;
        g_dev.tx_dirty = (e + 1U) % RTL_TX_DESC_COUNT;
    }
}

static bool rtl_setup_rings(void) {
    g_dev.tx_desc = (rtl_desc_t *)pmm_alloc(sizeof(rtl_desc_t) * RTL_TX_DESC_COUNT, 256U);
    g_dev.rx_desc = (rtl_desc_t *)pmm_alloc(sizeof(rtl_desc_t) * RTL_RX_DESC_COUNT, 256U);
    if (!g_dev.tx_desc || !g_dev.rx_desc) {
        return false;
    }
    memset(g_dev.tx_desc, 0, sizeof(rtl_desc_t) * RTL_TX_DESC_COUNT);
    memset(g_dev.rx_desc, 0, sizeof(rtl_desc_t) * RTL_RX_DESC_COUNT);
    memset(g_dev.tx_busy, 0, sizeof(g_dev.tx_busy));
    g_dev.tx_prod = 0U;
    g_dev.tx_dirty = 0U;
    g_dev.rx_cons = 0U;

    for (uint32_t i = 0; i < RTL_TX_DESC_COUNT; i++) {
        g_dev.tx_buf[i] = (uint8_t *)pmm_alloc(RTL_TX_BUF_SIZE, 16U);
        if (!g_dev.tx_buf[i]) {
            return false;
        }
        memset(g_dev.tx_buf[i], 0, RTL_TX_BUF_SIZE);
    }
    for (uint32_t i = 0; i < RTL_RX_DESC_COUNT; i++) {
        g_dev.rx_buf[i] = (uint8_t *)pmm_alloc(RTL_RX_BUF_SIZE, 16U);
        if (!g_dev.rx_buf[i]) {
            return false;
        }
        memset(g_dev.rx_buf[i], 0, RTL_RX_BUF_SIZE);
        g_dev.rx_desc[i].addr = (uint64_t)(uintptr_t)g_dev.rx_buf[i];
        g_dev.rx_desc[i].opts2 = 0U;
        g_dev.rx_desc[i].opts1 = RTL_DESC_OWN | RTL_RX_BUF_SIZE;
        if (i == RTL_RX_DESC_COUNT - 1U) {
            g_dev.rx_desc[i].opts1 |= RTL_DESC_RING_END;
        }
    }
    return true;
}

static bool rtl_hw_init(void) {
    if (!rtl_setup_rings()) {
        return false;
    }
    if (!rtl_hw_reset()) {
        return false;
    }

    rtl_w32(RTL_REG_INTR_MASK_8125, 0U);
    rtl_w32(RTL_REG_INTR_STAT_8125, 0xFFFFFFFFU);

    rtl_w32(RTL_REG_TX_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)g_dev.tx_desc >> 32));
    rtl_w32(RTL_REG_TX_DESC_LO, (uint32_t)((uint64_t)(uintptr_t)g_dev.tx_desc & 0xFFFFFFFFULL));
    rtl_w32(RTL_REG_RX_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)g_dev.rx_desc >> 32));
    rtl_w32(RTL_REG_RX_DESC_LO, (uint32_t)((uint64_t)(uintptr_t)g_dev.rx_desc & 0xFFFFFFFFULL));

    rtl_w16(RTL_REG_RX_MAX_SIZE, (uint16_t)(RTL_RX_BUF_SIZE + 1U));
    rtl_w32(RTL_REG_TX_CONFIG,
            (RTL_TX_DMA_BURST << RTL_TXCFG_DMA_SHIFT) |
            (RTL_INTER_FRAME_GAP << RTL_TXCFG_IFG_SHIFT));
    rtl_w32(RTL_REG_RX_CONFIG,
            RTL_RX_FETCH_DFLT_8125 |
            RTL_RX_DMA_BURST |
            RTL_ACCEPT_BROADCAST |
            RTL_ACCEPT_MY_PHYS);

    for (uint32_t i = 0; i < 6U; i++) {
        g_dev.mac[i] = rtl_r8(RTL_REG_MAC0 + i);
    }

    rtl_w8(RTL_REG_CHIPCMD, RTL_CMD_TX_EN | RTL_CMD_RX_EN);
    rtl_w32(RTL_REG_INTR_MASK_8125,
            RTL_INT_RX_OK | RTL_INT_TX_OK | RTL_INT_RX_ERR |
            RTL_INT_TX_ERR | RTL_INT_RX_OVERFLOW | RTL_INT_LINK_CHG);

    if (g_dev.irq) {
        gic_enable_irq(g_dev.irq, 0x90U);
    }
    return true;
}

void rtl8125_init(void) {
    memset(&g_dev, 0, sizeof(g_dev));
    g_pci_mmio_next = PCIE_MMIO_BASE + 0x02000000ULL;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint8_t max_fn = 1U;
            for (uint8_t fn = 0; fn < max_fn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFU);
                uint16_t device = (uint16_t)(id >> 16);
                if (vendor == 0xFFFFU) {
                    if (fn == 0U) {
                        break;
                    }
                    continue;
                }
                if (fn == 0U) {
                    uint8_t hdr = (uint8_t)((pci_read32((uint8_t)bus, dev, fn, 0x0C) >> 16) & 0xFFU);
                    if ((hdr & 0x80U) != 0U) {
                        max_fn = 8U;
                    }
                }

                uint32_t cls = pci_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t class_code = (uint8_t)((cls >> 24) & 0xFFU);
                if (!rtl_is_supported(vendor, device, class_code)) {
                    continue;
                }

                uint64_t bar = 0U;
                if (!pci_assign_bar32((uint8_t)bus, dev, fn, RTL_BAR2, &bar)) {
                    if (!pci_assign_bar32((uint8_t)bus, dev, fn, RTL_BAR0, &bar)) {
                        continue;
                    }
                }

                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                cmd |= 0x0006U;
                cmd &= (uint16_t)~(1U << 10);
                pci_write16((uint8_t)bus, dev, fn, 0x04, cmd);

                g_dev.bus = (uint8_t)bus;
                g_dev.dev = dev;
                g_dev.fn = fn;
                g_dev.irq = pci_irq_for_device((uint8_t)bus, dev, fn);
                g_dev.mmio = (volatile uint8_t *)(uintptr_t)(bar & ~0xFULL);
                if (!rtl_hw_init()) {
                    memset(&g_dev, 0, sizeof(g_dev));
                    continue;
                }
                g_dev.ready = true;
                uart_puts("[rtl8125] ready irq=");
                print_dec((int)g_dev.irq);
                uart_puts("\n");
                return;
            }
        }
    }
}

bool rtl8125_ready(void) {
    return g_dev.ready;
}

const uint8_t *rtl8125_mac_addr(void) {
    return g_dev.ready ? g_dev.mac : 0;
}

bool rtl8125_handle_irq(uint32_t intid) {
    if (!g_dev.ready || !g_dev.irq || intid != g_dev.irq) {
        return false;
    }
    uint32_t st = rtl_r32(RTL_REG_INTR_STAT_8125);
    if (st == 0U) {
        return false;
    }
    rtl_w32(RTL_REG_INTR_STAT_8125, st);
    rtl_reap_tx();
    return true;
}

void rtl8125_poll(void) {
    if (!g_dev.ready) {
        return;
    }
    rtl_reap_tx();
}

bool rtl8125_send_frame(const void *buf, size_t len) {
    if (!g_dev.ready || !buf || len < 14U || len > RTL_TX_BUF_SIZE) {
        return false;
    }
    rtl_reap_tx();
    uint32_t e = g_dev.tx_prod;
    if (g_dev.tx_busy[e]) {
        return false;
    }

    size_t plen = len < 60U ? 60U : len;
    memcpy(g_dev.tx_buf[e], buf, len);
    if (plen > len) {
        memset(g_dev.tx_buf[e] + len, 0, plen - len);
    }

    g_dev.tx_desc[e].addr = (uint64_t)(uintptr_t)g_dev.tx_buf[e];
    g_dev.tx_desc[e].opts2 = 0U;

    uint32_t opts = RTL_DESC_OWN | RTL_DESC_FIRST_FRAG | RTL_DESC_LAST_FRAG | (uint32_t)plen;
    if (e == RTL_TX_DESC_COUNT - 1U) {
        opts |= RTL_DESC_RING_END;
    }
    mb();
    g_dev.tx_desc[e].opts1 = opts;
    g_dev.tx_busy[e] = true;
    g_dev.tx_prod = (e + 1U) % RTL_TX_DESC_COUNT;

    rtl_w16(RTL_REG_TX_POLL_8125, 1U);
    return true;
}

int rtl8125_recv_frame(void *buf, size_t maxlen) {
    if (!g_dev.ready || !buf || maxlen == 0U) {
        return -1;
    }
    uint32_t e = g_dev.rx_cons;
    uint32_t st = g_dev.rx_desc[e].opts1;
    if ((st & RTL_DESC_OWN) != 0U) {
        return 0;
    }

    uint32_t len = st & RTL_DESC_LEN_MASK;
    if ((st & RTL_RX_DESC_ERR) != 0U || len == 0U) {
        len = 0U;
    } else if (len >= 4U) {
        len -= 4U;
    }

    if (len > maxlen) {
        len = (uint32_t)maxlen;
    }
    if (len > 0U) {
        memcpy(buf, g_dev.rx_buf[e], len);
    }

    uint32_t eor = (e == RTL_RX_DESC_COUNT - 1U) ? RTL_DESC_RING_END : 0U;
    mb();
    g_dev.rx_desc[e].opts1 = RTL_DESC_OWN | eor | RTL_RX_BUF_SIZE;
    g_dev.rx_desc[e].opts2 = 0U;
    g_dev.rx_cons = (e + 1U) % RTL_RX_DESC_COUNT;
    return (int)len;
}
