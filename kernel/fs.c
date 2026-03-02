#include "fs.h"
#include "ext4.h"
#include "string.h"
#include "pmm.h"
#include "config.h"
#include "uart.h"
#include "virtio_blk.h"
#include "ahci.h"
#include "nvme.h"
#include "block_cache.h"
#include "pagecache.h"
#include "task.h"
#include "print.h"
#include "timer.h"

#define INODE_POOL_MAX 128
#define MOUNT_TABLE_MAX 4
#define DEV_PARTS_PER_DISK_MAX 16
#define NVME_DEV_MAX (DEV_NVME7N1 - DEV_NVME0N1 + 1)

typedef struct {
    bool used;
    inode_t *mountpoint;
    fs_kind_t kind;
    dev_kind_t source_dev;
} mount_entry_t;

extern uint8_t _binary_build_user_init_elf_start[];
extern uint8_t _binary_build_user_init_elf_end[];
extern uint8_t _binary_build_user_sh_elf_start[];
extern uint8_t _binary_build_user_sh_elf_end[];
extern uint8_t _binary_build_user_ls_elf_start[];
extern uint8_t _binary_build_user_ls_elf_end[];
extern uint8_t _binary_build_user_cat_elf_start[];
extern uint8_t _binary_build_user_cat_elf_end[];
extern uint8_t _binary_build_user_echo_elf_start[];
extern uint8_t _binary_build_user_echo_elf_end[];
extern uint8_t _binary_build_user_clear_elf_start[];
extern uint8_t _binary_build_user_clear_elf_end[];
extern uint8_t _binary_build_user_mkdir_elf_start[];
extern uint8_t _binary_build_user_mkdir_elf_end[];
extern uint8_t _binary_build_user_rmdir_elf_start[];
extern uint8_t _binary_build_user_rmdir_elf_end[];
extern uint8_t _binary_build_user_rm_elf_start[];
extern uint8_t _binary_build_user_rm_elf_end[];
extern uint8_t _binary_build_user_pwd_elf_start[];
extern uint8_t _binary_build_user_pwd_elf_end[];
extern uint8_t _binary_build_user_touch_elf_start[];
extern uint8_t _binary_build_user_touch_elf_end[];
extern uint8_t _binary_build_user_cp_elf_start[];
extern uint8_t _binary_build_user_cp_elf_end[];
extern uint8_t _binary_build_user_mv_elf_start[];
extern uint8_t _binary_build_user_mv_elf_end[];
extern uint8_t _binary_build_user_sleep_elf_start[];
extern uint8_t _binary_build_user_sleep_elf_end[];
extern uint8_t _binary_build_user_kill_elf_start[];
extern uint8_t _binary_build_user_kill_elf_end[];
extern uint8_t _binary_build_user_mount_elf_start[];
extern uint8_t _binary_build_user_mount_elf_end[];
extern uint8_t _binary_build_user_umount_elf_start[];
extern uint8_t _binary_build_user_umount_elf_end[];
extern uint8_t _binary_build_user_ln_elf_start[];
extern uint8_t _binary_build_user_ln_elf_end[];
extern uint8_t _binary_build_user_mkfs_ext4_elf_start[];
extern uint8_t _binary_build_user_mkfs_ext4_elf_end[];
extern uint8_t _binary_build_user_fsck_ext4_elf_start[];
extern uint8_t _binary_build_user_fsck_ext4_elf_end[];

static inode_t inode_pool[INODE_POOL_MAX];
static inode_t *root_inode;
static inode_t *dev_root_inode;
static mount_entry_t mount_table[MOUNT_TABLE_MAX];
static void fs_sync_hotplug_block_devs(void);

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4U) << 32);
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFU);
    p[1] = (uint8_t)((v >> 16) & 0xFFU);
    p[2] = (uint8_t)((v >> 8) & 0xFFU);
    p[3] = (uint8_t)(v & 0xFFU);
}

static inode_t *inode_alloc(const char *name, inode_type_t type) {
    for (int i = 0; i < INODE_POOL_MAX; i++) {
        if (inode_pool[i].type == 0) {
            inode_t *ino = &inode_pool[i];
            memset(ino, 0, sizeof(*ino));
            strncpy(ino->name, name, INODE_NAME_MAX);
            ino->name[INODE_NAME_MAX] = '\0';
            ino->type = type;
            ino->fs_kind = FS_KIND_MEM;
            ino->fs_ino = 0;
            return ino;
        }
    }
    return 0;
}

static void inode_free(inode_t *ino) {
    if (!ino) {
        return;
    }
    memset(ino, 0, sizeof(*ino));
}

static mount_entry_t *mount_find_by_point(inode_t *mountpoint) {
    if (!mountpoint) {
        return 0;
    }
    for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
        if (mount_table[i].used && mount_table[i].mountpoint == mountpoint) {
            return &mount_table[i];
        }
    }
    return 0;
}

static bool mount_is_active(inode_t *mountpoint) {
    return mount_find_by_point(mountpoint) != 0;
}

static mount_entry_t *mount_alloc_entry(void) {
    for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
        if (!mount_table[i].used) {
            return &mount_table[i];
        }
    }
    return 0;
}

static bool mount_uses_source_dev(dev_kind_t source_dev) {
    for (int i = 0; i < MOUNT_TABLE_MAX; i++) {
        if (!mount_table[i].used) {
            continue;
        }
        if (mount_table[i].source_dev == source_dev) {
            return true;
        }
    }
    return false;
}

static bool dir_add_child(inode_t *dir, inode_t *child) {
    if (!dir || dir->type != INODE_DIR || dir->child_count >= DIR_MAX_CHILDREN) {
        return false;
    }
    dir->children[dir->child_count++] = child;
    child->parent = dir;
    return true;
}

static bool dir_remove_child(inode_t *dir, inode_t *child) {
    if (!dir || dir->type != INODE_DIR || !child) {
        return false;
    }
    for (uint32_t i = 0; i < dir->child_count; i++) {
        if (dir->children[i] == child) {
            for (uint32_t j = i + 1; j < dir->child_count; j++) {
                dir->children[j - 1] = dir->children[j];
            }
            dir->children[dir->child_count - 1] = 0;
            dir->child_count--;
            child->parent = 0;
            return true;
        }
    }
    return false;
}

static inode_t *dir_find_child(inode_t *dir, const char *name) {
    if (!dir || dir->type != INODE_DIR) {
        return 0;
    }
    for (uint32_t i = 0; i < dir->child_count; i++) {
        inode_t *child = dir->children[i];
        if (child && strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return 0;
}

static inode_t *fs_lookup_child_mode(inode_t *dir, const char *name, bool follow_symlink) {
    if (!dir || dir->type != INODE_DIR) {
        return 0;
    }
    if (dir->fs_kind == FS_KIND_EXT4) {
        if (follow_symlink) {
            return ext4_lookup_child(dir, name);
        }
        return ext4_lookup_child_nofollow(dir, name);
    }
    return dir_find_child(dir, name);
}

static int sata_index_for_kind(dev_kind_t kind) {
    if (kind < DEV_SDA || kind > DEV_SDH) {
        return -1;
    }
    return (int)(kind - DEV_SDA);
}

static int nvme_index_for_kind(dev_kind_t kind) {
    if (kind < DEV_NVME0N1 || kind > DEV_NVME7N1) {
        return -1;
    }
    return (int)(kind - DEV_NVME0N1);
}

static bool dev_kind_is_block(dev_kind_t kind) {
    return kind == DEV_VDA || sata_index_for_kind(kind) >= 0 || nvme_index_for_kind(kind) >= 0;
}

static bool dev_block_ready_kind(dev_kind_t kind) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_ready();
    }
    if (sidx >= 0) {
        return ahci_disk_present((uint32_t)sidx);
    }
    if (nidx >= 0) {
        return nvme_disk_present((uint32_t)nidx);
    }
    return false;
}

static uint64_t dev_block_capacity_sectors(dev_kind_t kind) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_capacity_sectors();
    }
    if (sidx >= 0) {
        return ahci_disk_capacity_sectors((uint32_t)sidx);
    }
    if (nidx >= 0) {
        return nvme_disk_capacity_sectors((uint32_t)nidx);
    }
    return 0U;
}

static int dev_block_rw_sector(dev_kind_t kind, uint64_t lba, void *buf, bool write) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_rw_sector(lba, buf, write);
    }
    if (sidx >= 0) {
        return ahci_rw_sector((uint32_t)sidx, lba, buf, write);
    }
    if (nidx >= 0) {
        return nvme_rw_sector((uint32_t)nidx, lba, buf, write);
    }
    return -1;
}

