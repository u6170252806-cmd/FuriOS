#include "../user.h"

int errno;

static int neg_errno(long rc) {
    errno = (int)(-rc);
    return -1;
}

__attribute__((noreturn)) void __sigreturn_trampoline(void) {
    (void)sys_sigreturn();
    for (;;) {
        __asm__ volatile("yield");
    }
}

int open(const char *path, int flags) {
    long rc = sys_open(path, flags);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int close(int fd) {
    long rc = sys_close(fd);
    return (rc < 0) ? neg_errno(rc) : 0;
}

long read(int fd, void *buf, unsigned long len) {
    long rc = sys_read(fd, buf, len);
    return (rc < 0) ? (long)neg_errno(rc) : rc;
}

long write(int fd, const void *buf, unsigned long len) {
    long rc = sys_write(fd, buf, len);
    return (rc < 0) ? (long)neg_errno(rc) : rc;
}

long lseek(int fd, long offset, int whence) {
    long rc = sys_lseek(fd, offset, whence);
    return (rc < 0) ? (long)neg_errno(rc) : rc;
}

int chdir(const char *path) {
    long rc = sys_chdir(path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int mkdir(const char *path) {
    long rc = sys_mkdir(path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int rmdir(const char *path) {
    long rc = sys_rmdir(path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int unlink(const char *path) {
    long rc = sys_unlink(path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int rename(const char *old_path, const char *new_path) {
    long rc = sys_rename(old_path, new_path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int link(const char *old_path, const char *new_path) {
    long rc = sys_link(old_path, new_path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int symlink(const char *target, const char *link_path) {
    long rc = sys_symlink(target, link_path);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int readlink(const char *path, char *buf, unsigned long buflen) {
    long rc = sys_readlink(path, buf, buflen);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int stat(const char *path, fu_stat_t *st) {
    long rc = sys_stat(path, st);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int lstat(const char *path, fu_stat_t *st) {
    long rc = sys_lstat(path, st);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int fstat(int fd, fu_stat_t *st) {
    long rc = sys_fstat(fd, st);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int chmod(const char *path, uint32_t mode) {
    long rc = sys_chmod(path, mode);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int fcntl(int fd, int cmd, int arg) {
    long rc = sys_fcntl(fd, cmd, arg);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int poll(fu_pollfd_t *fds, int nfds, unsigned long timeout_ticks) {
    long rc = sys_poll(fds, nfds, timeout_ticks);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int socket(int domain, int type, int protocol) {
    long rc = sys_socket(domain, type, protocol);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int bind(int fd, const fu_sockaddr_t *addr, unsigned long addrlen) {
    long rc = sys_bind(fd, addr, addrlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

long sendto(int fd, const void *buf, unsigned long len, int flags,
            const fu_sockaddr_t *dest_addr, unsigned long addrlen) {
    long rc = sys_sendto(fd, buf, len, flags, dest_addr, addrlen);
    return (rc < 0) ? (long)neg_errno(rc) : rc;
}

long recvfrom(int fd, void *buf, unsigned long len, int flags,
              fu_sockaddr_t *src_addr, unsigned long *addrlen) {
    long rc = sys_recvfrom(fd, buf, len, flags, src_addr, addrlen);
    return (rc < 0) ? (long)neg_errno(rc) : rc;
}

int setsockopt(int fd, int level, int optname, const void *optval, unsigned long optlen) {
    long rc = sys_setsockopt(fd, level, optname, optval, optlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int getsockopt(int fd, int level, int optname, void *optval, unsigned long *optlen) {
    long rc = sys_getsockopt(fd, level, optname, optval, optlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int connect(int fd, const fu_sockaddr_t *addr, unsigned long addrlen) {
    long rc = sys_connect(fd, addr, addrlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int listen(int fd, int backlog) {
    long rc = sys_listen(fd, backlog);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int accept(int fd, fu_sockaddr_t *addr, unsigned long *addrlen) {
    long rc = sys_accept(fd, addr, addrlen);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int getsockname(int fd, fu_sockaddr_t *addr, unsigned long *addrlen) {
    long rc = sys_getsockname(fd, addr, addrlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int getpeername(int fd, fu_sockaddr_t *addr, unsigned long *addrlen) {
    long rc = sys_getpeername(fd, addr, addrlen);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int shutdown(int fd, int how) {
    long rc = sys_shutdown(fd, how);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int dup2(int oldfd, int newfd) {
    long rc = sys_dup2(oldfd, newfd);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int pipe(int fds[2]) {
    long rc = sys_pipe(fds);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int kill(int pid, int sig) {
    long rc = sys_kill(pid, sig);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact) {
    long rc;
    fu_sigaction_t local;
    if (act) {
        local = *act;
        if (local.sa_handler != SIG_DFL &&
            local.sa_handler != SIG_IGN &&
            local.sa_restorer == 0U) {
            local.sa_flags |= SA_RESTORER;
            local.sa_restorer = (uint64_t)(uintptr_t)&__sigreturn_trampoline;
        }
        rc = sys_sigaction(sig, &local, oldact);
    } else {
        rc = sys_sigaction(sig, 0, oldact);
    }
    return (rc < 0) ? neg_errno(rc) : 0;
}

int sigprocmask(int how, uint64_t set, uint64_t *oldset) {
    long rc = sys_sigprocmask(how, set, oldset);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int fork(void) {
    long rc = sys_fork();
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int execv(const char *path, const char *const argv[]) {
    long rc = sys_exec(path, argv);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int execve(const char *path, const char *const argv[], const char *const envp[]) {
    (void)envp;
    return execv(path, argv);
}

int waitpid(int pid, int *status, int options) {
    long rc = sys_waitpid(pid, status, options);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int wait(int *status) {
    return waitpid(-1, status, 0);
}

int getpid(void) {
    long rc = sys_getpid();
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int setpgid(int pid, int pgid) {
    long rc = sys_setpgid(pid, pgid);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int getpgid(int pid) {
    long rc = sys_getpgid(pid);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

int sleep(unsigned long ticks) {
    long rc = sys_sleep(ticks);
    return (rc < 0) ? neg_errno(rc) : (int)rc;
}

char *getcwd(char *buf, unsigned long len) {
    long rc = sys_getcwd(buf, len);
    if (rc < 0) {
        (void)neg_errno(rc);
        return 0;
    }
    return buf;
}

void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset) {
    void *p = sys_mmap(addr, len, prot, flags, fd, offset);
    if (p == (void *)-1) {
        errno = 12;
    }
    return p;
}

int munmap(void *addr, unsigned long len) {
    long rc = sys_munmap(addr, len);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int mprotect(void *addr, unsigned long len, int prot) {
    long rc = sys_mprotect(addr, len, prot);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int fsync(int fd) {
    long rc = sys_fsync(fd);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int msync(void *addr, unsigned long len, unsigned long flags) {
    long rc = sys_msync(addr, len, flags);
    return (rc < 0) ? neg_errno(rc) : 0;
}

int brk(void *addr) {
    void *rc = sys_brk(addr);
    if (rc == (void *)-1 || rc != addr) {
        errno = 12;
        return -1;
    }
    return 0;
}

void *sbrk(long increment) {
    void *rc = sys_sbrk(increment);
    if (rc == (void *)-1) {
        errno = 12;
        return (void *)-1;
    }
    return rc;
}

void exit(int code) {
    sys_exit(code);
}

void _exit(int code) {
    sys_exit(code);
}
