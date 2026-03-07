#include "task.h"
#include "config.h"
#include "mmu.h"
#include "pmm.h"
#include "string.h"
#include "elf.h"
#include "pipe.h"
#include "print.h"
#include "syscall.h"
#include "uart.h"
#include "timer.h"
#include "pagecache.h"
#include "net.h"

extern void trap_enter_first(trapframe_t *tf, uint64_t ttbr0);

task_t tasks[MAX_TASKS];
static task_t *current_task_ptr;
static int next_pid = 1;
static int init_pid = -1;
static uint16_t next_asid = 1;
static uint32_t asid_epoch = 1;

#define STACK_GROW_SLOP 128UL
#define TASK_ASID_MIN 1U
#define TASK_ASID_MAX 255U
#define SIGBIT(sig) (1U << ((sig) & 31))
#define WAIT_STATUS_EXIT(code) (((code) & 0xFF) << 8)
#define WAIT_STATUS_SIG(sig) ((sig) & 0x7F)
#define WAIT_STATUS_STOP(sig) ((((sig) & 0xFF) << 8) | 0x7F)
#define WAIT_STATUS_CONT 0xFFFF
#define KERR_ESRCH (-3)
#define KERR_ECHILD (-10)
#define KERR_EINVAL (-22)

static void task_refresh_mem_accounting(task_t *t);
static bool task_deliver_pending_signal(task_t *t);
static bool task_signal_enqueue(task_t *t, int sig);
static void task_signal_clear_pending(task_t *t, int sig);
static void task_reset_signal_state_for_exec(task_t *t);
static void task_inherit_signal_state_for_fork(task_t *child, const task_t *parent);
static void task_debug_syms_clear(task_t *t);
static void task_debug_syms_load(task_t *t, const uint8_t *elf, size_t elf_size,
                                 const Elf64_Ehdr *eh);

typedef enum {
    TASK_CHLD_EXIT = 0,
    TASK_CHLD_STOP,
    TASK_CHLD_CONT,
} task_child_event_t;

static void task_set_comm_internal(task_t *task, const char *name) {
    if (!task) {
        return;
    }
    if (!name || name[0] == '\0') {
        task->comm[0] = '\0';
        return;
    }
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    if (base[0] == '\0') {
        base = name;
    }
    strncpy(task->comm, base, sizeof(task->comm) - 1U);
    task->comm[sizeof(task->comm) - 1U] = '\0';
}

static void task_debug_syms_clear(task_t *t) {
    if (!t) {
        return;
    }
    memset(t->debug_syms, 0, sizeof(t->debug_syms));
    t->debug_sym_count = 0U;
}

static void task_debug_syms_add(task_t *t, uint64_t start, uint64_t end, const char *name) {
    if (!t || !name || name[0] == '\0' || start < USER_VA_BASE || start >= USER_VA_LIMIT) {
        return;
    }
    if (end <= start) {
        end = start + 1U;
    }
    if (end > USER_VA_LIMIT) {
        end = USER_VA_LIMIT;
    }

    for (uint16_t i = 0; i < t->debug_sym_count; i++) {
        task_debug_sym_t *s = &t->debug_syms[i];
        if (!s->used) {
            continue;
        }
        if (s->start == start && strcmp(s->name, name) == 0) {
            if (end > s->end) {
                s->end = end;
            }
            return;
        }
    }

    if (t->debug_sym_count >= TASK_DEBUG_SYMS_MAX) {
        return;
    }

    task_debug_sym_t *slot = &t->debug_syms[t->debug_sym_count++];
    memset(slot, 0, sizeof(*slot));
    slot->start = start;
    slot->end = end;
    slot->used = true;
    strncpy(slot->name, name, sizeof(slot->name) - 1U);
    slot->name[sizeof(slot->name) - 1U] = '\0';
}

static void task_debug_syms_sort(task_t *t) {
    if (!t || t->debug_sym_count < 2U) {
        return;
    }
    for (uint16_t i = 1; i < t->debug_sym_count; i++) {
        task_debug_sym_t key = t->debug_syms[i];
        int j = (int)i - 1;
        while (j >= 0 && t->debug_syms[j].start > key.start) {
            t->debug_syms[j + 1] = t->debug_syms[j];
            j--;
        }
        t->debug_syms[j + 1] = key;
    }
}

static void task_debug_syms_load(task_t *t, const uint8_t *elf, size_t elf_size,
                                 const Elf64_Ehdr *eh) {
    if (!t || !elf || !eh) {
        return;
    }
    task_debug_syms_clear(t);

    if (eh->e_shoff == 0U || eh->e_shnum == 0U ||
        eh->e_shentsize != sizeof(Elf64_Shdr)) {
        return;
    }
    if (eh->e_shoff > elf_size ||
        (uint64_t)eh->e_shnum * eh->e_shentsize > elf_size - eh->e_shoff) {
        return;
    }
    if (eh->e_shnum > 1024U) {
        return;
    }

    const Elf64_Shdr *sh = (const Elf64_Shdr *)(const void *)(elf + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *symsh = &sh[i];
        if (symsh->sh_type != SHT_SYMTAB && symsh->sh_type != SHT_DYNSYM) {
            continue;
        }
        if (symsh->sh_link >= eh->e_shnum ||
            symsh->sh_entsize < sizeof(Elf64_Sym) ||
            symsh->sh_size < symsh->sh_entsize) {
            continue;
        }
        if (symsh->sh_offset > elf_size ||
            symsh->sh_size > elf_size - symsh->sh_offset) {
            continue;
        }

        const Elf64_Shdr *strsh = &sh[symsh->sh_link];
        if (strsh->sh_type != SHT_STRTAB ||
            strsh->sh_offset > elf_size ||
            strsh->sh_size > elf_size - strsh->sh_offset) {
            continue;
        }

        const char *strtab = (const char *)(const void *)(elf + strsh->sh_offset);
        const uint8_t *symbase = elf + symsh->sh_offset;
        uint64_t nsyms = symsh->sh_size / symsh->sh_entsize;
        for (uint64_t n = 0; n < nsyms; n++) {
            const Elf64_Sym *sym =
                (const Elf64_Sym *)(const void *)(symbase + n * symsh->sh_entsize);
            if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC ||
                sym->st_shndx == SHN_UNDEF ||
                sym->st_name >= strsh->sh_size) {
                continue;
            }
            const char *name = strtab + sym->st_name;
            if (!name[0]) {
                continue;
            }
            uint64_t start = sym->st_value;
            uint64_t end = start + sym->st_size;
            task_debug_syms_add(t, start, end, name);
            if (t->debug_sym_count >= TASK_DEBUG_SYMS_MAX) {
                break;
            }
        }
        if (t->debug_sym_count >= TASK_DEBUG_SYMS_MAX) {
            break;
        }
    }

    task_debug_syms_sort(t);
}

static uint32_t task_count_writable_intent_pages(const task_t *t) {
    uint32_t count = 0;
    if (!t) {
        return 0;
    }
    for (int i = 0; i < MAX_USER_PAGES; i++) {
        if (t->pages[i].used && t->pages[i].writable_intent) {
            count++;
        }
    }
    return count;
}

static bool task_fork_budget_ok(const task_t *parent) {
    if (!parent) {
        return false;
    }
    size_t avail = pmm_available_pages();
    size_t reserve = PMM_OOM_RESERVE_PAGES + FORK_MIN_HEADROOM_PAGES;
    if (avail <= reserve) {
        return false;
    }

    size_t need_now = (TASK_KSTACK_SIZE / PAGE_SIZE) + 4U;
    size_t cow_reserve = (size_t)task_count_writable_intent_pages(parent) / 2U;
    if (cow_reserve > FORK_MAX_COW_RESERVE_PAGES) {
        cow_reserve = FORK_MAX_COW_RESERVE_PAGES;
    }
    return avail > (need_now + cow_reserve + reserve);
}

static void task_asid_rollover(void) {
    mmu_tlb_flush_all();
    asid_epoch++;
    if (asid_epoch == 0) {
        asid_epoch = 1;
    }
    next_asid = TASK_ASID_MIN;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            tasks[i].asid_epoch = 0;
        }
    }

    if (current_task_ptr && current_task_ptr->state != TASK_UNUSED) {
        current_task_ptr->asid = next_asid++;
        current_task_ptr->asid_epoch = asid_epoch;
        mmu_switch_ttbr0(current_task_ptr->ttbr0_phys, current_task_ptr->asid);
    }
}

static uint16_t task_alloc_asid(void) {
    if (next_asid < TASK_ASID_MIN || next_asid > TASK_ASID_MAX) {
        task_asid_rollover();
    }
    uint16_t asid = next_asid;
    next_asid++;
    return asid;
}

static void task_ensure_asid(task_t *t) {
    if (!t) {
        return;
    }
    if (t->asid_epoch == asid_epoch) {
        return;
    }
    t->asid = task_alloc_asid();
    t->asid_epoch = asid_epoch;
}

static inline uint64_t align_down(uint64_t x, uint64_t a) {
    return x & ~(a - 1UL);
}

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1UL) & ~(a - 1UL);
}

static void task_fill_aux_random(const task_t *t, uint8_t out[16]) {
    if (!out) {
        return;
    }
    uint64_t seed = timer_ticks() ^
                    ((uint64_t)(t ? (uint32_t)t->pid : 0U) << 32) ^
                    (uint64_t)(uintptr_t)t;
    for (size_t i = 0; i < 16U; i++) {
        seed = seed * 6364136223846793005ULL + 1ULL;
        out[i] = (uint8_t)(seed >> 24);
    }
}

static trapframe_t *task_kstack_tf(task_t *t) {
    return (trapframe_t *)(t->kstack_top - sizeof(trapframe_t));
}

static void task_fd_ref(const fd_t *fd) {
    if (!fd || !fd->used) {
        return;
    }
    if (fd->kind == FD_PIPE_R) {
        if (!fd->pipe) {
            return;
        }
        pipe_ref_read(fd->pipe);
    } else if (fd->kind == FD_PIPE_W) {
        if (!fd->pipe) {
            return;
        }
        pipe_ref_write(fd->pipe);
    } else if (fd->kind == FD_SOCKET) {
        if (!fd->sock) {
            return;
        }
        net_socket_ref(fd->sock);
    }
}

static void task_fd_drop(const fd_t *fd) {
    if (!fd || !fd->used) {
        return;
    }
    if (fd->kind == FD_PIPE_R) {
        if (!fd->pipe) {
            return;
        }
        pipe_close_read(fd->pipe);
    } else if (fd->kind == FD_PIPE_W) {
        if (!fd->pipe) {
            return;
        }
        pipe_close_write(fd->pipe);
    } else if (fd->kind == FD_SOCKET) {
        if (!fd->sock) {
            return;
        }
        net_socket_close(fd->sock);
    }
}

static bool task_fd_needs_writeback(const fd_t *fd) {
    if (!fd || !fd->used || fd->kind != FD_INODE || !fd->inode) {
        return false;
    }
    if (fd->inode->type != INODE_FILE && !fs_is_block_dev(fd->inode)) {
        return false;
    }
    int mode = fd->flags & O_ACCMODE;
    return mode == O_WRONLY || mode == O_RDWR;
}

static uint64_t task_stack_floor(const task_t *t) {
    if (t && t->stack_base) {
        return t->stack_base;
    }
    return USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
}

static int task_fd_mode(int flags) {
    return flags & O_ACCMODE;
}

static bool task_vma_prot_valid(int prot) {
    return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static bool task_vma_flags_valid(int flags) {
    int map_kind = flags & (MAP_PRIVATE | MAP_SHARED);
    if (!(map_kind == MAP_PRIVATE || map_kind == MAP_SHARED)) {
        return false;
    }
    if (flags & ~(MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED)) {
        return false;
    }
    return true;
}

static vm_area_t *task_vma_slot(task_t *t) {
    if (!t) {
        return 0;
    }
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!t->vmas[i].used) {
            return &t->vmas[i];
        }
    }
    return 0;
}

static vm_area_t *task_vma_find(task_t *t, uint64_t va) {
    if (!t) {
        return 0;
    }
    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (v->used && va >= v->start && va < v->end) {
            return v;
        }
    }
    return 0;
}