static int dev_block_rw(dev_kind_t kind, uint64_t lba, void *buf, uint32_t count, bool write) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (count == 0U) {
        return 0;
    }
    if (kind == DEV_VDA) {
        uint8_t *p = (uint8_t *)buf;
        for (uint32_t i = 0; i < count; i++) {
            if (virtio_blk_rw_sector(lba + i, p + (uint64_t)i * VIRTIO_BLK_SECTOR_SIZE, write) != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (sidx >= 0) {
        return ahci_rw((uint32_t)sidx, lba, buf, count, write);
    }
    if (nidx >= 0) {
        return nvme_rw((uint32_t)nidx, lba, buf, count, write);
    }
    return -1;
}

static int dev_block_flush_kind(dev_kind_t kind) {
    int sidx = sata_index_for_kind(kind);
    int nidx = nvme_index_for_kind(kind);
    if (kind == DEV_VDA) {
        return virtio_blk_flush();
    }
    if (sidx >= 0) {
        return ahci_flush((uint32_t)sidx);
    }
    if (nidx >= 0) {
        return nvme_flush((uint32_t)nidx);
    }
    return -1;
}

static inode_t *fs_lookup_internal(const char *path, bool follow_final_symlink) {
    if (!path || !root_inode) {
        return 0;
    }
    if (path[0] == '/' &&
        path[1] == 'd' &&
        path[2] == 'e' &&
        path[3] == 'v' &&
        (path[4] == '\0' || path[4] == '/')) {
        fs_sync_hotplug_block_devs();
    }
    if (strcmp(path, "/") == 0) {
        return root_inode;
    }

    inode_t *cur = root_inode;
    const char *p = path;
    if (*p == '/') {
        p++;
    }

    while (*p) {
        char part[INODE_NAME_MAX + 1];
        size_t n = 0;
        bool last_component = false;
        while (*p && *p != '/' && n < INODE_NAME_MAX) {
            part[n++] = *p++;
        }
        part[n] = '\0';

        while (*p == '/') {
            p++;
        }
        last_component = (*p == '\0');

        if (n == 0) {
            continue;
        }

        cur = fs_lookup_child_mode(cur, part, !last_component || follow_final_symlink);
        if (!cur) {
            return 0;
        }
    }
    return cur;
}

static inode_t *mk_dir(inode_t *parent, const char *name) {
    inode_t *d = inode_alloc(name, INODE_DIR);
    if (!d) {
        return 0;
    }
    if (parent) {
        if (!dir_add_child(parent, d)) {
            inode_free(d);
            return 0;
        }
    }
    return d;
}

static inode_t *mk_file(inode_t *parent, const char *name, const uint8_t *data,
                        size_t size, bool executable, bool writable) {
    inode_t *f = inode_alloc(name, INODE_FILE);
    if (!f) {
        return 0;
    }
    if (data && size) {
        f->data = (uint8_t *)data;
        f->size = size;
        f->capacity = size;
    } else {
        f->capacity = FILE_CAPACITY;
        f->data = (uint8_t *)pmm_alloc(f->capacity, 16);
        if (!f->data) {
            inode_free(f);
            return 0;
        }
        f->size = 0;
    }
    f->executable = executable;
    f->writable = writable;
    if (parent) {
        if (!dir_add_child(parent, f)) {
            inode_free(f);
            return 0;
        }
    }
    return f;
}

static inode_t *mk_dev(inode_t *parent, const char *name, dev_kind_t kind, bool writable) {
    inode_t *d = inode_alloc(name, INODE_DEV);
    if (!d) {
        return 0;
    }
    d->dev_kind = kind;
    d->dev_lba_start = 0U;
    d->dev_lba_count = 0U;
    d->writable = writable;
    d->executable = false;
    d->data = 0;
    d->size = 0;
    d->capacity = 0;

    if (dev_kind_is_block(kind) && dev_block_ready_kind(kind)) {
        uint64_t sectors = dev_block_capacity_sectors(kind);
        uint64_t cap = sectors * (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
        if (cap > (uint64_t)SIZE_MAX) {
            cap = (uint64_t)SIZE_MAX;
        }
        d->dev_lba_count = sectors;
        d->size = (size_t)cap;
        d->capacity = d->size;
    }

    if (parent) {
        if (!dir_add_child(parent, d)) {
            inode_free(d);
            return 0;
        }
    }
    return d;
}

static inode_t *mk_dev_part(inode_t *parent, dev_kind_t base_kind, const char *name,
                            uint64_t lba_start, uint64_t lba_count) {
    inode_t *d;
    uint64_t disk_cap;
    uint64_t cap_bytes;

    if (!parent || !name || lba_count == 0U || !dev_block_ready_kind(base_kind)) {
        return 0;
    }
    disk_cap = dev_block_capacity_sectors(base_kind);
    if (lba_start >= disk_cap || lba_count > (disk_cap - lba_start)) {
        return 0;
    }

    d = mk_dev(parent, name, base_kind, true);
    if (!d) {
        return 0;
    }
    d->dev_lba_start = lba_start;
    d->dev_lba_count = lba_count;
    cap_bytes = lba_count * (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
    if (cap_bytes > (uint64_t)SIZE_MAX) {
        cap_bytes = (uint64_t)SIZE_MAX;
    }
    d->size = (size_t)cap_bytes;
    d->capacity = d->size;
    return d;
}

static int dev_block_read_inode(const inode_t *inode, size_t *offset, void *buf, size_t len) {
    dev_kind_t kind;
    uint64_t base_lba;
    uint64_t lba_count;
    if (!inode || !offset || !buf || inode->type != INODE_DEV) {
        return -1;
    }
    kind = inode->dev_kind;
    if (!dev_block_ready_kind(kind)) {
        return -1;
    }
    base_lba = inode->dev_lba_start;
    lba_count = inode->dev_lba_count ? inode->dev_lba_count : dev_block_capacity_sectors(kind);
    uint64_t cap = lba_count * (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
    if ((uint64_t)(*offset) >= cap) {
        return 0;
    }
    size_t todo = len;
    if ((uint64_t)(*offset) + todo > cap) {
        todo = (size_t)(cap - (uint64_t)(*offset));
    }

    uint8_t sec[VIRTIO_BLK_SECTOR_SIZE];
    size_t done = 0;
    while (done < todo) {
        uint64_t off = (uint64_t)(*offset) + done;
        uint64_t lba = off / VIRTIO_BLK_SECTOR_SIZE;
        size_t sec_off = (size_t)(off % VIRTIO_BLK_SECTOR_SIZE);
        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - sec_off;
        if (chunk > todo - done) {
            chunk = todo - done;
        }
        if (sec_off == 0U && chunk == VIRTIO_BLK_SECTOR_SIZE) {
            uint32_t nsec = (uint32_t)((todo - done) / VIRTIO_BLK_SECTOR_SIZE);
            if (nsec > 128U) {
                nsec = 128U;
            }
            if (nsec > 0U && dev_block_rw(kind, base_lba + lba,
                                          (uint8_t *)buf + done, nsec, false) == 0) {
                done += (size_t)nsec * VIRTIO_BLK_SECTOR_SIZE;
                continue;
            }
        }
        if (dev_block_rw_sector(kind, base_lba + lba, sec, false) != 0) {
            return done ? (int)done : -1;
        }
        memcpy((uint8_t *)buf + done, sec + sec_off, chunk);
        done += chunk;
    }
    *offset += done;
    return (int)done;
}

static int dev_block_write_inode(const inode_t *inode, size_t *offset, const void *buf, size_t len) {
    dev_kind_t kind;
    uint64_t base_lba;
    uint64_t lba_count;
    if (!inode || !offset || !buf || inode->type != INODE_DEV) {
        return -1;
    }
    kind = inode->dev_kind;
    if (!dev_block_ready_kind(kind)) {
        return -1;
    }
    base_lba = inode->dev_lba_start;
    lba_count = inode->dev_lba_count ? inode->dev_lba_count : dev_block_capacity_sectors(kind);
    uint64_t cap = lba_count * (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
    if ((uint64_t)(*offset) >= cap) {
        return -1;
    }
    size_t todo = len;
    if ((uint64_t)(*offset) + todo > cap) {
        todo = (size_t)(cap - (uint64_t)(*offset));
    }

    uint8_t sec[VIRTIO_BLK_SECTOR_SIZE];
    size_t done = 0;
    while (done < todo) {
        uint64_t off = (uint64_t)(*offset) + done;
        uint64_t lba = off / VIRTIO_BLK_SECTOR_SIZE;
        size_t sec_off = (size_t)(off % VIRTIO_BLK_SECTOR_SIZE);
        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - sec_off;
        if (chunk > todo - done) {
            chunk = todo - done;
        }

        if (sec_off == 0U && chunk == VIRTIO_BLK_SECTOR_SIZE) {
            uint32_t nsec = (uint32_t)((todo - done) / VIRTIO_BLK_SECTOR_SIZE);
            if (nsec > 128U) {
                nsec = 128U;
            }
            if (nsec > 0U && dev_block_rw(kind, base_lba + lba,
                                          (void *)((const uint8_t *)buf + done), nsec, true) == 0) {
                done += (size_t)nsec * VIRTIO_BLK_SECTOR_SIZE;
                continue;
            }
        }

        if (chunk != VIRTIO_BLK_SECTOR_SIZE || sec_off != 0U) {
            if (dev_block_rw_sector(kind, base_lba + lba, sec, false) != 0) {
                return done ? (int)done : -1;
            }
        }
        memcpy(sec + sec_off, (const uint8_t *)buf + done, chunk);
        if (dev_block_rw_sector(kind, base_lba + lba, sec, true) != 0) {
            return done ? (int)done : -1;
        }
        done += chunk;
    }
    *offset += done;
    if (done > 0U) {
        (void)dev_block_flush_kind(kind);
    }
    return (int)done;
}

static size_t blob_size(uint8_t *start, uint8_t *end) {
    return (size_t)(end - start);
}

static int u32_to_dec(uint32_t v, char *out, size_t outsz) {
    char tmp[12];
    int n = 0;
    int i;
    if (!out || outsz == 0U) {
        return -1;
    }
    if (v == 0U) {
        if (outsz < 2U) {
            return -1;
        }
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (v > 0U && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    if ((size_t)n + 1U > outsz) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

static bool make_part_name(const char *base, uint32_t pnum, char *out, size_t outsz) {
    size_t blen;
    bool needs_p;
    size_t pos;
    char num[12];
    int nlen;
    if (!base || !out) {
        return false;
    }
    blen = strlen(base);
    needs_p = blen > 0U && base[blen - 1U] >= '0' && base[blen - 1U] <= '9';
    nlen = u32_to_dec(pnum, num, sizeof(num));
    if (nlen <= 0) {
        return false;
    }
    if (blen + (needs_p ? 1U : 0U) + (size_t)nlen + 1U > outsz) {
        return false;
    }
    memcpy(out, base, blen);
    pos = blen;
    if (needs_p) {
        out[pos++] = 'p';
    }
    memcpy(out + pos, num, (size_t)nlen + 1U);
    return true;
}

static bool make_nvme_name(uint32_t idx, char *out, size_t outsz) {
    char num[12];
    int nlen;
    size_t pos = 0U;
    if (!out || outsz == 0U) {
        return false;
    }
    nlen = u32_to_dec(idx, num, sizeof(num));
    if (nlen <= 0) {
        return false;
    }
    if ((size_t)nlen + 7U + 1U > outsz) {
        return false;
    }
    memcpy(out + pos, "nvme", 4U);
    pos += 4U;
    memcpy(out + pos, num, (size_t)nlen);
    pos += (size_t)nlen;
    out[pos++] = 'n';
    out[pos++] = '1';
    out[pos] = '\0';
    return true;
}

static void probe_partitions(inode_t *dev_dir, dev_kind_t kind, const char *base_name) {
    uint8_t mbr[VIRTIO_BLK_SECTOR_SIZE];
    uint8_t sec0[VIRTIO_BLK_SECTOR_SIZE];
    uint8_t sec1[VIRTIO_BLK_SECTOR_SIZE];
    bool has_gpt = false;
    uint32_t created = 0U;

    if (!dev_dir || !base_name || !dev_kind_is_block(kind) || !dev_block_ready_kind(kind)) {
        return;
    }
    if (dev_block_rw_sector(kind, 0U, mbr, false) != 0) {
        return;
    }
    if (mbr[510] != 0x55U || mbr[511] != 0xAAU) {
        return;
    }
    for (uint32_t i = 0; i < 4U; i++) {
        const uint8_t *e = mbr + 446U + i * 16U;
        if (e[4] == 0xEEU) {
            has_gpt = true;
            break;
        }
    }

    if (has_gpt && dev_block_rw_sector(kind, 1U, sec0, false) == 0 &&
        memcmp(sec0, "EFI PART", 8U) == 0) {
        uint64_t ent_lba = rd_le64(sec0 + 72U);
        uint32_t ent_cnt = rd_le32(sec0 + 80U);
        uint32_t ent_sz = rd_le32(sec0 + 84U);
        if (ent_cnt > 128U) {
            ent_cnt = 128U;
        }
        if (ent_sz >= 128U && ent_sz <= 512U) {
            for (uint32_t i = 0; i < ent_cnt && created < DEV_PARTS_PER_DISK_MAX; i++) {
                uint64_t off = (uint64_t)i * ent_sz;
                uint64_t lba = ent_lba + (off / VIRTIO_BLK_SECTOR_SIZE);
                uint32_t in = (uint32_t)(off % VIRTIO_BLK_SECTOR_SIZE);
                const uint8_t *e;
                bool zero_type = true;
                uint64_t first_lba;
                uint64_t last_lba;
                char pname[INODE_NAME_MAX + 1];

                if (dev_block_rw_sector(kind, lba, sec0, false) != 0) {
                    break;
                }
                if (in + ent_sz <= VIRTIO_BLK_SECTOR_SIZE) {
                    e = sec0 + in;
                } else {
                    if (dev_block_rw_sector(kind, lba + 1U, sec1, false) != 0) {
                        break;
                    }
                    memcpy(mbr, sec0 + in, VIRTIO_BLK_SECTOR_SIZE - in);
                    memcpy(mbr + (VIRTIO_BLK_SECTOR_SIZE - in), sec1, ent_sz - (VIRTIO_BLK_SECTOR_SIZE - in));
                    e = mbr;
                }

                for (uint32_t b = 0; b < 16U; b++) {
                    if (e[b] != 0U) {
                        zero_type = false;
                        break;
                    }
                }
                if (zero_type) {
                    continue;
                }
                first_lba = rd_le64(e + 32U);
                last_lba = rd_le64(e + 40U);
                if (first_lba == 0U || last_lba < first_lba) {
                    continue;
                }
                if (!make_part_name(base_name, i + 1U, pname, sizeof(pname))) {
                    continue;
                }
                if (mk_dev_part(dev_dir, kind, pname, first_lba, last_lba - first_lba + 1U)) {
                    created++;
                }
            }
            if (created > 0U) {
                return;
            }
        }
    }

    for (uint32_t i = 0; i < 4U && created < DEV_PARTS_PER_DISK_MAX; i++) {
        const uint8_t *e = mbr + 446U + i * 16U;
        uint8_t type = e[4];
        uint32_t first = rd_le32(e + 8U);
        uint32_t count = rd_le32(e + 12U);
        char pname[INODE_NAME_MAX + 1];
        if (type == 0U || type == 0xEEU || first == 0U || count == 0U) {
            continue;
        }
        if (!make_part_name(base_name, i + 1U, pname, sizeof(pname))) {
            continue;
        }
        if (mk_dev_part(dev_dir, kind, pname, first, count)) {
            created++;
        }
    }
}

static void fs_sync_hotplug_block_devs(void) {
    if (!dev_root_inode || dev_root_inode->type != INODE_DIR) {
        return;
    }

    ahci_poll();
    nvme_poll();

    uint32_t sata = ahci_disk_count();
    if (sata > (uint32_t)(DEV_SDH - DEV_SDA + 1U)) {
        sata = (uint32_t)(DEV_SDH - DEV_SDA + 1U);
    }
    for (uint32_t i = 0; i < sata; i++) {
        char name[4] = {'s', 'd', 'a', '\0'};
        inode_t *n;
        dev_kind_t k = (dev_kind_t)(DEV_SDA + i);
        name[2] = (char)('a' + (char)i);

        if (!ahci_disk_present(i)) {
            if (mount_uses_source_dev(k)) {
                continue;
            }
            for (uint32_t j = 0; j < dev_root_inode->child_count;) {
                inode_t *c = dev_root_inode->children[j];
                if (c &&
                    c->name[0] == 's' &&
                    c->name[1] == 'd' &&
                    c->name[2] == name[2] &&
                    c->name[3] >= '1' &&
                    c->name[3] <= '9') {
                    if (dir_remove_child(dev_root_inode, c)) {
                        inode_free(c);
                        continue;
                    }
                }
                j++;
            }
            n = dir_find_child(dev_root_inode, name);
            if (n && dir_remove_child(dev_root_inode, n)) {
                inode_free(n);
            }
            continue;
        }
        if (dir_find_child(dev_root_inode, name)) {
            continue;
        }

        n = mk_dev(dev_root_inode, name, k, true);
        if (n) {
            probe_partitions(dev_root_inode, k, name);
        }
    }

    uint32_t nvme_cnt = nvme_disk_count();
    if (nvme_cnt > (uint32_t)NVME_DEV_MAX) {
        nvme_cnt = (uint32_t)NVME_DEV_MAX;
    }
    for (uint32_t i = 0; i < nvme_cnt; i++) {
        char name[INODE_NAME_MAX + 1];
        inode_t *n;
        dev_kind_t k = (dev_kind_t)(DEV_NVME0N1 + i);
        size_t base_len;
        if (!make_nvme_name(i, name, sizeof(name))) {
            continue;
        }
        base_len = strlen(name);

        if (!nvme_disk_present(i)) {
            if (mount_uses_source_dev(k)) {
                continue;
            }
            for (uint32_t j = 0; j < dev_root_inode->child_count;) {
                inode_t *c = dev_root_inode->children[j];
                if (c && strncmp(c->name, name, base_len) == 0 &&
                    (c->name[base_len] == '\0' ||
                     (c->name[base_len] == 'p' && c->name[base_len + 1U] >= '1' &&
                      c->name[base_len + 1U] <= '9'))) {
                    if (dir_remove_child(dev_root_inode, c)) {
                        inode_free(c);
                        continue;
                    }
                }
                j++;
            }
            continue;
        }
        if (dir_find_child(dev_root_inode, name)) {
            continue;
        }

        n = mk_dev(dev_root_inode, name, k, true);
        if (n) {
            probe_partitions(dev_root_inode, k, name);
        }
    }
}

void fs_init(void) {
    memset(inode_pool, 0, sizeof(inode_pool));
    memset(mount_table, 0, sizeof(mount_table));
    root_inode = mk_dir(0, "");
    dev_root_inode = 0;

    inode_t *bin = mk_dir(root_inode, "bin");
    inode_t *sbin = mk_dir(root_inode, "sbin");
    inode_t *etc = mk_dir(root_inode, "etc");
    inode_t *dev = mk_dir(root_inode, "dev");
    (void)mk_dir(root_inode, "mnt");

    mk_file(bin, "init", _binary_build_user_init_elf_start,
            blob_size(_binary_build_user_init_elf_start, _binary_build_user_init_elf_end),
            true, false);
    mk_file(bin, "sh", _binary_build_user_sh_elf_start,
            blob_size(_binary_build_user_sh_elf_start, _binary_build_user_sh_elf_end),
            true, false);
    mk_file(bin, "ls", _binary_build_user_ls_elf_start,
            blob_size(_binary_build_user_ls_elf_start, _binary_build_user_ls_elf_end),
            true, false);
    mk_file(bin, "cat", _binary_build_user_cat_elf_start,
            blob_size(_binary_build_user_cat_elf_start, _binary_build_user_cat_elf_end),
            true, false);
    mk_file(bin, "echo", _binary_build_user_echo_elf_start,
            blob_size(_binary_build_user_echo_elf_start, _binary_build_user_echo_elf_end),
            true, false);
    mk_file(bin, "clear", _binary_build_user_clear_elf_start,
            blob_size(_binary_build_user_clear_elf_start, _binary_build_user_clear_elf_end),
            true, false);
    mk_file(bin, "mkdir", _binary_build_user_mkdir_elf_start,
            blob_size(_binary_build_user_mkdir_elf_start, _binary_build_user_mkdir_elf_end),
            true, false);
    mk_file(bin, "rmdir", _binary_build_user_rmdir_elf_start,
            blob_size(_binary_build_user_rmdir_elf_start, _binary_build_user_rmdir_elf_end),
            true, false);
    mk_file(bin, "rm", _binary_build_user_rm_elf_start,
            blob_size(_binary_build_user_rm_elf_start, _binary_build_user_rm_elf_end),
            true, false);
    mk_file(bin, "pwd", _binary_build_user_pwd_elf_start,
            blob_size(_binary_build_user_pwd_elf_start, _binary_build_user_pwd_elf_end),
            true, false);
    mk_file(bin, "touch", _binary_build_user_touch_elf_start,
            blob_size(_binary_build_user_touch_elf_start, _binary_build_user_touch_elf_end),
            true, false);
    mk_file(bin, "cp", _binary_build_user_cp_elf_start,
            blob_size(_binary_build_user_cp_elf_start, _binary_build_user_cp_elf_end),
            true, false);
    mk_file(bin, "mv", _binary_build_user_mv_elf_start,
            blob_size(_binary_build_user_mv_elf_start, _binary_build_user_mv_elf_end),
            true, false);
    mk_file(bin, "sleep", _binary_build_user_sleep_elf_start,
            blob_size(_binary_build_user_sleep_elf_start, _binary_build_user_sleep_elf_end),
            true, false);
    mk_file(bin, "kill", _binary_build_user_kill_elf_start,
            blob_size(_binary_build_user_kill_elf_start, _binary_build_user_kill_elf_end),
            true, false);
    mk_file(bin, "mount", _binary_build_user_mount_elf_start,
            blob_size(_binary_build_user_mount_elf_start, _binary_build_user_mount_elf_end),
            true, false);
    mk_file(bin, "umount", _binary_build_user_umount_elf_start,
            blob_size(_binary_build_user_umount_elf_start, _binary_build_user_umount_elf_end),
            true, false);
    mk_file(bin, "ln", _binary_build_user_ln_elf_start,
            blob_size(_binary_build_user_ln_elf_start, _binary_build_user_ln_elf_end),
            true, false);
    if (sbin) {
        mk_file(sbin, "mkfs.ext4", _binary_build_user_mkfs_ext4_elf_start,
                blob_size(_binary_build_user_mkfs_ext4_elf_start, _binary_build_user_mkfs_ext4_elf_end),
                true, false);
        mk_file(sbin, "fsck.ext4", _binary_build_user_fsck_ext4_elf_start,
                blob_size(_binary_build_user_fsck_ext4_elf_start, _binary_build_user_fsck_ext4_elf_end),
                true, false);
    }

    inode_t *motd = mk_file(etc, "motd", 0, 0, false, true);
    const char *msg = "FuriOS EL0 userspace online\n";
    if (motd && motd->data) {
        memcpy(motd->data, msg, strlen(msg));
        motd->size = strlen(msg);
    }

    if (dev) {
        dev_root_inode = dev;
        inode_t *n = 0;
        (void)mk_dev(dev, "null", DEV_NULL, true);
        (void)mk_dev(dev, "zero", DEV_ZERO, true);
        (void)mk_dev(dev, "tty", DEV_TTY, true);
        if (dev_block_ready_kind(DEV_VDA)) {
            n = mk_dev(dev, "vda", DEV_VDA, true);
            if (n) {
                probe_partitions(dev, DEV_VDA, "vda");
            }
        }
        uint32_t sata = ahci_disk_count();
        if (sata > (uint32_t)(DEV_SDH - DEV_SDA + 1U)) {
            sata = (uint32_t)(DEV_SDH - DEV_SDA + 1U);
        }
        for (uint32_t i = 0; i < sata; i++) {
            if (!ahci_disk_present(i)) {
                continue;
            }
            char name[4] = {'s', 'd', 'a', '\0'};
            name[2] = (char)('a' + (char)i);
            n = mk_dev(dev, name, (dev_kind_t)(DEV_SDA + i), true);
            if (n) {
                probe_partitions(dev, (dev_kind_t)(DEV_SDA + i), name);
            }
        }
        uint32_t nvme_cnt = nvme_disk_count();
        if (nvme_cnt > (uint32_t)NVME_DEV_MAX) {
            nvme_cnt = (uint32_t)NVME_DEV_MAX;
        }
        for (uint32_t i = 0; i < nvme_cnt; i++) {
            char name[INODE_NAME_MAX + 1];
            if (!nvme_disk_present(i) || !make_nvme_name(i, name, sizeof(name))) {
                continue;
            }
            n = mk_dev(dev, name, (dev_kind_t)(DEV_NVME0N1 + i), true);
            if (n) {
                probe_partitions(dev, (dev_kind_t)(DEV_NVME0N1 + i), name);
            }
        }
    }

    if (fs_lookup("/dev/vda1")) {
        (void)fs_mount("/dev/vda1", "/mnt", "ext4", 0);
    } else if (fs_lookup("/dev/vda")) {
        (void)fs_mount("/dev/vda", "/mnt", "ext4", 0);
    } else if (fs_lookup("/dev/sda1")) {
        (void)fs_mount("/dev/sda1", "/mnt", "ext4", 0);
    } else if (fs_lookup("/dev/sda")) {
        (void)fs_mount("/dev/sda", "/mnt", "ext4", 0);
    } else if (fs_lookup("/dev/nvme0n1p1")) {
        (void)fs_mount("/dev/nvme0n1p1", "/mnt", "ext4", 0);
    } else if (fs_lookup("/dev/nvme0n1")) {
        (void)fs_mount("/dev/nvme0n1", "/mnt", "ext4", 0);
    }
}

inode_t *fs_lookup(const char *path) {
    return fs_lookup_internal(path, true);
}

inode_t *fs_lookup_nofollow(const char *path) {
    return fs_lookup_internal(path, false);
}

static inode_t *lookup_parent(const char *path, char *leaf_out) {
    if (!path || path[0] != '/') {
        return 0;
    }
    const char *last = path;
    const char *p = path;
    while (*p) {
        if (*p == '/') {
            last = p;
        }
        p++;
    }

    if (last == path) {
        strcpy(leaf_out, path + 1);
        return root_inode;
    }

    size_t parent_len = (size_t)(last - path);
    if (parent_len == 0 || parent_len >= MAX_PATH) {
        return 0;
    }

    char parent_path[MAX_PATH];
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';
    strcpy(leaf_out, last + 1);

    return fs_lookup(parent_path);
}

bool fs_is_tty(const inode_t *inode) {
    return inode && inode->type == INODE_DEV && inode->dev_kind == DEV_TTY;
}

static uint32_t fs_mode_from_inode(const inode_t *inode) {
    uint32_t perms = 0;
    if (!inode) {
        return 0;
    }

    if (inode->type == INODE_DIR) {
        perms = inode->writable ? 0755U : 0555U;
        return 0x4000U | perms;
    }
    if (inode->type == INODE_FILE) {
        perms = inode->writable ? 0644U : 0444U;
        if (inode->executable) {
            perms |= 0111U;
        }
        return 0x8000U | perms;
    }
    perms = inode->writable ? 0666U : 0444U;
    return 0x2000U | perms;
}

static int fs_fill_stat(inode_t *inode, fu_stat_t *st) {
    if (!inode || !st) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->type = (uint32_t)inode->type;
    st->mode = fs_mode_from_inode(inode);
    st->size = inode->size;
    st->nlink = 1U;
    st->fs_kind = (uint32_t)inode->fs_kind;

    if (inode->fs_kind == FS_KIND_EXT4) {
        return ext4_lstat(inode, st);
    }
    return 0;
}

#define MKFS_EXT4_BLOCK_SIZE             4096U
#define MKFS_EXT4_SECTORS_PER_BLOCK      (MKFS_EXT4_BLOCK_SIZE / VIRTIO_BLK_SECTOR_SIZE)
#define MKFS_EXT4_BLOCKS_PER_GROUP_MAX   (MKFS_EXT4_BLOCK_SIZE * 8U)
#define MKFS_EXT4_INODES_PER_GROUP_MAX   (MKFS_EXT4_BLOCK_SIZE * 8U)
#define MKFS_EXT4_DEFAULT_BYTES_PER_INODE 16384U
#define MKFS_EXT4_SMALL_BYTES_PER_INODE  4096U
#define MKFS_EXT4_LARGEFILE_BYTES_PER_INODE 1048576U
#define MKFS_EXT4_INODE_SIZE             128U
#define MKFS_EXT4_FIRST_INO              11U
#define MKFS_EXT4_JOURNAL_INO            8U

#define MKFS_EXT4_SUPER_OFFSET           1024U
#define MKFS_EXT4_SUPER_MAGIC            0xEF53U
#define MKFS_EXT4_COMPAT_HAS_JOURNAL     0x0004U
#define MKFS_EXT4_INCOMPAT_FILETYPE      0x0002U
#define MKFS_EXT4_INCOMPAT_RECOVER       0x0004U
#define MKFS_EXT4_INCOMPAT_EXTENTS       0x0040U
#define MKFS_EXT4_INCOMPAT_64BIT         0x0080U
#define MKFS_EXT4_INCOMPAT_FLEX_BG       0x0200U
#define MKFS_EXT4_ROCOMPAT_SPARSE_SUPER  0x0001U
#define MKFS_EXT4_ROCOMPAT_LARGE_FILE    0x0002U
#define MKFS_EXT4_ROCOMPAT_BTREE_DIR     0x0004U
#define MKFS_EXT4_ROCOMPAT_HUGE_FILE     0x0008U
#define MKFS_EXT4_ROCOMPAT_GDT_CSUM      0x0010U
#define MKFS_EXT4_ROCOMPAT_DIR_NLINK     0x0020U
#define MKFS_EXT4_ROCOMPAT_EXTRA_ISIZE   0x0040U
#define MKFS_EXT4_ROCOMPAT_BIGALLOC      0x0200U
#define MKFS_EXT4_ROCOMPAT_METADATA_CSUM 0x0400U
#define MKFS_EXT4_EXTENTS_FL             0x00080000U
#define MKFS_EXT4_EXT_MAGIC              0xF30AU

#define MKFS_EXT4_S_INODES_COUNT         0x00U
#define MKFS_EXT4_S_BLOCKS_COUNT_LO      0x04U
#define MKFS_EXT4_S_R_BLOCKS_COUNT_LO    0x08U
#define MKFS_EXT4_S_FREE_BLOCKS_COUNT_LO 0x0CU
#define MKFS_EXT4_S_FREE_INODES_COUNT    0x10U
#define MKFS_EXT4_S_FIRST_DATA_BLOCK     0x14U
#define MKFS_EXT4_S_LOG_BLOCK_SIZE       0x18U
#define MKFS_EXT4_S_BLOCKS_PER_GROUP     0x20U
#define MKFS_EXT4_S_INODES_PER_GROUP     0x28U
#define MKFS_EXT4_S_MAGIC                0x38U
#define MKFS_EXT4_S_STATE                0x3AU
#define MKFS_EXT4_S_ERRORS               0x3CU
#define MKFS_EXT4_S_REV_LEVEL            0x4CU
#define MKFS_EXT4_S_FIRST_INO            0x54U
#define MKFS_EXT4_S_BLOCK_GROUP_NR       0x5AU
#define MKFS_EXT4_S_INODE_SIZE           0x58U
#define MKFS_EXT4_S_FEATURE_COMPAT       0x5CU
#define MKFS_EXT4_S_FEATURE_INCOMPAT     0x60U
#define MKFS_EXT4_S_FEATURE_RO_COMPAT    0x64U
#define MKFS_EXT4_S_UUID                 0x68U
#define MKFS_EXT4_S_VOLUME_NAME          0x78U
#define MKFS_EXT4_S_LAST_MOUNTED         0x88U
#define MKFS_EXT4_S_JOURNAL_INUM         0xE0U
#define MKFS_EXT4_S_JOURNAL_DEV          0xE4U
#define MKFS_EXT4_S_LAST_ORPHAN          0xE8U
#define MKFS_EXT4_S_JNL_BLOCKS           0x10CU
#define MKFS_EXT4_S_DESC_SIZE            0xFEU
#define MKFS_EXT4_S_BLOCKS_COUNT_HI      0x150U
#define MKFS_EXT4_S_R_BLOCKS_COUNT_HI    0x154U
#define MKFS_EXT4_S_FREE_BLOCKS_COUNT_HI 0x158U
#define MKFS_EXT4_S_RAID_STRIDE          0x164U
#define MKFS_EXT4_S_CHECKSUM_TYPE        0x175U
#define MKFS_EXT4_S_CHECKSUM             0x3FCU

#define MKFS_EXT4_BG_BLOCK_BITMAP_LO     0x00U
#define MKFS_EXT4_BG_INODE_BITMAP_LO     0x04U
#define MKFS_EXT4_BG_INODE_TABLE_LO      0x08U
#define MKFS_EXT4_BG_FREE_BLOCKS_LO      0x0CU
#define MKFS_EXT4_BG_FREE_INODES_LO      0x0EU
#define MKFS_EXT4_BG_USED_DIRS_LO        0x10U
#define MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_LO 0x18U
#define MKFS_EXT4_BG_INODE_BITMAP_CSUM_LO 0x1AU
#define MKFS_EXT4_BG_ITABLE_UNUSED_LO    0x1CU
#define MKFS_EXT4_BG_CHECKSUM            0x1EU
#define MKFS_EXT4_BG_BLOCK_BITMAP_HI     0x20U
#define MKFS_EXT4_BG_INODE_BITMAP_HI     0x24U
#define MKFS_EXT4_BG_INODE_TABLE_HI      0x28U
#define MKFS_EXT4_BG_FREE_BLOCKS_HI      0x2CU
#define MKFS_EXT4_BG_FREE_INODES_HI      0x2EU
#define MKFS_EXT4_BG_USED_DIRS_HI        0x30U
#define MKFS_EXT4_BG_ITABLE_UNUSED_HI    0x32U
#define MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_HI 0x38U
#define MKFS_EXT4_BG_INODE_BITMAP_CSUM_HI 0x3AU

#define MKFS_EXT4_INODE_MODE             0x00U
#define MKFS_EXT4_INODE_SIZE_LO          0x04U
#define MKFS_EXT4_INODE_DTIME            0x14U
#define MKFS_EXT4_INODE_LINKS_COUNT      0x1AU
#define MKFS_EXT4_INODE_BLOCKS_LO        0x1CU
#define MKFS_EXT4_INODE_FLAGS            0x20U
#define MKFS_EXT4_INODE_BLOCK            0x28U
#define MKFS_EXT4_INODE_BLOCK_IND1       (MKFS_EXT4_INODE_BLOCK + 12U * 4U)
#define MKFS_EXT4_INODE_BLOCK_IND2       (MKFS_EXT4_INODE_BLOCK + 13U * 4U)
#define MKFS_EXT4_INODE_BLOCK_IND3       (MKFS_EXT4_INODE_BLOCK + 14U * 4U)
#define MKFS_EXT4_EXT_HDR_MAGIC          0x00U
#define MKFS_EXT4_EXT_HDR_ENTRIES        0x02U
#define MKFS_EXT4_EXT_HDR_MAX            0x04U
#define MKFS_EXT4_EXT_HDR_DEPTH          0x06U
#define MKFS_EXT4_EXT_EE_BLOCK           0x0CU
#define MKFS_EXT4_EXT_EE_LEN             0x10U
#define MKFS_EXT4_EXT_EE_START_HI        0x12U
#define MKFS_EXT4_EXT_EE_START_LO        0x14U
#define MKFS_EXT4_DIR_FT_DIR             2U
#define MKFS_EXT4_CSUM_TYPE_CRC32C       1U

#define MKFS_EXT4_S_IFMT                 0xF000U
#define MKFS_EXT4_S_IFDIR                0x4000U

#define MKFS_JBD2_MAGIC                  0xC03B3998U
#define MKFS_JBD2_SUPERBLOCK_V2          4U
#define MKFS_JBD2_FEATURE_INCOMPAT_REVOKE 0x00000001U
#define MKFS_JBD2_FEATURE_INCOMPAT_64BIT  0x00000002U
#define MKFS_JBD2_FEATURE_INCOMPAT_CSUM_V2 0x00000008U
#define MKFS_JBD2_FEATURE_INCOMPAT_CSUM_V3 0x00000010U
#define MKFS_JBD2_CSUM_TYPE_CRC32C       4U

#define MKFS_JBD2_HDR_MAGIC_OFF          0U
#define MKFS_JBD2_HDR_TYPE_OFF           4U
#define MKFS_JBD2_HDR_SEQ_OFF            8U
#define MKFS_JBD2_SB_BLOCKSIZE_OFF       12U
#define MKFS_JBD2_SB_MAXLEN_OFF          16U
#define MKFS_JBD2_SB_FIRST_OFF           20U
#define MKFS_JBD2_SB_SEQUENCE_OFF        24U
#define MKFS_JBD2_SB_START_OFF           28U
#define MKFS_JBD2_SB_FEAT_COMPAT_OFF     36U
#define MKFS_JBD2_SB_FEAT_INCOMPAT_OFF   40U
#define MKFS_JBD2_SB_CHECKSUM_TYPE_OFF   80U
#define MKFS_JBD2_SB_NUM_FC_BLKS_OFF     84U
#define MKFS_JBD2_SB_HEAD_OFF            88U

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint32_t group_blocks;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint16_t used_dirs;
} mkfs_ext4_group_t;

static void mkfs_set_bit(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static uint32_t mkfs_crc32c_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = ~crc;
    while (len-- > 0U) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c & 1U) ? ((c >> 1) ^ 0x82F63B78U) : (c >> 1);
        }
    }
    return ~c;
}

static void mkfs_fill_uuid(uint8_t uuid[16]) {
    uint64_t t0 = timer_ticks();
    uint64_t t1 = timer_ticks() ^ 0xA5A5A5A55A5A5A5AULL;
    for (int i = 0; i < 8; i++) {
        uuid[i] = (uint8_t)((t0 >> (i * 8)) & 0xFFU);
        uuid[i + 8] = (uint8_t)((t1 >> (i * 8)) & 0xFFU);
    }
    uuid[6] = (uint8_t)((uuid[6] & 0x0FU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3FU) | 0x80U);
}

static int mkfs_write_block(uint32_t fs_block, const uint8_t *buf) {
    uint64_t lba = (uint64_t)fs_block * (uint64_t)MKFS_EXT4_SECTORS_PER_BLOCK;
    return block_cache_write(lba, buf, MKFS_EXT4_SECTORS_PER_BLOCK);
}

static int mkfs_zero_block(uint32_t fs_block) {
    static uint8_t zero[MKFS_EXT4_BLOCK_SIZE];
    return mkfs_write_block(fs_block, zero);
}

static bool mkfs_is_power_of(uint32_t n, uint32_t base) {
    if (n == 0U || base < 2U) {
        return false;
    }
    while (n > 1U) {
        if ((n % base) != 0U) {
            return false;
        }
        n /= base;
    }
    return true;
}

static bool mkfs_group_has_super(uint32_t group, bool sparse_super) {
    if (!sparse_super) {
        return true;
    }
    if (group == 0U || group == 1U) {
        return true;
    }
    return mkfs_is_power_of(group, 3U) ||
           mkfs_is_power_of(group, 5U) ||
           mkfs_is_power_of(group, 7U);
}

static bool mkfs_group_layout(uint32_t group,
                              uint32_t groups_count,
                              uint32_t blocks_per_group,
                              uint32_t inodes_per_group,
                              uint32_t inode_table_blocks,
                              uint32_t group0_extra_used,
                              uint64_t total_blocks,
                              uint32_t gdt_blocks,
                              bool sparse_super,
                              uint32_t *group_blocks_out,
                              uint32_t *block_bitmap_out,
                              uint32_t *inode_bitmap_out,
                              uint32_t *inode_table_out,
                              uint32_t *used_blocks_out,
                              uint32_t *free_blocks_out,
                              uint32_t *free_inodes_out,
                              uint16_t *used_dirs_out,
                              bool *has_super_out) {
    if (!group_blocks_out || !block_bitmap_out || !inode_bitmap_out || !inode_table_out ||
        !used_blocks_out || !free_blocks_out || !free_inodes_out ||
        !used_dirs_out || !has_super_out || group >= groups_count) {
        return false;
    }

    uint64_t group_start = (uint64_t)group * (uint64_t)blocks_per_group;
    if (group_start >= total_blocks) {
        return false;
    }
    uint64_t remain_blocks = total_blocks - group_start;
    uint32_t group_blocks = remain_blocks > (uint64_t)blocks_per_group
                          ? blocks_per_group
                          : (uint32_t)remain_blocks;
    bool has_super = mkfs_group_has_super(group, sparse_super);
    uint32_t meta_lead = has_super ? (1U + gdt_blocks) : 0U;

    uint32_t bb = (uint32_t)group_start + meta_lead;
    uint32_t ib = bb + 1U;
    uint32_t it = ib + 1U;
    uint32_t used = meta_lead + 2U + inode_table_blocks;
    if (group == 0U) {
        used += group0_extra_used;
    }
    if (used > group_blocks) {
        return false;
    }
    *group_blocks_out = group_blocks;
    *block_bitmap_out = bb;
    *inode_bitmap_out = ib;
    *inode_table_out = it;
    *used_blocks_out = used;
    *free_blocks_out = group_blocks - used;
    *free_inodes_out = inodes_per_group - (group == 0U ? MKFS_EXT4_FIRST_INO : 0U);
    *used_dirs_out = (group == 0U) ? 2U : 0U;
    *has_super_out = has_super;
    return true;
}

static uint32_t mkfs_profile_bytes_per_inode(uint16_t profile) {
    switch (profile) {
        case MKFS_EXT4_PROFILE_SMALL:
            return MKFS_EXT4_SMALL_BYTES_PER_INODE;
        case MKFS_EXT4_PROFILE_LARGEFILE:
            return MKFS_EXT4_LARGEFILE_BYTES_PER_INODE;
        default:
            return MKFS_EXT4_DEFAULT_BYTES_PER_INODE;
    }
}

static bool fsck_bit_is_set(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

static uint64_t fsck_group_block_ref(const uint8_t *gd, bool has_64bit, uint32_t desc_size,
                                     uint32_t lo_off, uint32_t hi_off) {
    uint64_t v = rd_le32(gd + lo_off);
    if (has_64bit && desc_size >= 64U) {
        v |= ((uint64_t)rd_le32(gd + hi_off) << 32);
    }
    return v;
}

static uint32_t fsck_group_count16(const uint8_t *gd, bool has_64bit, uint32_t desc_size,
                                   uint32_t lo_off, uint32_t hi_off) {
    uint32_t v = rd_le16(gd + lo_off);
    if (has_64bit && desc_size >= 64U) {
        v |= ((uint32_t)rd_le16(gd + hi_off) << 16);
    }
    return v;
}

static bool fsck_read_fs_block(uint64_t block, uint32_t sectors_per_block, uint8_t *buf) {
    return block_cache_read(block * (uint64_t)sectors_per_block, buf, sectors_per_block) == 0;
}

static void fsck_bit_set(uint8_t *bitmap, uint64_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static bool fsck_dynamic_bit_is_set(const uint8_t *bitmap, uint64_t bit) {
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0U;
}

typedef struct {
    uint32_t inodes_count;
    uint64_t blocks_count;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t first_ino;
    uint32_t journal_inum;
    bool has_filetype;
    uint8_t *inode_alloc_map;
    uint8_t *block_alloc_map;
    uint8_t *seen_data_blocks;
    uint16_t *inode_modes;
    uint16_t *inode_links_disk;
    uint32_t *inode_links_refs;
    uint32_t *inode_parent;
    uint8_t *inode_dot_flags;
} fsck_state_t;

typedef struct {
    fsck_state_t *st;
    uint32_t ino;
    bool parse_dir;
} fsck_inode_visit_ctx_t;

static bool fsck_is_dir_mode(uint16_t mode) {
    return (mode & MKFS_EXT4_S_IFMT) == MKFS_EXT4_S_IFDIR;
}

static bool fsck_name_eq(const uint8_t *name, uint8_t len, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)len != n) {
        return false;
    }
    return memcmp(name, lit, n) == 0;
}

static uint16_t fsck_group_desc_checksum(const uint8_t *gd, uint32_t desc_size,
                                         uint32_t crc_seed, uint32_t group) {
    uint8_t tmp[256];
    uint8_t g_le[4];
    uint32_t crc;
    if (desc_size == 0U || desc_size > sizeof(tmp)) {
        return 0U;
    }
    memcpy(tmp, gd, desc_size);
    if (desc_size >= MKFS_EXT4_BG_CHECKSUM + 2U) {
        tmp[MKFS_EXT4_BG_CHECKSUM] = 0U;
        tmp[MKFS_EXT4_BG_CHECKSUM + 1U] = 0U;
    }
    wr_le32(g_le, group);
    crc = mkfs_crc32c_update(crc_seed, g_le, sizeof(g_le));
    crc = mkfs_crc32c_update(crc, tmp, desc_size);
    return (uint16_t)(crc & 0xFFFFU);
}

static uint32_t fsck_bitmap_checksum32(uint32_t crc_seed, uint32_t group, const uint8_t *bitmap,
                                       uint32_t bytes) {
    uint8_t g_le[4];
    uint32_t crc;
    wr_le32(g_le, group);
    crc = mkfs_crc32c_update(crc_seed, g_le, sizeof(g_le));
    return mkfs_crc32c_update(crc, bitmap, bytes);
}

static bool fsck_write_inode_links(uint32_t ino, uint16_t links,
                                   uint32_t inodes_per_group, uint32_t inode_size,
                                   uint32_t block_size, uint32_t sectors_per_block,
                                   const uint64_t *inode_tables, uint8_t *blk,
                                   uint32_t *blk_no_io) {
    if (!inode_tables || !blk || !blk_no_io || ino == 0U) {
        return false;
    }
    uint32_t idx = ino - 1U;
    uint32_t group = idx / inodes_per_group;
    uint32_t in_group = idx % inodes_per_group;
    uint64_t it = inode_tables[group];
    uint32_t byte_off = in_group * inode_size;
    uint32_t inode_blk = (uint32_t)it + (byte_off / block_size);
    uint32_t inode_off = byte_off % block_size;
    if (inode_blk != *blk_no_io) {
        if (!fsck_read_fs_block(inode_blk, sectors_per_block, blk)) {
            return false;
        }
        *blk_no_io = inode_blk;
    }
    wr_le16(blk + inode_off + MKFS_EXT4_INODE_LINKS_COUNT, links);
    return block_cache_write((uint64_t)inode_blk * sectors_per_block, blk, sectors_per_block) == 0;
}

static bool fsck_mark_data_block(fsck_state_t *st, uint32_t ino, uint64_t fs_block) {
    if (!st || fs_block == 0U || fs_block >= st->blocks_count) {
        uart_puts("[fsck.ext4] fail: inode block out of range ino=");
        print_dec((int)ino);
        uart_puts(" blk=");
        print_dec((int)fs_block);
        uart_puts("\n");
        return false;
    }
    if (!fsck_dynamic_bit_is_set(st->block_alloc_map, fs_block)) {
        uart_puts("[fsck.ext4] fail: inode block not allocated in bitmap ino=");
        print_dec((int)ino);
        uart_puts(" blk=");
        print_dec((int)fs_block);
        uart_puts("\n");
        return false;
    }
    if (fsck_dynamic_bit_is_set(st->seen_data_blocks, fs_block)) {
        uart_puts("[fsck.ext4] fail: duplicate data block ino=");
        print_dec((int)ino);
        uart_puts(" blk=");
        print_dec((int)fs_block);
        uart_puts("\n");
        return false;
    }
    fsck_bit_set(st->seen_data_blocks, fs_block);
    return true;
}

static bool fsck_scan_dir_block(fsck_state_t *st, uint32_t dir_ino, const uint8_t *blk) {
    uint32_t off = 0U;
    if (!st || !blk) {
        return false;
    }
    while (off < st->block_size) {
        const uint8_t *de = blk + off;
        uint32_t ent_ino = rd_le32(de + 0U);
        uint16_t rec_len = rd_le16(de + 4U);
        uint8_t name_len = de[6];
        if (rec_len < 8U || (rec_len & 3U) != 0U || off + rec_len > st->block_size) {
            uart_puts("[fsck.ext4] fail: bad dirent rec_len ino=");
            print_dec((int)dir_ino);
            uart_puts("\n");
            return false;
        }
        if (name_len > (uint8_t)(rec_len - 8U)) {
            uart_puts("[fsck.ext4] fail: bad dirent name len ino=");
            print_dec((int)dir_ino);
            uart_puts("\n");
            return false;
        }
        if (ent_ino != 0U) {
            if (ent_ino > st->inodes_count) {
                uart_puts("[fsck.ext4] fail: dirent inode out of range ino=");
                print_dec((int)dir_ino);
                uart_puts("\n");
                return false;
            }
            if (!fsck_dynamic_bit_is_set(st->inode_alloc_map, ent_ino)) {
                uart_puts("[fsck.ext4] fail: dirent points to free inode dir=");
                print_dec((int)dir_ino);
                uart_puts(" target=");
                print_dec((int)ent_ino);
                uart_puts("\n");
                return false;
            }
            st->inode_links_refs[ent_ino]++;
            const uint8_t *name = de + 8U;
            if (fsck_name_eq(name, name_len, ".")) {
                st->inode_dot_flags[dir_ino] |= 0x1U;
                if (ent_ino != dir_ino) {
                    uart_puts("[fsck.ext4] fail: '.' points wrong inode dir=");
                    print_dec((int)dir_ino);
                    uart_puts("\n");
                    return false;
                }
            } else if (fsck_name_eq(name, name_len, "..")) {
                st->inode_dot_flags[dir_ino] |= 0x2U;
                if (st->inode_parent[dir_ino] == 0U) {
                    st->inode_parent[dir_ino] = ent_ino;
                } else if (st->inode_parent[dir_ino] != ent_ino) {
                    uart_puts("[fsck.ext4] fail: conflicting '..' dir=");
                    print_dec((int)dir_ino);
                    uart_puts("\n");
                    return false;
                }
            }
        }
        off += rec_len;
    }
    return true;
}

typedef bool (*fsck_block_visit_fn)(uint64_t fs_block, void *opaque);

static bool fsck_extent_walk_node(const uint8_t *node, uint32_t node_bytes, uint16_t expect_depth,
                                  uint64_t blocks_count, uint32_t block_size, uint32_t sectors_per_block,
                                  fsck_block_visit_fn visit, void *opaque, uint32_t ino) {
    enum {
        EXT_EE_BLOCK_OFF = 0U,
        EXT_EE_LEN_OFF = 4U,
        EXT_EE_START_HI_OFF = 6U,
        EXT_EE_START_LO_OFF = 8U,
        EXT_EI_BLOCK_OFF = 0U,
        EXT_EI_LEAF_LO_OFF = 4U,
        EXT_EI_LEAF_HI_OFF = 8U,
    };
    uint16_t eh_magic, eh_entries, eh_max, eh_depth;
    uint32_t prev_lblk = 0U;
    bool have_prev = false;
    if (!node || node_bytes < 12U || !visit) {
        return false;
    }
    eh_magic = rd_le16(node + MKFS_EXT4_EXT_HDR_MAGIC);
    eh_entries = rd_le16(node + MKFS_EXT4_EXT_HDR_ENTRIES);
    eh_max = rd_le16(node + MKFS_EXT4_EXT_HDR_MAX);
    eh_depth = rd_le16(node + MKFS_EXT4_EXT_HDR_DEPTH);
    if (eh_magic != MKFS_EXT4_EXT_MAGIC || eh_depth != expect_depth) {
        uart_puts("[fsck.ext4] fail: bad extent header ino=");
        print_dec((int)ino);
        uart_puts("\n");
        return false;
    }
    uint16_t cap = (uint16_t)((node_bytes - 12U) / 12U);
    if (eh_max == 0U || eh_max > cap || eh_entries > eh_max) {
        uart_puts("[fsck.ext4] fail: bad extent capacity ino=");
        print_dec((int)ino);
        uart_puts("\n");
        return false;
    }
    if (eh_depth == 0U) {
        for (uint16_t i = 0; i < eh_entries; i++) {
            const uint8_t *ex = node + 12U + (uint32_t)i * 12U;
            uint32_t lblock = rd_le32(ex + EXT_EE_BLOCK_OFF);
            uint16_t elen = rd_le16(ex + EXT_EE_LEN_OFF) & 0x7FFFU;
            uint64_t pstart = ((uint64_t)rd_le16(ex + EXT_EE_START_HI_OFF) << 32) |
                              (uint64_t)rd_le32(ex + EXT_EE_START_LO_OFF);
            if (elen == 0U || pstart == 0U || pstart + (uint64_t)elen > blocks_count) {
                uart_puts("[fsck.ext4] fail: bad extent range ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return false;
            }
            if (have_prev && lblock <= prev_lblk) {
                uart_puts("[fsck.ext4] fail: non-monotonic extent map ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return false;
            }
            have_prev = true;
            prev_lblk = lblock;
            for (uint16_t j = 0; j < elen; j++) {
                if (!visit(pstart + (uint64_t)j, opaque)) {
                    return false;
                }
            }
        }
        return true;
    }

    for (uint16_t i = 0; i < eh_entries; i++) {
        uint8_t child[MKFS_EXT4_BLOCK_SIZE];
        const uint8_t *ix = node + 12U + (uint32_t)i * 12U;
        uint32_t lblock = rd_le32(ix + EXT_EI_BLOCK_OFF);
        uint64_t child_blk = ((uint64_t)rd_le16(ix + EXT_EI_LEAF_HI_OFF) << 32) |
                             (uint64_t)rd_le32(ix + EXT_EI_LEAF_LO_OFF);
        if (child_blk == 0U || child_blk >= blocks_count) {
            uart_puts("[fsck.ext4] fail: bad extent index block ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return false;
        }
        if (have_prev && lblock <= prev_lblk) {
            uart_puts("[fsck.ext4] fail: non-monotonic extent index ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return false;
        }
        have_prev = true;
        prev_lblk = lblock;
        if (!fsck_read_fs_block(child_blk, sectors_per_block, child)) {
            uart_puts("[fsck.ext4] fail: extent node read ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return false;
        }
        if (!fsck_extent_walk_node(child, block_size, (uint16_t)(expect_depth - 1U),
                                   blocks_count, block_size, sectors_per_block,
                                   visit, opaque, ino)) {
            return false;
        }
    }
    return true;
}

static bool fsck_iter_inode_data_blocks(const uint8_t *raw, fsck_state_t *st,
                                        fsck_block_visit_fn visit, void *opaque, uint32_t ino) {
    uint32_t flags;
    if (!raw || !st || !visit) {
        return false;
    }
    flags = rd_le32(raw + MKFS_EXT4_INODE_FLAGS);
    if ((flags & MKFS_EXT4_EXTENTS_FL) != 0U) {
        uint16_t depth = rd_le16(raw + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_DEPTH);
        return fsck_extent_walk_node(raw + MKFS_EXT4_INODE_BLOCK, 60U, depth,
                                     st->blocks_count, st->block_size, st->sectors_per_block,
                                     visit, opaque, ino);
    }
    if (rd_le32(raw + MKFS_EXT4_INODE_BLOCK_IND1) != 0U ||
        rd_le32(raw + MKFS_EXT4_INODE_BLOCK_IND2) != 0U ||
        rd_le32(raw + MKFS_EXT4_INODE_BLOCK_IND3) != 0U) {
        uart_puts("[fsck.ext4] fail: indirect blocks unsupported ino=");
        print_dec((int)ino);
        uart_puts("\n");
        return false;
    }
    for (uint32_t i = 0; i < 12U; i++) {
        uint64_t blk = rd_le32(raw + MKFS_EXT4_INODE_BLOCK + i * 4U);
        if (blk == 0U) {
            continue;
        }
        if (!visit(blk, opaque)) {
            return false;
        }
    }
    return true;
}

static bool fsck_visit_inode_block(uint64_t fs_block, void *opaque) {
    fsck_inode_visit_ctx_t *ctx = (fsck_inode_visit_ctx_t *)opaque;
    uint8_t blk[MKFS_EXT4_BLOCK_SIZE];
    if (!ctx || !ctx->st) {
        return false;
    }
    if (!fsck_mark_data_block(ctx->st, ctx->ino, fs_block)) {
        return false;
    }
    if (!ctx->parse_dir) {
        return true;
    }
    if (!fsck_read_fs_block(fs_block, ctx->st->sectors_per_block, blk)) {
        uart_puts("[fsck.ext4] fail: directory block read ino=");
        print_dec((int)ctx->ino);
        uart_puts("\n");
        return false;
    }
    return fsck_scan_dir_block(ctx->st, ctx->ino, blk);
}

static int fsck_compat_class(uint32_t compat, uint32_t incompat, uint32_t ro_compat,
                             uint32_t *unsupported_incompat_out, uint32_t *unsupported_ro_out) {
    uint32_t supported_incompat = MKFS_EXT4_INCOMPAT_FILETYPE |
                                  MKFS_EXT4_INCOMPAT_RECOVER |
                                  MKFS_EXT4_INCOMPAT_EXTENTS |
                                  MKFS_EXT4_INCOMPAT_64BIT |
                                  MKFS_EXT4_INCOMPAT_FLEX_BG;
    uint32_t supported_ro = MKFS_EXT4_ROCOMPAT_SPARSE_SUPER |
                            MKFS_EXT4_ROCOMPAT_LARGE_FILE |
                            MKFS_EXT4_ROCOMPAT_BTREE_DIR |
                            MKFS_EXT4_ROCOMPAT_HUGE_FILE |
                            MKFS_EXT4_ROCOMPAT_GDT_CSUM |
                            MKFS_EXT4_ROCOMPAT_DIR_NLINK |
                            MKFS_EXT4_ROCOMPAT_EXTRA_ISIZE |
                            MKFS_EXT4_ROCOMPAT_METADATA_CSUM;
    uint32_t unsupported_incompat = incompat & ~supported_incompat;
    uint32_t unsupported_ro = ro_compat & ~supported_ro;
    if (unsupported_incompat_out) {
        *unsupported_incompat_out = unsupported_incompat;
    }
    if (unsupported_ro_out) {
        *unsupported_ro_out = unsupported_ro;
    }
    if (unsupported_incompat != 0U) {
        return -1;
    }
    (void)compat;
    if (unsupported_ro != 0U ||
        (ro_compat & (MKFS_EXT4_ROCOMPAT_GDT_CSUM | MKFS_EXT4_ROCOMPAT_METADATA_CSUM)) != 0U) {
        return 1; /* ro-only */
    }
    return 0; /* rw-ok */
}

int fs_mkfs_ext4(const char *target, uint64_t flags, const fu_mkfs_ext4_opts_t *opts) {
    if (!target || flags != 0U) {
        return -1;
    }

    inode_t *dev = fs_lookup(target);
    if (!dev || dev->type != INODE_DEV || !dev_kind_is_block(dev->dev_kind) ||
        !dev_block_ready_kind(dev->dev_kind)) {
        return -1;
    }
    if (mount_uses_source_dev(dev->dev_kind)) {
        return -1;
    }
    if (block_cache_attach_inode(dev) != 0) {
        return -1;
    }

    uint32_t feature_flags = MKFS_EXT4_FEAT_EXTENTS |
                             MKFS_EXT4_FEAT_SPARSE_SUPER |
                             MKFS_EXT4_FEAT_HAS_JOURNAL;
    uint16_t reserved_pct = 0U;
    uint16_t stride = 0U;
    uint16_t profile = MKFS_EXT4_PROFILE_DEFAULT;
    uint16_t inode_size = MKFS_EXT4_INODE_SIZE;
    uint32_t bytes_per_inode = mkfs_profile_bytes_per_inode(profile);
    uint32_t journal_blocks_req = 0U;
    uint8_t uuid[16];
    char label[16];
    memset(label, 0, sizeof(label));
    memcpy(label, "FuriOS-ext4", 11U);
    mkfs_fill_uuid(uuid);

    if (opts) {
        if (opts->reserved_pct > 99U) {
            return -1;
        }
        feature_flags = opts->feature_flags;
        reserved_pct = opts->reserved_pct;
        stride = opts->stride;
        if (opts->profile > MKFS_EXT4_PROFILE_LARGEFILE) {
            return -1;
        }
        profile = opts->profile;
        bytes_per_inode = mkfs_profile_bytes_per_inode(profile);
        if (opts->bytes_per_inode != 0U) {
            if (opts->bytes_per_inode < 1024U) {
                return -1;
            }
            bytes_per_inode = opts->bytes_per_inode;
        }
        if (opts->inode_size != 0U && opts->inode_size != MKFS_EXT4_INODE_SIZE) {
            return -1;
        }
        journal_blocks_req = opts->journal_blocks;
        if (opts->opt_flags & MKFS_EXT4_OPT_UUID_SET) {
            memcpy(uuid, opts->uuid, sizeof(uuid));
        }
        if (opts->opt_flags & MKFS_EXT4_OPT_LABEL_SET) {
            memcpy(label, opts->label, sizeof(label));
        }
    }

    bool has_extents = (feature_flags & MKFS_EXT4_FEAT_EXTENTS) != 0U;
    bool has_64bit = (feature_flags & MKFS_EXT4_FEAT_64BIT) != 0U;
    bool has_metadata_csum = (feature_flags & MKFS_EXT4_FEAT_METADATA_CSUM) != 0U;
    bool sparse_super = (feature_flags & MKFS_EXT4_FEAT_SPARSE_SUPER) != 0U;
    bool has_journal = (feature_flags & MKFS_EXT4_FEAT_HAS_JOURNAL) != 0U;

    uint64_t sectors = block_cache_capacity_sectors();
    if (sectors < (uint64_t)MKFS_EXT4_SECTORS_PER_BLOCK * 64U) {
        return -1;
    }
    uint64_t total_blocks = sectors / MKFS_EXT4_SECTORS_PER_BLOCK;
    if (total_blocks < 64U) {
        return -1;
    }
    if (!has_64bit && total_blocks > 0xFFFFFFFFULL) {
        total_blocks = 0xFFFFFFFFULL;
    }
    if (bytes_per_inode == 0U) {
        return -1;
    }

    uint32_t blocks_per_group = MKFS_EXT4_BLOCKS_PER_GROUP_MAX;
    uint64_t groups = (total_blocks + blocks_per_group - 1U) / blocks_per_group;
    if (groups == 0U || groups > 65535U) {
        return -1;
    }
    uint32_t groups_count = (uint32_t)groups;

    uint64_t total_bytes = total_blocks * (uint64_t)MKFS_EXT4_BLOCK_SIZE;
    uint64_t target_inodes = (total_bytes + (uint64_t)bytes_per_inode - 1U) / (uint64_t)bytes_per_inode;
    if (target_inodes < (uint64_t)(MKFS_EXT4_FIRST_INO + 1U)) {
        target_inodes = (uint64_t)(MKFS_EXT4_FIRST_INO + 1U);
    }
    if (target_inodes > 0xFFFFFFFFULL) {
        target_inodes = 0xFFFFFFFFULL;
    }
    uint32_t inodes_per_block = MKFS_EXT4_BLOCK_SIZE / inode_size;
    if (inodes_per_block == 0U) {
        return -1;
    }
    uint32_t inodes_per_group = (uint32_t)((target_inodes + groups_count - 1U) / groups_count);
    if (inodes_per_group < (MKFS_EXT4_FIRST_INO + 1U)) {
        inodes_per_group = MKFS_EXT4_FIRST_INO + 1U;
    }
    inodes_per_group = ((inodes_per_group + inodes_per_block - 1U) / inodes_per_block) * inodes_per_block;
    if (inodes_per_group > MKFS_EXT4_INODES_PER_GROUP_MAX) {
        inodes_per_group = MKFS_EXT4_INODES_PER_GROUP_MAX;
    }
    if (inodes_per_group < (MKFS_EXT4_FIRST_INO + 1U)) {
        return -1;
    }
    uint32_t inode_table_blocks = (inodes_per_group * inode_size + MKFS_EXT4_BLOCK_SIZE - 1U) / MKFS_EXT4_BLOCK_SIZE;
    if (inode_table_blocks == 0U) {
        return -1;
    }

    uint32_t journal_blocks = 0U;
    if (has_journal) {
        if (journal_blocks_req != 0U) {
            journal_blocks = journal_blocks_req;
        } else {
            uint64_t auto_journal = total_blocks / 64U;
            if (auto_journal < 64U) {
                auto_journal = 64U;
            }
            if (auto_journal > 4096U) {
                auto_journal = 4096U;
            }
            journal_blocks = (uint32_t)auto_journal;
        }
        if (!has_extents && journal_blocks > 12U) {
            journal_blocks = 12U;
        }
        if (journal_blocks == 0U) {
            return -1;
        }
    }
    uint32_t group0_extra_used = 2U + journal_blocks;

    uint32_t desc_size = (has_64bit || has_metadata_csum) ? 64U : 32U;
    uint32_t gdt_blocks = (groups_count * desc_size + MKFS_EXT4_BLOCK_SIZE - 1U) / MKFS_EXT4_BLOCK_SIZE;
    if (gdt_blocks == 0U) {
        gdt_blocks = 1U;
    }

    uint32_t g0_group_blocks = 0U;
    uint32_t g0_block_bitmap = 0U;
    uint32_t g0_inode_bitmap = 0U;
    uint32_t g0_inode_table = 0U;
    uint32_t g0_used_blocks = 0U;
    uint32_t g0_free_blocks = 0U;
    uint32_t g0_free_inodes = 0U;
    uint16_t g0_used_dirs = 0U;
    bool g0_has_super = false;
    if (!mkfs_group_layout(0U, groups_count, blocks_per_group, inodes_per_group, inode_table_blocks,
                           group0_extra_used, total_blocks, gdt_blocks, sparse_super,
                           &g0_group_blocks, &g0_block_bitmap, &g0_inode_bitmap, &g0_inode_table,
                           &g0_used_blocks, &g0_free_blocks, &g0_free_inodes, &g0_used_dirs,
                           &g0_has_super)) {
        return -1;
    }
    uint32_t root_block = g0_inode_table + inode_table_blocks;
    uint32_t lost_block = root_block + 1U;
    uint32_t journal_first_block = lost_block + 1U;
    if (lost_block >= total_blocks) {
        return -1;
    }
    if (has_journal) {
        uint64_t journal_last = (uint64_t)journal_first_block + (uint64_t)journal_blocks - 1U;
        if (journal_last >= total_blocks) {
            return -1;
        }
        if (!has_extents && journal_blocks > 12U) {
            return -1;
        }
    }

    if ((MKFS_EXT4_JOURNAL_INO - 1U) * inode_size >= MKFS_EXT4_BLOCK_SIZE) {
        return -1;
    }

    uint64_t total_free_blocks = 0U;
    uint64_t total_free_inodes = 0U;
    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t group_blocks = 0U, bb = 0U, ib = 0U, it = 0U, used = 0U, free_blocks = 0U, free_inodes = 0U;
        uint16_t used_dirs = 0U;
        bool has_super = false;
        if (!mkfs_group_layout(g, groups_count, blocks_per_group, inodes_per_group, inode_table_blocks,
                               group0_extra_used, total_blocks, gdt_blocks, sparse_super,
                               &group_blocks, &bb, &ib, &it, &used, &free_blocks, &free_inodes, &used_dirs,
                               &has_super)) {
            return -1;
        }
        (void)group_blocks;
        (void)bb;
        (void)ib;
        (void)it;
        (void)used;
        (void)used_dirs;
        (void)has_super;
        total_free_blocks += free_blocks;
        total_free_inodes += free_inodes;
    }
    uint64_t reserved_blocks = (total_blocks * (uint64_t)reserved_pct) / 100U;
    if (reserved_blocks > total_free_blocks) {
        reserved_blocks = total_free_blocks;
    }

    uint8_t super_block[MKFS_EXT4_BLOCK_SIZE];
    memset(super_block, 0, sizeof(super_block));
    uint8_t *sb = super_block + MKFS_EXT4_SUPER_OFFSET;
    wr_le32(sb + MKFS_EXT4_S_INODES_COUNT, groups_count * inodes_per_group);
    wr_le32(sb + MKFS_EXT4_S_BLOCKS_COUNT_LO, (uint32_t)(total_blocks & 0xFFFFFFFFULL));
    wr_le32(sb + MKFS_EXT4_S_R_BLOCKS_COUNT_LO, (uint32_t)(reserved_blocks & 0xFFFFFFFFULL));
    wr_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_LO, (uint32_t)(total_free_blocks & 0xFFFFFFFFULL));
    wr_le32(sb + MKFS_EXT4_S_FREE_INODES_COUNT, (uint32_t)total_free_inodes);
    wr_le32(sb + MKFS_EXT4_S_FIRST_DATA_BLOCK, 0U);
    wr_le32(sb + MKFS_EXT4_S_LOG_BLOCK_SIZE, 2U);
    wr_le32(sb + MKFS_EXT4_S_BLOCKS_PER_GROUP, blocks_per_group);
    wr_le32(sb + MKFS_EXT4_S_INODES_PER_GROUP, inodes_per_group);
    wr_le16(sb + MKFS_EXT4_S_MAGIC, MKFS_EXT4_SUPER_MAGIC);
    wr_le16(sb + MKFS_EXT4_S_STATE, 1U);
    wr_le16(sb + MKFS_EXT4_S_ERRORS, 1U);
    wr_le32(sb + MKFS_EXT4_S_REV_LEVEL, 1U);
    wr_le32(sb + MKFS_EXT4_S_FIRST_INO, MKFS_EXT4_FIRST_INO);
    wr_le16(sb + MKFS_EXT4_S_INODE_SIZE, inode_size);
    uint32_t incompat = MKFS_EXT4_INCOMPAT_FILETYPE;
    if (has_extents) {
        incompat |= MKFS_EXT4_INCOMPAT_EXTENTS;
    }
    if (has_64bit) {
        incompat |= MKFS_EXT4_INCOMPAT_64BIT;
    }
    uint32_t ro_compat = 0U;
    if (sparse_super) {
        ro_compat |= MKFS_EXT4_ROCOMPAT_SPARSE_SUPER;
    }
    if (has_metadata_csum) {
        ro_compat |= MKFS_EXT4_ROCOMPAT_METADATA_CSUM;
    }
    uint32_t compat = has_journal ? MKFS_EXT4_COMPAT_HAS_JOURNAL : 0U;
    wr_le32(sb + MKFS_EXT4_S_FEATURE_COMPAT, compat);
    wr_le32(sb + MKFS_EXT4_S_FEATURE_INCOMPAT, incompat);
    wr_le32(sb + MKFS_EXT4_S_FEATURE_RO_COMPAT, ro_compat);
    wr_le16(sb + MKFS_EXT4_S_DESC_SIZE, (uint16_t)desc_size);
    wr_le32(sb + MKFS_EXT4_S_BLOCKS_COUNT_HI, (uint32_t)(total_blocks >> 32));
    wr_le32(sb + MKFS_EXT4_S_R_BLOCKS_COUNT_HI, (uint32_t)(reserved_blocks >> 32));
    wr_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_HI, (uint32_t)(total_free_blocks >> 32));
    wr_le16(sb + MKFS_EXT4_S_RAID_STRIDE, stride);
    memcpy(sb + MKFS_EXT4_S_UUID, uuid, sizeof(uuid));
    memcpy(sb + MKFS_EXT4_S_VOLUME_NAME, label, sizeof(label));
    wr_le32(sb + MKFS_EXT4_S_JOURNAL_INUM, has_journal ? MKFS_EXT4_JOURNAL_INO : 0U);
    wr_le32(sb + MKFS_EXT4_S_JOURNAL_DEV, 0U);
    sb[MKFS_EXT4_S_LAST_MOUNTED] = '/';

    uint8_t gd_block[MKFS_EXT4_BLOCK_SIZE];
    uint8_t bbm[MKFS_EXT4_BLOCK_SIZE];
    uint8_t ibm[MKFS_EXT4_BLOCK_SIZE];
    uint32_t desc_per_block = MKFS_EXT4_BLOCK_SIZE / desc_size;
    uint32_t csum_seed = mkfs_crc32c_update(0U, uuid, sizeof(uuid));

    for (uint32_t gb = 0; gb < gdt_blocks; gb++) {
        memset(gd_block, 0, sizeof(gd_block));
        if (mkfs_write_block(1U + gb, gd_block) != 0) {
            return -1;
        }
    }

    uint32_t active_gd_block = 0xFFFFFFFFU;
    bool active_gd_dirty = false;
    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t gd_block_idx = g / desc_per_block;
        uint32_t gd_slot = g % desc_per_block;
        if (active_gd_block != gd_block_idx) {
            if (active_gd_dirty) {
                if (mkfs_write_block(1U + active_gd_block, gd_block) != 0) {
                    return -1;
                }
                active_gd_dirty = false;
            }
            if (!fsck_read_fs_block(1U + gd_block_idx, MKFS_EXT4_SECTORS_PER_BLOCK, gd_block)) {
                return -1;
            }
            active_gd_block = gd_block_idx;
        }

        uint32_t group_blocks = 0U, bb = 0U, ib = 0U, it = 0U, used = 0U, free_blocks = 0U, free_inodes = 0U;
        uint16_t used_dirs = 0U;
        bool has_super = false;
        if (!mkfs_group_layout(g, groups_count, blocks_per_group, inodes_per_group, inode_table_blocks,
                               group0_extra_used, total_blocks, gdt_blocks, sparse_super,
                               &group_blocks, &bb, &ib, &it, &used, &free_blocks, &free_inodes, &used_dirs,
                               &has_super)) {
            return -1;
        }
        (void)has_super;
        uint8_t *gd = gd_block + gd_slot * desc_size;
        wr_le32(gd + MKFS_EXT4_BG_BLOCK_BITMAP_LO, bb);
        wr_le32(gd + MKFS_EXT4_BG_INODE_BITMAP_LO, ib);
        wr_le32(gd + MKFS_EXT4_BG_INODE_TABLE_LO, it);
        wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_LO, (uint16_t)(free_blocks & 0xFFFFU));
        wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_LO, (uint16_t)(free_inodes & 0xFFFFU));
        wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_LO, used_dirs);

        if (desc_size >= 64U) {
            wr_le32(gd + MKFS_EXT4_BG_BLOCK_BITMAP_HI, 0U);
            wr_le32(gd + MKFS_EXT4_BG_INODE_BITMAP_HI, 0U);
            wr_le32(gd + MKFS_EXT4_BG_INODE_TABLE_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_HI, 0U);
        }

        if (has_metadata_csum) {
            memset(bbm, 0, sizeof(bbm));
            for (uint32_t b = 0; b < used; b++) {
                mkfs_set_bit(bbm, b);
            }
            for (uint32_t b = group_blocks; b < blocks_per_group; b++) {
                mkfs_set_bit(bbm, b);
            }
            memset(ibm, 0, sizeof(ibm));
            if (g == 0U) {
                for (uint32_t bi = 0; bi < MKFS_EXT4_FIRST_INO; bi++) {
                    mkfs_set_bit(ibm, bi);
                }
            }
            uint8_t g_le[4];
            wr_le32(g_le, g);
            uint32_t bb_sum = mkfs_crc32c_update(csum_seed, g_le, sizeof(g_le));
            bb_sum = mkfs_crc32c_update(bb_sum, bbm, sizeof(bbm));
            uint32_t ib_sum = mkfs_crc32c_update(csum_seed, g_le, sizeof(g_le));
            ib_sum = mkfs_crc32c_update(ib_sum, ibm, sizeof(ibm));
            wr_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_LO, (uint16_t)(bb_sum & 0xFFFFU));
            wr_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_LO, (uint16_t)(ib_sum & 0xFFFFU));
            wr_le16(gd + MKFS_EXT4_BG_ITABLE_UNUSED_LO, (uint16_t)(free_inodes & 0xFFFFU));
            if (desc_size >= 64U) {
                wr_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_HI, (uint16_t)(bb_sum >> 16));
                wr_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_HI, (uint16_t)(ib_sum >> 16));
                wr_le16(gd + MKFS_EXT4_BG_ITABLE_UNUSED_HI, 0U);
            }
            wr_le16(gd + MKFS_EXT4_BG_CHECKSUM, 0U);
            uint32_t gd_sum = mkfs_crc32c_update(csum_seed, g_le, sizeof(g_le));
            gd_sum = mkfs_crc32c_update(gd_sum, gd, desc_size);
            wr_le16(gd + MKFS_EXT4_BG_CHECKSUM, (uint16_t)(gd_sum & 0xFFFFU));
        }
        active_gd_dirty = true;
    }
    if (active_gd_dirty) {
        if (mkfs_write_block(1U + active_gd_block, gd_block) != 0) {
            return -1;
        }
    }

    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t group_blocks = 0U, bb = 0U, ib = 0U, it = 0U, used = 0U, free_blocks = 0U, free_inodes = 0U;
        uint16_t used_dirs = 0U;
        bool has_super = false;
        if (!mkfs_group_layout(g, groups_count, blocks_per_group, inodes_per_group, inode_table_blocks,
                               group0_extra_used, total_blocks, gdt_blocks, sparse_super,
                               &group_blocks, &bb, &ib, &it, &used, &free_blocks, &free_inodes, &used_dirs,
                               &has_super)) {
            return -1;
        }
        (void)free_blocks;
        (void)free_inodes;
        (void)used_dirs;
        (void)has_super;
        memset(bbm, 0, sizeof(bbm));
        for (uint32_t b = 0; b < used; b++) {
            mkfs_set_bit(bbm, b);
        }
        for (uint32_t b = group_blocks; b < blocks_per_group; b++) {
            mkfs_set_bit(bbm, b);
        }
        if (mkfs_write_block(bb, bbm) != 0) {
            return -1;
        }

        memset(ibm, 0, sizeof(ibm));
        if (g == 0U) {
            for (uint32_t i = 0; i < MKFS_EXT4_FIRST_INO; i++) {
                mkfs_set_bit(ibm, i);
            }
        }
        if (mkfs_write_block(ib, ibm) != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < inode_table_blocks; i++) {
            if (mkfs_zero_block(it + i) != 0) {
                return -1;
            }
        }
    }

    memset(bbm, 0, sizeof(bbm));
    uint8_t *de = bbm;
    wr_le32(de + 0U, 2U);
    wr_le16(de + 4U, 12U);
    de[6] = 1U;
    de[7] = MKFS_EXT4_DIR_FT_DIR;
    de[8] = '.';
    de += 12U;
    wr_le32(de + 0U, 2U);
    wr_le16(de + 4U, 12U);
    de[6] = 2U;
    de[7] = MKFS_EXT4_DIR_FT_DIR;
    de[8] = '.';
    de[9] = '.';
    de += 12U;
    const char *lf = "lost+found";
    wr_le32(de + 0U, MKFS_EXT4_FIRST_INO);
    wr_le16(de + 4U, (uint16_t)(MKFS_EXT4_BLOCK_SIZE - 24U));
    de[6] = (uint8_t)strlen(lf);
    de[7] = MKFS_EXT4_DIR_FT_DIR;
    memcpy(de + 8U, lf, strlen(lf));
    if (mkfs_write_block(root_block, bbm) != 0) {
        return -1;
    }

    memset(bbm, 0, sizeof(bbm));
    de = bbm;
    wr_le32(de + 0U, MKFS_EXT4_FIRST_INO);
    wr_le16(de + 4U, 12U);
    de[6] = 1U;
    de[7] = MKFS_EXT4_DIR_FT_DIR;
    de[8] = '.';
    de += 12U;
    wr_le32(de + 0U, 2U);
    wr_le16(de + 4U, (uint16_t)(MKFS_EXT4_BLOCK_SIZE - 12U));
    de[6] = 2U;
    de[7] = MKFS_EXT4_DIR_FT_DIR;
    de[8] = '.';
    de[9] = '.';
    if (mkfs_write_block(lost_block, bbm) != 0) {
        return -1;
    }

    uint8_t inode_block[MKFS_EXT4_BLOCK_SIZE];
    memset(inode_block, 0, sizeof(inode_block));
    uint8_t *ino_root = inode_block + inode_size;
    wr_le16(ino_root + MKFS_EXT4_INODE_MODE, 0x41EDU);
    wr_le32(ino_root + MKFS_EXT4_INODE_SIZE_LO, MKFS_EXT4_BLOCK_SIZE);
    wr_le16(ino_root + MKFS_EXT4_INODE_LINKS_COUNT, 3U);
    wr_le32(ino_root + MKFS_EXT4_INODE_BLOCKS_LO, MKFS_EXT4_SECTORS_PER_BLOCK);
    if (has_extents) {
        wr_le32(ino_root + MKFS_EXT4_INODE_FLAGS, MKFS_EXT4_EXTENTS_FL);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAGIC, MKFS_EXT4_EXT_MAGIC);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_ENTRIES, 1U);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAX, 4U);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_DEPTH, 0U);
        wr_le32(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_BLOCK, 0U);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_LEN, 1U);
        wr_le16(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_HI, 0U);
        wr_le32(ino_root + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_LO, root_block);
    } else {
        wr_le32(ino_root + MKFS_EXT4_INODE_FLAGS, 0U);
        wr_le32(ino_root + MKFS_EXT4_INODE_BLOCK + 0U, root_block);
    }

    uint8_t *ino_lf = inode_block + (MKFS_EXT4_FIRST_INO - 1U) * inode_size;
    wr_le16(ino_lf + MKFS_EXT4_INODE_MODE, 0x41EDU);
    wr_le32(ino_lf + MKFS_EXT4_INODE_SIZE_LO, MKFS_EXT4_BLOCK_SIZE);
    wr_le16(ino_lf + MKFS_EXT4_INODE_LINKS_COUNT, 2U);
    wr_le32(ino_lf + MKFS_EXT4_INODE_BLOCKS_LO, MKFS_EXT4_SECTORS_PER_BLOCK);
    if (has_extents) {
        wr_le32(ino_lf + MKFS_EXT4_INODE_FLAGS, MKFS_EXT4_EXTENTS_FL);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAGIC, MKFS_EXT4_EXT_MAGIC);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_ENTRIES, 1U);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAX, 4U);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_DEPTH, 0U);
        wr_le32(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_BLOCK, 0U);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_LEN, 1U);
        wr_le16(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_HI, 0U);
        wr_le32(ino_lf + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_LO, lost_block);
    } else {
        wr_le32(ino_lf + MKFS_EXT4_INODE_FLAGS, 0U);
        wr_le32(ino_lf + MKFS_EXT4_INODE_BLOCK + 0U, lost_block);
    }

    uint32_t journal_i_block_words[15];
    memset(journal_i_block_words, 0, sizeof(journal_i_block_words));
    if (has_journal) {
        uint8_t *ino_j = inode_block + (MKFS_EXT4_JOURNAL_INO - 1U) * inode_size;
        wr_le16(ino_j + MKFS_EXT4_INODE_MODE, 0x8180U);
        wr_le16(ino_j + MKFS_EXT4_INODE_LINKS_COUNT, 1U);
        wr_le32(ino_j + MKFS_EXT4_INODE_SIZE_LO, journal_blocks * MKFS_EXT4_BLOCK_SIZE);
        wr_le32(ino_j + MKFS_EXT4_INODE_BLOCKS_LO, journal_blocks * MKFS_EXT4_SECTORS_PER_BLOCK);
        if (has_extents) {
            wr_le32(ino_j + MKFS_EXT4_INODE_FLAGS, MKFS_EXT4_EXTENTS_FL);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAGIC, MKFS_EXT4_EXT_MAGIC);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_ENTRIES, 1U);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_MAX, 4U);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_HDR_DEPTH, 0U);
            wr_le32(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_BLOCK, 0U);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_LEN, (uint16_t)journal_blocks);
            wr_le16(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_HI, 0U);
            wr_le32(ino_j + MKFS_EXT4_INODE_BLOCK + MKFS_EXT4_EXT_EE_START_LO, journal_first_block);
        } else {
            wr_le32(ino_j + MKFS_EXT4_INODE_FLAGS, 0U);
            for (uint32_t i = 0; i < journal_blocks && i < 12U; i++) {
                wr_le32(ino_j + MKFS_EXT4_INODE_BLOCK + i * 4U, journal_first_block + i);
            }
        }
        for (uint32_t i = 0; i < 15U; i++) {
            journal_i_block_words[i] = rd_le32(ino_j + MKFS_EXT4_INODE_BLOCK + i * 4U);
        }
    }

    if (mkfs_write_block(g0_inode_table, inode_block) != 0) {
        return -1;
    }

    if (has_journal) {
        for (uint32_t jb = 0U; jb < journal_blocks; jb++) {
            if (mkfs_zero_block(journal_first_block + jb) != 0) {
                return -1;
            }
        }
        uint8_t jsb[MKFS_EXT4_BLOCK_SIZE];
        memset(jsb, 0, sizeof(jsb));
        wr_be32(jsb + MKFS_JBD2_HDR_MAGIC_OFF, MKFS_JBD2_MAGIC);
        wr_be32(jsb + MKFS_JBD2_HDR_TYPE_OFF, MKFS_JBD2_SUPERBLOCK_V2);
        wr_be32(jsb + MKFS_JBD2_HDR_SEQ_OFF, 1U);
        wr_be32(jsb + MKFS_JBD2_SB_BLOCKSIZE_OFF, MKFS_EXT4_BLOCK_SIZE);
        wr_be32(jsb + MKFS_JBD2_SB_MAXLEN_OFF, journal_blocks);
        wr_be32(jsb + MKFS_JBD2_SB_FIRST_OFF, 1U);
        wr_be32(jsb + MKFS_JBD2_SB_SEQUENCE_OFF, 1U);
        wr_be32(jsb + MKFS_JBD2_SB_START_OFF, 0U);
        wr_be32(jsb + MKFS_JBD2_SB_FEAT_COMPAT_OFF, 0U);
        wr_be32(jsb + MKFS_JBD2_SB_FEAT_INCOMPAT_OFF, MKFS_JBD2_FEATURE_INCOMPAT_REVOKE);
        wr_be32(jsb + MKFS_JBD2_SB_CHECKSUM_TYPE_OFF, 0U);
        wr_be32(jsb + MKFS_JBD2_SB_NUM_FC_BLKS_OFF, 0U);
        wr_be32(jsb + MKFS_JBD2_SB_HEAD_OFF, 1U);
        memcpy(jsb + 48U, uuid, 16U);
        if (mkfs_write_block(journal_first_block, jsb) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < 15U; i++) {
            wr_le32(sb + MKFS_EXT4_S_JNL_BLOCKS + i * 4U, journal_i_block_words[i]);
        }
    }

    if (has_metadata_csum) {
        sb[MKFS_EXT4_S_CHECKSUM_TYPE] = MKFS_EXT4_CSUM_TYPE_CRC32C;
        wr_le32(sb + MKFS_EXT4_S_CHECKSUM, 0U);
        uint32_t seed = mkfs_crc32c_update(0U, uuid, sizeof(uuid));
        uint32_t sum = mkfs_crc32c_update(seed, sb, 1024U);
        wr_le32(sb + MKFS_EXT4_S_CHECKSUM, sum);
    }
    if (mkfs_write_block(0U, super_block) != 0) {
        return -1;
    }

    active_gd_block = 0xFFFFFFFFU;
    active_gd_dirty = false;
    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t gd_block_idx = g / desc_per_block;
        uint32_t gd_slot = g % desc_per_block;
        if (active_gd_block != gd_block_idx) {
            if (active_gd_dirty) {
                if (mkfs_write_block(1U + active_gd_block, gd_block) != 0) {
                    return -1;
                }
                active_gd_dirty = false;
            }
            if (!fsck_read_fs_block(1U + gd_block_idx, MKFS_EXT4_SECTORS_PER_BLOCK, gd_block)) {
                return -1;
            }
            active_gd_block = gd_block_idx;
        }
        uint32_t group_blocks = 0U, bb = 0U, ib = 0U, it = 0U, used = 0U, free_blocks = 0U, free_inodes = 0U;
        uint16_t used_dirs = 0U;
        bool has_super = false;
        if (!mkfs_group_layout(g, groups_count, blocks_per_group, inodes_per_group, inode_table_blocks,
                               group0_extra_used, total_blocks, gdt_blocks, sparse_super,
                               &group_blocks, &bb, &ib, &it, &used, &free_blocks, &free_inodes, &used_dirs,
                               &has_super)) {
            return -1;
        }
        (void)group_blocks;
        (void)used;
        (void)has_super;
        uint8_t *gd = gd_block + gd_slot * desc_size;
        wr_le32(gd + MKFS_EXT4_BG_BLOCK_BITMAP_LO, bb);
        wr_le32(gd + MKFS_EXT4_BG_INODE_BITMAP_LO, ib);
        wr_le32(gd + MKFS_EXT4_BG_INODE_TABLE_LO, it);
        wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_LO, (uint16_t)(free_blocks & 0xFFFFU));
        wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_LO, (uint16_t)(free_inodes & 0xFFFFU));
        wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_LO, used_dirs);
        if (desc_size >= 64U) {
            wr_le32(gd + MKFS_EXT4_BG_BLOCK_BITMAP_HI, 0U);
            wr_le32(gd + MKFS_EXT4_BG_INODE_BITMAP_HI, 0U);
            wr_le32(gd + MKFS_EXT4_BG_INODE_TABLE_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_HI, 0U);
            wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_HI, 0U);
        }
        active_gd_dirty = true;
    }
    if (active_gd_dirty) {
        if (mkfs_write_block(1U + active_gd_block, gd_block) != 0) {
            return -1;
        }
    }

    if (groups_count > 1U) {
        for (uint32_t g = 1U; g < groups_count; g++) {
            if (!mkfs_group_has_super(g, sparse_super)) {
                continue;
            }
            uint32_t group_start = g * blocks_per_group;
            uint8_t backup_super[MKFS_EXT4_BLOCK_SIZE];
            memcpy(backup_super, super_block, sizeof(backup_super));
            uint8_t *bsb = backup_super + MKFS_EXT4_SUPER_OFFSET;
            wr_le16(bsb + MKFS_EXT4_S_BLOCK_GROUP_NR, (uint16_t)g);
            if (has_metadata_csum) {
                wr_le32(bsb + MKFS_EXT4_S_CHECKSUM, 0U);
                uint32_t sum = mkfs_crc32c_update(csum_seed, bsb, 1024U);
                wr_le32(bsb + MKFS_EXT4_S_CHECKSUM, sum);
            }
            if (mkfs_write_block(group_start, backup_super) != 0) {
                return -1;
            }
            for (uint32_t gb = 0U; gb < gdt_blocks; gb++) {
                if (block_cache_read((uint64_t)(1U + gb) * MKFS_EXT4_SECTORS_PER_BLOCK,
                                     gd_block, MKFS_EXT4_SECTORS_PER_BLOCK) != 0) {
                    return -1;
                }
                if (mkfs_write_block(group_start + 1U + gb, gd_block) != 0) {
                    return -1;
                }
            }
        }
    }

    if (block_cache_flush() != 0) {
        return -1;
    }

    uart_puts("[mkfs.ext4] formatted ");
    uart_puts(target);
    uart_puts(" blocks=");
    print_dec((int)total_blocks);
    uart_puts(" groups=");
    print_dec((int)groups_count);
    uart_puts(" feat=");
    if (has_extents) {
        uart_puts("extents,");
    }
    if (has_64bit) {
        uart_puts("64bit,");
    }
    if (has_metadata_csum) {
        uart_puts("metadata_csum,");
    }
    if (sparse_super) {
        uart_puts("sparse_super,");
    }
    if (has_journal) {
        uart_puts("has_journal");
    }
    uart_puts("\n");
    return 0;
}

