#ifndef USER_USER_H
#define USER_USER_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../include/syscall.h"

#define INODE_NAME_MAX 31

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef struct {
    uint32_t s_addr;
} in_addr;

typedef struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
} hostent;

#define h_addr h_addr_list[0]

typedef struct {
    char name[INODE_NAME_MAX + 1];
    uint32_t type;
} dirent_t;

#define DIRENT_DIR  1u
#define DIRENT_FILE 2u

long sys_write(int fd, const void *buf, unsigned long len);
long sys_read(int fd, void *buf, unsigned long len);
int sys_open(const char *path, int flags);
int sys_close(int fd);
void sys_exit(int code) __attribute__((noreturn));
int sys_fork(void);
int sys_exec(const char *path, const char *const argv[]);
int sys_wait(int pid, int *status);
int sys_waitpid(int pid, int *status, int options);
int sys_getpid(void);
int sys_kill(int pid, int sig);
int sys_sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact);
int sys_sigprocmask(int how, uint64_t set, uint64_t *oldset);
int sys_sigreturn(void);
void __sigreturn_trampoline(void) __attribute__((noreturn));
int sys_fcntl(int fd, int cmd, int arg);
int sys_poll(fu_pollfd_t *fds, int nfds, unsigned long timeout_ticks);
int sys_socket(int domain, int type, int protocol);
int sys_bind(int fd, const fu_sockaddr_t *addr, unsigned long addrlen);
long sys_sendto(int fd, const void *buf, unsigned long len, int flags,
                const fu_sockaddr_t *dest_addr, unsigned long addrlen);
long sys_recvfrom(int fd, void *buf, unsigned long len, int flags,
                  fu_sockaddr_t *src_addr, unsigned long *addrlen);
int sys_setsockopt(int fd, int level, int optname, const void *optval, unsigned long optlen);
int sys_getsockopt(int fd, int level, int optname, void *optval, unsigned long *optlen);
int sys_connect(int fd, const fu_sockaddr_t *addr, unsigned long addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int sys_getsockname(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int sys_getpeername(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int sys_shutdown(int fd, int how);
int sys_setpgid(int pid, int pgid);
int sys_getpgid(int pid);
void *sys_mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset);
int sys_munmap(void *addr, unsigned long len);
int sys_mprotect(void *addr, unsigned long len, int prot);
int sys_fsync(int fd);
int sys_msync(void *addr, unsigned long len, unsigned long flags);
int sys_chdir(const char *path);
int sys_mkdir(const char *path);
int sys_rmdir(const char *path);
int sys_unlink(const char *path);
int sys_getcwd(char *buf, unsigned long len);
int sys_rename(const char *old_path, const char *new_path);
int sys_mount(const char *source, const char *target, const char *fstype,
              unsigned long flags, const void *data);
int sys_umount(const char *target, unsigned long flags);
int sys_mkfsext4(const char *target, unsigned long flags, const fu_mkfs_ext4_opts_t *opts);
int sys_fsckext4(const char *target, unsigned long flags);
int sys_link(const char *old_path, const char *new_path);
int sys_symlink(const char *target, const char *link_path);
int sys_readlink(const char *path, char *buf, unsigned long buflen);
int sys_lstat(const char *path, fu_stat_t *st);
int sys_stat(const char *path, fu_stat_t *st);
int sys_fstat(int fd, fu_stat_t *st);
long sys_lseek(int fd, long offset, int whence);
int sys_chmod(const char *path, uint32_t mode);
int sys_dup2(int oldfd, int newfd);
int sys_pipe(int fds[2]);
int sys_sleep(unsigned long ticks);
void *sys_brk(void *addr);
void *sys_sbrk(long increment);
void sys_yield(void);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memmove(void *dst, const void *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

void puts(const char *s);
void putc(char c);
int putchar(int c);
int getchar(void);
int readline(char *buf, int maxlen);
int tokenize(char *line, char **argv, int max_args);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t n, size_t size);
void abort(void) __attribute__((noreturn));

int open(const char *path, int flags);
int close(int fd);
long read(int fd, void *buf, unsigned long len);
long write(int fd, const void *buf, unsigned long len);
long lseek(int fd, long offset, int whence);
int chdir(const char *path);
int mkdir(const char *path);
int rmdir(const char *path);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int link(const char *old_path, const char *new_path);
int symlink(const char *target, const char *link_path);
int readlink(const char *path, char *buf, unsigned long buflen);
int stat(const char *path, fu_stat_t *st);
int lstat(const char *path, fu_stat_t *st);
int fstat(int fd, fu_stat_t *st);
int chmod(const char *path, uint32_t mode);
int fcntl(int fd, int cmd, int arg);
int poll(fu_pollfd_t *fds, int nfds, unsigned long timeout_ticks);
int socket(int domain, int type, int protocol);
int bind(int fd, const fu_sockaddr_t *addr, unsigned long addrlen);
long sendto(int fd, const void *buf, unsigned long len, int flags,
            const fu_sockaddr_t *dest_addr, unsigned long addrlen);
long recvfrom(int fd, void *buf, unsigned long len, int flags,
              fu_sockaddr_t *src_addr, unsigned long *addrlen);
int setsockopt(int fd, int level, int optname, const void *optval, unsigned long optlen);
int getsockopt(int fd, int level, int optname, void *optval, unsigned long *optlen);
int connect(int fd, const fu_sockaddr_t *addr, unsigned long addrlen);
int listen(int fd, int backlog);
int accept(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int getsockname(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int getpeername(int fd, fu_sockaddr_t *addr, unsigned long *addrlen);
int shutdown(int fd, int how);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
int kill(int pid, int sig);
int sigaction(int sig, const fu_sigaction_t *act, fu_sigaction_t *oldact);
int sigprocmask(int how, uint64_t set, uint64_t *oldset);
int fork(void);
int execv(const char *path, const char *const argv[]);
int execve(const char *path, const char *const argv[], const char *const envp[]);
int waitpid(int pid, int *status, int options);
int wait(int *status);
int getpid(void);
int setpgid(int pid, int pgid);
int getpgid(int pid);
int sleep(unsigned long ticks);
char *getcwd(char *buf, unsigned long len);
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset);
int munmap(void *addr, unsigned long len);
int mprotect(void *addr, unsigned long len, int prot);
int fsync(int fd);
int msync(void *addr, unsigned long len, unsigned long flags);
int brk(void *addr);
void *sbrk(long increment);
void _exit(int code) __attribute__((noreturn));
extern int errno;

int inet_aton(const char *src, in_addr *out);
unsigned long inet_addr(const char *src);
char *inet_ntoa(in_addr in);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, unsigned long size);
hostent *gethostbyname(const char *name);
int fu_parse_ipv4(const char *src, uint32_t *out_be);
void fu_format_ipv4(uint32_t ip_be, char *buf, unsigned long size);
int fu_resolver_nameserver(uint32_t *ip_be, uint16_t *port);
int fu_resolve_name_ipv4(const char *name, uint32_t *out_be);
int fu_resolve_name_ipv4_at(const char *name, uint32_t server_ip_be, uint16_t server_port, uint32_t *out_be);

#endif
