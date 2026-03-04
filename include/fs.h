#ifndef FUROS_FS_H
#define FUROS_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "syscall.h"
#include "nvme.h"

#define INODE_NAME_MAX 31
#define DIR_MAX_CHILDREN 32
#define FILE_CAPACITY 4096

typedef enum {
    INODE_DIR = 1,
    INODE_FILE = 2,
    INODE_DEV = 3,
} inode_type_t;

typedef enum {
    FS_KIND_MEM = 0,
    FS_KIND_EXT4 = 1,
} fs_kind_t;

typedef enum {
    DEV_NONE = 0,
    DEV_NULL = 1,
    DEV_ZERO = 2,
    DEV_TTY = 3,
    DEV_VDA = 4,
    DEV_SDA = 5,
    DEV_SDB = 6,
    DEV_SDC = 7,
    DEV_SDD = 8,
    DEV_SDE = 9,
    DEV_SDF = 10,
    DEV_SDG = 11,
    DEV_SDH = 12,
    DEV_NVME_BASE = 13,
    DEV_NVME_LAST = DEV_NVME_BASE + (NVME_MAX_CONTROLLERS * NVME_MAX_NAMESPACES) - 1,
} dev_kind_t;

typedef struct inode inode_t;

struct inode {
    char name[INODE_NAME_MAX + 1];
    inode_type_t type;
    fs_kind_t fs_kind;
    uint32_t fs_ino;
    bool executable;
    bool writable;
    dev_kind_t dev_kind;
    uint64_t dev_lba_start;
    uint64_t dev_lba_count;
    inode_t *parent;
    inode_t *children[DIR_MAX_CHILDREN];
    uint32_t child_count;
    uint8_t *data;
    size_t size;
    size_t capacity;
};

typedef struct {
    char name[INODE_NAME_MAX + 1];
    uint32_t type;
} dirent_t;

void fs_init(void);
inode_t *fs_lookup(const char *path);
inode_t *fs_lookup_nofollow(const char *path);
inode_t *fs_create_file(const char *path);
inode_t *fs_create_dir(const char *path);
int fs_link(const char *old_path, const char *new_path);
int fs_symlink(const char *target, const char *link_path);
int fs_readlink(const char *path, char *buf, size_t buflen);
int fs_lstat(const char *path, fu_stat_t *st);
int fs_stat(const char *path, fu_stat_t *st);
int fs_stat_inode(inode_t *inode, fu_stat_t *st);
int fs_chmod(const char *path, uint32_t mode);
int fs_unlink(const char *path);
int fs_rmdir(const char *path);
int fs_rename(const char *old_path, const char *new_path);
int fs_read(inode_t *inode, size_t *offset, void *buf, size_t len);
int fs_write(inode_t *inode, size_t *offset, const void *buf, size_t len);
int fs_truncate(inode_t *inode, size_t size);
int fs_sync_inode(inode_t *inode);
int fs_sync_all(void);
int fs_mount(const char *source, const char *target, const char *fstype, uint64_t flags);
int fs_umount(const char *target, uint64_t flags);
int fs_mkfs_ext4(const char *target, uint64_t flags, const fu_mkfs_ext4_opts_t *opts);
int fs_fsck_ext4(const char *target, uint64_t flags);
bool fs_is_tty(const inode_t *inode);
bool fs_is_block_dev(const inode_t *inode);

#endif