int fs_fsck_ext4(const char *target, uint64_t flags) {
    bool no_repair = (flags & FSCK_EXT4_F_NO_REPAIR) != 0U;
    bool do_preen = (flags & FSCK_EXT4_F_PREEN) != 0U;
    bool do_yes = (flags & FSCK_EXT4_F_YES) != 0U;
    bool can_repair = false;

    if (!target) {
        uart_puts("[fsck.ext4] fail: bad args\n");
        return -1;
    }
    if ((flags & ~(FSCK_EXT4_F_NO_REPAIR | FSCK_EXT4_F_PREEN | FSCK_EXT4_F_YES)) != 0U) {
        uart_puts("[fsck.ext4] fail: bad flags\n");
        return -1;
    }
    if (no_repair && (do_preen || do_yes)) {
        uart_puts("[fsck.ext4] fail: conflicting fsck mode\n");
        return -1;
    }
    if (do_preen && do_yes) {
        uart_puts("[fsck.ext4] fail: conflicting fsck mode\n");
        return -1;
    }
    can_repair = do_preen || do_yes;

    inode_t *dev = fs_lookup(target);
    if (!dev || dev->type != INODE_DEV || !dev_kind_is_block(dev->dev_kind) ||
        !dev_block_ready_kind(dev->dev_kind)) {
        uart_puts("[fsck.ext4] fail: bad device\n");
        return -1;
    }
    if (mount_uses_source_dev(dev->dev_kind)) {
        uart_puts("[fsck.ext4] fail: device mounted\n");
        return -1;
    }
    if (block_cache_attach_inode(dev) != 0) {
        uart_puts("[fsck.ext4] fail: attach block cache\n");
        return -1;
    }

    static uint8_t blk_a[MKFS_EXT4_BLOCK_SIZE];
    static uint8_t blk_b[MKFS_EXT4_BLOCK_SIZE];
    static uint8_t blk_j[MKFS_EXT4_BLOCK_SIZE];
    static uint8_t bbm[MKFS_EXT4_BLOCK_SIZE];
    static uint8_t ibm[MKFS_EXT4_BLOCK_SIZE];

    if (!fsck_read_fs_block(0U, MKFS_EXT4_SECTORS_PER_BLOCK, blk_a)) {
        uart_puts("[fsck.ext4] fail: read super block0\n");
        return -1;
    }
    uint8_t *sb = blk_a + MKFS_EXT4_SUPER_OFFSET;
    if (rd_le16(sb + MKFS_EXT4_S_MAGIC) != MKFS_EXT4_SUPER_MAGIC) {
        uart_puts("[fsck.ext4] fail: super magic\n");
        return -1;
    }

    uint32_t compat = rd_le32(sb + MKFS_EXT4_S_FEATURE_COMPAT);
    uint32_t incompat = rd_le32(sb + MKFS_EXT4_S_FEATURE_INCOMPAT);
    uint32_t ro_compat = rd_le32(sb + MKFS_EXT4_S_FEATURE_RO_COMPAT);
    uint32_t unsupported_incompat = 0U;
    uint32_t unsupported_ro = 0U;
    int compat_class = fsck_compat_class(compat, incompat, ro_compat,
                                         &unsupported_incompat, &unsupported_ro);
    uart_puts("[fsck.ext4] compat class: ");
    if (compat_class < 0) {
        uart_puts("unsupported (incompat=");
        print_hex64((uint64_t)unsupported_incompat);
        uart_puts(")\n");
        return -1;
    } else if (compat_class > 0) {
        uart_puts("ro-only");
        if (unsupported_ro != 0U) {
            uart_puts(" (unknown_ro=");
            print_hex64((uint64_t)unsupported_ro);
            uart_puts(")");
        }
        uart_puts("\n");
    } else {
        uart_puts("rw-ok\n");
    }
    if ((ro_compat & MKFS_EXT4_ROCOMPAT_BIGALLOC) != 0U) {
        uart_puts("[fsck.ext4] fail: BIGALLOC unsupported\n");
        return -1;
    }

    uint32_t log_block_size = rd_le32(sb + MKFS_EXT4_S_LOG_BLOCK_SIZE);
    if (log_block_size > 2U) {
        uart_puts("[fsck.ext4] fail: log block size\n");
        return -1;
    }
    uint32_t block_size = 1024U << log_block_size;
    if (block_size < 1024U || block_size > MKFS_EXT4_BLOCK_SIZE) {
        uart_puts("[fsck.ext4] fail: block size\n");
        return -1;
    }
    if ((block_size % VIRTIO_BLK_SECTOR_SIZE) != 0U) {
        uart_puts("[fsck.ext4] fail: sector align\n");
        return -1;
    }
    uint32_t sectors_per_block = block_size / VIRTIO_BLK_SECTOR_SIZE;
    uint32_t blocks_per_group = rd_le32(sb + MKFS_EXT4_S_BLOCKS_PER_GROUP);
    uint32_t inodes_per_group = rd_le32(sb + MKFS_EXT4_S_INODES_PER_GROUP);
    uint32_t inodes_count = rd_le32(sb + MKFS_EXT4_S_INODES_COUNT);
    uint32_t inode_size = rd_le16(sb + MKFS_EXT4_S_INODE_SIZE);
    uint32_t first_data_block = rd_le32(sb + MKFS_EXT4_S_FIRST_DATA_BLOCK);
    uint32_t first_ino = rd_le32(sb + MKFS_EXT4_S_FIRST_INO);
    uint32_t desc_size = rd_le16(sb + MKFS_EXT4_S_DESC_SIZE);
    if (desc_size == 0U) {
        desc_size = 32U;
    }
    if (blocks_per_group == 0U || inodes_per_group == 0U || inodes_count == 0U ||
        inode_size < 128U || inode_size > block_size ||
        desc_size < 32U || desc_size > block_size) {
        uart_puts("[fsck.ext4] fail: super geometry\n");
        return -1;
    }
    if (first_ino < MKFS_EXT4_FIRST_INO) {
        first_ino = MKFS_EXT4_FIRST_INO;
    }
    bool has_64bit = (incompat & MKFS_EXT4_INCOMPAT_64BIT) != 0U;
    bool sparse_super = (ro_compat & MKFS_EXT4_ROCOMPAT_SPARSE_SUPER) != 0U;
    bool has_journal = (compat & MKFS_EXT4_COMPAT_HAS_JOURNAL) != 0U;
    bool has_metadata_csum = (ro_compat & MKFS_EXT4_ROCOMPAT_METADATA_CSUM) != 0U;
    bool has_filetype = (incompat & MKFS_EXT4_INCOMPAT_FILETYPE) != 0U;

    uint64_t blocks_count = rd_le32(sb + MKFS_EXT4_S_BLOCKS_COUNT_LO);
    if (has_64bit) {
        blocks_count |= ((uint64_t)rd_le32(sb + MKFS_EXT4_S_BLOCKS_COUNT_HI) << 32);
    }
    if (blocks_count <= first_data_block) {
        uart_puts("[fsck.ext4] fail: blocks count\n");
        return -1;
    }
    uint64_t data_blocks = blocks_count - first_data_block;
    uint32_t groups_count = (uint32_t)((data_blocks + blocks_per_group - 1U) / blocks_per_group);
    if (groups_count == 0U) {
        uart_puts("[fsck.ext4] fail: groups count\n");
        return -1;
    }

    uint64_t super_free_blocks = rd_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_LO);
    if (has_64bit) {
        super_free_blocks |= ((uint64_t)rd_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_HI) << 32);
    }
    uint64_t super_free_inodes = rd_le32(sb + MKFS_EXT4_S_FREE_INODES_COUNT);
    uint32_t journal_inum = rd_le32(sb + MKFS_EXT4_S_JOURNAL_INUM);
    if (has_journal && journal_inum == 0U) {
        uart_puts("[fsck.ext4] fail: missing journal inode number\n");
        return -1;
    }

    uint32_t repaired = 0U;
    bool super_dirty = false;
    uint32_t csum_seed = mkfs_crc32c_update(0U, sb + MKFS_EXT4_S_UUID, 16U);
    if (has_metadata_csum) {
        if (sb[MKFS_EXT4_S_CHECKSUM_TYPE] != MKFS_EXT4_CSUM_TYPE_CRC32C) {
            if (!can_repair) {
                uart_puts("[fsck.ext4] fail: unsupported super checksum type\n");
                return -1;
            }
            sb[MKFS_EXT4_S_CHECKSUM_TYPE] = MKFS_EXT4_CSUM_TYPE_CRC32C;
            super_dirty = true;
            repaired++;
        }
        uint8_t sblk[1024];
        memcpy(sblk, sb, sizeof(sblk));
        uint32_t stored = rd_le32(sblk + MKFS_EXT4_S_CHECKSUM);
        wr_le32(sblk + MKFS_EXT4_S_CHECKSUM, 0U);
        uint32_t calc = mkfs_crc32c_update(csum_seed, sblk, sizeof(sblk));
        if (stored != calc) {
            if (!can_repair) {
                uart_puts("[fsck.ext4] fail: super checksum\n");
                return -1;
            }
            wr_le32(sb + MKFS_EXT4_S_CHECKSUM, calc);
            super_dirty = true;
            repaired++;
        }
    }

    uint32_t last_orphan = rd_le32(sb + MKFS_EXT4_S_LAST_ORPHAN);
    if (last_orphan != 0U) {
        if (!can_repair) {
            uart_puts("[fsck.ext4] fail: orphan list present\n");
            return -1;
        }
        wr_le32(sb + MKFS_EXT4_S_LAST_ORPHAN, 0U);
        super_dirty = true;
        repaired++;
    }

    size_t inode_bits_bytes = (size_t)(((uint64_t)inodes_count + 8ULL) / 8ULL);
    size_t block_bits_bytes = (size_t)((blocks_count + 8ULL) / 8ULL);
    fsck_state_t st;
    memset(&st, 0, sizeof(st));
    st.inodes_count = inodes_count;
    st.blocks_count = blocks_count;
    st.block_size = block_size;
    st.sectors_per_block = sectors_per_block;
    st.first_ino = first_ino;
    st.journal_inum = journal_inum;
    st.has_filetype = has_filetype;
    st.inode_alloc_map = (uint8_t *)pmm_alloc(inode_bits_bytes, 16U);
    st.block_alloc_map = (uint8_t *)pmm_alloc(block_bits_bytes, 16U);
    st.seen_data_blocks = (uint8_t *)pmm_alloc(block_bits_bytes, 16U);
    st.inode_modes = (uint16_t *)pmm_alloc(((size_t)inodes_count + 1U) * sizeof(uint16_t), 8U);
    st.inode_links_disk = (uint16_t *)pmm_alloc(((size_t)inodes_count + 1U) * sizeof(uint16_t), 8U);
    st.inode_links_refs = (uint32_t *)pmm_alloc(((size_t)inodes_count + 1U) * sizeof(uint32_t), 8U);
    st.inode_parent = (uint32_t *)pmm_alloc(((size_t)inodes_count + 1U) * sizeof(uint32_t), 8U);
    st.inode_dot_flags = (uint8_t *)pmm_alloc((size_t)inodes_count + 1U, 8U);
    uint64_t *inode_tables = (uint64_t *)pmm_alloc((size_t)groups_count * sizeof(uint64_t), 8U);
    if (!st.inode_alloc_map || !st.block_alloc_map || !st.seen_data_blocks ||
        !st.inode_modes || !st.inode_links_disk || !st.inode_links_refs ||
        !st.inode_parent || !st.inode_dot_flags || !inode_tables) {
        uart_puts("[fsck.ext4] fail: out of memory\n");
        return -1;
    }
    memset(st.inode_alloc_map, 0, inode_bits_bytes);
    memset(st.block_alloc_map, 0, block_bits_bytes);
    memset(st.seen_data_blocks, 0, block_bits_bytes);
    memset(st.inode_modes, 0, ((size_t)inodes_count + 1U) * sizeof(uint16_t));
    memset(st.inode_links_disk, 0, ((size_t)inodes_count + 1U) * sizeof(uint16_t));
    memset(st.inode_links_refs, 0, ((size_t)inodes_count + 1U) * sizeof(uint32_t));
    memset(st.inode_parent, 0, ((size_t)inodes_count + 1U) * sizeof(uint32_t));
    memset(st.inode_dot_flags, 0, (size_t)inodes_count + 1U);

    uint32_t gdt_base_block = (block_size == 1024U) ? 2U : 1U;
    uint32_t last_gdt_block = 0xFFFFFFFFU;
    bool gdt_dirty = false;
    uint32_t used_inodes = 0U;
    bool root_ok = false;
    bool journal_ok = !has_journal;
    uint64_t scan_free_blocks = 0U;
    uint64_t scan_free_inodes = 0U;
    uint32_t last_itbl_block = 0xFFFFFFFFU;

    for (uint32_t g = 0; g < groups_count; g++) {
        uint64_t gd_off = (uint64_t)g * (uint64_t)desc_size;
        uint32_t gd_blk = gdt_base_block + (uint32_t)(gd_off / block_size);
        uint32_t gd_inblk_off = (uint32_t)(gd_off % block_size);
        if (gd_blk != last_gdt_block) {
            if (gdt_dirty) {
                if (block_cache_write((uint64_t)last_gdt_block * sectors_per_block, blk_b, sectors_per_block) != 0) {
                    uart_puts("[fsck.ext4] fail: gdt write\n");
                    return -1;
                }
                gdt_dirty = false;
            }
            if (!fsck_read_fs_block(gd_blk, sectors_per_block, blk_b)) {
                uart_puts("[fsck.ext4] fail: gdt read\n");
                return -1;
            }
            last_gdt_block = gd_blk;
        }
        uint8_t *gd = blk_b + gd_inblk_off;
        uint64_t group_start = (uint64_t)first_data_block + (uint64_t)g * (uint64_t)blocks_per_group;
        if (group_start >= blocks_count) {
            uart_puts("[fsck.ext4] fail: group start\n");
            return -1;
        }
        uint32_t group_blocks = blocks_per_group;
        if (group_start + group_blocks > blocks_count) {
            group_blocks = (uint32_t)(blocks_count - group_start);
        }

        uint64_t bb = fsck_group_block_ref(gd, has_64bit, desc_size,
                                           MKFS_EXT4_BG_BLOCK_BITMAP_LO, MKFS_EXT4_BG_BLOCK_BITMAP_HI);
        uint64_t ib = fsck_group_block_ref(gd, has_64bit, desc_size,
                                           MKFS_EXT4_BG_INODE_BITMAP_LO, MKFS_EXT4_BG_INODE_BITMAP_HI);
        uint64_t it = fsck_group_block_ref(gd, has_64bit, desc_size,
                                           MKFS_EXT4_BG_INODE_TABLE_LO, MKFS_EXT4_BG_INODE_TABLE_HI);
        inode_tables[g] = it;
        uint32_t inode_table_blocks = (inodes_per_group * inode_size + block_size - 1U) / block_size;
        if (bb < group_start || bb >= group_start + group_blocks ||
            ib < group_start || ib >= group_start + group_blocks ||
            it < group_start || it + inode_table_blocks > group_start + group_blocks) {
            uart_puts("[fsck.ext4] fail: group refs range g=");
            print_dec((int)g);
            uart_puts("\n");
            return -1;
        }
        if (!fsck_read_fs_block(bb, sectors_per_block, bbm) ||
            !fsck_read_fs_block(ib, sectors_per_block, ibm)) {
            uart_puts("[fsck.ext4] fail: bitmap read\n");
            return -1;
        }

        uint32_t free_blocks_scan = 0U;
        for (uint32_t b = 0; b < group_blocks; b++) {
            uint64_t abs_b = group_start + (uint64_t)b;
            if (!fsck_bit_is_set(bbm, b)) {
                free_blocks_scan++;
            } else {
                fsck_bit_set(st.block_alloc_map, abs_b);
            }
        }
        uint32_t free_inodes_scan = 0U;
        uint32_t used_dirs_scan = 0U;
        for (uint32_t i = 0; i < inodes_per_group; i++) {
            uint32_t ino = g * inodes_per_group + i + 1U;
            if (ino > inodes_count) {
                free_inodes_scan++;
                continue;
            }
            bool alloc = fsck_bit_is_set(ibm, i);
            if (alloc) {
                fsck_bit_set(st.inode_alloc_map, ino);
            } else {
                free_inodes_scan++;
            }
        }

        uint32_t free_blocks_desc = fsck_group_count16(gd, has_64bit, desc_size,
                                                       MKFS_EXT4_BG_FREE_BLOCKS_LO, MKFS_EXT4_BG_FREE_BLOCKS_HI);
        uint32_t free_inodes_desc = fsck_group_count16(gd, has_64bit, desc_size,
                                                       MKFS_EXT4_BG_FREE_INODES_LO, MKFS_EXT4_BG_FREE_INODES_HI);
        uint32_t used_dirs_desc = fsck_group_count16(gd, has_64bit, desc_size,
                                                     MKFS_EXT4_BG_USED_DIRS_LO, MKFS_EXT4_BG_USED_DIRS_HI);
        if (free_blocks_desc != free_blocks_scan || free_inodes_desc != free_inodes_scan) {
            if (!can_repair) {
                uart_puts("[fsck.ext4] fail: desc free mismatch g=");
                print_dec((int)g);
                uart_puts("\n");
                return -1;
            }
            wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_LO, (uint16_t)(free_blocks_scan & 0xFFFFU));
            wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_LO, (uint16_t)(free_inodes_scan & 0xFFFFU));
            if (has_64bit && desc_size >= 64U) {
                wr_le16(gd + MKFS_EXT4_BG_FREE_BLOCKS_HI, (uint16_t)(free_blocks_scan >> 16));
                wr_le16(gd + MKFS_EXT4_BG_FREE_INODES_HI, (uint16_t)(free_inodes_scan >> 16));
            }
            gdt_dirty = true;
            repaired++;
        }

        uint32_t bb_bit = (uint32_t)(bb - group_start);
        uint32_t ib_bit = (uint32_t)(ib - group_start);
        uint32_t it_bit = (uint32_t)(it - group_start);
        if (!fsck_bit_is_set(bbm, bb_bit) || !fsck_bit_is_set(bbm, ib_bit)) {
            uart_puts("[fsck.ext4] fail: bitmap meta bit clear\n");
            return -1;
        }
        for (uint32_t bi = 0; bi < inode_table_blocks; bi++) {
            if (!fsck_bit_is_set(bbm, it_bit + bi)) {
                uart_puts("[fsck.ext4] fail: inode table bit clear\n");
                return -1;
            }
        }
        if (mkfs_group_has_super(g, sparse_super)) {
            uint32_t head_meta = 1U + ((groups_count * desc_size + block_size - 1U) / block_size);
            for (uint32_t bi = 0; bi < head_meta; bi++) {
                if (bi >= group_blocks || !fsck_bit_is_set(bbm, bi)) {
                    uart_puts("[fsck.ext4] fail: sparse-super head bit clear\n");
                    return -1;
                }
            }
        }

        for (uint32_t i = 0; i < inodes_per_group; i++) {
            uint32_t ino = g * inodes_per_group + i + 1U;
            if (ino > inodes_count) {
                break;
            }
            uint32_t byte_off = i * inode_size;
            uint32_t inode_blk = (uint32_t)it + (byte_off / block_size);
            uint32_t inode_off = byte_off % block_size;
            if (inode_blk != last_itbl_block) {
                if (!fsck_read_fs_block(inode_blk, sectors_per_block, blk_a)) {
                    uart_puts("[fsck.ext4] fail: inode table read\n");
                    return -1;
                }
                last_itbl_block = inode_blk;
            }
            const uint8_t *raw = blk_a + inode_off;
            uint16_t mode = rd_le16(raw + MKFS_EXT4_INODE_MODE);
            uint16_t links = rd_le16(raw + MKFS_EXT4_INODE_LINKS_COUNT);
            st.inode_modes[ino] = mode;
            st.inode_links_disk[ino] = links;

            bool alloc = fsck_bit_is_set(ibm, i);
            if (alloc && mode == 0U && ino >= first_ino) {
                uart_puts("[fsck.ext4] fail: alloc inode zero mode ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return -1;
            }
            if (!alloc && mode != 0U && ino >= first_ino) {
                uart_puts("[fsck.ext4] fail: free inode nonzero mode ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return -1;
            }
            if (!alloc || mode == 0U) {
                continue;
            }
            used_inodes++;
            if (fsck_is_dir_mode(mode)) {
                used_dirs_scan++;
            }
            if (ino == 2U && fsck_is_dir_mode(mode)) {
                root_ok = true;
            }

            if (has_journal && ino == journal_inum) {
                uint32_t first_jblk = 0U;
                uint32_t iflags = rd_le32(raw + MKFS_EXT4_INODE_FLAGS);
                if ((iflags & MKFS_EXT4_EXTENTS_FL) != 0U) {
                    const uint8_t *iblock = raw + MKFS_EXT4_INODE_BLOCK;
                    if (rd_le16(iblock + MKFS_EXT4_EXT_HDR_MAGIC) != MKFS_EXT4_EXT_MAGIC ||
                        rd_le16(iblock + MKFS_EXT4_EXT_HDR_ENTRIES) == 0U ||
                        rd_le16(iblock + MKFS_EXT4_EXT_HDR_DEPTH) != 0U) {
                        uart_puts("[fsck.ext4] fail: journal extent header\n");
                        return -1;
                    }
                    first_jblk = rd_le32(iblock + MKFS_EXT4_EXT_EE_START_LO);
                } else {
                    first_jblk = rd_le32(raw + MKFS_EXT4_INODE_BLOCK);
                }
                if (first_jblk == 0U || first_jblk >= blocks_count) {
                    uart_puts("[fsck.ext4] fail: journal first block invalid\n");
                    return -1;
                }
                if (!fsck_read_fs_block(first_jblk, sectors_per_block, blk_j)) {
                    uart_puts("[fsck.ext4] fail: journal block read\n");
                    return -1;
                }
                if (rd_be32(blk_j + MKFS_JBD2_HDR_MAGIC_OFF) != MKFS_JBD2_MAGIC ||
                    rd_be32(blk_j + MKFS_JBD2_HDR_TYPE_OFF) != MKFS_JBD2_SUPERBLOCK_V2) {
                    uart_puts("[fsck.ext4] fail: journal super magic/type\n");
                    return -1;
                }
                uint32_t j_block_size = rd_be32(blk_j + MKFS_JBD2_SB_BLOCKSIZE_OFF);
                uint32_t j_maxlen = rd_be32(blk_j + MKFS_JBD2_SB_MAXLEN_OFF);
                uint32_t j_first = rd_be32(blk_j + MKFS_JBD2_SB_FIRST_OFF);
                uint32_t j_incompat = rd_be32(blk_j + MKFS_JBD2_SB_FEAT_INCOMPAT_OFF);
                if (j_block_size != block_size || j_maxlen == 0U || j_first == 0U || j_first >= j_maxlen) {
                    uart_puts("[fsck.ext4] fail: journal geometry\n");
                    return -1;
                }
                if ((j_incompat & MKFS_JBD2_FEATURE_INCOMPAT_REVOKE) == 0U) {
                    uart_puts("[fsck.ext4] fail: journal revoke feature missing\n");
                    return -1;
                }
                if ((j_incompat & ~(MKFS_JBD2_FEATURE_INCOMPAT_REVOKE |
                                    MKFS_JBD2_FEATURE_INCOMPAT_64BIT |
                                    MKFS_JBD2_FEATURE_INCOMPAT_CSUM_V2 |
                                    MKFS_JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0U) {
                    uart_puts("[fsck.ext4] fail: journal incompat feature unsupported\n");
                    return -1;
                }
                journal_ok = true;
            }

            fsck_inode_visit_ctx_t visit_ctx = {
                .st = &st,
                .ino = ino,
                .parse_dir = fsck_is_dir_mode(mode),
            };
            if (!fsck_iter_inode_data_blocks(raw, &st, fsck_visit_inode_block, &visit_ctx, ino)) {
                return -1;
            }
        }

        if (used_dirs_desc != used_dirs_scan) {
            if (!can_repair) {
                uart_puts("[fsck.ext4] fail: used_dirs mismatch g=");
                print_dec((int)g);
                uart_puts("\n");
                return -1;
            }
            wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_LO, (uint16_t)(used_dirs_scan & 0xFFFFU));
            if (has_64bit && desc_size >= 64U) {
                wr_le16(gd + MKFS_EXT4_BG_USED_DIRS_HI, (uint16_t)(used_dirs_scan >> 16));
            }
            gdt_dirty = true;
            repaired++;
        }

        if (has_metadata_csum) {
            uint32_t bb_sum = fsck_bitmap_checksum32(csum_seed, g, bbm, block_size);
            uint32_t ib_sum = fsck_bitmap_checksum32(csum_seed, g, ibm, block_size);
            uint32_t bb_stored = rd_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_LO);
            uint32_t ib_stored = rd_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_LO);
            if (has_64bit && desc_size >= 64U) {
                bb_stored |= ((uint32_t)rd_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_HI) << 16);
                ib_stored |= ((uint32_t)rd_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_HI) << 16);
            }
            if (bb_stored != bb_sum || ib_stored != ib_sum) {
                if (!can_repair) {
                    uart_puts("[fsck.ext4] fail: bitmap checksum g=");
                    print_dec((int)g);
                    uart_puts("\n");
                    return -1;
                }
                wr_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_LO, (uint16_t)(bb_sum & 0xFFFFU));
                wr_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_LO, (uint16_t)(ib_sum & 0xFFFFU));
                if (has_64bit && desc_size >= 64U) {
                    wr_le16(gd + MKFS_EXT4_BG_BLOCK_BITMAP_CSUM_HI, (uint16_t)(bb_sum >> 16));
                    wr_le16(gd + MKFS_EXT4_BG_INODE_BITMAP_CSUM_HI, (uint16_t)(ib_sum >> 16));
                }
                gdt_dirty = true;
                repaired++;
            }

            uint16_t gd_calc = fsck_group_desc_checksum(gd, desc_size, csum_seed, g);
            uint16_t gd_stored = rd_le16(gd + MKFS_EXT4_BG_CHECKSUM);
            if (gd_stored != gd_calc) {
                if (!can_repair) {
                    uart_puts("[fsck.ext4] fail: group descriptor checksum g=");
                    print_dec((int)g);
                    uart_puts("\n");
                    return -1;
                }
                wr_le16(gd + MKFS_EXT4_BG_CHECKSUM, gd_calc);
                gdt_dirty = true;
                repaired++;
            }
        }

        scan_free_blocks += free_blocks_scan;
        scan_free_inodes += free_inodes_scan;
    }

    if (gdt_dirty) {
        if (block_cache_write((uint64_t)last_gdt_block * sectors_per_block, blk_b, sectors_per_block) != 0) {
            uart_puts("[fsck.ext4] fail: gdt write flush\n");
            return -1;
        }
    }

    if (!root_ok || used_inodes == 0U) {
        uart_puts("[fsck.ext4] fail: root inode missing\n");
        return -1;
    }
    if (has_journal && !journal_ok) {
        uart_puts("[fsck.ext4] fail: journal inode missing\n");
        return -1;
    }

    for (uint32_t ino = 1U; ino <= inodes_count; ino++) {
        if (!fsck_dynamic_bit_is_set(st.inode_alloc_map, ino) || !fsck_is_dir_mode(st.inode_modes[ino])) {
            continue;
        }
        uint8_t flags_dir = st.inode_dot_flags[ino];
        if ((flags_dir & 0x1U) == 0U || (flags_dir & 0x2U) == 0U) {
            uart_puts("[fsck.ext4] fail: directory missing '.'/'..' ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return -1;
        }
        uint32_t parent = st.inode_parent[ino];
        if (ino == 2U) {
            if (parent != 2U) {
                uart_puts("[fsck.ext4] fail: root '..' invalid\n");
                return -1;
            }
            continue;
        }
        if (parent == 0U || parent > inodes_count ||
            !fsck_dynamic_bit_is_set(st.inode_alloc_map, parent) ||
            !fsck_is_dir_mode(st.inode_modes[parent])) {
            uart_puts("[fsck.ext4] fail: bad directory parent ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return -1;
        }
        uint32_t cur = ino;
        for (uint32_t depth = 0U; depth <= inodes_count; depth++) {
            if (cur == 2U) {
                break;
            }
            uint32_t p = st.inode_parent[cur];
            if (p == 0U || p > inodes_count ||
                !fsck_dynamic_bit_is_set(st.inode_alloc_map, p) ||
                !fsck_is_dir_mode(st.inode_modes[p])) {
                uart_puts("[fsck.ext4] fail: disconnected directory ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return -1;
            }
            if (depth == inodes_count) {
                uart_puts("[fsck.ext4] fail: directory parent loop ino=");
                print_dec((int)ino);
                uart_puts("\n");
                return -1;
            }
            cur = p;
        }
    }

    uint32_t inode_patch_blk = 0xFFFFFFFFU;
    for (uint32_t ino = 1U; ino <= inodes_count; ino++) {
        if (!fsck_dynamic_bit_is_set(st.inode_alloc_map, ino) || st.inode_modes[ino] == 0U) {
            continue;
        }
        if (ino == journal_inum) {
            continue;
        }
        if (ino < first_ino && ino != 2U) {
            continue;
        }
        uint32_t expect_links = st.inode_links_refs[ino];
        if (expect_links > 0xFFFFU) {
            uart_puts("[fsck.ext4] fail: link count overflow ino=");
            print_dec((int)ino);
            uart_puts("\n");
            return -1;
        }
        uint16_t disk_links = st.inode_links_disk[ino];
        if (disk_links == (uint16_t)expect_links) {
            continue;
        }
        if (!can_repair) {
            uart_puts("[fsck.ext4] fail: inode link mismatch ino=");
            print_dec((int)ino);
            uart_puts(" disk=");
            print_dec((int)disk_links);
            uart_puts(" expect=");
            print_dec((int)expect_links);
            uart_puts("\n");
            return -1;
        }
        if (!fsck_write_inode_links(ino, (uint16_t)expect_links,
                                    inodes_per_group, inode_size,
                                    block_size, sectors_per_block,
                                    inode_tables, blk_j, &inode_patch_blk)) {
            uart_puts("[fsck.ext4] fail: inode link repair write\n");
            return -1;
        }
        st.inode_links_disk[ino] = (uint16_t)expect_links;
        repaired++;
    }

    if (scan_free_blocks != super_free_blocks || scan_free_inodes != super_free_inodes) {
        if (!can_repair) {
            uart_puts("[fsck.ext4] fail: super free mismatch\n");
            uart_puts(" blocks ");
            print_dec((int)scan_free_blocks);
            uart_puts("/");
            print_dec((int)super_free_blocks);
            uart_puts(" inodes ");
            print_dec((int)scan_free_inodes);
            uart_puts("/");
            print_dec((int)super_free_inodes);
            uart_puts("\n");
            return -1;
        }
        wr_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_LO, (uint32_t)(scan_free_blocks & 0xFFFFFFFFULL));
        if (has_64bit) {
            wr_le32(sb + MKFS_EXT4_S_FREE_BLOCKS_COUNT_HI, (uint32_t)(scan_free_blocks >> 32));
        }
        wr_le32(sb + MKFS_EXT4_S_FREE_INODES_COUNT, (uint32_t)scan_free_inodes);
        super_dirty = true;
        repaired++;
    }

    if (has_metadata_csum) {
        uint8_t sblk[1024];
        memcpy(sblk, sb, sizeof(sblk));
        wr_le32(sblk + MKFS_EXT4_S_CHECKSUM, 0U);
        uint32_t calc = mkfs_crc32c_update(csum_seed, sblk, sizeof(sblk));
        if (rd_le32(sb + MKFS_EXT4_S_CHECKSUM) != calc) {
            if (!can_repair) {
                uart_puts("[fsck.ext4] fail: super checksum final\n");
                return -1;
            }
            wr_le32(sb + MKFS_EXT4_S_CHECKSUM, calc);
            super_dirty = true;
            repaired++;
        }
    }

    if (super_dirty) {
        if (block_cache_write(0U, blk_a, MKFS_EXT4_SECTORS_PER_BLOCK) != 0) {
            uart_puts("[fsck.ext4] fail: super write\n");
            return -1;
        }
    }

    if (block_cache_flush() != 0) {
        uart_puts("[fsck.ext4] fail: flush\n");
        return -1;
    }

    if (repaired > 0U) {
        uart_puts("[fsck.ext4] repaired ");
        uart_puts(target);
        uart_puts(" count=");
        print_dec((int)repaired);
        uart_puts(" groups=");
        print_dec((int)groups_count);
        uart_puts("\n");
        return 1;
    }

    uart_puts("[fsck.ext4] clean ");
    uart_puts(target);
    uart_puts(" groups=");
    print_dec((int)groups_count);
    uart_puts(" free_blocks=");
    print_dec((int)scan_free_blocks);
    uart_puts(" free_inodes=");
    print_dec((int)scan_free_inodes);
    uart_puts("\n");
    return 0;
}

int fs_mount(const char *source, const char *target, const char *fstype, uint64_t flags) {
    if (!source || !target || !fstype || flags != 0U) {
        return -1;
    }
    if (strcmp(fstype, "ext4") != 0) {
        return -1;
    }

    inode_t *src = fs_lookup(source);
    inode_t *mnt = fs_lookup(target);
    if (!src || src->type != INODE_DEV || !dev_kind_is_block(src->dev_kind) ||
        !dev_block_ready_kind(src->dev_kind)) {
        return -1;
    }
    if (!mnt || mnt->type != INODE_DIR || mnt == root_inode) {
        return -1;
    }
    if (mnt->fs_kind != FS_KIND_MEM || mnt->child_count != 0U) {
        return -1;
    }
    if (mount_is_active(mnt)) {
        return -1;
    }

    mount_entry_t *e = mount_alloc_entry();
    if (!e) {
        return -1;
    }
    if (block_cache_attach_inode(src) != 0) {
        return -1;
    }
    if (!ext4_mount(mnt)) {
        return -1;
    }

    e->used = true;
    e->mountpoint = mnt;
    e->kind = FS_KIND_EXT4;
    e->source_dev = src->dev_kind;
    return 0;
}

int fs_umount(const char *target, uint64_t flags) {
    if (!target || flags != 0U) {
        return -1;
    }
    inode_t *mnt = fs_lookup(target);
    if (!mnt || mnt->type != INODE_DIR) {
        return -1;
    }
    mount_entry_t *e = mount_find_by_point(mnt);
    if (!e) {
        return -1;
    }
    if (task_mount_busy(mnt, target)) {
        return -1;
    }

    if (e->kind == FS_KIND_EXT4) {
        if (!ext4_unmount(mnt)) {
            return -1;
        }
    } else {
        return -1;
    }

    memset(e, 0, sizeof(*e));
    return 0;
}

inode_t *fs_create_file(const char *path) {
    char name[INODE_NAME_MAX + 1];
    inode_t *parent = lookup_parent(path, name);
    if (!parent || parent->type != INODE_DIR || name[0] == '\0') {
        return 0;
    }
    if (fs_lookup_child_mode(parent, name, false)) {
        return 0;
    }
    if (parent->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return 0;
        }
        inode_t *n = ext4_create_file(parent, name);
        if (!n || !ext4_tx_commit()) {
            ext4_tx_abort();
            return 0;
        }
        return n;
    }
    if (parent->fs_kind != FS_KIND_MEM) {
        return 0;
    }
    return mk_file(parent, name, 0, 0, false, true);
}

