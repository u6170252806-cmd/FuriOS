#include "timer.h"

static uint64_t tick_interval;
static uint64_t tick_base_count;

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(uint32_t hz) {
    uint64_t freq = read_cntfrq();
    if (hz == 0) {
        hz = 1;
    }
    tick_interval = freq / (uint64_t)hz;
    if (tick_interval == 0) {
        tick_interval = 1;
    }

    tick_base_count = read_cntpct();
    write_cntp_tval(tick_interval);
    write_cntp_ctl(1);
}

void timer_handle_irq(void) {
    write_cntp_tval(tick_interval);
}

uint64_t timer_ticks(void) {
    uint64_t now = read_cntpct();
    if (now < tick_base_count) {
        return 0;
    }
    return (now - tick_base_count) / tick_interval;
}
