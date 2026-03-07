#include "rtl8139.h"
#include "config.h"
#include "gic.h"
#include "pmm.h"
#include "print.h"
#include "string.h"
#include "uart.h"

#define RTL8139_VENDOR_ID        0x10ECU
#define RTL8139_DEVICE_ID        0x8139U
#define RTL8139_CLASS_NETWORK    0x02U

#define RTL8139_BAR_MEM          0x14U

#define RTL_REG_IDR0             0x00U
#define RTL_REG_MAR0             0x08U
#define RTL_REG_TSD0             0x10U
#define RTL_REG_TSAD0            0x20U
#define RTL_REG_RBSTART          0x30U
#define RTL_REG_CR               0x37U
#define RTL_REG_CAPR             0x38U
#define RTL_REG_CBR              0x3AU
#define RTL_REG_IMR              0x3CU
#define RTL_REG_ISR              0x3EU
#define RTL_REG_TCR              0x40U
#define RTL_REG_RCR              0x44U
#define RTL_REG_MPC              0x4CU
#define RTL_REG_9346CR           0x50U
#define RTL_REG_CONFIG1          0x52U

#define RTL_CMD_RX_EMPTY         0x01U
#define RTL_CMD_TX_ENABLE        0x04U
#define RTL_CMD_RX_ENABLE        0x08U
#define RTL_CMD_RESET            0x10U

#define RTL_ISR_ROK              0x0001U
#define RTL_ISR_RER              0x0002U
#define RTL_ISR_TOK              0x0004U
#define RTL_ISR_TER              0x0008U
#define RTL_ISR_RXOVW            0x0010U
#define RTL_ISR_LINKCHG          0x0020U
#define RTL_ISR_RXFIFO_OVER      0x0040U
#define RTL_ISR_LENCHG           0x2000U
#define RTL_ISR_SYSTEM_ERR       0x8000U

#define RTL_RCR_AAP              0x00000001U
#define RTL_RCR_APM              0x00000002U
#define RTL_RCR_AM               0x00000004U
#define RTL_RCR_AB               0x00000008U
#define RTL_RCR_WRAP             0x00000080U
#define RTL_RCR_MXDMA_UNLIMITED  (7U << 8)
#define RTL_RCR_RBLEN_32K        (2U << 11)

#define RTL_TCR_IFG96            (3U << 24)
#define RTL_TCR_MXDMA_2048       (6U << 8)

#define RTL_9346_UNLOCK          0xC0U
#define RTL_9346_LOCK            0x00U

#define RTL_TSD_OWN              (1U << 13)
#define RTL_TSD_TOK              (1U << 15)
#define RTL_TSD_TUN              (1U << 14)
#define RTL_TSD_TABT             (1U << 30)
#define RTL_TSD_CRS              (1U << 31)

#define RTL_RXSTAT_ROK           0x0001U

#define RTL_TX_RING_COUNT        4U
#define RTL_TX_BUF_SIZE          2048U
#define RTL_RX_RING_LEN          (32U * 1024U)
#define RTL_RX_RING_PAD          (16U + 1536U)
#define RTL_RX_ALLOC_SIZE        (RTL_RX_RING_LEN + RTL_RX_RING_PAD)

typedef struct {
    bool ready;
    uint32_t irq;
    volatile uint8_t *mmio;
    uint8_t mac[6];
    uint8_t *rx_buf;
    uint8_t *tx_buf[RTL_TX_RING_COUNT];
    bool tx_inflight[RTL_TX_RING_COUNT];
    uint32_t tx_prod;
    uint32_t rx_read;
} rtl8139_dev_t;

static rtl8139_dev_t g_dev;
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

static bool rtl8139_reset(void) {
    rtl_w8(RTL_REG_CR, RTL_CMD_RESET);
    for (uint32_t i = 0; i < 2000000U; i++) {
        if ((rtl_r8(RTL_REG_CR) & RTL_CMD_RESET) == 0U) {
            return true;
        }
    }
    return false;
}

static void rtl8139_unlock_config(void) {
    rtl_w8(RTL_REG_9346CR, RTL_9346_UNLOCK);
}

static void rtl8139_lock_config(void) {
    rtl_w8(RTL_REG_9346CR, RTL_9346_LOCK);
}