inode_t *fs_create_dir(const char *path) {
    char name[INODE_NAME_MAX + 1];
    inode_t *parent = lookup_parent(path, name);
    if (!parent || parent->type != INODE_DIR || name[0] == '\0') {
        return 0;
    }
    if (fs_lookup_child_mode(parent, name, false)) {
        return 0;
    }
    if (parent->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return 0;
        }
        inode_t *n = ext4_create_dir(parent, name);
        if (!n || !ext4_tx_commit()) {
            ext4_tx_abort();
            return 0;
        }
        return n;
    }
    if (parent->fs_kind != FS_KIND_MEM) {
        return 0;
    }
    return mk_dir(parent, name);
}

int fs_link(const char *old_path, const char *new_path) {
    inode_t *src = fs_lookup_nofollow(old_path);
    if (!src || src->type != INODE_FILE) {
        return -1;
    }
    if (src->fs_kind == FS_KIND_MEM) {
        return -1;
    }

    char name[INODE_NAME_MAX + 1];
    inode_t *parent = lookup_parent(new_path, name);
    if (!parent || parent->type != INODE_DIR || name[0] == '\0') {
        return -1;
    }
    if (fs_lookup_nofollow(new_path)) {
        return -1;
    }
    if (src->fs_kind != parent->fs_kind) {
        return -1;
    }

    if (src->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return -1;
        }
        int rc = ext4_link(src, parent, name);
        if (rc != 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        return 0;
    }
    return -1;
}

