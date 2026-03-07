#ifndef FUROS_SYSCALL_H
#define FUROS_SYSCALL_H

#include <stdint.h>

#define FUROS_SYSCALL_LIST(X) \
    X(SYS_WRITE, 0, "write", 3) \
    X(SYS_READ, 1, "read", 3) \
    X(SYS_OPEN, 2, "open", 2) \
    X(SYS_CLOSE, 3, "close", 1) \
    X(SYS_EXIT, 4, "exit", 1) \
    X(SYS_YIELD, 5, "yield", 0) \
    X(SYS_FORK, 6, "fork", 0) \
    X(SYS_EXEC, 7, "exec", 2) \
    X(SYS_WAIT, 8, "wait", 3) \
    X(SYS_GETPID, 9, "getpid", 0) \
    X(SYS_CHDIR, 10, "chdir", 1) \
    X(SYS_MKDIR, 11, "mkdir", 1) \
    X(SYS_RMDIR, 12, "rmdir", 1) \
    X(SYS_UNLINK, 13, "unlink", 1) \
    X(SYS_GETCWD, 14, "getcwd", 2) \
    X(SYS_RENAME, 15, "rename", 2) \
    X(SYS_DUP2, 16, "dup2", 2) \
    X(SYS_PIPE, 17, "pipe", 1) \
    X(SYS_SLEEP, 18, "sleep", 1) \
    X(SYS_BRK, 19, "brk", 1) \
    X(SYS_KILL, 20, "kill", 2) \
    X(SYS_FCNTL, 21, "fcntl", 3) \
    X(SYS_POLL, 22, "poll", 3) \
    X(SYS_SETPGID, 23, "setpgid", 2) \
    X(SYS_GETPGID, 24, "getpgid", 1) \
    X(SYS_MMAP, 25, "mmap", 6) \
    X(SYS_MUNMAP, 26, "munmap", 2) \
    X(SYS_MPROTECT, 27, "mprotect", 3) \
    X(SYS_MOUNT, 28, "mount", 5) \
    X(SYS_UMOUNT, 29, "umount", 2) \
    X(SYS_LINK, 30, "link", 2) \
    X(SYS_SYMLINK, 31, "symlink", 2) \
    X(SYS_READLINK, 32, "readlink", 3) \
    X(SYS_LSTAT, 33, "lstat", 2) \
    X(SYS_MKFSEXT4, 34, "mkfsext4", 3) \
    X(SYS_FSCKEXT4, 35, "fsckext4", 2) \
    X(SYS_LSEEK, 36, "lseek", 3) \
    X(SYS_FSTAT, 37, "fstat", 2) \
    X(SYS_STAT, 38, "stat", 2) \
    X(SYS_CHMOD, 39, "chmod", 2) \
    X(SYS_FSYNC, 40, "fsync", 1) \
    X(SYS_MSYNC, 41, "msync", 3) \
    X(SYS_SIGACTION, 42, "sigaction", 3) \
    X(SYS_SIGPROCMASK, 43, "sigprocmask", 3) \
    X(SYS_SIGRETURN, 44, "sigreturn", 0) \
    X(SYS_SOCKET, 45, "socket", 3) \
    X(SYS_BIND, 46, "bind", 3) \
    X(SYS_SENDTO, 47, "sendto", 6) \
    X(SYS_RECVFROM, 48, "recvfrom", 6) \
    X(SYS_SETSOCKOPT, 49, "setsockopt", 5) \
    X(SYS_CONNECT, 50, "connect", 3) \
    X(SYS_LISTEN, 51, "listen", 2) \
    X(SYS_ACCEPT, 52, "accept", 3) \
    X(SYS_GETSOCKNAME, 53, "getsockname", 3) \
    X(SYS_GETPEERNAME, 54, "getpeername", 3) \
    X(SYS_SHUTDOWN, 55, "shutdown", 2) \
    X(SYS_GETSOCKOPT, 56, "getsockopt", 5)

enum {
#define X(sym, nr, name, argc) sym = nr,
    FUROS_SYSCALL_LIST(X)
#undef X
    SYS_MAX,
};

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_ACCMODE 0x3
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800

#define F_GETFL 3
#define F_SETFL 4

#define WNOHANG 0x1
#define WUNTRACED 0x2
#define WCONTINUED 0x8

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGPIPE  13
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19

#define SIG_DFL  0UL
#define SIG_IGN  1UL

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_RESTART  0x10000000UL
#define SA_RESTORER 0x04000000UL
#define SA_NODEFER  0x40000000UL
#define SA_RESETHAND 0x80000000UL
#define SA_NOCLDSTOP 0x00000001UL
#define SA_NOCLDWAIT 0x00000002UL

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MS_ASYNC      0x1
#define MS_INVALIDATE 0x2
#define MS_SYNC       0x4

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

#define AF_INET 2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_NONBLOCK 0x800

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_BROADCAST 6

#define IPPROTO_IP  0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define MSG_DONTWAIT 0x40

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 8

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MKFS_EXT4_FEAT_EXTENTS      0x0001U
#define MKFS_EXT4_FEAT_64BIT        0x0002U
#define MKFS_EXT4_FEAT_METADATA_CSUM 0x0004U
#define MKFS_EXT4_FEAT_SPARSE_SUPER 0x0008U
#define MKFS_EXT4_FEAT_HAS_JOURNAL  0x0010U

#define MKFS_EXT4_OPT_LABEL_SET     0x0001U
#define MKFS_EXT4_OPT_UUID_SET      0x0002U
#define MKFS_EXT4_F_STRICT_KERNEL   0x0001U

#define FSCK_EXT4_F_NO_REPAIR       0x0001U
#define FSCK_EXT4_F_PREEN           0x0002U
#define FSCK_EXT4_F_YES             0x0004U

#define MKFS_EXT4_PROFILE_DEFAULT    0U
#define MKFS_EXT4_PROFILE_SMALL      1U
#define MKFS_EXT4_PROFILE_LARGEFILE  2U
#define MKFS_EXT4_PROFILE_LARGEFILE4 3U

typedef struct {
    int fd;
    int16_t events;
    int16_t revents;
} fu_pollfd_t;

typedef struct {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    uint32_t nlink;
    uint32_t fs_kind;
} fu_stat_t;

typedef struct {
    uint32_t size;
    uint32_t opt_flags;
    uint32_t feature_flags;
    uint16_t reserved_pct;
    uint16_t stride;
    uint16_t profile;
    uint16_t inode_size;
    uint32_t bytes_per_inode;
    uint32_t journal_blocks;
    uint8_t uuid[16];
    char label[16];
} fu_mkfs_ext4_opts_t;

typedef struct {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
} fu_sigaction_t;

typedef struct {
    uint16_t sa_family;
    char sa_data[14];
} fu_sockaddr_t;

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} fu_sockaddr_in_t;

#endif