static bool rtl8139_setup_buffers(void) {
    g_dev.rx_buf = (uint8_t *)pmm_alloc(RTL_RX_ALLOC_SIZE, 256U);
    if (!g_dev.rx_buf) {
        return false;
    }
    memset(g_dev.rx_buf, 0, RTL_RX_ALLOC_SIZE);
    for (uint32_t i = 0; i < RTL_TX_RING_COUNT; i++) {
        g_dev.tx_buf[i] = (uint8_t *)pmm_alloc(RTL_TX_BUF_SIZE, 16U);
        if (!g_dev.tx_buf[i]) {
            return false;
        }
        memset(g_dev.tx_buf[i], 0, RTL_TX_BUF_SIZE);
        g_dev.tx_inflight[i] = false;
    }
    g_dev.tx_prod = 0U;
    g_dev.rx_read = 0U;
    return true;
}

static bool rtl8139_tx_slot_complete(uint32_t slot) {
    uint32_t tsd = rtl_r32(RTL_REG_TSD0 + (slot * 4U));
    return ((tsd & RTL_TSD_OWN) != 0U) ||
           ((tsd & RTL_TSD_TOK) != 0U) ||
           ((tsd & (RTL_TSD_TABT | RTL_TSD_TUN | RTL_TSD_CRS)) != 0U);
}

static void rtl8139_reap_tx(void) {
    for (uint32_t i = 0; i < RTL_TX_RING_COUNT; i++) {
        if (g_dev.tx_inflight[i] && rtl8139_tx_slot_complete(i)) {
            g_dev.tx_inflight[i] = false;
        }
    }
}

static bool rtl8139_hw_init(void) {
    if (!rtl8139_setup_buffers()) {
        return false;
    }

    rtl8139_unlock_config();
    rtl_w8(RTL_REG_CONFIG1, 0x00U);
    rtl8139_lock_config();

    if (!rtl8139_reset()) {
        return false;
    }

    rtl_w16(RTL_REG_IMR, 0U);
    rtl_w16(RTL_REG_ISR, 0xFFFFU);
    rtl_w32(RTL_REG_MPC, 0U);

    for (uint32_t i = 0; i < 6U; i++) {
        g_dev.mac[i] = rtl_r8(RTL_REG_IDR0 + i);
    }

    rtl_w32(RTL_REG_RBSTART, (uint32_t)(uintptr_t)g_dev.rx_buf);
    for (uint32_t i = 0; i < RTL_TX_RING_COUNT; i++) {
        rtl_w32(RTL_REG_TSAD0 + (i * 4U), (uint32_t)(uintptr_t)g_dev.tx_buf[i]);
    }
    rtl_w32(RTL_REG_MAR0, 0xFFFFFFFFU);
    rtl_w32(RTL_REG_MAR0 + 4U, 0xFFFFFFFFU);
    rtl_w32(RTL_REG_TCR, RTL_TCR_IFG96 | RTL_TCR_MXDMA_2048);
    rtl_w32(RTL_REG_RCR, RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM |
                             RTL_RCR_AB | RTL_RCR_WRAP |
                             RTL_RCR_MXDMA_UNLIMITED | RTL_RCR_RBLEN_32K);
    rtl_w16(RTL_REG_CAPR, 0xFFF0U);
    rtl_w8(RTL_REG_CR, RTL_CMD_RX_ENABLE | RTL_CMD_TX_ENABLE);
    rtl_w16(RTL_REG_IMR, RTL_ISR_ROK | RTL_ISR_RER | RTL_ISR_TOK | RTL_ISR_TER |
                              RTL_ISR_RXOVW | RTL_ISR_RXFIFO_OVER |
                              RTL_ISR_LINKCHG | RTL_ISR_LENCHG | RTL_ISR_SYSTEM_ERR);

    if (g_dev.irq) {
        gic_enable_irq(g_dev.irq, 0x90U);
    }
    return true;
}