int fs_symlink(const char *target, const char *link_path) {
    if (!target || target[0] == '\0') {
        return -1;
    }
    if (strlen(target) >= MAX_PATH) {
        return -1;
    }

    char name[INODE_NAME_MAX + 1];
    inode_t *parent = lookup_parent(link_path, name);
    if (!parent || parent->type != INODE_DIR || name[0] == '\0') {
        return -1;
    }
    if (fs_lookup_nofollow(link_path)) {
        return -1;
    }

    if (parent->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return -1;
        }
        inode_t *n = ext4_symlink(parent, name, target);
        if (!n || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        return 0;
    }
    return -1;
}

int fs_readlink(const char *path, char *buf, size_t buflen) {
    inode_t *ino;
    if (!buf) {
        return -1;
    }
    ino = fs_lookup_nofollow(path);
    if (!ino || ino->type != INODE_FILE) {
        return -1;
    }
    if (ino->fs_kind == FS_KIND_EXT4) {
        return ext4_readlink(ino, buf, buflen);
    }
    return -1;
}

int fs_lstat(const char *path, fu_stat_t *st) {
    inode_t *ino = fs_lookup_nofollow(path);
    if (!ino || !st) {
        return -1;
    }
    return fs_fill_stat(ino, st);
}

int fs_unlink(const char *path) {
    inode_t *ino = fs_lookup_nofollow(path);
    if (!ino || ino->type != INODE_FILE || !ino->parent) {
        return -1;
    }
    if (ino->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return -1;
        }
        int rc = ext4_unlink(ino);
        if (rc != 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        pagecache_invalidate_inode(ino);
        return 0;
    }
    if (ino->fs_kind != FS_KIND_MEM) {
        return -1;
    }
    if (!dir_remove_child(ino->parent, ino)) {
        return -1;
    }
    inode_free(ino);
    return 0;
}