static bool task_inode_same_file(const inode_t *a, const inode_t *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b || a->type != b->type || a->fs_kind != b->fs_kind) {
        return false;
    }
    if (a->type == INODE_DEV) {
        return a->dev_kind == b->dev_kind &&
               a->dev_lba_start == b->dev_lba_start &&
               a->dev_lba_count == b->dev_lba_count;
    }
    if (a->fs_kind == FS_KIND_EXT4 && a->fs_id != 0U && b->fs_id != 0U &&
        a->fs_ino != 0U && b->fs_ino != 0U) {
        return a->fs_id == b->fs_id && a->fs_ino == b->fs_ino;
    }
    return false;
}

static bool task_vma_overlaps(task_t *t, uint64_t start, uint64_t end) {
    if (!t || start >= end) {
        return true;
    }
    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (start < v->end && end > v->start) {
            return true;
        }
    }
    return false;
}

static bool task_vma_range_mapped(task_t *t, uint64_t start, uint64_t end) {
    if (!t || start >= end) {
        return false;
    }
    uint64_t cur = start;
    while (cur < end) {
        vm_area_t *v = task_vma_find(t, cur);
        if (!v || v->end <= cur) {
            return false;
        }
        cur = v->end < end ? v->end : end;
    }
    return true;
}

static void task_recompute_heap_limit(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t limit = task_stack_floor(t);
    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (v->start >= t->heap_end && v->start < limit) {
            limit = v->start;
        }
    }
    t->heap_limit = limit;
}

static void task_clear_vmas(task_t *t) {
    if (!t) {
        return;
    }
    memset(t->vmas, 0, sizeof(t->vmas));
    task_recompute_heap_limit(t);
}

static task_t *task_alloc(void) {
    if (pmm_available_pages() <= (TASK_KSTACK_SIZE / PAGE_SIZE) + PMM_OOM_RESERVE_PAGES) {
        return 0;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            memset(&tasks[i], 0, sizeof(task_t));
            tasks[i].kstack_base = (uint8_t *)pmm_alloc(TASK_KSTACK_SIZE, PAGE_SIZE);
            if (!tasks[i].kstack_base) {
                memset(&tasks[i], 0, sizeof(task_t));
                return 0;
            }
            tasks[i].kstack_top = (uint64_t)tasks[i].kstack_base + TASK_KSTACK_SIZE;
            tasks[i].tf = task_kstack_tf(&tasks[i]);
            memset(tasks[i].tf, 0, sizeof(*tasks[i].tf));
            tasks[i].pid = next_pid++;
            tasks[i].pgid = tasks[i].pid;
            tasks[i].asid = task_alloc_asid();
            tasks[i].asid_epoch = asid_epoch;
            if (tasks[i].asid == 0) {
                for (uint64_t p = (uint64_t)tasks[i].kstack_base; p < tasks[i].kstack_top; p += PAGE_SIZE) {
                    pmm_free_page((void *)p);
                }
                memset(&tasks[i], 0, sizeof(task_t));
                return 0;
            }
            tasks[i].state = TASK_RUNNABLE;
            tasks[i].wait_pid = -1;
            tasks[i].wait_kind = WAIT_NONE;
            tasks[i].wait_obj = 0;
            tasks[i].wake_tick = 0;
            task_set_comm_internal(&tasks[i], "task");
            for (int fd = 0; fd < 3; fd++) {
                tasks[i].fds[fd].used = true;
                tasks[i].fds[fd].kind = FD_NONE;
                tasks[i].fds[fd].inode = 0;
                tasks[i].fds[fd].pipe = 0;
                tasks[i].fds[fd].sock = 0;
                if (fd == 0) {
                    tasks[i].fds[fd].flags = O_RDONLY;
                } else {
                    tasks[i].fds[fd].flags = O_WRONLY;
                }
            }
            strcpy(tasks[i].cwd, "/");
            task_refresh_mem_accounting(&tasks[i]);
            return &tasks[i];
        }
    }
    return 0;
}

static bool task_pt_setup(task_t *t) {
    if (!t) {
        return false;
    }
    if (pmm_available_pages() <= 4U + PMM_OOM_RESERVE_PAGES) {
        return false;
    }
    t->l1 = (uint64_t *)pmm_alloc_page();
    t->l2_low = (uint64_t *)pmm_alloc_page();
    t->l3_user_a = (uint64_t *)pmm_alloc_page();
    t->l3_user_b = (uint64_t *)pmm_alloc_page();
    if (!t->l1 || !t->l2_low || !t->l3_user_a || !t->l3_user_b) {
        return false;
    }

    memset(t->l1, 0, PAGE_SIZE);
    memset(t->l2_low, 0, PAGE_SIZE);
    memset(t->l3_user_a, 0, PAGE_SIZE);
    memset(t->l3_user_b, 0, PAGE_SIZE);

    uint64_t normal = PTE_AF | PTE_SH_INNER | PTE_AP_EL1_RW | PTE_ATTR_NORMAL;
    uint64_t device = PTE_AF | PTE_AP_EL1_RW | PTE_ATTR_DEVICE | PTE_UXN | PTE_PXN;

    t->l1[0] = mmu_make_table((uint64_t)t->l2_low);
    t->l1[1] = mmu_make_block_l1(0x40000000UL, normal);

    t->l2_low[0x08000000UL >> 21] = mmu_make_block_l2(0x08000000UL, device);
    t->l2_low[0x09000000UL >> 21] = mmu_make_block_l2(0x09000000UL, device);
    t->l2_low[0x0A000000UL >> 21] = mmu_make_block_l2(0x0A000000UL, device);
    {
        uint64_t start = PCIE_MMIO_BASE & ~((uint64_t)(0x200000 - 1));
        uint64_t end = (PCIE_MMIO_BASE + PCIE_MMIO_SIZE + 0x1FFFFFUL) & ~((uint64_t)(0x200000 - 1));
        for (uint64_t pa = start; pa < end; pa += 0x200000ULL) {
            t->l2_low[pa >> 21] = mmu_make_block_l2(pa, device);
        }
    }
    t->l1[PCIE_ECAM_BASE >> 30] = mmu_make_block_l1(PCIE_ECAM_BASE & 0x0000FFFFC0000000UL, device);

    uint64_t idx_a = USER_VA_BASE >> 21;
    uint64_t idx_b = (USER_VA_LIMIT - 1) >> 21;
    t->l2_low[idx_a] = mmu_make_table((uint64_t)t->l3_user_a);
    t->l2_low[idx_b] = mmu_make_table((uint64_t)t->l3_user_b);

    t->ttbr0_phys = (uint64_t)t->l1;
    task_refresh_mem_accounting(t);
    return true;
}

static uint64_t *task_l3_for_va(task_t *t, uint64_t va) {
    uint64_t l2_idx = va >> 21;
    if (l2_idx == (USER_VA_BASE >> 21)) {
        return t->l3_user_a;
    }
    if (l2_idx == ((USER_VA_LIMIT - 1) >> 21)) {
        return t->l3_user_b;
    }
    return 0;
}

static uint64_t task_user_attrs_prot(int prot, bool cow_tag) {
    uint64_t attrs = PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL;
    if (prot & PROT_WRITE) {
        attrs |= PTE_AP_EL0_RW;
    } else if (prot & (PROT_READ | PROT_EXEC)) {
        attrs |= PTE_AP_EL0_RO;
    } else {
        attrs |= PTE_AP_EL1_RW;
    }
    attrs |= PTE_PXN;
    if ((prot & PROT_EXEC) == 0) {
        attrs |= PTE_UXN;
    }
    if (cow_tag) {
        attrs |= PTE_SW_COW;
    }
    return attrs;
}

static uint64_t task_user_attrs(bool pte_writable, bool executable, bool cow_tag) {
    int prot = PROT_READ;
    if (pte_writable) {
        prot |= PROT_WRITE;
    }
    if (executable) {
        prot |= PROT_EXEC;
    }
    return task_user_attrs_prot(prot, cow_tag);
}

