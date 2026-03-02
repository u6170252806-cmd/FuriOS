#ifndef USER_USER_H
#define USER_USER_H

#include <stdint.h>
#include <stddef.h>
#include "../include/syscall.h"

#define INODE_NAME_MAX 31

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
int sys_fcntl(int fd, int cmd, int arg);
int sys_poll(fu_pollfd_t *fds, int nfds, unsigned long timeout_ticks);
int sys_setpgid(int pid, int pgid);
int sys_getpgid(int pid);
void *sys_mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset);
int sys_munmap(void *addr, unsigned long len);
int sys_mprotect(void *addr, unsigned long len, int prot);
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

void puts(const char *s);
void putc(char c);
int readline(char *buf, int maxlen);
int tokenize(char *line, char **argv, int max_args);

#endif