int fs_rmdir(const char *path) {
    inode_t *ino = fs_lookup_nofollow(path);
    if (!ino || ino == root_inode || ino->type != INODE_DIR || !ino->parent) {
        return -1;
    }
    if (mount_is_active(ino)) {
        return -1;
    }
    if (ino->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return -1;
        }
        int rc = ext4_rmdir(ino);
        if (rc != 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        return 0;
    }
    if (ino->fs_kind != FS_KIND_MEM) {
        return -1;
    }
    if (ino->child_count != 0) {
        return -1;
    }
    if (!dir_remove_child(ino->parent, ino)) {
        return -1;
    }
    inode_free(ino);
    return 0;
}

int fs_rename(const char *old_path, const char *new_path) {
    inode_t *ino = fs_lookup_nofollow(old_path);
    if (!ino || ino == root_inode || !ino->parent) {
        return -1;
    }
    if (mount_is_active(ino)) {
        return -1;
    }

    if (strcmp(old_path, new_path) == 0) {
        return 0;
    }

    if (fs_lookup_nofollow(new_path)) {
        return -1;
    }

    char new_name[INODE_NAME_MAX + 1];
    inode_t *new_parent = lookup_parent(new_path, new_name);
    if (!new_parent || new_parent->type != INODE_DIR || new_name[0] == '\0') {
        return -1;
    }

    if (ino->fs_kind == FS_KIND_EXT4) {
        if (new_parent->fs_kind != FS_KIND_EXT4) {
            return -1;
        }
        if (!ext4_tx_begin()) {
            return -1;
        }
        int rc = ext4_rename(ino, new_parent, new_name);
        if (rc != 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        return 0;
    }

    if (ino->fs_kind != FS_KIND_MEM || new_parent->fs_kind != FS_KIND_MEM) {
        return -1;
    }
    if (new_parent->child_count >= DIR_MAX_CHILDREN) {
        return -1;
    }

    if (ino->type == INODE_DIR) {
        inode_t *p = new_parent;
        while (p) {
            if (p == ino) {
                return -1;
            }
            p = p->parent;
        }
    }

    inode_t *old_parent = ino->parent;
    char old_name[INODE_NAME_MAX + 1];
    strcpy(old_name, ino->name);

    if (!dir_remove_child(old_parent, ino)) {
        return -1;
    }

    strncpy(ino->name, new_name, INODE_NAME_MAX);
    ino->name[INODE_NAME_MAX] = '\0';

    if (!dir_add_child(new_parent, ino)) {
        strncpy(ino->name, old_name, INODE_NAME_MAX);
        ino->name[INODE_NAME_MAX] = '\0';
        (void)dir_add_child(old_parent, ino);
        return -1;
    }

    return 0;
}

int fs_read(inode_t *inode, size_t *offset, void *buf, size_t len) {
    if (!inode || !offset || !buf) {
        return -1;
    }
    if (inode->fs_kind == FS_KIND_EXT4) {
        size_t start = *offset;
        int n = ext4_read(inode, offset, buf, len);
        if (n > 0 && inode->type == INODE_FILE) {
            pagecache_overlay_read(inode, start, buf, (size_t)n);
        }
        return n;
    }

    if (inode->type == INODE_DEV) {
        if (len == 0U) {
            return 0;
        }
        switch (inode->dev_kind) {
            case DEV_NULL:
                return 0;
            case DEV_ZERO:
                memset(buf, 0, len);
                *offset += len;
                return (int)len;
            case DEV_TTY: {
                uint8_t *dst = (uint8_t *)buf;
                size_t done = 0;
                while (done < len) {
                    int c = uart_getc();
                    dst[done++] = (uint8_t)c;
                    if (c == '\n' || c == '\r') {
                        break;
                    }
                }
                *offset += done;
                return (int)done;
            }
            case DEV_VDA:
            case DEV_SDA:
            case DEV_SDB:
            case DEV_SDC:
            case DEV_SDD:
            case DEV_SDE:
            case DEV_SDF:
            case DEV_SDG:
            case DEV_SDH:
            case DEV_NVME0N1:
            case DEV_NVME1N1:
            case DEV_NVME2N1:
            case DEV_NVME3N1:
            case DEV_NVME4N1:
            case DEV_NVME5N1:
            case DEV_NVME6N1:
            case DEV_NVME7N1:
                return dev_block_read_inode(inode, offset, buf, len);
            default:
                return -1;
        }
    }

    if (inode->type == INODE_DIR) {
        size_t idx = *offset / sizeof(dirent_t);
        if (idx >= inode->child_count) {
            return 0;
        }
        if (len < sizeof(dirent_t)) {
            return -1;
        }
        dirent_t ent;
        memset(&ent, 0, sizeof(ent));
        strncpy(ent.name, inode->children[idx]->name, INODE_NAME_MAX);
        ent.type = (uint32_t)inode->children[idx]->type;
        memcpy(buf, &ent, sizeof(ent));
        *offset += sizeof(ent);
        return (int)sizeof(ent);
    }

    if (inode->type != INODE_FILE || *offset >= inode->size) {
        return 0;
    }

    size_t n = len;
    if (*offset + n > inode->size) {
        n = inode->size - *offset;
    }
    memcpy(buf, inode->data + *offset, n);
    pagecache_overlay_read(inode, *offset, buf, n);
    *offset += n;
    return (int)n;
}

int fs_write(inode_t *inode, size_t *offset, const void *buf, size_t len) {
    if (!inode || !offset || !buf) {
        return -1;
    }
    if (inode->fs_kind == FS_KIND_EXT4) {
        size_t start = *offset;
        if (!ext4_tx_begin()) {
            return -1;
        }
        int n = ext4_write(inode, offset, buf, len);
        if (n <= 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        if (n > 0 && inode->type == INODE_FILE) {
            pagecache_notify_write(inode, start, buf, (size_t)n);
        }
        return n;
    }
    if (inode->type == INODE_DEV) {
        switch (inode->dev_kind) {
            case DEV_NULL:
            case DEV_ZERO:
                *offset += len;
                return (int)len;
            case DEV_TTY: {
                const uint8_t *src = (const uint8_t *)buf;
                for (size_t i = 0; i < len; i++) {
                    uart_putc((char)src[i]);
                }
                *offset += len;
                return (int)len;
            }
            case DEV_VDA:
            case DEV_SDA:
            case DEV_SDB:
            case DEV_SDC:
            case DEV_SDD:
            case DEV_SDE:
            case DEV_SDF:
            case DEV_SDG:
            case DEV_SDH:
            case DEV_NVME0N1:
            case DEV_NVME1N1:
            case DEV_NVME2N1:
            case DEV_NVME3N1:
            case DEV_NVME4N1:
            case DEV_NVME5N1:
            case DEV_NVME6N1:
            case DEV_NVME7N1:
                return dev_block_write_inode(inode, offset, buf, len);
            default:
                return -1;
        }
    }
    if (inode->fs_kind != FS_KIND_MEM || inode->type != INODE_FILE || !inode->writable) {
        return -1;
    }
    if (*offset >= inode->capacity) {
        return -1;
    }
    size_t n = len;
    if (*offset + n > inode->capacity) {
        n = inode->capacity - *offset;
    }
    memcpy(inode->data + *offset, buf, n);
    pagecache_notify_write(inode, *offset, buf, n);
    *offset += n;
    if (*offset > inode->size) {
        inode->size = *offset;
    }
    return (int)n;
}

int fs_truncate(inode_t *inode, size_t size) {
    if (!inode) {
        return -1;
    }
    if (inode->fs_kind == FS_KIND_EXT4) {
        if (!ext4_tx_begin()) {
            return -1;
        }
        int rc = ext4_truncate(inode, size);
        if (rc != 0 || !ext4_tx_commit()) {
            ext4_tx_abort();
            return -1;
        }
        if (rc == 0) {
            pagecache_invalidate_inode(inode);
        }
        return rc;
    }
    if (inode->fs_kind != FS_KIND_MEM || inode->type != INODE_FILE || !inode->writable) {
        return -1;
    }
    if (size > inode->capacity) {
        return -1;
    }
    if (size > inode->size) {
        memset(inode->data + inode->size, 0, size - inode->size);
    }
    inode->size = size;
    pagecache_invalidate_inode(inode);
    return 0;
}