static bool task_set_user_pte(task_t *t, uint64_t va, uint64_t pa, uint64_t attrs) {
    if (va < USER_VA_BASE || va >= USER_VA_LIMIT || (va & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    uint64_t *l3 = task_l3_for_va(t, va);
    if (!l3) {
        return false;
    }
    uint64_t l3_idx = (va >> 12) & 0x1FF;
    l3[l3_idx] = mmu_make_page(pa, attrs);
    return true;
}

static bool task_get_user_pte(task_t *t, uint64_t va, uint64_t *pte_out) {
    if (!pte_out || va < USER_VA_BASE || va >= USER_VA_LIMIT || (va & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    uint64_t *l3 = task_l3_for_va(t, va);
    if (!l3) {
        return false;
    }
    uint64_t l3_idx = (va >> 12) & 0x1FF;
    *pte_out = l3[l3_idx];
    return true;
}

static bool task_pte_is_user_page(uint64_t pte) {
    if ((pte & PTE_VALID) == 0) {
        return false;
    }
    if ((pte & PTE_PAGE) == 0) {
        return false;
    }
    return true;
}

static bool task_pte_is_user_writable(uint64_t pte) {
    uint64_t ap = pte & (3UL << 6);
    return ap == PTE_AP_EL0_RW;
}

static bool task_pte_is_user_executable(uint64_t pte) {
    return (pte & PTE_UXN) == 0;
}

static bool task_pte_is_cow(uint64_t pte) {
    return (pte & PTE_SW_COW) != 0;
}

static bool task_inode_under_mount(const inode_t *inode, const inode_t *mountpoint) {
    const inode_t *cur = inode;
    while (cur) {
        if (cur == mountpoint) {
            return true;
        }
        cur = cur->parent;
    }
    return false;
}

static bool task_path_under_mount(const char *path, const char *mount_path) {
    size_t mlen;
    if (!path || !mount_path || mount_path[0] != '/') {
        return false;
    }
    if (strcmp(mount_path, "/") == 0) {
        return true;
    }
    mlen = strlen(mount_path);
    if (mlen == 0 || strncmp(path, mount_path, mlen) != 0) {
        return false;
    }
    return path[mlen] == '\0' || path[mlen] == '/';
}

static bool task_page_validate_hw(task_t *t, const user_page_t *up, uint64_t *pte_out,
                                  bool *hw_writable, bool *hw_executable, bool *hw_cow) {
    if (!t || !up || !up->used) {
        return false;
    }

    uint64_t pte = 0;
    if (!task_get_user_pte(t, up->va, &pte) || !task_pte_is_user_page(pte)) {
        return false;
    }

    uint64_t pa = pte & PTE_ADDR_MASK;
    bool pte_wr = task_pte_is_user_writable(pte);
    bool pte_x = task_pte_is_user_executable(pte);
    bool pte_cow = task_pte_is_cow(pte);

    if (pa != up->pa) {
        return false;
    }
    if (!up->writable_intent && (pte_wr || pte_cow)) {
        return false;
    }
    if (pte_cow && pte_wr) {
        return false;
    }
    if (pte_cow && !up->writable_intent) {
        return false;
    }

    if (pte_out) {
        *pte_out = pte;
    }
    if (hw_writable) {
        *hw_writable = pte_wr;
    }
    if (hw_executable) {
        *hw_executable = pte_x;
    }
    if (hw_cow) {
        *hw_cow = pte_cow;
    }
    return true;
}

static void task_refresh_mem_accounting(task_t *t) {
    if (!t) {
        return;
    }

    uint32_t user_pages = 0;
    uint32_t cow_pages = 0;
    uint32_t private_pages = 0;
    for (int i = 0; i < MAX_USER_PAGES; i++) {
        user_page_t *up = &t->pages[i];
        if (!up->used) {
            continue;
        }

        bool hw_writable = false;
        bool hw_executable = false;
        bool hw_cow = false;
        if (!task_page_validate_hw(t, up, 0, &hw_writable, &hw_executable, &hw_cow)) {
            continue;
        }
        (void)hw_writable;
        (void)hw_executable;

        user_pages++;
        if (hw_cow) {
            cow_pages++;
        }
        if (pmm_page_refcount((void *)up->pa) <= 1U) {
            private_pages++;
        }
    }

    uint32_t fixed_pages = 0;
    if (t->kstack_base) {
        fixed_pages += (uint32_t)(TASK_KSTACK_SIZE / PAGE_SIZE);
    }
    if (t->l1) {
        fixed_pages++;
    }
    if (t->l2_low) {
        fixed_pages++;
    }
    if (t->l3_user_a) {
        fixed_pages++;
    }
    if (t->l3_user_b) {
        fixed_pages++;
    }

    t->user_page_count = user_pages;
    t->cow_page_count = cow_pages;
    t->private_page_count = private_pages;
    t->mem_charge_pages = fixed_pages + private_pages;
}

static bool task_track_page(task_t *t, uint64_t va, uint64_t pa,
                            bool writable_intent) {
    for (int i = 0; i < MAX_USER_PAGES; i++) {
        if (!t->pages[i].used) {
            t->pages[i].used = true;
            t->pages[i].va = va;
            t->pages[i].pa = pa;
            t->pages[i].writable_intent = writable_intent;
            return true;
        }
    }
    return false;
}

static user_page_t *task_find_page(task_t *t, uint64_t va_page) {
    for (int i = 0; i < MAX_USER_PAGES; i++) {
        if (t->pages[i].used && t->pages[i].va == va_page) {
            return &t->pages[i];
        }
    }
    return 0;
}

static bool task_map_user_page(task_t *t, uint64_t va, uint64_t pa, bool pte_writable,
                               bool executable, bool pte_cow, bool writable_intent) {
    if (task_find_page(t, va)) {
        return false;
    }
    uint64_t attrs = task_user_attrs(pte_writable, executable, pte_cow);
    if (!task_set_user_pte(t, va, pa, attrs)) {
        return false;
    }
    if (!task_track_page(t, va, pa, writable_intent)) {
        uint64_t *l3 = task_l3_for_va(t, va);
        if (l3) {
            uint64_t l3_idx = (va >> 12) & 0x1FF;
            l3[l3_idx] = 0;
        }
        return false;
    }
    task_refresh_mem_accounting(t);
    return true;
}

static bool task_map_user_anon_fault_page(task_t *t, uint64_t va, bool writable, bool executable) {
    if (!t || pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
        return false;
    }
    void *page = pmm_alloc_page();
    if (!page) {
        return false;
    }
    if (!task_map_user_page(t, va, (uint64_t)page, writable, executable, false, writable)) {
        pmm_free_page(page);
        return false;
    }
    mmu_tlb_flush_va(va, t->asid);
    return true;
}

static bool task_vma_writeback_page(task_t *t, const vm_area_t *v, const user_page_t *up) {
    if (!t || !v || !up || !v->used || !v->inode) {
        return true;
    }
    if ((v->flags & MAP_SHARED) == 0) {
        return true;
    }
    inode_t *ino = v->inode;
    if ((ino->type != INODE_FILE && !fs_is_block_dev(ino)) || !ino->writable) {
        return true;
    }
    if (up->va < v->start || up->va >= v->end) {
        return false;
    }
    bool hw_writable = false;
    bool hw_executable = false;
    bool hw_cow = false;
    if (!task_page_validate_hw(t, up, 0, &hw_writable, &hw_executable, &hw_cow)) {
        return false;
    }
    (void)hw_executable;
    if (hw_cow) {
        return false;
    }
    if (!hw_writable) {
        return true;
    }

    uint64_t map_off = up->va - v->start;
    uint64_t file_off = v->file_offset + map_off;
    size_t max_bytes = (size_t)(v->end - up->va);
    if (max_bytes > PAGE_SIZE) {
        max_bytes = PAGE_SIZE;
    }
    return pagecache_writeback(ino, file_off, up->pa, max_bytes);
}

static bool task_map_user_file_fault_page(task_t *t, const vm_area_t *v, uint64_t va,
                                          bool writable, bool executable) {
    if (!t || !v || !v->used || !v->inode) {
        return false;
    }
    uint64_t map_off = va - v->start;
    uint64_t file_off = v->file_offset + map_off;
    bool map_shared = (v->flags & MAP_SHARED) != 0;
    if (map_shared) {
        uint64_t pa = 0;
        if (!pagecache_get_or_create(v->inode, file_off, &pa)) {
            return false;
        }
        if (!task_map_user_page(t, va, pa, false, executable, false, writable)) {
            pmm_free_page((void *)pa);
            return false;
        }
        mmu_tlb_flush_va(va, t->asid);
        return true;
    }

    if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
        return false;
    }

    void *page = pmm_alloc_page();
    if (!page) {
        return false;
    }

    uint64_t cached_pa = 0;
    if (!pagecache_get_or_create(v->inode, file_off, &cached_pa)) {
        pmm_free_page(page);
        return false;
    }
    memcpy(page, (const void *)(uintptr_t)cached_pa, PAGE_SIZE);
    pmm_free_page((void *)(uintptr_t)cached_pa);

    if (!task_map_user_page(t, va, (uint64_t)page, writable, executable, false, writable)) {
        pmm_free_page(page);
        return false;
    }
    mmu_tlb_flush_va(va, t->asid);
    return true;
}

static bool task_unmap_user_page(task_t *t, uint64_t va_page) {
    user_page_t *up = task_find_page(t, va_page);
    if (!up) {
        return false;
    }
    if (!task_page_validate_hw(t, up, 0, 0, 0, 0)) {
        return false;
    }
    uint64_t *l3 = task_l3_for_va(t, va_page);
    if (!l3) {
        return false;
    }

    vm_area_t *v = task_vma_find(t, va_page);
    if (v && !task_vma_writeback_page(t, v, up)) {
        return false;
    }

    uint64_t l3_idx = (va_page >> 12) & 0x1FF;
    l3[l3_idx] = 0;
    mmu_tlb_flush_va(va_page, t->asid);
    pmm_free_page((void *)up->pa);
    memset(up, 0, sizeof(*up));
    task_refresh_mem_accounting(t);
    return true;
}

static void task_unmap_user(task_t *t) {
    for (int i = 0; i < MAX_USER_PAGES; i++) {
        if (t->pages[i].used) {
            vm_area_t *v = task_vma_find(t, t->pages[i].va);
            (void)task_vma_writeback_page(t, v, &t->pages[i]);
            pmm_free_page((void *)t->pages[i].pa);
        }
    }
    memset(t->pages, 0, sizeof(t->pages));

    if (t->l3_user_a) {
        memset(t->l3_user_a, 0, PAGE_SIZE);
    }
    if (t->l3_user_b) {
        memset(t->l3_user_b, 0, PAGE_SIZE);
    }
    if (t->asid != 0) {
        mmu_tlb_flush_asid(t->asid);
    }
    task_refresh_mem_accounting(t);
}

static void task_release(task_t *t) {
    if (!t) {
        return;
    }

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (t->fds[fd].used) {
            task_fd_close(t, fd);
        }
    }

    task_unmap_user(t);
    task_clear_vmas(t);

    if (t->l1) {
        pmm_free_page(t->l1);
        t->l1 = 0;
    }
    if (t->l2_low) {
        pmm_free_page(t->l2_low);
        t->l2_low = 0;
    }
    if (t->l3_user_a) {
        pmm_free_page(t->l3_user_a);
        t->l3_user_a = 0;
    }
    if (t->l3_user_b) {
        pmm_free_page(t->l3_user_b);
        t->l3_user_b = 0;
    }
    if (t->kstack_base) {
        for (uint64_t p = (uint64_t)t->kstack_base; p < t->kstack_top; p += PAGE_SIZE) {
            pmm_free_page((void *)p);
        }
        t->kstack_base = 0;
        t->kstack_top = 0;
        t->tf = 0;
    }
    t->ttbr0_phys = 0;
}

static void task_reap_slot(task_t *t) {
    if (t && t->pid == init_pid) {
        init_pid = -1;
    }
    task_release(t);
    memset(t, 0, sizeof(*t));
    t->state = TASK_UNUSED;
}

static task_t *task_find_live_init(void) {
    if (init_pid <= 0) {
        return 0;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid != init_pid) {
            continue;
        }
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_ZOMBIE) {
            return 0;
        }
        return &tasks[i];
    }
    return 0;
}

static bool task_wait_matches_filter(const task_t *parent, const task_t *child, int filter) {
    if (!parent || !child || child->ppid != parent->pid) {
        return false;
    }
    if (filter > 0) {
        return child->pid == filter;
    }
    if (filter == -1) {
        return true;
    }
    if (filter == 0) {
        return child->pgid == parent->pgid;
    }
    return child->pgid == -filter;
}

static task_t *task_parent_of(const task_t *child) {
    if (!child || child->ppid <= 0) {
        return 0;
    }
    return task_by_pid(child->ppid);
}

static void task_signal_queue_remove_index(task_t *t, uint8_t idx) {
    if (!t || idx >= t->signal_queue_count) {
        return;
    }
    for (uint8_t i = (uint8_t)(idx + 1U); i < t->signal_queue_count; i++) {
        t->signal_queue[i - 1U] = t->signal_queue[i];
    }
    t->signal_queue_count--;
}

static bool task_signal_enqueue(task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= 32) {
        return false;
    }
    if (sig == SIGCONT) {
        task_signal_clear_pending(t, SIGSTOP);
    } else if (sig == SIGSTOP) {
        task_signal_clear_pending(t, SIGCONT);
    }

    uint32_t bit = SIGBIT(sig);
    if ((t->pending_signals & bit) != 0U) {
        return true;
    }

    t->pending_signals |= bit;
    if (t->signal_queue_count < TASK_SIGNAL_QUEUE_LEN) {
        t->signal_queue[t->signal_queue_count++] = (uint8_t)sig;
    }
    return true;
}

static void task_signal_clear_pending(task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= 32) {
        return;
    }

    t->pending_signals &= ~SIGBIT(sig);
    for (uint8_t i = 0; i < t->signal_queue_count;) {
        if (t->signal_queue[i] == (uint8_t)sig) {
            task_signal_queue_remove_index(t, i);
            continue;
        }
        i++;
    }
}

static void task_notify_sigchld(const task_t *child, task_child_event_t ev) {
    task_t *parent = task_parent_of(child);
    if (!parent || parent->state == TASK_UNUSED || parent->state == TASK_ZOMBIE) {
        return;
    }
    if ((ev == TASK_CHLD_STOP || ev == TASK_CHLD_CONT) &&
        (parent->signal_flags[SIGCHLD] & SA_NOCLDSTOP) != 0U) {
        return;
    }
    (void)task_signal_enqueue(parent, SIGCHLD);
}

static bool task_sigchld_auto_reap(const task_t *parent) {
    if (!parent || parent->state == TASK_UNUSED || parent->state == TASK_ZOMBIE) {
        return false;
    }
    if (parent->signal_handler[SIGCHLD] == SIG_IGN) {
        return true;
    }
    return (parent->signal_flags[SIGCHLD] & SA_NOCLDWAIT) != 0U;
}

static void task_reap_sigchld_zombies(task_t *parent) {
    if (!task_sigchld_auto_reap(parent)) {
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *child = &tasks[i];
        if (child->state != TASK_ZOMBIE || child->ppid != parent->pid) {
            continue;
        }
        task_reap_slot(child);
    }
}

static void task_reparent_children(int old_ppid) {
    if (old_ppid <= 0) {
        return;
    }
    task_t *init = task_find_live_init();
    int new_ppid = init ? init->pid : -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED || child->ppid != old_ppid) {
            continue;
        }
        child->ppid = new_ppid;
        if (new_ppid <= 0 || !init) {
            continue;
        }
        if (child->state == TASK_ZOMBIE) {
            if (task_sigchld_auto_reap(init)) {
                task_reap_slot(child);
                continue;
            }
            task_notify_sigchld(child, TASK_CHLD_EXIT);
            task_wake_waiters(child->pid);
            continue;
        }
        if (child->state == TASK_STOPPED && child->stop_report_pending) {
            task_notify_sigchld(child, TASK_CHLD_STOP);
            task_wake_waiters(child->pid);
            continue;
        }
        if (child->cont_report_pending) {
            task_notify_sigchld(child, TASK_CHLD_CONT);
            task_wake_waiters(child->pid);
        }
    }
}

static void task_force_exit(task_t *victim, int code) {
    if (!victim || victim->state == TASK_UNUSED || victim->state == TASK_ZOMBIE) {
        return;
    }
    task_t *parent = task_parent_of(victim);
    bool auto_reap = task_sigchld_auto_reap(parent);
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (victim->fds[fd].used) {
            task_fd_close(victim, fd);
        }
    }
    victim->exit_code = code;
    victim->state = TASK_ZOMBIE;
    victim->wait_pid = -1;
    victim->wait_kind = WAIT_NONE;
    victim->wait_obj = 0;
    victim->wake_tick = 0;
    victim->pending_signals = 0;
    victim->signal_queue_count = 0U;
    victim->signal_mask = 0;
    victim->signal_active = false;
    victim->signal_active_num = 0;
    victim->signal_saved_mask = 0;
    victim->stop_report_pending = false;
    victim->cont_report_pending = false;
    victim->stop_signal = 0U;
    task_notify_sigchld(victim, TASK_CHLD_EXIT);
    task_reparent_children(victim->pid);
    task_wake_waiters(victim->pid);
    if (auto_reap) {
        task_reap_slot(victim);
    }
}

