#include "../user.h"

static long syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

long sys_write(int fd, const void *buf, unsigned long len) {
    long rc;
    do {
        rc = syscall6(SYS_WRITE, fd, (long)buf, len, 0, 0, 0);
    } while (rc == -2);
    return rc;
}

long sys_read(int fd, void *buf, unsigned long len) {
    long rc;
    do {
        rc = syscall6(SYS_READ, fd, (long)buf, len, 0, 0, 0);
    } while (rc == -2);
    return rc;
}

int sys_open(const char *path, int flags) {
    return (int)syscall6(SYS_OPEN, (long)path, flags, 0, 0, 0, 0);
}

int sys_close(int fd) {
    return (int)syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

void sys_exit(int code) {
    syscall6(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;) {
        __asm__ volatile("wfe");
    }
}

int sys_fork(void) {
    return (int)syscall6(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

int sys_exec(const char *path, const char *const argv[]) {
    return (int)syscall6(SYS_EXEC, (long)path, (long)argv, 0, 0, 0, 0);
}

int sys_wait(int pid, int *status) {
    return sys_waitpid(pid, status, 0);
}

int sys_waitpid(int pid, int *status, int options) {
    int rc;
    do {
        rc = (int)syscall6(SYS_WAIT, pid, (long)status, options, 0, 0, 0);
    } while (rc == -2);
    return rc;
}

int sys_getpid(void) {
    return (int)syscall6(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

int sys_kill(int pid, int sig) {
    return (int)syscall6(SYS_KILL, pid, sig, 0, 0, 0, 0);
}

int sys_sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact) {
    return (int)syscall6(SYS_SIGACTION, sig, (long)act, (long)oldact, 0, 0, 0);
}

int sys_sigprocmask(int how, uint64_t set, uint64_t *oldset) {
    return (int)syscall6(SYS_SIGPROCMASK, how, (long)set, (long)oldset, 0, 0, 0);
}

int sys_sigreturn(void) {
    return (int)syscall6(SYS_SIGRETURN, 0, 0, 0, 0, 0, 0);
}

int sys_fcntl(int fd, int cmd, int arg) {
    return (int)syscall6(SYS_FCNTL, fd, cmd, arg, 0, 0, 0);
}

int sys_poll(fu_pollfd_t *fds, int nfds, unsigned long timeout_ticks) {
    long rc;
    unsigned long remain = timeout_ticks;
    for (;;) {
        rc = syscall6(SYS_POLL, (long)fds, (long)nfds, (long)remain, 0, 0, 0);
        if (rc != -2) {
            return (int)rc;
        }
        if (remain > 0) {
            remain--;
        } else {
            return 0;
        }
    }
}

int sys_setpgid(int pid, int pgid) {
    return (int)syscall6(SYS_SETPGID, pid, pgid, 0, 0, 0, 0);
}

int sys_getpgid(int pid) {
    return (int)syscall6(SYS_GETPGID, pid, 0, 0, 0, 0, 0);
}

void *sys_mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset) {
    long rc = syscall6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, (long)offset);
    if (rc < 0) {
        return (void *)-1;
    }
    return (void *)rc;
}

int sys_munmap(void *addr, unsigned long len) {
    return (int)syscall6(SYS_MUNMAP, (long)addr, (long)len, 0, 0, 0, 0);
}

int sys_mprotect(void *addr, unsigned long len, int prot) {
    return (int)syscall6(SYS_MPROTECT, (long)addr, (long)len, prot, 0, 0, 0);
}

int sys_fsync(int fd) {
    return (int)syscall6(SYS_FSYNC, (long)fd, 0, 0, 0, 0, 0);
}

int sys_msync(void *addr, unsigned long len, unsigned long flags) {
    return (int)syscall6(SYS_MSYNC, (long)addr, (long)len, (long)flags, 0, 0, 0);
}

int sys_chdir(const char *path) {
    return (int)syscall6(SYS_CHDIR, (long)path, 0, 0, 0, 0, 0);
}

int sys_mkdir(const char *path) {
    return (int)syscall6(SYS_MKDIR, (long)path, 0, 0, 0, 0, 0);
}

int sys_rmdir(const char *path) {
    return (int)syscall6(SYS_RMDIR, (long)path, 0, 0, 0, 0, 0);
}

int sys_unlink(const char *path) {
    return (int)syscall6(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0);
}

int sys_getcwd(char *buf, unsigned long len) {
    return (int)syscall6(SYS_GETCWD, (long)buf, (long)len, 0, 0, 0, 0);
}

int sys_rename(const char *old_path, const char *new_path) {
    return (int)syscall6(SYS_RENAME, (long)old_path, (long)new_path, 0, 0, 0, 0);
}

int sys_mount(const char *source, const char *target, const char *fstype,
              unsigned long flags, const void *data) {
    return (int)syscall6(SYS_MOUNT, (long)source, (long)target, (long)fstype,
                         (long)flags, (long)data, 0);
}

int sys_umount(const char *target, unsigned long flags) {
    return (int)syscall6(SYS_UMOUNT, (long)target, (long)flags, 0, 0, 0, 0);
}

int sys_mkfsext4(const char *target, unsigned long flags, const fu_mkfs_ext4_opts_t *opts) {
    return (int)syscall6(SYS_MKFSEXT4, (long)target, (long)flags, (long)opts, 0, 0, 0);
}

int sys_fsckext4(const char *target, unsigned long flags) {
    return (int)syscall6(SYS_FSCKEXT4, (long)target, (long)flags, 0, 0, 0, 0);
}

int sys_link(const char *old_path, const char *new_path) {
    return (int)syscall6(SYS_LINK, (long)old_path, (long)new_path, 0, 0, 0, 0);
}

int sys_symlink(const char *target, const char *link_path) {
    return (int)syscall6(SYS_SYMLINK, (long)target, (long)link_path, 0, 0, 0, 0);
}

int sys_readlink(const char *path, char *buf, unsigned long buflen) {
    return (int)syscall6(SYS_READLINK, (long)path, (long)buf, (long)buflen, 0, 0, 0);
}

int sys_lstat(const char *path, fu_stat_t *st) {
    return (int)syscall6(SYS_LSTAT, (long)path, (long)st, 0, 0, 0, 0);
}

int sys_stat(const char *path, fu_stat_t *st) {
    return (int)syscall6(SYS_STAT, (long)path, (long)st, 0, 0, 0, 0);
}

int sys_fstat(int fd, fu_stat_t *st) {
    return (int)syscall6(SYS_FSTAT, (long)fd, (long)st, 0, 0, 0, 0);
}

long sys_lseek(int fd, long offset, int whence) {
    return syscall6(SYS_LSEEK, (long)fd, offset, (long)whence, 0, 0, 0);
}

int sys_chmod(const char *path, uint32_t mode) {
    return (int)syscall6(SYS_CHMOD, (long)path, (long)mode, 0, 0, 0, 0);
}

int sys_dup2(int oldfd, int newfd) {
    return (int)syscall6(SYS_DUP2, (long)oldfd, (long)newfd, 0, 0, 0, 0);
}

int sys_pipe(int fds[2]) {
    return (int)syscall6(SYS_PIPE, (long)fds, 0, 0, 0, 0, 0);
}

int sys_sleep(unsigned long ticks) {
    long rc;
    do {
        rc = syscall6(SYS_SLEEP, (long)ticks, 0, 0, 0, 0, 0);
        ticks = 0;
    } while (rc == -2);
    return (int)rc;
}

void *sys_brk(void *addr) {
    long rc = syscall6(SYS_BRK, (long)addr, 0, 0, 0, 0, 0);
    if (rc < 0) {
        return (void *)-1;
    }
    return (void *)rc;
}

void *sys_sbrk(long increment) {
    static long cached_break = -1;
    if (cached_break < 0) {
        void *cur = sys_brk(0);
        if (cur == (void *)-1) {
            return (void *)-1;
        }
        cached_break = (long)cur;
    }

    long next = cached_break + increment;
    if ((increment > 0 && next < cached_break) ||
        (increment < 0 && next > cached_break)) {
        return (void *)-1;
    }

    void *rc = sys_brk((void *)next);
    if (rc == (void *)-1) {
        return (void *)-1;
    }

    long old = cached_break;
    cached_break = (long)rc;
    return (void *)old;
}

void sys_yield(void) {
    (void)syscall6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}