void rtl8139_init(void) {
    memset(&g_dev, 0, sizeof(g_dev));
    g_pci_mmio_next = PCIE_MMIO_BASE + 0x03000000ULL;

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
                if (vendor != RTL8139_VENDOR_ID || device != RTL8139_DEVICE_ID ||
                    class_code != RTL8139_CLASS_NETWORK) {
                    continue;
                }

                uint64_t bar = 0U;
                if (!pci_assign_bar32((uint8_t)bus, dev, fn, RTL8139_BAR_MEM, &bar) || bar == 0U) {
                    continue;
                }

                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                cmd |= 0x0006U;
                cmd &= (uint16_t)~(1U << 10);
                pci_write16((uint8_t)bus, dev, fn, 0x04, cmd);

                g_dev.irq = pci_irq_for_device((uint8_t)bus, dev, fn);
                g_dev.mmio = (volatile uint8_t *)(uintptr_t)(bar & ~0xFULL);
                if (!rtl8139_hw_init()) {
                    memset(&g_dev, 0, sizeof(g_dev));
                    continue;
                }

                g_dev.ready = true;
                uart_puts("[rtl8139] ready irq=");
                print_dec((int)g_dev.irq);
                uart_puts("\n");
                return;
            }
        }
    }
}

bool rtl8139_ready(void) {
    return g_dev.ready;
}

const uint8_t *rtl8139_mac_addr(void) {
    return g_dev.ready ? g_dev.mac : 0;
}

bool rtl8139_handle_irq(uint32_t intid) {
    if (!g_dev.ready || !g_dev.irq || intid != g_dev.irq) {
        return false;
    }
    uint16_t isr = rtl_r16(RTL_REG_ISR);
    if (isr == 0U) {
        return false;
    }
    rtl_w16(RTL_REG_ISR, isr);
    rtl8139_reap_tx();
    return true;
}

void rtl8139_poll(void) {
    if (!g_dev.ready) {
        return;
    }
    rtl8139_reap_tx();
}

bool rtl8139_send_frame(const void *buf, size_t len) {
    if (!g_dev.ready || !buf || len < 14U || len > RTL_TX_BUF_SIZE) {
        return false;
    }

    rtl8139_reap_tx();
    uint32_t slot = g_dev.tx_prod;
    if (g_dev.tx_inflight[slot] && !rtl8139_tx_slot_complete(slot)) {
        return false;
    }

    size_t plen = len < 60U ? 60U : len;
    memcpy(g_dev.tx_buf[slot], buf, len);
    if (plen > len) {
        memset(g_dev.tx_buf[slot] + len, 0, plen - len);
    }
    mb();
    rtl_w32(RTL_REG_TSD0 + (slot * 4U), (uint32_t)plen);
    g_dev.tx_inflight[slot] = true;
    g_dev.tx_prod = (slot + 1U) % RTL_TX_RING_COUNT;
    return true;
}

int rtl8139_recv_frame(void *buf, size_t maxlen) {
    if (!g_dev.ready || !buf || maxlen == 0U) {
        return -1;
    }
    if ((rtl_r8(RTL_REG_CR) & RTL_CMD_RX_EMPTY) != 0U) {
        return 0;
    }

    uint32_t pos = g_dev.rx_read;
    uint8_t *pkt = g_dev.rx_buf + pos;
    uint16_t status = (uint16_t)pkt[0] | (uint16_t)((uint16_t)pkt[1] << 8);
    uint16_t frame_len = (uint16_t)pkt[2] | (uint16_t)((uint16_t)pkt[3] << 8);
    uint32_t copy_len = 0U;

    if ((status & RTL_RXSTAT_ROK) != 0U && frame_len >= 4U) {
        copy_len = (uint32_t)(frame_len - 4U);
        if (copy_len > maxlen) {
            copy_len = (uint32_t)maxlen;
        }
        memcpy(buf, pkt + 4U, copy_len);
    }

    uint32_t advance = ((uint32_t)frame_len + 4U + 3U) & ~3U;
    if (advance == 0U || advance > RTL_RX_ALLOC_SIZE) {
        g_dev.rx_read = 0U;
        rtl_w16(RTL_REG_CAPR, 0xFFF0U);
        return 0;
    }

    pos = (pos + advance) % RTL_RX_RING_LEN;
    g_dev.rx_read = pos;
    mb();
    rtl_w16(RTL_REG_CAPR, (uint16_t)((pos - 16U) & 0xFFFFU));
    return (int)copy_len;
}