static bool task_parent_alive(const task_t *t) {
    if (!t || t->ppid <= 0) {
        return false;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == t->ppid &&
            tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_ZOMBIE) {
            return true;
        }
    }
    return false;
}

static void task_reap_orphan_zombies(const task_t *exclude) {
    if (task_find_live_init()) {
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t == exclude) {
            continue;
        }
        if (t->state == TASK_ZOMBIE && !task_parent_alive(t)) {
            task_reap_slot(t);
        }
    }
}

static bool task_parent_has_child_sigchld_event(const task_t *parent) {
    if (!parent) {
        return false;
    }
    bool nocldstop = (parent->signal_flags[SIGCHLD] & SA_NOCLDSTOP) != 0U;
    for (int i = 0; i < MAX_TASKS; i++) {
        const task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED || child->ppid != parent->pid) {
            continue;
        }
        if (child->state == TASK_ZOMBIE ||
            (!nocldstop && child->state == TASK_STOPPED && child->stop_report_pending) ||
            (!nocldstop && child->cont_report_pending)) {
            return true;
        }
    }
    return false;
}

static void task_reset_signal_state_for_exec(task_t *t) {
    if (!t) {
        return;
    }

    t->pending_signals = 0U;
    t->signal_queue_count = 0U;
    memset(t->signal_queue, 0, sizeof(t->signal_queue));
    t->signal_active = false;
    t->signal_active_num = 0U;
    t->signal_saved_mask = 0U;
    memset(&t->signal_saved_tf, 0, sizeof(t->signal_saved_tf));

    for (int sig = 1; sig < 32; sig++) {
        if (t->signal_handler[sig] == SIG_IGN) {
            t->signal_flags[sig] = 0U;
            t->signal_restorer[sig] = 0U;
            t->signal_action_mask[sig] = 0U;
            continue;
        }
        t->signal_handler[sig] = SIG_DFL;
        t->signal_flags[sig] = 0U;
        t->signal_restorer[sig] = 0U;
        t->signal_action_mask[sig] = 0U;
    }

    t->signal_mask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
}

static void task_inherit_signal_state_for_fork(task_t *child, const task_t *parent) {
    if (!child || !parent) {
        return;
    }

    child->signal_mask = parent->signal_mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    memcpy(child->signal_handler, parent->signal_handler, sizeof(child->signal_handler));
    memcpy(child->signal_restorer, parent->signal_restorer, sizeof(child->signal_restorer));
    memcpy(child->signal_flags, parent->signal_flags, sizeof(child->signal_flags));
    memcpy(child->signal_action_mask, parent->signal_action_mask, sizeof(child->signal_action_mask));

    child->pending_signals = 0U;
    child->signal_queue_count = 0U;
    memset(child->signal_queue, 0, sizeof(child->signal_queue));
    child->signal_active = false;
    child->signal_active_num = 0U;
    child->signal_saved_mask = 0U;
    memset(&child->signal_saved_tf, 0, sizeof(child->signal_saved_tf));
}

static bool task_translate(task_t *t, uint64_t va, uint64_t *pa_out) {
    uint64_t va_page = align_down(va, PAGE_SIZE);
    user_page_t *up = task_find_page(t, va_page);
    if (!up) {
        return false;
    }
    *pa_out = up->pa + (va - va_page);
    return true;
}

static bool task_copy_to_user_va(task_t *t, uint64_t dst_va, const void *src, size_t len) {
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++) {
        uint64_t pa;
        if (!task_translate(t, dst_va + i, &pa)) {
            return false;
        }
        *(uint8_t *)pa = s[i];
    }
    return true;
}

static bool task_validate_elf_loads(const Elf64_Phdr *ph, uint16_t phnum, uint16_t phentsize,
                                    size_t elf_size, uint64_t *max_load_end_out,
                                    size_t *mapped_pages_out) {
    if (!ph || phentsize != sizeof(Elf64_Phdr) || !max_load_end_out || !mapped_pages_out) {
        return false;
    }

    enum {
        USER_EXEC_PAGE_SLOTS = (USER_VA_LIMIT - USER_VA_BASE) / PAGE_SIZE
    };
    bool seen[USER_EXEC_PAGE_SLOTS];
    memset(seen, 0, sizeof(seen));

    uint64_t max_load_end = USER_VA_BASE;
    size_t mapped_pages = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0U) {
            continue;
        }
        if (ph[i].p_filesz > ph[i].p_memsz) {
            return false;
        }
        if (ph[i].p_vaddr < USER_VA_BASE) {
            return false;
        }
        if (ph[i].p_vaddr > USER_VA_LIMIT - ph[i].p_memsz) {
            return false;
        }
        if (ph[i].p_offset > (uint64_t)elf_size) {
            return false;
        }
        if (ph[i].p_filesz > (uint64_t)elf_size - ph[i].p_offset) {
            return false;
        }

        uint64_t start = align_down(ph[i].p_vaddr, PAGE_SIZE);
        uint64_t end = align_up(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);
        if (end > USER_VA_LIMIT || end < start) {
            return false;
        }
        if (end > max_load_end) {
            max_load_end = end;
        }

        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            size_t idx = (size_t)((va - USER_VA_BASE) / PAGE_SIZE);
            if (idx >= USER_EXEC_PAGE_SLOTS) {
                return false;
            }
            if (!seen[idx]) {
                seen[idx] = true;
                mapped_pages++;
            }
        }
    }

    if (mapped_pages == 0U) {
        return false;
    }
    *max_load_end_out = max_load_end;
    *mapped_pages_out = mapped_pages;
    return true;
}

static uint64_t task_elf_phdr_user_addr(const Elf64_Ehdr *eh, const Elf64_Phdr *ph, size_t elf_size) {
    if (!eh || !ph || eh->e_phoff >= elf_size) {
        return 0;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == 6U) { /* PT_PHDR */
            return ph[i].p_vaddr;
        }
    }

    uint64_t phoff = eh->e_phoff;
    uint64_t phsize = (uint64_t)eh->e_phnum * eh->e_phentsize;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0U) {
            continue;
        }
        if (ph[i].p_offset > phoff) {
            continue;
        }
        if (phoff + phsize > ph[i].p_offset + ph[i].p_filesz) {
            continue;
        }
        return ph[i].p_vaddr + (phoff - ph[i].p_offset);
    }
    return 0;
}

static bool task_load_elf(task_t *t, const uint8_t *elf, size_t elf_size,
                          const char *const argv[]) {
    if (elf_size < sizeof(Elf64_Ehdr)) {
        return false;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_machine != EM_AARCH64 ||
        eh->e_type != ET_EXEC) {
        return false;
    }

    if (eh->e_phoff > elf_size ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > elf_size - eh->e_phoff) {
        return false;
    }
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(elf + eh->e_phoff);
    uint64_t max_load_end = USER_VA_BASE;
    size_t mapped_pages = 0;
    if (!task_validate_elf_loads(ph, eh->e_phnum, eh->e_phentsize, elf_size,
                                 &max_load_end, &mapped_pages)) {
        return false;
    }

    size_t needed_pages = mapped_pages + 1U;
    if (needed_pages > MAX_USER_PAGES ||
        pmm_available_pages() <= PMM_OOM_RESERVE_PAGES + needed_pages) {
        return false;
    }
    task_load_debug_symbols(t, elf, elf_size);

    task_unmap_user(t);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        if (ph[i].p_memsz == 0) {
            continue;
        }

        uint64_t start = align_down(ph[i].p_vaddr, PAGE_SIZE);
        uint64_t end = align_up(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);
        bool writable = (ph[i].p_flags & PF_W) != 0;
        bool executable = (ph[i].p_flags & PF_X) != 0;

        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            user_page_t *existing = task_find_page(t, va);
            if (existing) {
                bool cur_w = false;
                bool cur_x = false;
                bool cur_cow = false;
                if (!task_page_validate_hw(t, existing, 0, &cur_w, &cur_x, &cur_cow)) {
                    return false;
                }
                bool merged_w = cur_w || writable;
                bool merged_x = cur_x || executable;
                bool merged_cow = cur_cow && !merged_w;
                if (!task_set_user_pte(t, va, existing->pa,
                                       task_user_attrs(merged_w, merged_x, merged_cow))) {
                    return false;
                }
                existing->writable_intent = existing->writable_intent || writable;
                mmu_tlb_flush_va(va, t->asid);
                continue;
            }
            if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
                return false;
            }
            void *page = pmm_alloc_page();
            if (!page) {
                return false;
            }
            if (!task_map_user_page(t, va, (uint64_t)page, writable, executable, false, writable)) {
                return false;
            }
        }

        if (ph[i].p_filesz) {
            if (!task_copy_to_user_va(t, ph[i].p_vaddr, elf + ph[i].p_offset, ph[i].p_filesz)) {
                return false;
            }
        }

        uint64_t bss_start = ph[i].p_vaddr + ph[i].p_filesz;
        uint64_t bss_len = ph[i].p_memsz - ph[i].p_filesz;
        uint8_t zero = 0;
        for (uint64_t z = 0; z < bss_len; z++) {
            if (!task_copy_to_user_va(t, bss_start + z, &zero, 1)) {
                return false;
            }
        }
    }

    /* Some static linkers emit overlapping PT_LOAD segments where entry lands
       in a page also covered by a non-exec segment. Ensure entry page remains
       executable if it is mapped at all. */
    uint64_t entry_page = align_down(eh->e_entry, PAGE_SIZE);
    user_page_t *entry_up = task_find_page(t, entry_page);
    if (!entry_up) {
        return false;
    }
    bool entry_w = false;
    bool entry_x = false;
    bool entry_cow = false;
    if (!task_page_validate_hw(t, entry_up, 0, &entry_w, &entry_x, &entry_cow)) {
        return false;
    }
    if (!entry_x) {
        if (!task_set_user_pte(t, entry_page, entry_up->pa,
                               task_user_attrs(entry_w, true, entry_cow))) {
            return false;
        }
        mmu_tlb_flush_va(entry_page, t->asid);
    }

    uint64_t stack_base = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    uint64_t stack_top_page = USER_STACK_TOP - PAGE_SIZE;
    {
        if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
            return false;
        }
        void *page = pmm_alloc_page();
        if (!page) {
            return false;
        }
        if (!task_map_user_page(t, stack_top_page, (uint64_t)page, true, false, false, true)) {
            pmm_free_page(page);
            return false;
        }
    }

    t->stack_base = stack_base;
    t->stack_mapped_bottom = stack_top_page;
    t->heap_base = align_up(max_load_end, PAGE_SIZE);
    t->heap_end = t->heap_base;
    task_recompute_heap_limit(t);
    if (t->heap_base > t->heap_limit) {
        return false;
    }

    char kargv[MAX_ARGV][MAX_ARG_LEN];
    int argc = 0;
    if (argv) {
        while (argc < MAX_ARGV && argv[argc]) {
            strncpy(kargv[argc], argv[argc], MAX_ARG_LEN - 1);
            kargv[argc][MAX_ARG_LEN - 1] = '\0';
            argc++;
        }
    }

    uint64_t sp = USER_STACK_TOP;
    uint64_t arg_ptrs[MAX_ARGV + 1];
    enum { TASK_AUXV_CAP = 24 };
    Elf64_auxv_t auxv[TASK_AUXV_CAP];
    size_t auxc = 0U;

    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(kargv[i]) + 1;
        sp -= n;
        if (!task_copy_to_user_va(t, sp, kargv[i], n)) {
            return false;
        }
        arg_ptrs[i] = sp;
    }
    arg_ptrs[argc] = 0;

    uint8_t aux_random[16];
    task_fill_aux_random(t, aux_random);
    sp -= sizeof(aux_random);
    if (!task_copy_to_user_va(t, sp, aux_random, sizeof(aux_random))) {
        return false;
    }
    uint64_t aux_random_user = sp;

    uint64_t phdr_addr = task_elf_phdr_user_addr(eh, ph, elf_size);

    auxv[auxc++] = (Elf64_auxv_t){AT_PHDR, phdr_addr};
    auxv[auxc++] = (Elf64_auxv_t){AT_PHENT, eh->e_phentsize};
    auxv[auxc++] = (Elf64_auxv_t){AT_PHNUM, eh->e_phnum};
    auxv[auxc++] = (Elf64_auxv_t){AT_PAGESZ, PAGE_SIZE};
    auxv[auxc++] = (Elf64_auxv_t){AT_BASE, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_ENTRY, eh->e_entry};
    auxv[auxc++] = (Elf64_auxv_t){AT_UID, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_EUID, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_GID, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_EGID, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_HWCAP, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_HWCAP2, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_CLKTCK, TIMER_HZ};
    auxv[auxc++] = (Elf64_auxv_t){AT_SECURE, 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_RANDOM, aux_random_user};
    auxv[auxc++] = (Elf64_auxv_t){AT_EXECFN, (argc > 0) ? arg_ptrs[0] : 0U};
    auxv[auxc++] = (Elf64_auxv_t){AT_NULL, 0U};

    sp &= ~0xFUL;
    size_t stack_words = 1U + (size_t)argc + 1U + 1U + (auxc * 2U);
    size_t stack_bytes = stack_words * sizeof(uint64_t);
    sp -= (uint64_t)stack_bytes;

    uint64_t frame[1 + MAX_ARGV + 1 + 1 + (TASK_AUXV_CAP * 2)];
    size_t wi = 0U;
    frame[wi++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        frame[wi++] = arg_ptrs[i];
    }
    frame[wi++] = 0U; /* argv terminator */
    frame[wi++] = 0U; /* envp terminator (no env yet) */
    for (size_t i = 0; i < auxc; i++) {
        frame[wi++] = auxv[i].a_type;
        frame[wi++] = auxv[i].a_val;
    }
    if (wi != stack_words) {
        return false;
    }

    if (!task_copy_to_user_va(t, sp, frame, stack_bytes)) {
        return false;
    }

    if (!t->tf) {
        return false;
    }

    memset(t->tf, 0, sizeof(*t->tf));
    t->tf->elr_el1 = eh->e_entry;
    t->tf->sp_el0 = sp;
    t->tf->spsr_el1 = 0;
    t->tf->x[0] = (uint64_t)argc;
    t->tf->x[1] = sp + sizeof(uint64_t);
    t->tf->x[2] = sp + sizeof(uint64_t) * (2U + (uint64_t)argc);
    t->tf->x[3] = sp + sizeof(uint64_t) * (3U + (uint64_t)argc);

    task_refresh_mem_accounting(t);
    return true;
}

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    current_task_ptr = 0;
    next_pid = 1;
    init_pid = -1;
    next_asid = TASK_ASID_MIN;
    asid_epoch = 1;
}

