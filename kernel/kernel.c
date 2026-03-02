#include "kernel.h"
#include "config.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "pmm.h"
#include "mmu.h"
#include "task.h"
#include "fs.h"
#include "gic.h"
#include "timer.h"
#include "pipe.h"
#include "pagecache.h"
#include "virtio_blk.h"
#include "ahci.h"
#include "nvme.h"
#include "block_cache.h"

void panic(const char *msg) {
    uart_puts("[panic] ");
    uart_puts(msg);
    uart_puts("\n");
    while (1) {
        __asm__ volatile("wfe");
    }
}

void trap_init(void) {
    uint64_t vbar = (uint64_t)vector_table;

    __asm__ volatile("msr vbar_el1, %0" :: "r"(vbar));
    __asm__ volatile("isb" ::: "memory");
}

static void print_boot_info(void) {
    uart_puts("FuriOS aarch64 boot\n");
    uart_puts("CurrentEL at entry: EL");
    print_dec((int)boot_current_el);
    uart_puts("\n");
}

static void enable_fp_simd(void) {
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3UL << 20); /* FPEN: enable FP/SIMD at EL0/EL1 */
    __asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
    __asm__ volatile("isb" ::: "memory");
}

void kernel_main(void) {
    uart_init();
    print_boot_info();
    enable_fp_simd();

    pmm_init((uint64_t)__heap_start, (uint64_t)__heap_end);

    mmu_init();
    trap_init();
    gic_init();
    timer_init(TIMER_HZ);

    task_init();
    pagecache_init();
    pipe_init();
    virtio_blk_init();
    ahci_init();
    nvme_init();
    block_cache_init();
    fs_init();

    inode_t *init_elf = fs_lookup("/bin/init");
    if (!init_elf) {
        panic("/bin/init missing");
    }

    task_t *init = task_create_user(init_elf->data, init_elf->size, 0);
    if (!init) {
        panic("failed to create init task");
    }

    const char *argv[] = {"/bin/init", 0};
    if (task_exec(init, init_elf->data, init_elf->size, argv) != 0) {
        panic("failed to exec init");
    }

    uart_puts("[kernel] entering EL0 init\n");
    task_enter_first(init);

    panic("returned from task_enter_first");
}
