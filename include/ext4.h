#ifndef FUROS_EXT4_H
#define FUROS_EXT4_H

#include <stddef.h>
#include <stdbool.h>
#include "fs.h"

bool ext4_mount(inode_t *mountpoint, const inode_t *source_dev);
bool ext4_unmount(inode_t *mountpoint);
inode_t *ext4_lookup_child(inode_t *dir, const char *name);
inode_t *ext4_lookup_child_nofollow(inode_t *dir, const char *name);
int ext4_read(inode_t *inode, size_t *offset, void *buf, size_t len);
inode_t *ext4_create_file(inode_t *parent, const char *name);
inode_t *ext4_create_dir(inode_t *parent, const char *name);
int ext4_link(inode_t *inode, inode_t *new_parent, const char *new_name);
inode_t *ext4_symlink(inode_t *parent, const char *name, const char *target);
int ext4_readlink(inode_t *inode, char *buf, size_t buflen);
int ext4_lstat(inode_t *inode, fu_stat_t *st);
int ext4_chmod(inode_t *inode, uint32_t mode);
int ext4_write(inode_t *inode, size_t *offset, const void *buf, size_t len);
int ext4_unlink(inode_t *inode);
int ext4_rmdir(inode_t *inode);
int ext4_rename(inode_t *inode, inode_t *new_parent, const char *new_name);
int ext4_truncate(inode_t *inode, size_t new_size);
bool ext4_tx_begin(void);
bool ext4_tx_commit(void);
void ext4_tx_abort(void);
int ext4_last_error(void);
bool ext4_sync_filesystem(void);
void ext4_periodic_maintenance(uint64_t now_ticks);

#endif