task_t *task_current(void) {
    return current_task_ptr;
}

task_t *task_by_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return 0;
}

int task_exec(task_t *task, const uint8_t *elf, size_t elf_size, const char *const argv[]) {
    if (!task) {
        return -1;
    }
    if (!task_load_elf(task, elf, elf_size, argv)) {
        return -1;
    }
    task_reset_signal_state_for_exec(task);
    if (argv && argv[0]) {
        task_set_comm_internal(task, argv[0]);
    }
    return 0;
}

task_t *task_create_user(const uint8_t *elf, size_t elf_size, int ppid) {
    task_t *t = task_alloc();
    if (!t) {
        return 0;
    }
    t->ppid = ppid;
    if (ppid > 0) {
        task_t *parent = task_by_pid(ppid);
        if (parent) {
            t->pgid = parent->pgid;
        }
    }
    if (ppid <= 0 && init_pid <= 0) {
        init_pid = t->pid;
    }
    if (!task_pt_setup(t)) {
        task_reap_slot(t);
        return 0;
    }
    if (task_exec(t, elf, elf_size, 0) != 0) {
        task_reap_slot(t);
        return 0;
    }
    if (ppid <= 0 && init_pid == t->pid) {
        task_set_comm_internal(t, "init");
    }
    return t;
}

static task_t *task_next_runnable(task_t *from) {
    int start = 0;
    if (from) {
        start = (int)(from - tasks) + 1;
    }

    for (int pass = 0; pass < 2; pass++) {
        int begin = pass == 0 ? start : 0;
        int end = pass == 0 ? MAX_TASKS : start;
        for (int i = begin; i < end; i++) {
            if (tasks[i].state == TASK_RUNNABLE) {
                return &tasks[i];
            }
        }
    }
    return 0;
}

trapframe_t *task_schedule_from_trap(trapframe_t *current_tf, bool force_resched) {
    if (current_task_ptr && current_tf) {
        current_task_ptr->tf = current_tf;
        if (current_task_ptr->state == TASK_RUNNING) {
            current_task_ptr->state = TASK_RUNNABLE;
        }
    }
    task_reap_orphan_zombies(current_task_ptr);

    task_t *next = 0;
    if (!force_resched && current_task_ptr && current_task_ptr->state == TASK_RUNNABLE) {
        next = current_task_ptr;
    }
    if (!next) {
        next = task_next_runnable(current_task_ptr);
    }
    if (!next && current_task_ptr && current_task_ptr->state == TASK_RUNNABLE) {
        next = current_task_ptr;
    }
    while (!next) {
        if (task_wake_sleepers(timer_ticks())) {
            next = task_next_runnable(current_task_ptr);
            if (!next && current_task_ptr && current_task_ptr->state == TASK_RUNNABLE) {
                next = current_task_ptr;
            }
        }
        if (!next) {
            __asm__ volatile("yield");
        }
    }

    while (next) {
        if (!task_deliver_pending_signal(next) ||
            next->state == TASK_ZOMBIE ||
            next->state == TASK_STOPPED ||
            next->state == TASK_WAITING) {
            task_t *from = next;
            next = task_next_runnable(from);
            if (!next && current_task_ptr && current_task_ptr->state == TASK_RUNNABLE) {
                next = current_task_ptr;
            }
            continue;
        }
        break;
    }

    while (!next) {
        if (task_wake_sleepers(timer_ticks())) {
            next = task_next_runnable(current_task_ptr);
            if (!next && current_task_ptr && current_task_ptr->state == TASK_RUNNABLE) {
                next = current_task_ptr;
            }
        }
        if (!next) {
            __asm__ volatile("yield");
        }
    }

    current_task_ptr = next;
    task_ensure_asid(next);
    next->state = TASK_RUNNING;
    mmu_switch_ttbr0(next->ttbr0_phys, next->asid);
    return next->tf;
}

void task_enter_first(task_t *task) {
    current_task_ptr = task;
    task_ensure_asid(task);
    task->state = TASK_RUNNING;
    trap_enter_first(task->tf, mmu_ttbr0_value(task->ttbr0_phys, task->asid));
}

void task_set_syscall_ret(task_t *task, uint64_t value) {
    if (task && task->tf) {
        task->tf->x[0] = value;
    }
}

int task_fork(const trapframe_t *parent_tf) {
    task_t *parent = current_task_ptr;
    if (!parent || !parent_tf) {
        return -1;
    }
    if (!task_fork_budget_ok(parent)) {
        return -1;
    }

    task_t *child = task_alloc();
    if (!child) {
        return -1;
    }
    child->ppid = parent->pid;
    child->pgid = parent->pgid;

    if (!task_pt_setup(child)) {
        task_reap_slot(child);
        return -1;
    }

    for (int i = 0; i < MAX_USER_PAGES; i++) {
        user_page_t *pp = &parent->pages[i];
        if (!pp->used) {
            continue;
        }

        uint64_t va = pp->va;
        bool hw_writable = false;
        bool hw_executable = false;
        bool hw_cow = false;
        if (!task_page_validate_hw(parent, pp, 0, &hw_writable, &hw_executable, &hw_cow)) {
            task_reap_slot(child);
            return -1;
        }

        uint64_t pa = pp->pa;
        bool writable_intent = pp->writable_intent;
        vm_area_t *vma = task_vma_find(parent, va);
        bool map_shared = vma && ((vma->flags & MAP_SHARED) != 0);
        if (!writable_intent && hw_cow) {
            task_reap_slot(child);
            return -1;
        }
        if (hw_cow && hw_writable) {
            task_reap_slot(child);
            return -1;
        }
        if (writable_intent && !hw_cow && !hw_writable) {
            task_reap_slot(child);
            return -1;
        }
        if (writable_intent && hw_cow && hw_writable) {
            task_reap_slot(child);
            return -1;
        }

        if (map_shared) {
            if (hw_cow) {
                task_reap_slot(child);
                return -1;
            }
            pmm_ref_page((void *)pa);
            if (!task_map_user_page(child, va, pa, hw_writable, hw_executable, false,
                                    writable_intent)) {
                pmm_free_page((void *)pa);
                task_reap_slot(child);
                return -1;
            }
            continue;
        }

        pmm_ref_page((void *)pa);
        if (!task_map_user_page(child, va, pa, false, hw_executable, writable_intent, writable_intent)) {
            pmm_free_page((void *)pa);
            task_reap_slot(child);
            return -1;
        }

        if (writable_intent && !hw_cow) {
            if (!task_set_user_pte(parent, va, pa, task_user_attrs(false, hw_executable, true))) {
                task_reap_slot(child);
                return -1;
            }
            mmu_tlb_flush_va(va, parent->asid);
        }
    }
    child->heap_base = parent->heap_base;
    child->heap_end = parent->heap_end;
    child->heap_limit = parent->heap_limit;
    child->stack_base = parent->stack_base;
    child->stack_mapped_bottom = parent->stack_mapped_bottom;
    memcpy(child->vmas, parent->vmas, sizeof(child->vmas));

    if (!child->tf) {
        task_reap_slot(child);
        return -1;
    }
    *child->tf = *parent_tf;
    child->tf->x[0] = 0;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        child->fds[fd] = parent->fds[fd];
        if (child->fds[fd].used) {
            task_fd_ref(&child->fds[fd]);
        }
    }
    strcpy(child->cwd, parent->cwd);
    strncpy(child->comm, parent->comm, sizeof(child->comm) - 1U);
    child->comm[sizeof(child->comm) - 1U] = '\0';
    task_inherit_signal_state_for_fork(child, parent);

    child->state = TASK_RUNNABLE;
    task_refresh_mem_accounting(parent);
    task_refresh_mem_accounting(child);
    return child->pid;
}

void task_mark_exit(int code) {
    task_t *self = current_task_ptr;
    if (!self) {
        return;
    }
    task_force_exit(self, WAIT_STATUS_EXIT(code));
}

void task_wake_waiters(int child_pid) {
    task_t *child = task_by_pid(child_pid);
    if (!child) {
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *waiter = &tasks[i];
        if (waiter->state != TASK_WAITING || waiter->wait_kind != WAIT_CHILD) {
            continue;
        }
        if (!task_wait_matches_filter(waiter, child, waiter->wait_pid)) {
            continue;
        }
        waiter->state = TASK_RUNNABLE;
        waiter->wait_pid = -1;
        waiter->wait_kind = WAIT_NONE;
        waiter->wait_obj = 0;
        waiter->wake_tick = 0;
    }
}

int task_wait(int pid, int options, int *status_out) {
    task_t *self = current_task_ptr;
    if (!self) {
        return -1;
    }
    if ((options & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0) {
        return KERR_EINVAL;
    }

    bool has_child = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_UNUSED) {
            continue;
        }
        if (!task_wait_matches_filter(self, t, pid)) {
            continue;
        }
        has_child = true;
        if (t->state == TASK_ZOMBIE) {
            int ret_pid = t->pid;
            if (status_out) {
                *status_out = t->exit_code;
            }
            task_reap_slot(t);
            if (!task_parent_has_child_sigchld_event(self)) {
                task_signal_clear_pending(self, SIGCHLD);
            }
            return ret_pid;
        }
        if ((options & WUNTRACED) &&
            t->state == TASK_STOPPED &&
            t->stop_report_pending) {
            int stop_sig = t->stop_signal ? (int)t->stop_signal : SIGSTOP;
            t->stop_report_pending = false;
            if (status_out) {
                *status_out = WAIT_STATUS_STOP(stop_sig);
            }
            if (!task_parent_has_child_sigchld_event(self)) {
                task_signal_clear_pending(self, SIGCHLD);
            }
            return t->pid;
        }
        if ((options & WCONTINUED) && t->cont_report_pending) {
            t->cont_report_pending = false;
            if (status_out) {
                *status_out = WAIT_STATUS_CONT;
            }
            if (!task_parent_has_child_sigchld_event(self)) {
                task_signal_clear_pending(self, SIGCHLD);
            }
            return t->pid;
        }
    }

    if (!has_child) {
        task_signal_clear_pending(self, SIGCHLD);
        return KERR_ECHILD;
    }
    if (options & WNOHANG) {
        return 0;
    }

    self->state = TASK_WAITING;
    self->wait_pid = pid;
    self->wait_kind = WAIT_CHILD;
    self->wait_obj = 0;
    self->wake_tick = 0;
    return -2;
}

