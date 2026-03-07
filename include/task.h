#ifndef FUROS_TASK_H
#define FUROS_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"
#include "fs.h"
#include "syscall.h"

typedef struct pipe pipe_t;

typedef struct trapframe {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t tpidr_el0;
    uint64_t reserved0;
} trapframe_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_WAITING,
    TASK_STOPPED,
    TASK_ZOMBIE,
} task_state_t;

typedef enum {
    FD_NONE = 0,
    FD_INODE,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_SOCKET,
} fd_kind_t;

typedef struct {
    fd_kind_t kind;
    inode_t *inode;
    pipe_t *pipe;
    void *sock;
    size_t offset;
    int flags;
    bool used;
} fd_t;

typedef enum {
    WAIT_NONE = 0,
    WAIT_CHILD,
    WAIT_PIPE_READ,
    WAIT_PIPE_WRITE,
    WAIT_SLEEP,
} wait_kind_t;

typedef struct {
    uint64_t va;
    uint64_t pa;
    bool writable_intent;
    bool used;
} user_page_t;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t file_offset;
    inode_t *inode;
    uint32_t prot;
    uint32_t flags;
    bool used;
} vm_area_t;

#define TASK_DEBUG_SYMS_MAX 128
#define TASK_DEBUG_NAME_MAX 48

typedef struct {
    uint64_t start;
    uint64_t end;
    char name[TASK_DEBUG_NAME_MAX];
    bool used;
} task_debug_sym_t;

#define TASK_SIGNAL_QUEUE_LEN 64

typedef struct task {
    int pid;
    int ppid;
    int pgid;
    task_state_t state;
    int exit_code;
    int wait_pid;
    wait_kind_t wait_kind;
    const void *wait_obj;
    uint64_t wake_tick;
    uint16_t asid;
    uint32_t asid_epoch;

    uint64_t ttbr0_phys;
    uint64_t *l1;
    uint64_t *l2_low;
    uint64_t *l3_user_a;
    uint64_t *l3_user_b;

    user_page_t pages[MAX_USER_PAGES];
    uint64_t heap_base;
    uint64_t heap_end;
    uint64_t heap_limit;
    uint64_t stack_base;
    uint64_t stack_mapped_bottom;
    vm_area_t vmas[MAX_VMAS];
    uint32_t user_page_count;
    uint32_t cow_page_count;
    uint32_t private_page_count;
    uint32_t mem_charge_pages;
    uint32_t pending_signals;
    uint8_t signal_queue[TASK_SIGNAL_QUEUE_LEN];
    uint8_t signal_queue_count;
    uint32_t signal_mask;
    uint64_t signal_handler[32];
    uint64_t signal_restorer[32];
    uint32_t signal_flags[32];
    uint32_t signal_action_mask[32];
    trapframe_t signal_saved_tf;
    uint32_t signal_saved_mask;
    bool signal_active;
    uint8_t signal_active_num;
    bool stop_report_pending;
    bool cont_report_pending;
    uint8_t stop_signal;
    char comm[32];
    task_debug_sym_t debug_syms[TASK_DEBUG_SYMS_MAX];
    uint16_t debug_sym_count;
    fd_t fds[MAX_FDS];
    char cwd[MAX_PATH];
    uint8_t *kstack_base;
    uint64_t kstack_top;
    trapframe_t *tf;
} task_t;

void task_init(void);
task_t *task_current(void);
task_t *task_by_pid(int pid);
task_t *task_create_user(const uint8_t *elf, size_t elf_size, int ppid);
void task_enter_first(task_t *task);
void task_mark_exit(int code);
void task_wake_waiters(int child_pid);
int task_fork(const trapframe_t *parent_tf);
int task_exec(task_t *task, const uint8_t *elf, size_t elf_size, const char *const argv[]);
int task_wait(int pid, int options, int *status_out);
int task_kill(int pid, int sig);
int task_setpgid(int pid, int pgid);
int task_getpgid(int pid);
long task_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset);
int task_munmap(uint64_t addr, uint64_t len);
int task_mprotect(uint64_t addr, uint64_t len, int prot);
int task_msync(uint64_t addr, uint64_t len, uint64_t flags);
int task_sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact);
int task_sigprocmask(int how, uint64_t set, uint64_t *oldset);
int task_sigreturn(trapframe_t *tf);
trapframe_t *task_schedule_from_trap(trapframe_t *current_tf, bool force_resched);
void task_set_syscall_ret(task_t *task, uint64_t value);
void task_yield(void);
void task_dump(void);

int task_fd_alloc(task_t *task);
int task_fd_close(task_t *task, int fd);
int task_fd_dup2(task_t *task, int oldfd, int newfd);
void task_block_on_pipe_read(const void *pipe);
void task_block_on_pipe_write(const void *pipe);
void task_wake_pipe(const void *pipe);
void task_block_on_sleep(uint64_t wake_tick);
bool task_wake_sleepers(uint64_t now_tick);
long task_brk(uint64_t new_break);
bool task_mount_busy(const inode_t *mountpoint, const char *mount_path);
bool task_handle_page_fault(uint64_t fault_va, bool is_write, bool is_translation,
                            bool is_instruction, uint64_t user_sp);
bool task_pagecache_writeprotect_shared(inode_t *inode, uint64_t file_off);
void task_set_comm(task_t *task, const char *name);
const char *task_comm(const task_t *task);
bool task_symbolize_pc(const task_t *task, uint64_t pc, const char **name_out,
                       uint64_t *sym_start_out, uint64_t *sym_end_out);
void task_load_debug_symbols(task_t *task, const uint8_t *elf, size_t elf_size);

#endif