static bool task_group_exists(int pgid) {
    if (pgid <= 0) {
        return false;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pgid == pgid) {
            return true;
        }
    }
    return false;
}

int task_setpgid(int pid, int pgid) {
    task_t *self = current_task_ptr;
    if (!self || pid < 0 || pgid < 0) {
        return -1;
    }

    if (pid == 0) {
        pid = self->pid;
    }

    task_t *target = task_by_pid(pid);
    if (!target || target->state == TASK_UNUSED || target->state == TASK_ZOMBIE) {
        return -1;
    }
    if (target != self && target->ppid != self->pid) {
        return -1;
    }

    if (pgid == 0) {
        pgid = target->pid;
    }
    if (pgid != target->pid && !task_group_exists(pgid)) {
        return -1;
    }
    target->pgid = pgid;
    return 0;
}

int task_getpgid(int pid) {
    task_t *self = current_task_ptr;
    if (!self || pid < 0) {
        return -1;
    }
    if (pid == 0) {
        pid = self->pid;
    }
    task_t *target = task_by_pid(pid);
    if (!target || target->state == TASK_UNUSED) {
        return -1;
    }
    return target->pgid;
}

static bool task_signal_supported(int sig) {
    return sig == 0 || sig == SIGHUP || sig == SIGINT || sig == SIGQUIT ||
           sig == SIGPIPE || sig == SIGTERM || sig == SIGKILL || sig == SIGSTOP ||
           sig == SIGCONT || sig == SIGCHLD;
}

typedef enum {
    TASK_SIG_DFL_TERM = 0,
    TASK_SIG_DFL_IGNORE,
    TASK_SIG_DFL_STOP,
    TASK_SIG_DFL_CONT,
} task_sig_default_action_t;

static task_sig_default_action_t task_signal_default_action(int sig) {
    switch (sig) {
        case SIGCHLD:
            return TASK_SIG_DFL_IGNORE;
        case SIGSTOP:
            return TASK_SIG_DFL_STOP;
        case SIGCONT:
            return TASK_SIG_DFL_CONT;
        case SIGHUP:
        case SIGINT:
        case SIGPIPE:
        case SIGQUIT:
        case SIGTERM:
        case SIGKILL:
        default:
            return TASK_SIG_DFL_TERM;
    }
}

static bool task_signal_blocked(const task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= 32) {
        return false;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return false;
    }
    return (t->signal_mask & SIGBIT(sig)) != 0U;
}

static bool task_signal_may_restart_wait(wait_kind_t kind) {
    return kind == WAIT_PIPE_READ || kind == WAIT_PIPE_WRITE || kind == WAIT_CHILD;
}

static void task_signal_wake_waiter(task_t *victim, bool handler_restart) {
    if (!victim || victim->state != TASK_WAITING) {
        return;
    }
    wait_kind_t wait_kind = victim->wait_kind;
    victim->state = TASK_RUNNABLE;
    victim->wait_pid = -1;
    victim->wait_kind = WAIT_NONE;
    victim->wait_obj = 0;
    victim->wake_tick = 0;
    bool restart_ok = handler_restart && task_signal_may_restart_wait(wait_kind);
    if (victim->tf && (int64_t)victim->tf->x[0] == -2 && !restart_ok) {
        victim->tf->x[0] = (uint64_t)-4;
    }
}

static int task_signal_one(task_t *self, task_t *victim, int sig) {
    uint64_t handler = SIG_DFL;
    uint32_t flags = 0U;

    if (!victim || victim->state == TASK_UNUSED) {
        return 0;
    }
    if (victim->pid == init_pid && self->pid != init_pid) {
        return 0;
    }
    if (sig == 0 || victim->state == TASK_ZOMBIE) {
        return 1;
    }
    if (sig > 0 && sig < 32) {
        handler = victim->signal_handler[sig];
        flags = victim->signal_flags[sig];
    }

    if (sig == SIGKILL) {
        task_force_exit(victim, WAIT_STATUS_SIG(sig));
        return 1;
    }

    if (handler == SIG_IGN && sig != SIGSTOP && sig != SIGKILL) {
        return 1;
    }

    (void)task_signal_enqueue(victim, sig);

    if (task_signal_blocked(victim, sig)) {
        return 1;
    }

    if (sig == SIGCONT) {
        task_signal_clear_pending(victim, SIGSTOP);
        if (victim->state == TASK_STOPPED) {
            victim->state = TASK_RUNNABLE;
            victim->cont_report_pending = true;
            victim->stop_report_pending = false;
            victim->stop_signal = 0U;
            task_notify_sigchld(victim, TASK_CHLD_CONT);
            task_wake_waiters(victim->pid);
        }
        task_signal_wake_waiter(victim, true);
        return 1;
    }

    if (sig == SIGSTOP) {
        task_signal_clear_pending(victim, SIGCONT);
        if (victim->state != TASK_STOPPED) {
            victim->state = TASK_STOPPED;
            victim->wait_pid = -1;
            victim->wait_kind = WAIT_NONE;
            victim->wait_obj = 0;
            victim->wake_tick = 0;
            victim->cont_report_pending = false;
            victim->stop_report_pending = true;
            victim->stop_signal = (uint8_t)sig;
            task_notify_sigchld(victim, TASK_CHLD_STOP);
            task_wake_waiters(victim->pid);
        }
        return 1;
    }

    bool restart_ok = (handler != SIG_DFL && (flags & SA_RESTART) != 0U);
    task_signal_wake_waiter(victim, restart_ok);
    return 1;
}

static int task_pick_pending_unblocked(task_t *t) {
    if (!t) {
        return 0;
    }

    for (uint8_t i = 0U; i < t->signal_queue_count;) {
        int sig = (int)t->signal_queue[i];
        if (sig <= 0 || sig >= 32 || (t->pending_signals & SIGBIT(sig)) == 0U) {
            task_signal_queue_remove_index(t, i);
            continue;
        }
        if (task_signal_blocked(t, sig)) {
            i++;
            continue;
        }
        task_signal_queue_remove_index(t, i);
        t->pending_signals &= ~SIGBIT(sig);
        return sig;
    }

    for (int sig = 1; sig < 32; sig++) {
        if ((t->pending_signals & SIGBIT(sig)) == 0U) {
            continue;
        }
        if (task_signal_blocked(t, sig)) {
            continue;
        }
        t->pending_signals &= ~SIGBIT(sig);
        return sig;
    }
    return 0;
}

static bool task_deliver_pending_signal(task_t *t) {
    if (!t || !t->tf || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
        return true;
    }

    while (true) {
        int sig = task_pick_pending_unblocked(t);
        if (sig <= 0) {
            return true;
        }

        uint64_t handler = t->signal_handler[sig];
        uint64_t restorer = t->signal_restorer[sig];
        uint32_t flags = t->signal_flags[sig];
        uint32_t action_mask = t->signal_action_mask[sig];
        task_sig_default_action_t dfl = task_signal_default_action(sig);

        if (handler == SIG_IGN || (handler == SIG_DFL && dfl == TASK_SIG_DFL_IGNORE)) {
            continue;
        }

        if (handler == SIG_DFL) {
            if (dfl == TASK_SIG_DFL_CONT) {
                if (t->state == TASK_STOPPED) {
                    t->state = TASK_RUNNABLE;
                    t->cont_report_pending = true;
                    t->stop_report_pending = false;
                    t->stop_signal = 0U;
                    task_notify_sigchld(t, TASK_CHLD_CONT);
                    task_wake_waiters(t->pid);
                }
                continue;
            }
            if (dfl == TASK_SIG_DFL_STOP) {
                if (t->state != TASK_STOPPED) {
                    t->state = TASK_STOPPED;
                    t->wait_pid = -1;
                    t->wait_kind = WAIT_NONE;
                    t->wait_obj = 0;
                    t->wake_tick = 0;
                    t->cont_report_pending = false;
                    t->stop_report_pending = true;
                    t->stop_signal = (uint8_t)sig;
                    task_notify_sigchld(t, TASK_CHLD_STOP);
                    task_wake_waiters(t->pid);
                }
                return false;
            }
            task_force_exit(t, WAIT_STATUS_SIG(sig));
            return false;
        }

        if (t->signal_active) {
            (void)task_signal_enqueue(t, sig);
            return true;
        }
        if ((flags & SA_RESTORER) == 0U || restorer < USER_VA_BASE || restorer >= USER_VA_LIMIT ||
            handler < USER_VA_BASE || handler >= USER_VA_LIMIT) {
            task_force_exit(t, WAIT_STATUS_SIG(SIGTERM));
            return false;
        }

        trapframe_t saved = *t->tf;
        if ((int64_t)saved.x[0] == -2 && (flags & SA_RESTART) == 0U) {
            saved.x[0] = (uint64_t)-4;
        }
        t->signal_saved_tf = saved;
        t->signal_saved_mask = t->signal_mask;
        t->signal_active = true;
        t->signal_active_num = (uint8_t)sig;
        if ((flags & SA_RESETHAND) != 0U) {
            t->signal_handler[sig] = SIG_DFL;
            t->signal_flags[sig] = 0U;
            t->signal_restorer[sig] = 0U;
            t->signal_action_mask[sig] = 0U;
        }

        t->signal_mask |= (uint32_t)(action_mask & 0x7FFFFFFFUL);
        if ((flags & SA_NODEFER) == 0U) {
            t->signal_mask |= SIGBIT(sig);
        }
        t->signal_mask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));

        t->tf->elr_el1 = handler;
        t->tf->x[0] = (uint64_t)sig;
        t->tf->x[30] = restorer;
        return true;
    }
}

int task_kill(int pid, int sig) {
    task_t *self = current_task_ptr;
    if (!self || !task_signal_supported(sig)) {
        return KERR_EINVAL;
    }

    int matched = 0;
    if (pid > 0) {
        task_t *victim = task_by_pid(pid);
        return task_signal_one(self, victim, sig) ? 0 : KERR_ESRCH;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *victim = &tasks[i];
        if (victim->state == TASK_UNUSED) {
            continue;
        }
        if (pid == 0 && victim->pgid != self->pgid) {
            continue;
        }
        if (pid < -1 && victim->pgid != -pid) {
            continue;
        }
        if (pid == -1 && victim->pid == init_pid) {
            continue;
        }
        matched += task_signal_one(self, victim, sig);
    }
    if (matched == 0) {
        return KERR_ESRCH;
    }
    return 0;
}

int task_sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact) {
    task_t *self = current_task_ptr;
    if (!self || sig <= 0 || sig >= 32 || !task_signal_supported(sig)) {
        return KERR_EINVAL;
    }
    if (oldact) {
        oldact->sa_handler = self->signal_handler[sig];
        oldact->sa_flags = self->signal_flags[sig];
        oldact->sa_restorer = self->signal_restorer[sig];
        oldact->sa_mask = self->signal_action_mask[sig];
    }
    if (!act) {
        return 0;
    }
    if ((sig == SIGKILL || sig == SIGSTOP) && act->sa_handler != SIG_DFL) {
        return KERR_EINVAL;
    }
    self->signal_handler[sig] = act->sa_handler;
    self->signal_flags[sig] = (uint32_t)(act->sa_flags &
                                         (SA_RESTART | SA_RESTORER |
                                          SA_NODEFER | SA_RESETHAND |
                                          SA_NOCLDSTOP | SA_NOCLDWAIT));
    self->signal_restorer[sig] = act->sa_restorer;
    self->signal_action_mask[sig] = (uint32_t)(act->sa_mask & 0x7FFFFFFFUL);
    if (sig == SIGCHLD) {
        task_reap_sigchld_zombies(self);
        if (!task_parent_has_child_sigchld_event(self)) {
            task_signal_clear_pending(self, SIGCHLD);
        }
    }
    return 0;
}

int task_sigprocmask(int how, uint64_t set, uint64_t *oldset) {
    task_t *self = current_task_ptr;
    if (!self) {
        return KERR_EINVAL;
    }
    if (oldset) {
        *oldset = self->signal_mask;
    }
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        return KERR_EINVAL;
    }
    uint32_t newmask = self->signal_mask;
    uint32_t set32 = (uint32_t)set;
    if (how == SIG_BLOCK) {
        newmask |= set32;
    } else if (how == SIG_UNBLOCK) {
        newmask &= ~set32;
    } else {
        newmask = set32;
    }
    newmask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    self->signal_mask = newmask;
    return 0;
}

int task_sigreturn(trapframe_t *tf) {
    task_t *self = current_task_ptr;
    if (!self || !tf || !self->signal_active) {
        return KERR_EINVAL;
    }
    *tf = self->signal_saved_tf;
    self->signal_mask = self->signal_saved_mask;
    self->signal_active = false;
    self->signal_active_num = 0;
    self->signal_saved_mask = 0U;
    return (int)tf->x[0];
}

void task_yield(void) {
    if (current_task_ptr && current_task_ptr->state == TASK_RUNNING) {
        current_task_ptr->state = TASK_RUNNABLE;
    }
}

int task_fd_alloc(task_t *task) {
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!task->fds[fd].used) {
            task->fds[fd].used = true;
            task->fds[fd].kind = FD_NONE;
            task->fds[fd].offset = 0;
            task->fds[fd].inode = 0;
            task->fds[fd].pipe = 0;
            task->fds[fd].sock = 0;
            task->fds[fd].flags = 0;
            return fd;
        }
    }
    return -1;
}

int task_fd_close(task_t *task, int fd) {
    if (!task || fd < 0 || fd >= MAX_FDS) {
        return -1;
    }
    if (!task->fds[fd].used) {
        return -1;
    }
    int rc = 0;
    fd_t closing = task->fds[fd];
    if (task_fd_needs_writeback(&closing)) {
        if (pagecache_flush_inode(closing.inode) != 0) {
            rc = -1;
        }
    }

    const void *wait_obj = 0;
    if (closing.kind == FD_PIPE_R || closing.kind == FD_PIPE_W) {
        wait_obj = closing.pipe;
    }
    task_fd_drop(&closing);
    memset(&task->fds[fd], 0, sizeof(task->fds[fd]));
    if (wait_obj) {
        task_wake_pipe(wait_obj);
    }
    return rc;
}

int task_fd_dup2(task_t *task, int oldfd, int newfd) {
    if (!task || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS) {
        return -1;
    }
    if (!task->fds[oldfd].used) {
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }

    task_fd_close(task, newfd);
    task->fds[newfd] = task->fds[oldfd];
    task->fds[newfd].used = true;
    task_fd_ref(&task->fds[newfd]);
    return newfd;
}

void task_block_on_pipe_read(const void *pipe) {
    if (!current_task_ptr) {
        return;
    }
    current_task_ptr->state = TASK_WAITING;
    current_task_ptr->wait_kind = WAIT_PIPE_READ;
    current_task_ptr->wait_obj = pipe;
    current_task_ptr->wake_tick = 0;
}

void task_block_on_pipe_write(const void *pipe) {
    if (!current_task_ptr) {
        return;
    }
    current_task_ptr->state = TASK_WAITING;
    current_task_ptr->wait_kind = WAIT_PIPE_WRITE;
    current_task_ptr->wait_obj = pipe;
    current_task_ptr->wake_tick = 0;
}

void task_wake_pipe(const void *pipe) {
    if (!pipe) {
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state != TASK_WAITING) {
            continue;
        }
        if ((t->wait_kind == WAIT_PIPE_READ || t->wait_kind == WAIT_PIPE_WRITE) &&
            t->wait_obj == pipe) {
            t->state = TASK_RUNNABLE;
            t->wait_kind = WAIT_NONE;
            t->wait_obj = 0;
            t->wake_tick = 0;
        }
    }
}

void task_block_on_sleep(uint64_t wake_tick) {
    if (!current_task_ptr) {
        return;
    }
    current_task_ptr->state = TASK_WAITING;
    current_task_ptr->wait_kind = WAIT_SLEEP;
    current_task_ptr->wait_obj = 0;
    current_task_ptr->wake_tick = wake_tick;
}

bool task_wake_sleepers(uint64_t now_tick) {
    bool woke = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_WAITING &&
            t->wait_kind == WAIT_SLEEP &&
            t->wake_tick <= now_tick) {
            t->state = TASK_RUNNABLE;
            t->wait_kind = WAIT_NONE;
            t->wait_obj = 0;
            t->wake_tick = 0;
            woke = true;
        }
    }
    return woke;
}

bool task_mount_busy(const inode_t *mountpoint, const char *mount_path) {
    if (!mountpoint || !mount_path) {
        return false;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        const task_t *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
            continue;
        }

        if (task_path_under_mount(t->cwd, mount_path)) {
            return true;
        }

        for (int fd = 0; fd < MAX_FDS; fd++) {
            if (!t->fds[fd].used || t->fds[fd].kind != FD_INODE || !t->fds[fd].inode) {
                continue;
            }
            if (task_inode_under_mount(t->fds[fd].inode, mountpoint)) {
                return true;
            }
        }

        for (int v = 0; v < MAX_VMAS; v++) {
            if (!t->vmas[v].used || !t->vmas[v].inode) {
                continue;
            }
            if (task_inode_under_mount(t->vmas[v].inode, mountpoint)) {
                return true;
            }
        }
    }

    return false;
}

long task_brk(uint64_t new_break) {
    task_t *t = current_task_ptr;
    if (!t) {
        return -1;
    }

    if (new_break == 0) {
        return (long)t->heap_end;
    }
    if (new_break < t->heap_base || new_break > t->heap_limit) {
        return -1;
    }

    uint64_t old_end = t->heap_end;
    t->heap_end = new_break;

    uint64_t old_top = align_up(old_end, PAGE_SIZE);
    uint64_t new_top = align_up(new_break, PAGE_SIZE);
    if (new_top < old_top) {
        for (uint64_t va = new_top; va < old_top; va += PAGE_SIZE) {
            (void)task_unmap_user_page(t, va);
        }
    }
    return (long)t->heap_end;
}

static int task_vma_free_slots(const task_t *t) {
    int free_slots = 0;
    if (!t) {
        return 0;
    }
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!t->vmas[i].used) {
            free_slots++;
        }
    }
    return free_slots;
}

static bool task_vma_split_at(task_t *t, uint64_t addr) {
    if (!t || (addr & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (addr <= v->start || addr >= v->end) {
            continue;
        }
        vm_area_t *slot = task_vma_slot(t);
        if (!slot) {
            return false;
        }
        uint64_t old_start = v->start;
        *slot = *v;
        slot->start = addr;
        slot->file_offset += (addr - old_start);
        v->end = addr;
        return true;
    }
    return true;
}

static int task_vma_split_need(const task_t *t, uint64_t start, uint64_t end) {
    if (!t || start >= end) {
        return -1;
    }

    int need = 0;
    for (int i = 0; i < MAX_VMAS; i++) {
        const vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (end <= v->start || start >= v->end) {
            continue;
        }

        uint64_t cut_start = start > v->start ? start : v->start;
        uint64_t cut_end = end < v->end ? end : v->end;
        if (cut_start > v->start && cut_end < v->end) {
            need++;
        }
    }
    return need;
}

static bool task_vma_unmap_range(task_t *t, uint64_t start, uint64_t end, bool require_mapped) {
    if (!t || start >= end) {
        return false;
    }
    if (require_mapped && !task_vma_range_mapped(t, start, end)) {
        return false;
    }

    int need = task_vma_split_need(t, start, end);
    if (need < 0 || task_vma_free_slots(t) < need) {
        return false;
    }

    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (end <= v->start || start >= v->end) {
            continue;
        }

        uint64_t cut_start = start > v->start ? start : v->start;
        uint64_t cut_end = end < v->end ? end : v->end;
        for (uint64_t va = cut_start; va < cut_end; va += PAGE_SIZE) {
            (void)task_unmap_user_page(t, va);
        }

        if (cut_start <= v->start && cut_end >= v->end) {
            memset(v, 0, sizeof(*v));
            continue;
        }
        if (cut_start <= v->start) {
            v->start = cut_end;
            v->file_offset += (cut_end - cut_start);
            continue;
        }
        if (cut_end >= v->end) {
            v->end = cut_start;
            continue;
        }

        vm_area_t *slot = task_vma_slot(t);
        if (!slot) {
            return false;
        }
        uint64_t old_start = v->start;
        *slot = *v;
        slot->start = cut_end;
        slot->file_offset += (cut_end - old_start);
        v->end = cut_start;
    }

    task_recompute_heap_limit(t);
    task_refresh_mem_accounting(t);
    return true;
}

static bool task_page_set_prot(task_t *t, user_page_t *up, int prot) {
    if (!t || !up || !up->used) {
        return false;
    }

    bool hw_writable = false;
    bool hw_executable = false;
    bool hw_cow = false;
    if (!task_page_validate_hw(t, up, 0, &hw_writable, &hw_executable, &hw_cow)) {
        return false;
    }
    (void)hw_writable;
    (void)hw_executable;
    (void)hw_cow;

    vm_area_t *vma = task_vma_find(t, up->va);
    bool map_shared = vma && ((vma->flags & MAP_SHARED) != 0);
    bool pte_writable = false;

    if (prot & PROT_WRITE) {
        uint32_t refs = pmm_page_refcount((void *)up->pa);
        if (refs == 0) {
            return false;
        }
        if (!map_shared && refs > 1) {
            if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
                return false;
            }
            void *new_page = pmm_alloc_page();
            if (!new_page) {
                return false;
            }
            memcpy(new_page, (void *)up->pa, PAGE_SIZE);
            pmm_free_page((void *)up->pa);
            up->pa = (uint64_t)new_page;
        }
        up->writable_intent = true;
        if (!map_shared) {
            pte_writable = true;
        }
    } else {
        up->writable_intent = false;
    }

    uint64_t attrs = task_user_attrs(pte_writable, (prot & PROT_EXEC) != 0, false);
    if (!task_set_user_pte(t, up->va, up->pa, attrs)) {
        return false;
    }
    mmu_tlb_flush_va(up->va, t->asid);
    return true;
}

long task_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset) {
    task_t *t = current_task_ptr;
    if (!t || len == 0 || !task_vma_prot_valid(prot) || !task_vma_flags_valid(flags)) {
        return -1;
    }

    uint64_t map_len = align_up(len, PAGE_SIZE);
    if (map_len == 0) {
        return -1;
    }
    if ((offset & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    bool map_anon = (flags & MAP_ANONYMOUS) != 0;
    bool map_shared = (flags & MAP_SHARED) != 0;
    inode_t *file = 0;

    if (map_anon) {
        if (fd != -1) {
            return -1;
        }
    } else {
        if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].used) {
            return -1;
        }
        fd_t *f = &t->fds[fd];
        if (f->kind != FD_INODE || !f->inode ||
            (f->inode->type != INODE_FILE && !fs_is_block_dev(f->inode))) {
            return -1;
        }
        int mode = task_fd_mode(f->flags);
        if (mode == O_WRONLY) {
            return -1;
        }
        if (map_shared && (prot & PROT_WRITE) && mode == O_RDONLY) {
            return -1;
        }
        if (map_shared && (prot & PROT_WRITE) && !f->inode->writable) {
            return -1;
        }
        if (offset + map_len < offset) {
            return -1;
        }
        file = f->inode;
    }

    uint64_t floor = align_up(t->heap_end, PAGE_SIZE);
    uint64_t top = task_stack_floor(t);
    if (floor >= top || map_len > (top - floor)) {
        return -1;
    }

    uint64_t start = 0;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        start = addr;
        uint64_t end = start + map_len;
        if (end <= start || start < USER_VA_BASE || end > top) {
            return -1;
        }
        if (!task_vma_unmap_range(t, start, end, false)) {
            return -1;
        }
    } else {
        uint64_t cand = align_down(top - map_len, PAGE_SIZE);
        bool found = false;
        while (cand >= floor) {
            uint64_t end = cand + map_len;
            if (!task_vma_overlaps(t, cand, end)) {
                start = cand;
                found = true;
                break;
            }
            if (cand < floor + PAGE_SIZE) {
                break;
            }
            cand -= PAGE_SIZE;
        }
        if (!found) {
            return -1;
        }
    }

    vm_area_t *slot = task_vma_slot(t);
    if (!slot) {
        return -1;
    }
    slot->used = true;
    slot->start = start;
    slot->end = start + map_len;
    slot->file_offset = offset;
    slot->inode = file;
    slot->prot = (uint32_t)prot;
    slot->flags = (uint32_t)flags;
    task_recompute_heap_limit(t);
    return (long)start;
}

int task_munmap(uint64_t addr, uint64_t len) {
    task_t *t = current_task_ptr;
    if (!t || len == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    uint64_t start = addr;
    uint64_t raw_end = addr + len;
    if (raw_end < addr) {
        return -1;
    }
    uint64_t end = align_up(raw_end, PAGE_SIZE);
    if (end <= start) {
        return -1;
    }
    return task_vma_unmap_range(t, start, end, false) ? 0 : -1;
}

int task_mprotect(uint64_t addr, uint64_t len, int prot) {
    task_t *t = current_task_ptr;
    if (!t || len == 0 || (addr & (PAGE_SIZE - 1)) != 0 || !task_vma_prot_valid(prot)) {
        return -1;
    }

    uint64_t start = addr;
    uint64_t raw_end = addr + len;
    if (raw_end < addr) {
        return -1;
    }
    uint64_t end = align_up(raw_end, PAGE_SIZE);
    if (end <= start) {
        return -1;
    }
    if (!task_vma_range_mapped(t, start, end)) {
        return -1;
    }

    if (prot & PROT_WRITE) {
        for (int i = 0; i < MAX_VMAS; i++) {
            vm_area_t *v = &t->vmas[i];
            if (!v->used) {
                continue;
            }
            if (end <= v->start || start >= v->end) {
                continue;
            }
            if ((v->flags & MAP_SHARED) && v->inode && !v->inode->writable) {
                return -1;
            }
        }
    }

    int need_slots = 0;
    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (start > v->start && start < v->end) {
            need_slots++;
        }
        if (end > v->start && end < v->end) {
            need_slots++;
        }
    }
    if (task_vma_free_slots(t) < need_slots) {
        return -1;
    }

    if (!task_vma_split_at(t, start) || !task_vma_split_at(t, end)) {
        return -1;
    }

    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used) {
            continue;
        }
        if (v->start >= start && v->end <= end) {
            v->prot = (uint32_t)prot;
        }
    }

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        user_page_t *up = task_find_page(t, va);
        if (!up) {
            continue;
        }
        if (!task_page_set_prot(t, up, prot)) {
            return -1;
        }
    }
    task_refresh_mem_accounting(t);
    return 0;
}

int task_msync(uint64_t addr, uint64_t len, uint64_t flags) {
    task_t *t = current_task_ptr;
    if (!t || len == 0U || (addr & (PAGE_SIZE - 1U)) != 0U) {
        return -1;
    }
    if ((flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0U) {
        return -1;
    }
    uint64_t mode = flags & (MS_ASYNC | MS_SYNC);
    if (mode == (MS_ASYNC | MS_SYNC)) {
        return -1;
    }
    if (mode == 0U) {
        flags |= MS_ASYNC;
    }

    uint64_t start = addr;
    uint64_t raw_end = addr + len;
    if (raw_end < addr) {
        return -1;
    }
    uint64_t end = align_up(raw_end, PAGE_SIZE);
    if (end <= start) {
        return -1;
    }
    if (!task_vma_range_mapped(t, start, end)) {
        return -1;
    }

    inode_t *synced[MAX_VMAS];
    uint32_t synced_count = 0U;
    memset(synced, 0, sizeof(synced));

    for (int i = 0; i < MAX_VMAS; i++) {
        vm_area_t *v = &t->vmas[i];
        if (!v->used || end <= v->start || start >= v->end) {
            continue;
        }
        if ((v->flags & MAP_SHARED) == 0 || !v->inode ||
            (v->inode->type != INODE_FILE && !fs_is_block_dev(v->inode))) {
            continue;
        }

        uint64_t ov_start = start > v->start ? start : v->start;
        uint64_t ov_end = end < v->end ? end : v->end;
        if (ov_start >= ov_end) {
            continue;
        }
        uint64_t file_off = v->file_offset + (ov_start - v->start);
        uint64_t span = ov_end - ov_start;
        if (pagecache_flush_inode_range(v->inode, file_off, span) != 0) {
            return -1;
        }
        if (flags & MS_INVALIDATE) {
            if (pagecache_invalidate_inode_range(v->inode, file_off, span) != 0) {
                return -1;
            }
        }

        if (flags & MS_SYNC) {
            bool seen = false;
            for (uint32_t s = 0; s < synced_count; s++) {
                if (synced[s] == v->inode) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                if (fs_sync_inode(v->inode) != 0) {
                    return -1;
                }
                if (synced_count < MAX_VMAS) {
                    synced[synced_count++] = v->inode;
                }
            }
        }
    }
    return 0;
}

bool task_pagecache_writeprotect_shared(inode_t *inode, uint64_t file_off) {
    if (!inode) {
        return true;
    }
    uint64_t page_off = align_down(file_off, PAGE_SIZE);
    for (int ti = 0; ti < MAX_TASKS; ti++) {
        task_t *t = &tasks[ti];
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
            continue;
        }
        for (int pi = 0; pi < MAX_USER_PAGES; pi++) {
            user_page_t *up = &t->pages[pi];
            if (!up->used || !up->writable_intent) {
                continue;
            }
            vm_area_t *v = task_vma_find(t, up->va);
            if (!v || (v->flags & MAP_SHARED) == 0 || (v->prot & PROT_WRITE) == 0 ||
                !v->inode || !task_inode_same_file(v->inode, inode)) {
                continue;
            }
            uint64_t v_file_off = v->file_offset + (up->va - v->start);
            if (v_file_off != page_off) {
                continue;
            }

            bool hw_writable = false;
            bool hw_executable = false;
            bool hw_cow = false;
            if (!task_page_validate_hw(t, up, 0, &hw_writable, &hw_executable, &hw_cow)) {
                return false;
            }
            if (hw_cow) {
                return false;
            }
            if (!hw_writable) {
                continue;
            }
            if (!task_set_user_pte(t, up->va, up->pa,
                                   task_user_attrs(false, hw_executable, false))) {
                return false;
            }
            mmu_tlb_flush_va(up->va, t->asid);
        }
    }
    return true;
}

bool task_handle_page_fault(uint64_t fault_va, bool is_write, bool is_translation,
                            bool is_instruction, uint64_t user_sp) {
    task_t *t = current_task_ptr;
    if (!t) {
        return false;
    }
    if (fault_va < USER_VA_BASE || fault_va >= USER_VA_LIMIT) {
        return false;
    }

    uint64_t va_page = align_down(fault_va, PAGE_SIZE);

    if (is_translation) {
        if (task_find_page(t, va_page)) {
            return false;
        }
        uint64_t cur_pte = 0;
        if (task_get_user_pte(t, va_page, &cur_pte) && task_pte_is_user_page(cur_pte)) {
            return false;
        }

        vm_area_t *v = task_vma_find(t, va_page);
        if (v) {
            int prot = (int)v->prot;
            if (is_instruction && (prot & PROT_EXEC) == 0) {
                return false;
            }
            if (is_write && (prot & PROT_WRITE) == 0) {
                return false;
            }
            if (!is_write && !is_instruction && (prot & (PROT_READ | PROT_WRITE)) == 0) {
                return false;
            }
            if (v->inode && (v->flags & MAP_ANONYMOUS) == 0) {
                return task_map_user_file_fault_page(t, v, va_page,
                                                     (prot & PROT_WRITE) != 0,
                                                     (prot & PROT_EXEC) != 0);
            }
            return task_map_user_anon_fault_page(t, va_page, (prot & PROT_WRITE) != 0,
                                                 (prot & PROT_EXEC) != 0);
        }

        if (is_instruction) {
            return false;
        }

        uint64_t heap_top = align_up(t->heap_end, PAGE_SIZE);
        if (va_page >= t->heap_base && va_page < heap_top) {
            return task_map_user_anon_fault_page(t, va_page, true, false);
        }

        if (va_page >= t->stack_base && va_page < USER_STACK_TOP) {
            if (t->stack_mapped_bottom <= t->stack_base) {
                return false;
            }
            if (user_sp < t->stack_base || user_sp >= USER_STACK_TOP) {
                return false;
            }
            if (fault_va + (2U * PAGE_SIZE) + STACK_GROW_SLOP < user_sp) {
                return false;
            }
            uint64_t sp_page = align_down(user_sp, PAGE_SIZE);
            if (sp_page < t->stack_base || sp_page >= USER_STACK_TOP) {
                return false;
            }
            if (va_page >= t->stack_mapped_bottom) {
                return false;
            }

            uint64_t need_bottom = va_page;
            if (sp_page < need_bottom) {
                need_bottom = sp_page;
            }

            while (t->stack_mapped_bottom > need_bottom) {
                uint64_t next_page = t->stack_mapped_bottom - PAGE_SIZE;
                if (!task_map_user_anon_fault_page(t, next_page, true, false)) {
                    return false;
                }
                t->stack_mapped_bottom = next_page;
            }
            return true;
        }
        return false;
    }

    if (!is_write) {
        return false;
    }

    user_page_t *up = task_find_page(t, va_page);
    bool hw_writable = false;
    bool hw_executable = false;
    bool hw_cow = false;
    if (!up || !task_page_validate_hw(t, up, 0, &hw_writable, &hw_executable, &hw_cow)) {
        return false;
    }
    if (!up->writable_intent) {
        return false;
    }
    if (hw_writable) {
        return false;
    }
    if (!hw_cow) {
        vm_area_t *v = task_vma_find(t, va_page);
        if (!v || (v->flags & MAP_SHARED) == 0 || (v->prot & PROT_WRITE) == 0 || !v->inode) {
            return false;
        }
        uint64_t file_off = v->file_offset + (va_page - v->start);
        pagecache_mark_dirty(v->inode, file_off, PAGE_SIZE);
        if (!task_set_user_pte(t, va_page, up->pa,
                               task_user_attrs(true, hw_executable, false))) {
            return false;
        }
        mmu_tlb_flush_va(va_page, t->asid);
        task_refresh_mem_accounting(t);
        return true;
    }
    uint32_t refs = pmm_page_refcount((void *)up->pa);
    if (refs == 0) {
        return false;
    }

    if (refs > 1) {
        if (pmm_available_pages() <= PMM_OOM_RESERVE_PAGES) {
            return false;
        }
        void *new_page = pmm_alloc_page();
        if (!new_page) {
            return false;
        }
        memcpy(new_page, (void *)up->pa, PAGE_SIZE);
        pmm_free_page((void *)up->pa);
        up->pa = (uint64_t)new_page;
    }

    uint64_t attrs = task_user_attrs(true, hw_executable, false);
    if (!task_set_user_pte(t, va_page, up->pa, attrs)) {
        return false;
    }
    mmu_tlb_flush_va(va_page, t->asid);
    task_refresh_mem_accounting(t);
    return true;
}

void task_dump(void) {
    uart_puts("PID table: ");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            print_dec(tasks[i].pid);
            uart_putc(' ');
        }
    }
    uart_puts("\n");
}

void task_set_comm(task_t *task, const char *name) {
    task_set_comm_internal(task, name);
}

const char *task_comm(const task_t *task) {
    if (!task) {
        return "";
    }
    return task->comm;
}

void task_load_debug_symbols(task_t *task, const uint8_t *elf, size_t elf_size) {
    if (!task) {
        return;
    }
    task_debug_syms_clear(task);
    if (!elf || elf_size < sizeof(Elf64_Ehdr)) {
        return;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)elf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_machine != EM_AARCH64) {
        return;
    }
    task_debug_syms_load(task, elf, elf_size, eh);
}

bool task_symbolize_pc(const task_t *task, uint64_t pc, const char **name_out,
                       uint64_t *sym_start_out, uint64_t *sym_end_out) {
    if (!task || task->debug_sym_count == 0U) {
        return false;
    }

    int best = -1;
    for (uint16_t i = 0; i < task->debug_sym_count; i++) {
        const task_debug_sym_t *s = &task->debug_syms[i];
        if (!s->used || s->name[0] == '\0' || pc < s->start) {
            continue;
        }
        if (pc < s->end) {
            if (best < 0 || s->start >= task->debug_syms[best].start) {
                best = (int)i;
            }
            continue;
        }
        if (best < 0 || s->start > task->debug_syms[best].start) {
            best = (int)i;
        }
    }

    if (best < 0) {
        return false;
    }
    const task_debug_sym_t *sym = &task->debug_syms[best];
    if (name_out) {
        *name_out = sym->name;
    }
    if (sym_start_out) {
        *sym_start_out = sym->start;
    }
    if (sym_end_out) {
        *sym_end_out = sym->end;
    }
    return true;
}
