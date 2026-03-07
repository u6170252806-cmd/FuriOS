#include "ext4.h"
#include "block_cache.h"
#include "virtio_blk.h"
#include "pagecache.h"
#include "string.h"
#include "uart.h"
#include "print.h"
#include "config.h"
#include "timer.h"

#define EXT4_SUPER_OFFSET             1024U
#define EXT4_SUPER_SIZE               1024U
#define EXT4_SUPER_MAGIC              0xEF53U

#define EXT4_ROOT_INO                 2U
#define EXT4_NDIR_BLOCKS              12U

#define EXT4_INCOMPAT_FILETYPE        0x0002U
#define EXT4_INCOMPAT_RECOVER         0x0004U
#define EXT4_INCOMPAT_META_BG         0x0010U
#define EXT4_INCOMPAT_EXTENTS         0x0040U
#define EXT4_INCOMPAT_64BIT           0x0080U
#define EXT4_INCOMPAT_FLEX_BG         0x0200U

#define EXT4_ROCOMPAT_BIGALLOC        0x0200U
#define EXT4_ROCOMPAT_SPARSE_SUPER    0x0001U
#define EXT4_ROCOMPAT_LARGE_FILE      0x0002U
#define EXT4_ROCOMPAT_BTREE_DIR       0x0004U
#define EXT4_ROCOMPAT_HUGE_FILE       0x0008U
#define EXT4_ROCOMPAT_GDT_CSUM        0x0010U
#define EXT4_ROCOMPAT_DIR_NLINK       0x0020U
#define EXT4_ROCOMPAT_EXTRA_ISIZE     0x0040U
#define EXT4_ROCOMPAT_METADATA_CSUM   0x0400U

#define EXT4_COMPAT_HAS_JOURNAL       0x0004U

#define EXT4_EXTENTS_FL               0x00080000U

#define EXT4_EXT_MAGIC                0xF30AU

#define EXT4_FT_REG_FILE              1U
#define EXT4_FT_DIR                   2U
#define EXT4_FT_SYMLINK               7U

#define EXT4_S_INODES_COUNT           0x00
#define EXT4_S_BLOCKS_COUNT_LO        0x04
#define EXT4_S_FIRST_DATA_BLOCK       0x14
#define EXT4_S_LOG_BLOCK_SIZE         0x18
#define EXT4_S_BLOCKS_PER_GROUP       0x20
#define EXT4_S_INODES_PER_GROUP       0x28
#define EXT4_S_MAGIC                  0x38
#define EXT4_S_FREE_BLOCKS_COUNT_LO   0x0C
#define EXT4_S_FREE_INODES_COUNT      0x10
#define EXT4_S_FIRST_INO              0x54
#define EXT4_S_INODE_SIZE             0x58
#define EXT4_S_FEATURE_COMPAT         0x5C
#define EXT4_S_FEATURE_INCOMPAT       0x60
#define EXT4_S_FEATURE_RO_COMPAT      0x64
#define EXT4_S_UUID                   0x68
#define EXT4_S_JOURNAL_INUM           0xE0
#define EXT4_S_JOURNAL_DEV            0xE4
#define EXT4_S_DESC_SIZE              0xFE
#define EXT4_S_BLOCKS_COUNT_HI        0x150
#define EXT4_S_FREE_BLOCKS_COUNT_HI   0x158

#define EXT4_BG_BLOCK_BITMAP_LO       0x00
#define EXT4_BG_INODE_BITMAP_LO       0x04
#define EXT4_BG_INODE_TABLE_LO        0x08
#define EXT4_BG_FREE_BLOCKS_COUNT_LO  0x0C
#define EXT4_BG_FREE_INODES_COUNT_LO  0x0E
#define EXT4_BG_INODE_TABLE_HI        0x28
#define EXT4_BG_BLOCK_BITMAP_HI       0x20
#define EXT4_BG_INODE_BITMAP_HI       0x24
#define EXT4_BG_FREE_BLOCKS_COUNT_HI  0x2C
#define EXT4_BG_FREE_INODES_COUNT_HI  0x2E

#define EXT4_INODE_MODE               0x00
#define EXT4_INODE_SIZE_LO            0x04
#define EXT4_INODE_LINKS_COUNT        0x1A
#define EXT4_INODE_BLOCKS_LO          0x1C
#define EXT4_INODE_FLAGS              0x20
#define EXT4_INODE_BLOCK              0x28
#define EXT4_INODE_SIZE_HIGH          0x6C

#define EXT4_S_IFMT                   0xF000U
#define EXT4_S_IFDIR                  0x4000U
#define EXT4_S_IFREG                  0x8000U
#define EXT4_S_IFLNK                  0xA000U

#define EXT4_CACHE_MAX                96
#define EXT4_SYMLINK_MAX_DEPTH        8
#define EXT4_EXT_MUT_MAX_RUNS         256
#define EXT4_EXT_MUT_MAX_NODES        512
#define EXT4_EXT_LEN_MASK             0x7FFFU

#define EXT4_JLOG_MAGIC               0x46554A31U /* FUJ1 */
#define EXT4_JLOG_STATE_CLEAN         0U
#define EXT4_JLOG_STATE_PREP          1U
#define EXT4_JLOG_STATE_COMMIT        2U
#define EXT4_JLOG_REGION_BYTES        (256U * 1024U)
#define EXT4_JLOG_MAX_RECORDS         512U
#define EXT4_JLOG_DATA_BYTES          (192U * 1024U)

#define JBD2_MAGIC_NUMBER             0xC03B3998U
#define JBD2_DESCRIPTOR_BLOCK         1U
#define JBD2_COMMIT_BLOCK             2U
#define JBD2_SUPERBLOCK_V1            3U
#define JBD2_SUPERBLOCK_V2            4U
#define JBD2_REVOKE_BLOCK             5U

#define JBD2_FLAG_ESCAPE              1U
#define JBD2_FLAG_SAME_UUID           2U
#define JBD2_FLAG_DELETED             4U
#define JBD2_FLAG_LAST_TAG            8U

#define JBD2_FEATURE_COMPAT_CHECKSUM  0x00000001U
#define JBD2_FEATURE_INCOMPAT_REVOKE  0x00000001U
#define JBD2_FEATURE_INCOMPAT_64BIT   0x00000002U
#define JBD2_FEATURE_INCOMPAT_ASYNC   0x00000004U
#define JBD2_FEATURE_INCOMPAT_CSUM_V2 0x00000008U
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 0x00000010U
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT 0x00000020U

#define JBD2_HDR_MAGIC_OFF            0U
#define JBD2_HDR_TYPE_OFF             4U
#define JBD2_HDR_SEQ_OFF              8U

#define JBD2_SB_BLOCKSIZE_OFF         12U
#define JBD2_SB_MAXLEN_OFF            16U
#define JBD2_SB_FIRST_OFF             20U
#define JBD2_SB_SEQUENCE_OFF          24U
#define JBD2_SB_START_OFF             28U
#define JBD2_SB_FEAT_COMPAT_OFF       36U
#define JBD2_SB_FEAT_INCOMPAT_OFF     40U
#define JBD2_SB_CHECKSUM_TYPE_OFF     80U
#define JBD2_SB_NUM_FC_BLKS_OFF       84U
#define JBD2_SB_HEAD_OFF              88U
#define JBD2_SB_CHECKSUM_OFF          252U

#define JBD2_REVOKE_COUNT_OFF         12U
#define JBD2_REVOKE_ENTRIES_OFF       16U

#define JBD2_COMMIT_CSUM_TYPE_OFF     12U
#define JBD2_COMMIT_CSUM_SIZE_OFF     13U
#define JBD2_COMMIT_CSUM_OFF          16U

#define JBD2_CRC32C_CHKSUM            4U

#define EXT4_JBD2_MAX_UPDATES         8192U
#define EXT4_JBD2_MAX_REVOKES         8192U
#define EXT4_JBD2_TX_MAX_BLOCKS       256U
#define EXT4_JBD2_TX_MAX_REVOKES      512U
#define EXT4_JBD2_MAX_PENDING_TX      4U
#define EXT4_JBD2_CHECKPOINT_BATCH    4U
#define EXT4_JBD2_CHECKPOINT_AGE      20U

typedef struct {
    bool valid;
    uint32_t ino;
    uint16_t mode;
    uint16_t links;
    uint32_t flags;
    uint32_t blocks_lo;
    uint64_t size;
    uint32_t block[15];
} ext4_meta_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

typedef struct {
    uint32_t lblock;
    uint32_t len;
    uint64_t pblock;
} ext4_extent_run_t;

typedef struct {
    uint32_t lblock;
    uint64_t block;
} ext4_extent_node_ref_t;

typedef struct {
    bool mounted;
    bool write_enabled;
    uint32_t instance_id;
    bool has_filetype;
    bool has_64bit;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inodes_count;
    uint32_t first_ino;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t desc_size;
    uint32_t first_data_block;
    uint64_t blocks_count;
    uint64_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t groups_count;
    inode_t *mountpoint;
    dev_kind_t source_dev_kind;
    uint64_t source_lba_start;
    uint64_t source_lba_count;
    ext4_meta_t root_meta;
    inode_t cache_nodes[EXT4_CACHE_MAX];
    ext4_meta_t cache_meta[EXT4_CACHE_MAX];
} ext4_fs_t;

typedef struct {
    uint64_t byte_off;
    uint32_t len;
    uint32_t data_off;
} __attribute__((packed)) ext4_jlog_rec_t;

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t seq;
    uint32_t rec_count;
    uint32_t data_bytes;
    uint32_t reserved[11];
} __attribute__((packed)) ext4_jlog_hdr_t;

typedef struct {
    bool enabled;
    bool tx_active;
    bool replaying;
    uint32_t seq;
    uint64_t base_off;
    uint64_t area_bytes;
    uint32_t rec_count;
    uint32_t data_bytes;
    ext4_jlog_rec_t recs[EXT4_JLOG_MAX_RECORDS];
    uint8_t data[EXT4_JLOG_DATA_BYTES];
} ext4_journal_t;

static ext4_fs_t g_ext4;
static ext4_journal_t g_journal;
static uint32_t g_ext4_instance_seq;
typedef struct {
    uint64_t fs_block;
    uint32_t j_block;
    uint32_t seq;
    bool escape;
} ext4_jbd2_update_t;
typedef struct {
    uint64_t fs_block;
    uint32_t seq;
} ext4_jbd2_revoke_t;
typedef struct {
    bool used;
    uint64_t fs_block;
    uint8_t data[4096];
} ext4_jbd2_dirty_t;
typedef struct {
    uint64_t fs_block;
    uint8_t data[4096];
} ext4_jbd2_pending_block_t;
typedef struct {
    bool used;
    uint32_t seq;
    uint32_t start;
    uint32_t end;
    uint32_t dirty_count;
    ext4_jbd2_pending_block_t dirty[EXT4_JBD2_TX_MAX_BLOCKS];
} ext4_jbd2_pending_t;
typedef struct {
    bool enabled;
    bool write_ready;
    bool tx_active;
    bool io_bypass;
    bool feat_64bit;
    bool csum_v2;
    bool csum_v3;
    uint32_t block_size;
    uint32_t maxlen;
    uint32_t first;
    uint32_t sequence;
    uint32_t start;
    uint32_t head;
    uint32_t tail;
    uint32_t checksum_type;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint8_t uuid[16];
    uint32_t csum_seed;
    uint64_t last_checkpoint_tick;
    ext4_meta_t journal_meta;
    uint32_t tx_dirty_count;
    ext4_jbd2_dirty_t tx_dirty[EXT4_JBD2_TX_MAX_BLOCKS];
    uint32_t tx_revoke_count;
    uint64_t tx_revokes[EXT4_JBD2_TX_MAX_REVOKES];
    bool tx_revoke_overflow;
    bool tx_enospc;
    uint64_t last_enospc_log_tick;
    uint32_t pending_count;
    ext4_jbd2_pending_t pending[EXT4_JBD2_MAX_PENDING_TX];
} ext4_jbd2_state_t;
static ext4_jbd2_update_t g_jbd2_updates[EXT4_JBD2_MAX_UPDATES];
static ext4_jbd2_revoke_t g_jbd2_revokes[EXT4_JBD2_MAX_REVOKES];
static ext4_jbd2_state_t g_jbd2;
static int g_ext4_last_errno;
static uint32_t g_ext4_symlink_depth;
static int ext4_read_file_at(const ext4_meta_t *m, uint64_t off, void *buf, size_t len);
static bool ext4_is_inline_symlink(const ext4_meta_t *m);
static bool ext4_map_file_block(const ext4_meta_t *m, uint32_t lblock, uint64_t *pblock);
static bool ext4_load_meta(uint32_t ino, ext4_meta_t *out);
static bool ext4_jbd2_tx_start(void);
static void ext4_jbd2_tx_abort(void);
static bool ext4_jbd2_tx_commit(void);
static bool ext4_jbd2_tx_overlay_read(uint64_t byte_off, void *buf, size_t len);
static bool ext4_jbd2_pending_overlay_read(uint64_t byte_off, void *buf, size_t len);
static bool ext4_jbd2_tx_write_bytes(uint64_t byte_off, const void *buf, size_t len);
static void ext4_jbd2_note_revoke(uint64_t fs_block);
static bool ext4_jbd2_parse_revoke_block(const uint8_t *blk, uint32_t seq,
                                         ext4_jbd2_revoke_t *revokes,
                                         uint32_t revoke_max,
                                         uint32_t *revoke_count_io);
static bool ext4_jbd2_parse_descriptor_block(const ext4_meta_t *journal_meta,
                                             const uint8_t *blk, uint32_t seq,
                                             uint32_t first, uint32_t maxlen,
                                             uint32_t *cursor_io,
                                             const ext4_jbd2_revoke_t *revokes,
                                             uint32_t revoke_count,
                                             ext4_jbd2_update_t *updates,
                                             uint32_t update_max,
                                             uint32_t *update_count_io);
static bool ext4_jbd2_apply_updates(const ext4_meta_t *journal_meta,
                                    uint32_t update_count,
                                    uint32_t revoke_count);
static uint32_t ext4_jbd2_tx_tag_bytes(void);
static uint32_t ext4_jbd2_tx_reserve_blocks(void);
static bool ext4_jbd2_maybe_checkpoint(bool force);
static bool ext4_jbd2_checkpoint_until_free(uint32_t min_free_blocks);
static void ext4_jbd2_log_enospc(uint32_t need_blocks);

static uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFU);
    p[1] = (uint8_t)(v & 0xFFU);
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

static uint32_t align_up4(uint32_t n) {
    return (n + 3U) & ~3U;
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static uint32_t crc32c_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = ~crc;
    while (len-- > 0) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c & 1U) ? ((c >> 1) ^ 0x82F63B78U) : (c >> 1);
        }
    }
    return ~c;
}

static bool ext4_attach_source_dev(void) {
    if (g_ext4.source_dev_kind == DEV_NONE || g_ext4.source_lba_count == 0U) {
        return false;
    }
    inode_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.type = INODE_DEV;
    dev.dev_kind = g_ext4.source_dev_kind;
    dev.dev_lba_start = g_ext4.source_lba_start;
    dev.dev_lba_count = g_ext4.source_lba_count;
    return block_cache_attach_inode(&dev) == 0;
}

static bool ext4_raw_read_bytes(uint64_t byte_off, void *buf, size_t len) {
    uint8_t sec[VIRTIO_BLK_SECTOR_SIZE];
    uint8_t *dst = (uint8_t *)buf;

    if (!ext4_attach_source_dev()) {
        return false;
    }

    while (len > 0) {
        uint64_t lba = byte_off / VIRTIO_BLK_SECTOR_SIZE;
        size_t sec_off = (size_t)(byte_off % VIRTIO_BLK_SECTOR_SIZE);
        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - sec_off;
        if (chunk > len) {
            chunk = len;
        }
        if (block_cache_read(lba, sec, 1) != 0) {
            return false;
        }
        memcpy(dst, sec + sec_off, chunk);
        dst += chunk;
        byte_off += chunk;
        len -= chunk;
    }
    return true;
}

static bool ext4_raw_write_bytes(uint64_t byte_off, const void *buf, size_t len) {
    uint8_t sec[VIRTIO_BLK_SECTOR_SIZE];
    const uint8_t *src = (const uint8_t *)buf;

    if (!ext4_attach_source_dev()) {
        return false;
    }

    while (len > 0) {
        uint64_t lba = byte_off / VIRTIO_BLK_SECTOR_SIZE;
        size_t sec_off = (size_t)(byte_off % VIRTIO_BLK_SECTOR_SIZE);
        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - sec_off;
        if (chunk > len) {
            chunk = len;
        }
        if (block_cache_read(lba, sec, 1) != 0) {
            return false;
        }
        memcpy(sec + sec_off, src, chunk);
        if (block_cache_write(lba, sec, 1) != 0) {
            return false;
        }
        src += chunk;
        byte_off += chunk;
        len -= chunk;
    }
    return true;
}

static bool ext4_journal_write_header(uint32_t state, uint32_t rec_count, uint32_t data_bytes, uint32_t seq) {
    if (!g_journal.enabled || g_journal.area_bytes < EXT4_JLOG_REGION_BYTES) {
        return true;
    }
    ext4_jlog_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = EXT4_JLOG_MAGIC;
    h.state = state;
    h.seq = seq;
    h.rec_count = rec_count;
    h.data_bytes = data_bytes;
    return ext4_raw_write_bytes(g_journal.base_off, &h, sizeof(h));
}

static bool ext4_journal_clear(void) {
    if (!g_journal.enabled) {
        return true;
    }
    if (!ext4_journal_write_header(EXT4_JLOG_STATE_CLEAN, 0U, 0U, g_journal.seq)) {
        return false;
    }
    return block_cache_flush() == 0;
}

bool ext4_tx_begin(void) {
    g_ext4_last_errno = 0;
    if (g_jbd2.enabled) {
        bool ok = ext4_jbd2_tx_start();
        if (!ok) {
            g_ext4_last_errno = g_jbd2.tx_enospc ? 28 : 5;
        }
        return ok;
    }
    if (!g_journal.enabled) {
        return true;
    }
    if (g_journal.tx_active) {
        g_ext4_last_errno = 16;
        return false;
    }
    g_journal.tx_active = true;
    g_journal.rec_count = 0U;
    g_journal.data_bytes = 0U;
    g_journal.seq++;
    if (!ext4_journal_write_header(EXT4_JLOG_STATE_PREP, 0U, 0U, g_journal.seq)) {
        g_journal.tx_active = false;
        g_ext4_last_errno = 5;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_journal.tx_active = false;
        g_ext4_last_errno = 5;
        return false;
    }
    return true;
}

void ext4_tx_abort(void) {
    if (g_jbd2.enabled) {
        ext4_jbd2_tx_abort();
        return;
    }
    if (!g_journal.enabled || !g_journal.tx_active) {
        return;
    }
    (void)ext4_journal_clear();
    g_journal.tx_active = false;
    g_journal.rec_count = 0U;
    g_journal.data_bytes = 0U;
}

bool ext4_tx_commit(void) {
    uint32_t rec_count;
    uint32_t data_bytes;

    if (g_jbd2.enabled) {
        bool ok = ext4_jbd2_tx_commit();
        if (!ok) {
            g_ext4_last_errno = g_jbd2.tx_enospc ? 28 : 5;
        }
        return ok;
    }
    if (!g_journal.enabled) {
        return true;
    }
    if (!g_journal.tx_active) {
        return false;
    }
    rec_count = g_journal.rec_count;
    data_bytes = g_journal.data_bytes;
    g_journal.tx_active = false;

    if (!ext4_journal_write_header(EXT4_JLOG_STATE_COMMIT, rec_count,
                                   data_bytes, g_journal.seq)) {
        g_ext4_last_errno = 5;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_ext4_last_errno = 5;
        return false;
    }

    g_journal.replaying = true;
    for (uint32_t i = 0; i < rec_count; i++) {
        const ext4_jlog_rec_t *r = &g_journal.recs[i];
        if (!ext4_raw_write_bytes(r->byte_off, g_journal.data + r->data_off, r->len)) {
            g_journal.replaying = false;
            g_ext4_last_errno = 5;
            return false;
        }
    }
    g_journal.replaying = false;

    if (block_cache_flush() != 0) {
        g_ext4_last_errno = 5;
        return false;
    }
    if (!ext4_journal_clear()) {
        g_ext4_last_errno = 5;
        return false;
    }

    g_journal.rec_count = 0U;
    g_journal.data_bytes = 0U;
    g_ext4_last_errno = 0;
    return true;
}

int ext4_last_error(void) {
    return g_ext4_last_errno;
}

void ext4_periodic_maintenance(uint64_t now_ticks) {
    if (!g_ext4.mounted) {
        return;
    }
    if (g_jbd2.enabled && !g_jbd2.tx_active && g_jbd2.pending_count > 0U) {
        if (g_jbd2.pending_count >= EXT4_JBD2_CHECKPOINT_BATCH ||
            now_ticks - g_jbd2.last_checkpoint_tick >= (EXT4_JBD2_CHECKPOINT_AGE / 2U)) {
            (void)ext4_jbd2_maybe_checkpoint(false);
        }
    }
}

static bool ext4_disk_read_bytes(uint64_t byte_off, void *buf, size_t len) {
    if (!ext4_raw_read_bytes(byte_off, buf, len)) {
        return false;
    }
    if (g_jbd2.enabled && !ext4_jbd2_pending_overlay_read(byte_off, buf, len)) {
        return false;
    }
    if (g_jbd2.enabled && g_jbd2.tx_active && !g_jbd2.io_bypass) {
        if (!ext4_jbd2_tx_overlay_read(byte_off, buf, len)) {
            return false;
        }
    }
    if (!g_journal.enabled || !g_journal.tx_active || len == 0U) {
        return true;
    }

    uint64_t req_start = byte_off;
    uint64_t req_end = req_start + (uint64_t)len;
    if (req_end < req_start) {
        return false;
    }

    for (uint32_t i = 0; i < g_journal.rec_count; i++) {
        const ext4_jlog_rec_t *r = &g_journal.recs[i];
        uint64_t rec_start = r->byte_off;
        uint64_t rec_end = rec_start + (uint64_t)r->len;
        if (rec_end < rec_start || rec_end <= req_start || rec_start >= req_end) {
            continue;
        }
        uint64_t ov_start = rec_start > req_start ? rec_start : req_start;
        uint64_t ov_end = rec_end < req_end ? rec_end : req_end;
        size_t ov_len = (size_t)(ov_end - ov_start);
        size_t dst_off = (size_t)(ov_start - req_start);
        size_t src_off = (size_t)(ov_start - rec_start);
        memcpy((uint8_t *)buf + dst_off, g_journal.data + r->data_off + src_off, ov_len);
    }
    return true;
}

static bool ext4_disk_write_bytes(uint64_t byte_off, const void *buf, size_t len) {
    if (g_jbd2.enabled && g_jbd2.tx_active && !g_jbd2.io_bypass) {
        return ext4_jbd2_tx_write_bytes(byte_off, buf, len);
    }
    if (!g_journal.enabled || !g_journal.tx_active || g_journal.replaying) {
        return ext4_raw_write_bytes(byte_off, buf, len);
    }
    if (len == 0U) {
        return true;
    }
    if (g_journal.rec_count >= EXT4_JLOG_MAX_RECORDS) {
        return false;
    }
    if ((uint64_t)g_journal.data_bytes + (uint64_t)len > (uint64_t)EXT4_JLOG_DATA_BYTES) {
        return false;
    }

    ext4_jlog_rec_t rec;
    rec.byte_off = byte_off;
    rec.len = (uint32_t)len;
    rec.data_off = g_journal.data_bytes;
    g_journal.recs[g_journal.rec_count] = rec;
    memcpy(g_journal.data + g_journal.data_bytes, buf, len);

    uint64_t rec_base = g_journal.base_off + sizeof(ext4_jlog_hdr_t);
    uint64_t data_base = rec_base + (uint64_t)EXT4_JLOG_MAX_RECORDS * sizeof(ext4_jlog_rec_t);
    if (!ext4_raw_write_bytes(rec_base + (uint64_t)g_journal.rec_count * sizeof(ext4_jlog_rec_t),
                              &rec, sizeof(rec))) {
        return false;
    }
    if (!ext4_raw_write_bytes(data_base + rec.data_off, buf, len)) {
        return false;
    }

    g_journal.rec_count++;
    g_journal.data_bytes += (uint32_t)len;
    return true;
}

static bool ext4_read_block(uint64_t block_no, void *buf) {
    uint64_t off = block_no * (uint64_t)g_ext4.block_size;
    return ext4_disk_read_bytes(off, buf, g_ext4.block_size);
}

static bool ext4_write_block(uint64_t block_no, const void *buf) {
    uint64_t off = block_no * (uint64_t)g_ext4.block_size;
    return ext4_disk_write_bytes(off, buf, g_ext4.block_size);
}

static uint32_t ext4_jbd2_seed_from_uuid(const uint8_t *uuid) {
    return crc32c_update(0U, uuid, 16U);
}

static uint32_t ext4_jbd2_checksum_with_hole(uint32_t seed,
                                             const uint8_t *blk,
                                             uint32_t blk_size,
                                             uint32_t csum_off) {
    uint32_t zero = 0U;
    uint32_t crc = seed;
    crc = crc32c_update(crc, blk, csum_off);
    crc = crc32c_update(crc, &zero, sizeof(zero));
    crc = crc32c_update(crc, blk + csum_off + 4U, blk_size - csum_off - 4U);
    return crc;
}

static uint32_t ext4_jbd2_next_block(uint32_t cur, uint32_t first, uint32_t maxlen) {
    if (maxlen <= first || cur < first || cur >= maxlen) {
        return 0U;
    }
    cur++;
    if (cur >= maxlen) {
        cur = first;
    }
    return cur;
}

static uint32_t ext4_jbd2_ring_advance(uint32_t cur, uint32_t steps) {
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring == 0U || cur < g_jbd2.first || cur >= g_jbd2.maxlen) {
        return 0U;
    }
    uint32_t idx = cur - g_jbd2.first;
    idx = (idx + (steps % ring)) % ring;
    return g_jbd2.first + idx;
}

static uint32_t ext4_jbd2_ring_distance(uint32_t from, uint32_t to) {
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring == 0U ||
        from < g_jbd2.first || from >= g_jbd2.maxlen ||
        to < g_jbd2.first || to >= g_jbd2.maxlen) {
        return 0U;
    }
    uint32_t from_idx = from - g_jbd2.first;
    uint32_t to_idx = to - g_jbd2.first;
    if (to_idx >= from_idx) {
        return to_idx - from_idx;
    }
    return ring - (from_idx - to_idx);
}

static uint32_t ext4_jbd2_ring_free_blocks(void) {
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U) {
        return 0U;
    }

    uint32_t head = g_jbd2.head;
    if (head < g_jbd2.first || head >= g_jbd2.maxlen) {
        head = g_jbd2.first;
    }
    uint32_t tail = (g_jbd2.pending_count > 0U) ? g_jbd2.tail : head;
    if (tail < g_jbd2.first || tail >= g_jbd2.maxlen) {
        tail = g_jbd2.first;
    }

    if (g_jbd2.pending_count == 0U) {
        return ring - 1U;
    }
    if (head == tail) {
        return 0U;
    }
    uint32_t distance = ext4_jbd2_ring_distance(head, tail);
    if (distance <= 1U) {
        return 0U;
    }
    return distance - 1U;
}

static uint32_t ext4_jbd2_pending_log_blocks(const ext4_jbd2_pending_t *p) {
    if (!p || !p->used) {
        return 0U;
    }
    return ext4_jbd2_ring_distance(p->start, p->end);
}

static uint32_t ext4_jbd2_target_free_blocks(void) {
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U) {
        return 0U;
    }
    uint32_t target = ring / 4U;
    if (target < 8U) {
        target = 8U;
    }
    if (target >= ring) {
        target = ring - 1U;
    }
    return target;
}

static uint32_t ext4_jbd2_pressure_min_free_blocks(void) {
    uint32_t target = ext4_jbd2_target_free_blocks();
    uint32_t reserve = ext4_jbd2_tx_reserve_blocks();
    return (reserve > target) ? reserve : target;
}

static uint32_t ext4_jbd2_checkpoint_batch_for_need(uint32_t min_free_blocks) {
    uint32_t free_blocks = ext4_jbd2_ring_free_blocks();
    if (g_jbd2.pending_count == 0U || free_blocks >= min_free_blocks) {
        return 0U;
    }

    uint32_t need_blocks = min_free_blocks - free_blocks;
    uint32_t batch = 0U;
    uint32_t reclaim = 0U;
    while (batch < g_jbd2.pending_count && batch < EXT4_JBD2_CHECKPOINT_BATCH) {
        uint32_t blocks = ext4_jbd2_pending_log_blocks(&g_jbd2.pending[batch]);
        if (blocks == 0U) {
            blocks = 1U;
        }
        reclaim += blocks;
        batch++;
        if (reclaim >= need_blocks) {
            break;
        }
    }
    return batch;
}

static uint32_t ext4_jbd2_tx_reserve_blocks(void) {
    if (!g_jbd2.enabled) {
        return 0U;
    }
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U || g_jbd2.block_size == 0U) {
        return 0U;
    }

    uint32_t tag_bytes = ext4_jbd2_tx_tag_bytes();
    uint32_t tail_bytes = (g_jbd2.csum_v2 || g_jbd2.csum_v3) ? 4U : 0U;
    if (g_jbd2.block_size <= 12U + tail_bytes || tag_bytes == 0U) {
        return ring - 1U;
    }
    uint32_t tags_per_desc = (g_jbd2.block_size - 12U - tail_bytes) / tag_bytes;
    if (tags_per_desc == 0U) {
        return ring - 1U;
    }

    uint32_t revoke_entry_bytes = g_jbd2.feat_64bit ? 8U : 4U;
    uint32_t revoke_payload = g_jbd2.block_size - JBD2_REVOKE_ENTRIES_OFF - tail_bytes;
    uint32_t revoke_per_block = revoke_payload / revoke_entry_bytes;
    if (revoke_per_block == 0U) {
        revoke_per_block = 1U;
    }

    uint32_t desc_blocks =
        (EXT4_JBD2_TX_MAX_BLOCKS + tags_per_desc - 1U) / tags_per_desc;
    uint32_t revoke_blocks =
        (EXT4_JBD2_TX_MAX_REVOKES + revoke_per_block - 1U) / revoke_per_block;
    uint32_t total = EXT4_JBD2_TX_MAX_BLOCKS + desc_blocks + revoke_blocks + 1U;
    if (total >= ring) {
        total = ring - 1U;
    }
    return total;
}

static bool ext4_jbd2_read_jblock(const ext4_meta_t *journal_meta, uint32_t j_block, uint8_t *buf) {
    uint64_t pblock = 0;
    if (!journal_meta || !buf) {
        return false;
    }
    if (!ext4_map_file_block(journal_meta, j_block, &pblock) || pblock == 0U) {
        return false;
    }
    return ext4_read_block(pblock, buf);
}

static bool ext4_jbd2_write_jblock(const ext4_meta_t *journal_meta, uint32_t j_block, const uint8_t *buf) {
    uint64_t pblock = 0;
    if (!journal_meta || !buf) {
        return false;
    }
    if (!ext4_map_file_block(journal_meta, j_block, &pblock) || pblock == 0U) {
        return false;
    }
    return ext4_write_block(pblock, buf);
}

static bool ext4_jbd2_revoke_contains(const ext4_jbd2_revoke_t *revokes,
                                      uint32_t revoke_count,
                                      uint64_t fs_block) {
    for (uint32_t i = 0; i < revoke_count; i++) {
        if (revokes[i].fs_block == fs_block) {
            return true;
        }
    }
    return false;
}

static bool ext4_jbd2_add_revoke(ext4_jbd2_revoke_t *revokes,
                                 uint32_t revoke_max,
                                 uint64_t fs_block,
                                 uint32_t seq,
                                 bool keep_latest_seq,
                                 uint32_t *revoke_count_io) {
    if (!revokes || !revoke_count_io || fs_block >= g_ext4.blocks_count) {
        return false;
    }
    for (uint32_t i = 0; i < *revoke_count_io; i++) {
        if (revokes[i].fs_block == fs_block) {
            if (keep_latest_seq && revokes[i].seq < seq) {
                revokes[i].seq = seq;
            }
            return true;
        }
    }
    if (*revoke_count_io >= revoke_max) {
        return false;
    }
    revokes[*revoke_count_io].fs_block = fs_block;
    revokes[*revoke_count_io].seq = seq;
    (*revoke_count_io)++;
    return true;
}

static bool ext4_jbd2_add_update(ext4_jbd2_update_t *updates,
                                 uint32_t update_max,
                                 uint64_t fs_block,
                                 uint32_t j_block,
                                 uint32_t seq,
                                 bool escape,
                                 bool dedupe_same_seq,
                                 uint32_t *update_count_io) {
    if (!updates || !update_count_io) {
        return false;
    }
    if (fs_block >= g_ext4.blocks_count) {
        return false;
    }
    if (j_block < g_jbd2.first || j_block >= g_jbd2.maxlen) {
        return false;
    }
    for (uint32_t i = 0; i < *update_count_io; i++) {
        if (updates[i].fs_block != fs_block) {
            continue;
        }
        if (!dedupe_same_seq || updates[i].seq != seq) {
            continue;
        }
        /* Later tags for the same block in the same tx override earlier images. */
        updates[i].j_block = j_block;
        updates[i].escape = escape;
        return true;
    }
    if (*update_count_io >= update_max) {
        return false;
    }
    updates[*update_count_io].fs_block = fs_block;
    updates[*update_count_io].j_block = j_block;
    updates[*update_count_io].seq = seq;
    updates[*update_count_io].escape = escape;
    (*update_count_io)++;
    return true;
}

static bool ext4_jbd2_revoke_hits_update(uint64_t fs_block,
                                         uint32_t update_seq,
                                         uint32_t revoke_count) {
    for (uint32_t i = 0; i < revoke_count; i++) {
        if (g_jbd2_revokes[i].fs_block != fs_block) {
            continue;
        }
        if (g_jbd2_revokes[i].seq >= update_seq) {
            return true;
        }
    }
    return false;
}

static bool ext4_jbd2_verify_tail_checksum(const uint8_t *blk) {
    if (!blk || g_jbd2.block_size < 4U) {
        return false;
    }
    if (!(g_jbd2.csum_v2 || g_jbd2.csum_v3)) {
        return true;
    }
    uint32_t stored = rd_be32(blk + g_jbd2.block_size - 4U);
    uint32_t calc = ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, blk,
                                                 g_jbd2.block_size,
                                                 g_jbd2.block_size - 4U);
    return stored == calc;
}

static void ext4_jbd2_set_tail_checksum(uint8_t *blk) {
    if (!blk || !(g_jbd2.csum_v2 || g_jbd2.csum_v3) || g_jbd2.block_size < 4U) {
        return;
    }
    wr_be32(blk + g_jbd2.block_size - 4U, 0U);
    wr_be32(blk + g_jbd2.block_size - 4U,
            ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, blk,
                                         g_jbd2.block_size,
                                         g_jbd2.block_size - 4U));
}

static bool ext4_jbd2_verify_commit_checksum(const uint8_t *blk) {
    if (!blk || g_jbd2.block_size < (JBD2_COMMIT_CSUM_OFF + 4U)) {
        return false;
    }
    if (!(g_jbd2.csum_v2 || g_jbd2.csum_v3)) {
        return true;
    }
    if (blk[JBD2_COMMIT_CSUM_TYPE_OFF] != (uint8_t)JBD2_CRC32C_CHKSUM ||
        blk[JBD2_COMMIT_CSUM_SIZE_OFF] != 4U) {
        return false;
    }
    uint32_t stored = rd_be32(blk + JBD2_COMMIT_CSUM_OFF);
    uint32_t calc = ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, blk,
                                                 g_jbd2.block_size,
                                                 JBD2_COMMIT_CSUM_OFF);
    return stored == calc;
}

static void ext4_jbd2_set_commit_checksum(uint8_t *blk) {
    if (!blk || !(g_jbd2.csum_v2 || g_jbd2.csum_v3) || g_jbd2.block_size < (JBD2_COMMIT_CSUM_OFF + 4U)) {
        return;
    }
    blk[JBD2_COMMIT_CSUM_TYPE_OFF] = (uint8_t)JBD2_CRC32C_CHKSUM;
    blk[JBD2_COMMIT_CSUM_SIZE_OFF] = 4U;
    wr_be32(blk + JBD2_COMMIT_CSUM_OFF, 0U);
    wr_be32(blk + JBD2_COMMIT_CSUM_OFF,
            ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, blk,
                                         g_jbd2.block_size,
                                         JBD2_COMMIT_CSUM_OFF));
}

static uint32_t ext4_jbd2_data_checksum32(uint32_t seq, const uint8_t *data) {
    uint8_t seq_be[4];
    wr_be32(seq_be, seq);
    uint32_t crc = crc32c_update(g_jbd2.csum_seed, seq_be, sizeof(seq_be));
    return crc32c_update(crc, data, g_jbd2.block_size);
}

static bool ext4_jbd2_verify_super_checksum(const uint8_t *sb) {
    if (!sb || g_jbd2.block_size <= JBD2_SB_CHECKSUM_OFF) {
        return false;
    }
    if (!(g_jbd2.csum_v2 || g_jbd2.csum_v3)) {
        return true;
    }
    uint32_t stored = rd_be32(sb + JBD2_SB_CHECKSUM_OFF);
    uint32_t calc = ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, sb,
                                                 g_jbd2.block_size,
                                                 JBD2_SB_CHECKSUM_OFF);
    return stored == calc;
}

static bool ext4_jbd2_store_super(void) {
    uint8_t sb[4096];
    uint32_t super_seq = g_jbd2.sequence;
    if (g_jbd2.pending_count > 0U) {
        super_seq = g_jbd2.pending[0].seq;
    }
    if (!g_jbd2.enabled || g_jbd2.block_size > sizeof(sb)) {
        return false;
    }
    if (!ext4_jbd2_read_jblock(&g_jbd2.journal_meta, 0U, sb)) {
        return false;
    }
    wr_be32(sb + JBD2_SB_SEQUENCE_OFF, super_seq);
    wr_be32(sb + JBD2_SB_START_OFF, g_jbd2.start);
    wr_be32(sb + JBD2_SB_HEAD_OFF, g_jbd2.head);
    if (g_jbd2.csum_v2 || g_jbd2.csum_v3) {
        wr_be32(sb + JBD2_SB_CHECKSUM_TYPE_OFF, JBD2_CRC32C_CHKSUM);
        wr_be32(sb + JBD2_SB_CHECKSUM_OFF, 0U);
        wr_be32(sb + JBD2_SB_CHECKSUM_OFF,
                ext4_jbd2_checksum_with_hole(g_jbd2.csum_seed, sb,
                                             g_jbd2.block_size,
                                             JBD2_SB_CHECKSUM_OFF));
    }
    return ext4_jbd2_write_jblock(&g_jbd2.journal_meta, 0U, sb);
}

static void ext4_jbd2_refresh_start_tail(void) {
    if (g_jbd2.pending_count == 0U) {
        g_jbd2.start = 0U;
        g_jbd2.tail = g_jbd2.head;
        return;
    }
    g_jbd2.start = g_jbd2.pending[0].start;
    g_jbd2.tail = g_jbd2.pending[0].start;
}

static bool ext4_jbd2_pending_push(uint32_t seq, uint32_t start, uint32_t end,
                                   const ext4_jbd2_dirty_t *dirty, uint32_t dirty_count) {
    if (g_jbd2.pending_count >= EXT4_JBD2_MAX_PENDING_TX) {
        return false;
    }
    if (!dirty || dirty_count > EXT4_JBD2_TX_MAX_BLOCKS) {
        return false;
    }
    uint32_t i = g_jbd2.pending_count++;
    g_jbd2.pending[i].used = true;
    g_jbd2.pending[i].seq = seq;
    g_jbd2.pending[i].start = start;
    g_jbd2.pending[i].end = end;
    g_jbd2.pending[i].dirty_count = dirty_count;
    for (uint32_t d = 0; d < dirty_count; d++) {
        g_jbd2.pending[i].dirty[d].fs_block = dirty[d].fs_block;
        memcpy(g_jbd2.pending[i].dirty[d].data, dirty[d].data, g_ext4.block_size);
    }
    ext4_jbd2_refresh_start_tail();
    return true;
}

static bool ext4_jbd2_pending_pop(void) {
    if (g_jbd2.pending_count == 0U) {
        return false;
    }
    for (uint32_t i = 1U; i < g_jbd2.pending_count; i++) {
        g_jbd2.pending[i - 1U] = g_jbd2.pending[i];
    }
    g_jbd2.pending_count--;
    if (g_jbd2.pending_count < EXT4_JBD2_MAX_PENDING_TX) {
        memset(&g_jbd2.pending[g_jbd2.pending_count], 0, sizeof(g_jbd2.pending[g_jbd2.pending_count]));
    }
    ext4_jbd2_refresh_start_tail();
    return true;
}

static void ext4_jbd2_remove_revoke(uint64_t fs_block) {
    for (uint32_t i = 0; i < g_jbd2.tx_revoke_count; i++) {
        if (g_jbd2.tx_revokes[i] != fs_block) {
            continue;
        }
        for (uint32_t j = i + 1U; j < g_jbd2.tx_revoke_count; j++) {
            g_jbd2.tx_revokes[j - 1U] = g_jbd2.tx_revokes[j];
        }
        g_jbd2.tx_revoke_count--;
        return;
    }
}

static void ext4_jbd2_remove_dirty(uint64_t fs_block) {
    for (uint32_t i = 0; i < g_jbd2.tx_dirty_count; i++) {
        if (!g_jbd2.tx_dirty[i].used || g_jbd2.tx_dirty[i].fs_block != fs_block) {
            continue;
        }
        for (uint32_t j = i + 1U; j < g_jbd2.tx_dirty_count; j++) {
            g_jbd2.tx_dirty[j - 1U] = g_jbd2.tx_dirty[j];
        }
        g_jbd2.tx_dirty_count--;
        if (g_jbd2.tx_dirty_count < EXT4_JBD2_TX_MAX_BLOCKS) {
            memset(&g_jbd2.tx_dirty[g_jbd2.tx_dirty_count], 0,
                   sizeof(g_jbd2.tx_dirty[g_jbd2.tx_dirty_count]));
        }
        return;
    }
}

static bool ext4_jbd2_checkpoint_some(uint32_t budget) {
    if (!g_jbd2.enabled || g_jbd2.tx_active || g_jbd2.pending_count == 0U) {
        return true;
    }

    g_jbd2.io_bypass = true;
    while (budget > 0U && g_jbd2.pending_count > 0U) {
        const ext4_jbd2_pending_t *p = &g_jbd2.pending[0];
        for (uint32_t i = 0; i < p->dirty_count; i++) {
            if (!ext4_write_block(p->dirty[i].fs_block, p->dirty[i].data)) {
                g_jbd2.io_bypass = false;
                return false;
            }
        }
        if (!ext4_jbd2_pending_pop()) {
            g_jbd2.io_bypass = false;
            return false;
        }
        budget--;
    }

    if (block_cache_flush() != 0) {
        g_jbd2.io_bypass = false;
        return false;
    }
    if (!ext4_jbd2_store_super()) {
        g_jbd2.io_bypass = false;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_jbd2.io_bypass = false;
        return false;
    }
    g_jbd2.last_checkpoint_tick = timer_ticks();
    g_jbd2.io_bypass = false;
    return true;
}

static bool ext4_jbd2_maybe_checkpoint(bool force) {
    if (!g_jbd2.enabled || g_jbd2.pending_count == 0U) {
        return true;
    }
    if (force) {
        return ext4_jbd2_checkpoint_some(EXT4_JBD2_CHECKPOINT_BATCH);
    }
    uint32_t min_free = ext4_jbd2_pressure_min_free_blocks();
    if (min_free != 0U && ext4_jbd2_ring_free_blocks() < min_free) {
        return ext4_jbd2_checkpoint_until_free(min_free);
    }
    if (g_jbd2.pending_count >= EXT4_JBD2_CHECKPOINT_BATCH) {
        return ext4_jbd2_checkpoint_some(EXT4_JBD2_CHECKPOINT_BATCH);
    }
    uint64_t now = timer_ticks();
    if (now - g_jbd2.last_checkpoint_tick >= EXT4_JBD2_CHECKPOINT_AGE) {
        return ext4_jbd2_checkpoint_some(1U);
    }
    return true;
}

static bool ext4_jbd2_checkpoint_until_free(uint32_t min_free_blocks) {
    if (!g_jbd2.enabled) {
        return true;
    }
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U) {
        return false;
    }
    if (min_free_blocks >= ring) {
        min_free_blocks = ring - 1U;
    }
    uint32_t tries = 0U;
    while (g_jbd2.pending_count > 0U &&
           ext4_jbd2_ring_free_blocks() < min_free_blocks) {
        uint32_t batch = ext4_jbd2_checkpoint_batch_for_need(min_free_blocks);
        if (batch == 0U) {
            batch = 1U;
        }
        if (!ext4_jbd2_checkpoint_some(batch)) {
            return false;
        }
        tries++;
        if (tries > (EXT4_JBD2_MAX_PENDING_TX * 2U)) {
            break;
        }
    }
    return ext4_jbd2_ring_free_blocks() >= min_free_blocks;
}

bool ext4_sync_filesystem(void) {
    if (!g_ext4.mounted) {
        return true;
    }
    if (g_jbd2.enabled) {
        if (g_jbd2.tx_active) {
            return false;
        }
        while (g_jbd2.pending_count > 0U) {
            if (!ext4_jbd2_checkpoint_some(EXT4_JBD2_CHECKPOINT_BATCH)) {
                return false;
            }
        }
        if (!ext4_jbd2_store_super()) {
            return false;
        }
    }
    if (g_journal.tx_active) {
        return false;
    }
    return block_cache_flush() == 0;
}

static int ext4_jbd2_find_dirty(uint64_t fs_block) {
    for (uint32_t i = 0; i < g_jbd2.tx_dirty_count; i++) {
        if (g_jbd2.tx_dirty[i].used && g_jbd2.tx_dirty[i].fs_block == fs_block) {
            return (int)i;
        }
    }
    return -1;
}

static int ext4_jbd2_alloc_dirty(uint64_t fs_block) {
    if (g_jbd2.tx_dirty_count >= EXT4_JBD2_TX_MAX_BLOCKS) {
        g_jbd2.tx_enospc = true;
        return -1;
    }
    uint32_t i = g_jbd2.tx_dirty_count++;
    g_jbd2.tx_dirty[i].used = true;
    g_jbd2.tx_dirty[i].fs_block = fs_block;
    if (!ext4_disk_read_bytes(fs_block * (uint64_t)g_jbd2.block_size,
                              g_jbd2.tx_dirty[i].data, g_jbd2.block_size)) {
        g_jbd2.tx_dirty[i].used = false;
        g_jbd2.tx_dirty_count--;
        return -1;
    }
    return (int)i;
}

static bool ext4_jbd2_tx_overlay_read(uint64_t byte_off, void *buf, size_t len) {
    if (!g_jbd2.tx_active || len == 0U) {
        return true;
    }
    uint64_t req_start = byte_off;
    uint64_t req_end = req_start + (uint64_t)len;
    if (req_end < req_start) {
        return false;
    }
    for (uint32_t i = 0; i < g_jbd2.tx_dirty_count; i++) {
        if (!g_jbd2.tx_dirty[i].used) {
            continue;
        }
        uint64_t blk_start = g_jbd2.tx_dirty[i].fs_block * (uint64_t)g_jbd2.block_size;
        uint64_t blk_end = blk_start + (uint64_t)g_jbd2.block_size;
        if (blk_end <= req_start || blk_start >= req_end) {
            continue;
        }
        uint64_t ov_start = blk_start > req_start ? blk_start : req_start;
        uint64_t ov_end = blk_end < req_end ? blk_end : req_end;
        size_t ov_len = (size_t)(ov_end - ov_start);
        size_t dst_off = (size_t)(ov_start - req_start);
        size_t src_off = (size_t)(ov_start - blk_start);
        memcpy((uint8_t *)buf + dst_off, g_jbd2.tx_dirty[i].data + src_off, ov_len);
    }
    return true;
}

static bool ext4_jbd2_pending_overlay_read(uint64_t byte_off, void *buf, size_t len) {
    if (!g_jbd2.enabled || g_jbd2.pending_count == 0U || len == 0U || g_jbd2.io_bypass) {
        return true;
    }
    uint64_t req_start = byte_off;
    uint64_t req_end = req_start + (uint64_t)len;
    if (req_end < req_start) {
        return false;
    }
    for (uint32_t p = 0; p < g_jbd2.pending_count; p++) {
        const ext4_jbd2_pending_t *pt = &g_jbd2.pending[p];
        if (!pt->used) {
            continue;
        }
        for (uint32_t i = 0; i < pt->dirty_count; i++) {
            uint64_t blk_start = pt->dirty[i].fs_block * (uint64_t)g_jbd2.block_size;
            uint64_t blk_end = blk_start + (uint64_t)g_jbd2.block_size;
            if (blk_end <= req_start || blk_start >= req_end) {
                continue;
            }
            uint64_t ov_start = blk_start > req_start ? blk_start : req_start;
            uint64_t ov_end = blk_end < req_end ? blk_end : req_end;
            size_t ov_len = (size_t)(ov_end - ov_start);
            size_t dst_off = (size_t)(ov_start - req_start);
            size_t src_off = (size_t)(ov_start - blk_start);
            memcpy((uint8_t *)buf + dst_off, pt->dirty[i].data + src_off, ov_len);
        }
    }
    return true;
}

static bool ext4_jbd2_tx_write_bytes(uint64_t byte_off, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    if (!g_jbd2.tx_active) {
        return false;
    }
    while (len > 0U) {
        uint64_t fs_block = byte_off / g_jbd2.block_size;
        size_t in_block = (size_t)(byte_off % g_jbd2.block_size);
        size_t chunk = g_jbd2.block_size - in_block;
        if (chunk > len) {
            chunk = len;
        }
        if (fs_block >= g_ext4.blocks_count) {
            return false;
        }
        int idx = ext4_jbd2_find_dirty(fs_block);
        if (idx < 0) {
            idx = ext4_jbd2_alloc_dirty(fs_block);
            if (idx < 0) {
                return false;
            }
        }
        /* Any metadata write to this block supersedes a prior revoke in this tx. */
        ext4_jbd2_remove_revoke(fs_block);
        memcpy(g_jbd2.tx_dirty[idx].data + in_block, src, chunk);
        src += chunk;
        byte_off += chunk;
        len -= chunk;
    }
    return true;
}

static void ext4_jbd2_note_revoke(uint64_t fs_block) {
    if (!g_jbd2.enabled || !g_jbd2.tx_active ||
        fs_block >= g_ext4.blocks_count) {
        return;
    }
    /* A revoke supersedes any prior logged image for this fs block in the tx. */
    ext4_jbd2_remove_dirty(fs_block);
    for (uint32_t i = 0; i < g_jbd2.tx_revoke_count; i++) {
        if (g_jbd2.tx_revokes[i] == fs_block) {
            return;
        }
    }
    if (g_jbd2.tx_revoke_count < EXT4_JBD2_TX_MAX_REVOKES) {
        g_jbd2.tx_revokes[g_jbd2.tx_revoke_count++] = fs_block;
        return;
    }
    g_jbd2.tx_revoke_overflow = true;
}

static bool ext4_jbd2_parse_revoke_block(const uint8_t *blk,
                                         uint32_t seq,
                                         ext4_jbd2_revoke_t *revokes,
                                         uint32_t revoke_max,
                                         uint32_t *revoke_count_io) {
    if (!blk || !revokes || !revoke_count_io || g_ext4.block_size < JBD2_REVOKE_ENTRIES_OFF) {
        return false;
    }
    if (!ext4_jbd2_verify_tail_checksum(blk)) {
        return false;
    }
    uint32_t used = rd_be32(blk + JBD2_REVOKE_COUNT_OFF);
    if (used < JBD2_REVOKE_ENTRIES_OFF || used > g_ext4.block_size) {
        return false;
    }

    uint32_t off = JBD2_REVOKE_ENTRIES_OFF;
    while (off + 4U <= used) {
        uint64_t fs_block = rd_be32(blk + off);
        off += 4U;
        if (g_jbd2.feat_64bit) {
            if (off + 4U > used) {
                return false;
            }
            fs_block |= ((uint64_t)rd_be32(blk + off) << 32);
            off += 4U;
        }
        if (!ext4_jbd2_add_revoke(revokes, revoke_max, fs_block, seq, false, revoke_count_io)) {
            return false;
        }
    }
    return off == used;
}

static bool ext4_jbd2_parse_descriptor_block(const ext4_meta_t *journal_meta,
                                             const uint8_t *blk,
                                             uint32_t seq,
                                             uint32_t first,
                                             uint32_t maxlen,
                                             uint32_t *cursor_io,
                                             const ext4_jbd2_revoke_t *revokes,
                                             uint32_t revoke_count,
                                             ext4_jbd2_update_t *updates,
                                             uint32_t update_max,
                                             uint32_t *update_count_io) {
    if (!journal_meta || !blk || !cursor_io || !updates || !update_count_io || *cursor_io == 0U) {
        return false;
    }
    if (!ext4_jbd2_verify_tail_checksum(blk)) {
        return false;
    }

    uint8_t data_blk[4096];
    uint32_t off = 12U;
    uint32_t data_block = ext4_jbd2_next_block(*cursor_io, first, maxlen);
    if (data_block == 0U) {
        return false;
    }

    while (off + 8U <= g_ext4.block_size) {
        uint64_t fs_block = 0;
        uint32_t flags = 0U;
        bool has_last = false;
        bool escape = false;
        uint32_t tag_sum32 = 0U;
        uint16_t tag_sum16 = 0U;

        if (g_jbd2.csum_v3) {
            if (off + 16U > g_ext4.block_size) {
                return false;
            }
            fs_block = rd_be32(blk + off);
            flags = rd_be32(blk + off + 4U);
            fs_block |= ((uint64_t)rd_be32(blk + off + 8U) << 32);
            tag_sum32 = rd_be32(blk + off + 12U);
            off += 16U;
            if ((flags & JBD2_FLAG_SAME_UUID) == 0U) {
                if (off + 16U > g_ext4.block_size) {
                    return false;
                }
                off += 16U;
            }
        } else {
            if (off + 8U > g_ext4.block_size) {
                return false;
            }
            fs_block = rd_be32(blk + off);
            tag_sum16 = rd_be16(blk + off + 4U);
            flags = rd_be16(blk + off + 6U);
            off += 8U;
            if (g_jbd2.feat_64bit) {
                if (off + 4U > g_ext4.block_size) {
                    return false;
                }
                fs_block |= ((uint64_t)rd_be32(blk + off) << 32);
                off += 4U;
            }
            if ((flags & JBD2_FLAG_SAME_UUID) == 0U) {
                if (off + 16U > g_ext4.block_size) {
                    return false;
                }
                off += 16U;
            }
        }

        has_last = (flags & JBD2_FLAG_LAST_TAG) != 0U;
        escape = (flags & JBD2_FLAG_ESCAPE) != 0U;
        bool deleted = (flags & JBD2_FLAG_DELETED) != 0U;
        if ((flags & ~(JBD2_FLAG_ESCAPE | JBD2_FLAG_SAME_UUID |
                       JBD2_FLAG_DELETED | JBD2_FLAG_LAST_TAG)) != 0U) {
            return false;
        }
        if (fs_block >= g_ext4.blocks_count) {
            return false;
        }

        if (!ext4_jbd2_read_jblock(journal_meta, data_block, data_blk)) {
            return false;
        }
        if (g_jbd2.csum_v2 || g_jbd2.csum_v3) {
            uint32_t calc32 = ext4_jbd2_data_checksum32(seq, data_blk);
            if (g_jbd2.csum_v3) {
                if (calc32 != tag_sum32) {
                    return false;
                }
            } else {
                if (((uint16_t)calc32) != tag_sum16) {
                    return false;
                }
            }
        }

        if (!deleted && !ext4_jbd2_revoke_contains(revokes, revoke_count, fs_block)) {
            if (!ext4_jbd2_add_update(updates, update_max, fs_block, data_block,
                                      seq, escape, true, update_count_io)) {
                return false;
            }
        }

        data_block = ext4_jbd2_next_block(data_block, first, maxlen);
        if (data_block == 0U) {
            return false;
        }
        if (has_last) {
            *cursor_io = data_block;
            return true;
        }
    }
    return false;
}

static bool ext4_jbd2_apply_updates(const ext4_meta_t *journal_meta,
                                    uint32_t update_count,
                                    uint32_t revoke_count) {
    uint8_t blk[4096];
    if (!journal_meta || g_ext4.block_size > sizeof(blk)) {
        return false;
    }
    for (uint32_t i = 0; i < update_count; i++) {
        const ext4_jbd2_update_t *u = &g_jbd2_updates[i];
        if (u->fs_block >= g_ext4.blocks_count) {
            return false;
        }
        if (ext4_jbd2_revoke_hits_update(u->fs_block, u->seq, revoke_count)) {
            continue;
        }
        if (!ext4_jbd2_read_jblock(journal_meta, u->j_block, blk)) {
            return false;
        }
        if (u->escape) {
            wr_be32(blk, JBD2_MAGIC_NUMBER);
        }
        if (!ext4_write_block(u->fs_block, blk)) {
            return false;
        }
    }
    return true;
}

static bool ext4_jbd2_load_state(const uint8_t *ext4_sb) {
    if (!ext4_sb) {
        return false;
    }
    uint32_t journal_inum = rd_le32(ext4_sb + EXT4_S_JOURNAL_INUM);
    uint32_t journal_dev = rd_le32(ext4_sb + EXT4_S_JOURNAL_DEV);
    if (journal_inum == 0U) {
        if (journal_dev != 0U) {
            uart_puts("[ext4] mount failed: external journal unsupported\n");
            return false;
        }
        return false;
    }

    uint8_t jsb[4096];
    memset(&g_jbd2, 0, sizeof(g_jbd2));
    if (!ext4_load_meta(journal_inum, &g_jbd2.journal_meta)) {
        uart_puts("[ext4] mount failed: journal inode load failed\n");
        return false;
    }
    if (g_ext4.block_size > sizeof(jsb)) {
        return false;
    }
    if (!ext4_jbd2_read_jblock(&g_jbd2.journal_meta, 0U, jsb)) {
        uart_puts("[ext4] mount failed: journal super read failed\n");
        return false;
    }
    if (rd_be32(jsb + JBD2_HDR_MAGIC_OFF) != JBD2_MAGIC_NUMBER) {
        uart_puts("[ext4] mount failed: invalid jbd2 super magic\n");
        return false;
    }
    uint32_t sb_type = rd_be32(jsb + JBD2_HDR_TYPE_OFF);
    if (sb_type != JBD2_SUPERBLOCK_V1 && sb_type != JBD2_SUPERBLOCK_V2) {
        uart_puts("[ext4] mount failed: invalid jbd2 super type\n");
        return false;
    }

    g_jbd2.block_size = rd_be32(jsb + JBD2_SB_BLOCKSIZE_OFF);
    g_jbd2.maxlen = rd_be32(jsb + JBD2_SB_MAXLEN_OFF);
    g_jbd2.first = rd_be32(jsb + JBD2_SB_FIRST_OFF);
    g_jbd2.sequence = rd_be32(jsb + JBD2_SB_SEQUENCE_OFF);
    g_jbd2.start = rd_be32(jsb + JBD2_SB_START_OFF);
    g_jbd2.feature_compat = rd_be32(jsb + JBD2_SB_FEAT_COMPAT_OFF);
    g_jbd2.feature_incompat = rd_be32(jsb + JBD2_SB_FEAT_INCOMPAT_OFF);
    g_jbd2.checksum_type = rd_be32(jsb + JBD2_SB_CHECKSUM_TYPE_OFF);
    g_jbd2.head = rd_be32(jsb + JBD2_SB_HEAD_OFF);
    g_jbd2.feat_64bit = (g_jbd2.feature_incompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0U;
    g_jbd2.csum_v2 = (g_jbd2.feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0U;
    g_jbd2.csum_v3 = (g_jbd2.feature_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0U;
    memcpy(g_jbd2.uuid, jsb + 48U, sizeof(g_jbd2.uuid));
    g_jbd2.csum_seed = ext4_jbd2_seed_from_uuid(g_jbd2.uuid);

    if (g_jbd2.block_size != g_ext4.block_size || g_jbd2.maxlen == 0U ||
        g_jbd2.first == 0U || g_jbd2.first >= g_jbd2.maxlen) {
        uart_puts("[ext4] mount failed: unsupported jbd2 geometry\n");
        return false;
    }
    if ((g_jbd2.feature_incompat & ~(JBD2_FEATURE_INCOMPAT_REVOKE |
                                     JBD2_FEATURE_INCOMPAT_64BIT |
                                     JBD2_FEATURE_INCOMPAT_ASYNC |
                                     JBD2_FEATURE_INCOMPAT_CSUM_V2 |
                                     JBD2_FEATURE_INCOMPAT_CSUM_V3 |
                                     JBD2_FEATURE_INCOMPAT_FAST_COMMIT)) != 0U) {
        uart_puts("[ext4] mount failed: unsupported jbd2 incompat feature\n");
        return false;
    }
    if ((g_jbd2.feature_incompat & JBD2_FEATURE_INCOMPAT_FAST_COMMIT) != 0U &&
        rd_be32(jsb + JBD2_SB_NUM_FC_BLKS_OFF) != 0U) {
        uart_puts("[ext4] mount failed: fast-commit journal unsupported\n");
        return false;
    }
    if (g_jbd2.head < g_jbd2.first || g_jbd2.head >= g_jbd2.maxlen) {
        g_jbd2.head = g_jbd2.first;
    }
    g_jbd2.tail = (g_jbd2.start != 0U) ? g_jbd2.start : g_jbd2.head;
    g_jbd2.pending_count = 0U;
    g_jbd2.last_checkpoint_tick = timer_ticks();
    if ((g_jbd2.csum_v2 || g_jbd2.csum_v3) &&
        g_jbd2.checksum_type != JBD2_CRC32C_CHKSUM) {
        uart_puts("[ext4] mount failed: unsupported jbd2 checksum type\n");
        return false;
    }
    g_jbd2.enabled = true;
    if (!ext4_jbd2_verify_super_checksum(jsb)) {
        uart_puts("[ext4] mount failed: invalid jbd2 super checksum\n");
        return false;
    }
    g_jbd2.write_ready = true;
    return true;
}

static bool ext4_jbd2_replay_if_needed(const uint8_t *ext4_sb) {
    if (!ext4_sb || (g_ext4.feature_compat & EXT4_COMPAT_HAS_JOURNAL) == 0U) {
        return true;
    }
    if (!ext4_jbd2_load_state(ext4_sb)) {
        return false;
    }
    if (g_jbd2.start == 0U) {
        return true;
    }
    if (g_jbd2.start < g_jbd2.first || g_jbd2.start >= g_jbd2.maxlen) {
        uart_puts("[ext4] mount failed: invalid jbd2 start\n");
        return false;
    }

    uint8_t blk[4096];
    uint32_t cursor = g_jbd2.start;
    uint32_t seq = g_jbd2.sequence;
    uint32_t update_count = 0U;
    uint32_t revoke_count = 0U;
    uint32_t tx_update_count = 0U;
    uint32_t tx_revoke_count = 0U;
    ext4_jbd2_update_t tx_updates[EXT4_JBD2_TX_MAX_BLOCKS];
    ext4_jbd2_revoke_t tx_revokes[EXT4_JBD2_TX_MAX_REVOKES];
    uint32_t replayed = 0U;
    bool tx_open = false;
    uint32_t guard = g_jbd2.maxlen * 4U;
    bool exhausted = true;
    if (guard < 64U) {
        guard = 64U;
    }
    memset(g_jbd2_updates, 0, sizeof(g_jbd2_updates));
    memset(g_jbd2_revokes, 0, sizeof(g_jbd2_revokes));
    memset(tx_updates, 0, sizeof(tx_updates));
    memset(tx_revokes, 0, sizeof(tx_revokes));

    for (uint32_t i = 0; i < guard; i++) {
        if (!ext4_jbd2_read_jblock(&g_jbd2.journal_meta, cursor, blk)) {
            return false;
        }
        uint32_t magic = rd_be32(blk + JBD2_HDR_MAGIC_OFF);
        uint32_t type = rd_be32(blk + JBD2_HDR_TYPE_OFF);
        uint32_t blk_seq = rd_be32(blk + JBD2_HDR_SEQ_OFF);

        if (magic != JBD2_MAGIC_NUMBER || blk_seq != seq) {
            tx_update_count = 0U;
            tx_revoke_count = 0U;
            tx_open = false;
            exhausted = false;
            break;
        }

        if (type == JBD2_DESCRIPTOR_BLOCK) {
            tx_open = true;
            if (!ext4_jbd2_parse_descriptor_block(&g_jbd2.journal_meta, blk, seq,
                                                  g_jbd2.first, g_jbd2.maxlen, &cursor,
                                                  tx_revokes, tx_revoke_count,
                                                  tx_updates, EXT4_JBD2_TX_MAX_BLOCKS,
                                                  &tx_update_count)) {
                return false;
            }
            continue;
        }
        if (type == JBD2_REVOKE_BLOCK) {
            if ((g_jbd2.feature_incompat & JBD2_FEATURE_INCOMPAT_REVOKE) == 0U) {
                return false;
            }
            tx_open = true;
            if (!ext4_jbd2_parse_revoke_block(blk, seq, tx_revokes,
                                              EXT4_JBD2_TX_MAX_REVOKES,
                                              &tx_revoke_count)) {
                return false;
            }
            cursor = ext4_jbd2_next_block(cursor, g_jbd2.first, g_jbd2.maxlen);
            if (cursor == 0U) {
                return false;
            }
            continue;
        }
        if (type == JBD2_COMMIT_BLOCK) {
            if (!tx_open || !ext4_jbd2_verify_commit_checksum(blk)) {
                return false;
            }
            for (uint32_t r = 0U; r < tx_revoke_count; r++) {
                if (!ext4_jbd2_add_revoke(g_jbd2_revokes, EXT4_JBD2_MAX_REVOKES,
                                          tx_revokes[r].fs_block, tx_revokes[r].seq,
                                          true, &revoke_count)) {
                    return false;
                }
            }
            for (uint32_t u = 0U; u < tx_update_count; u++) {
                if (!ext4_jbd2_add_update(g_jbd2_updates, EXT4_JBD2_MAX_UPDATES,
                                          tx_updates[u].fs_block, tx_updates[u].j_block,
                                          tx_updates[u].seq, tx_updates[u].escape,
                                          false, &update_count)) {
                    return false;
                }
            }
            tx_update_count = 0U;
            tx_revoke_count = 0U;
            tx_open = false;
            replayed++;
            seq++;
            cursor = ext4_jbd2_next_block(cursor, g_jbd2.first, g_jbd2.maxlen);
            if (cursor == 0U) {
                return false;
            }
            continue;
        }
        if (type == JBD2_SUPERBLOCK_V1 || type == JBD2_SUPERBLOCK_V2) {
            if (tx_open || tx_update_count != 0U || tx_revoke_count != 0U) {
                return false;
            }
            if (!ext4_jbd2_verify_super_checksum(blk)) {
                return false;
            }
            cursor = ext4_jbd2_next_block(cursor, g_jbd2.first, g_jbd2.maxlen);
            if (cursor == 0U) {
                return false;
            }
            continue;
        }
        return false;
    }
    if (exhausted) {
        return false;
    }

    if (replayed == 0U) {
        if (g_jbd2.start != 0U) {
            g_jbd2.start = 0U;
            g_jbd2.head = cursor;
            ext4_jbd2_refresh_start_tail();
            if (!ext4_jbd2_store_super()) {
                return false;
            }
            if (block_cache_flush() != 0) {
                return false;
            }
        }
        return true;
    }

    if (!ext4_jbd2_apply_updates(&g_jbd2.journal_meta, update_count, revoke_count)) {
        return false;
    }

    g_jbd2.sequence = seq;
    g_jbd2.start = 0U;
    g_jbd2.head = cursor;
    ext4_jbd2_refresh_start_tail();
    if (!ext4_jbd2_store_super()) {
        return false;
    }

    if (g_ext4.feature_incompat & EXT4_INCOMPAT_RECOVER) {
        uint8_t ext4_sb_buf[EXT4_SUPER_SIZE];
        if (!ext4_disk_read_bytes(EXT4_SUPER_OFFSET, ext4_sb_buf, sizeof(ext4_sb_buf))) {
            return false;
        }
        uint32_t incompat = rd_le32(ext4_sb_buf + EXT4_S_FEATURE_INCOMPAT);
        incompat &= ~EXT4_INCOMPAT_RECOVER;
        wr_le32(ext4_sb_buf + EXT4_S_FEATURE_INCOMPAT, incompat);
        if (!ext4_disk_write_bytes(EXT4_SUPER_OFFSET, ext4_sb_buf, sizeof(ext4_sb_buf))) {
            return false;
        }
        g_ext4.feature_incompat = incompat;
    }

    if (block_cache_flush() != 0) {
        return false;
    }

    (void)replayed;
    return true;
}

static bool ext4_jbd2_tx_start(void) {
    if (!g_jbd2.enabled) {
        return true;
    }
    g_jbd2.tx_enospc = false;
    uint32_t min_free = ext4_jbd2_pressure_min_free_blocks();
    if (g_jbd2.pending_count >= EXT4_JBD2_MAX_PENDING_TX) {
        if (!ext4_jbd2_checkpoint_some(EXT4_JBD2_CHECKPOINT_BATCH) ||
            g_jbd2.pending_count >= EXT4_JBD2_MAX_PENDING_TX) {
            g_jbd2.tx_enospc = true;
            ext4_jbd2_log_enospc(1U);
            return false;
        }
    }
    if (min_free != 0U && ext4_jbd2_ring_free_blocks() < min_free) {
        if (!ext4_jbd2_checkpoint_until_free(min_free)) {
            g_jbd2.tx_enospc = true;
            ext4_jbd2_log_enospc(min_free);
            return false;
        }
    }
    if (!ext4_jbd2_maybe_checkpoint(false)) {
        return false;
    }
    if (!g_jbd2.write_ready || g_jbd2.tx_active) {
        return false;
    }
    g_jbd2.tx_active = true;
    g_jbd2.io_bypass = false;
    g_jbd2.tx_enospc = false;
    g_jbd2.tx_dirty_count = 0U;
    g_jbd2.tx_revoke_count = 0U;
    g_jbd2.tx_revoke_overflow = false;
    memset(g_jbd2.tx_dirty, 0, sizeof(g_jbd2.tx_dirty));
    return true;
}

static void ext4_jbd2_tx_abort(void) {
    if (!g_jbd2.enabled || !g_jbd2.tx_active) {
        return;
    }
    g_jbd2.tx_active = false;
    g_jbd2.io_bypass = false;
    g_jbd2.tx_enospc = false;
    g_jbd2.tx_dirty_count = 0U;
    g_jbd2.tx_revoke_count = 0U;
    g_jbd2.tx_revoke_overflow = false;
}

static bool ext4_jbd2_tx_emit_commit(uint32_t tx_seq, uint32_t *cursor_io) {
    uint8_t blk[4096];
    if (!cursor_io || g_jbd2.block_size > sizeof(blk)) {
        return false;
    }
    memset(blk, 0, g_jbd2.block_size);
    wr_be32(blk + JBD2_HDR_MAGIC_OFF, JBD2_MAGIC_NUMBER);
    wr_be32(blk + JBD2_HDR_TYPE_OFF, JBD2_COMMIT_BLOCK);
    wr_be32(blk + JBD2_HDR_SEQ_OFF, tx_seq);
    ext4_jbd2_set_commit_checksum(blk);
    if (!ext4_jbd2_write_jblock(&g_jbd2.journal_meta, *cursor_io, blk)) {
        return false;
    }
    *cursor_io = ext4_jbd2_next_block(*cursor_io, g_jbd2.first, g_jbd2.maxlen);
    return *cursor_io != 0U;
}

static uint32_t ext4_jbd2_tx_tag_bytes(void) {
    if (g_jbd2.csum_v3) {
        return 16U;
    }
    return g_jbd2.feat_64bit ? 12U : 8U;
}

static bool ext4_jbd2_reserve_ring(uint32_t blocks, uint32_t *start_out, uint32_t *next_head_out) {
    if (!start_out || !next_head_out || blocks == 0U) {
        return false;
    }
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U || blocks >= ring) {
        return false;
    }
    if (blocks > ext4_jbd2_ring_free_blocks()) {
        return false;
    }

    uint32_t head = g_jbd2.head;
    if (head < g_jbd2.first || head >= g_jbd2.maxlen) {
        head = g_jbd2.first;
    }
    *start_out = head;
    *next_head_out = ext4_jbd2_ring_advance(head, blocks);
    if (*next_head_out == 0U) {
        return false;
    }
    if (g_jbd2.pending_count == 0U) {
        return *next_head_out != head;
    }
    uint32_t tail = g_jbd2.tail;
    if (tail < g_jbd2.first || tail >= g_jbd2.maxlen) {
        tail = g_jbd2.first;
    }
    return *next_head_out != tail;
}

static void ext4_jbd2_log_enospc(uint32_t need_blocks) {
    uint64_t now = timer_ticks();
    uint64_t min_gap = TIMER_HZ / 5U;
    if (min_gap == 0U) {
        min_gap = 1U;
    }
    if (now - g_jbd2.last_enospc_log_tick < min_gap) {
        return;
    }
    g_jbd2.last_enospc_log_tick = now;

    uart_puts("[ext4] jbd2 enospc need=");
    print_dec((int)need_blocks);
    uart_puts(" free=");
    print_dec((int)ext4_jbd2_ring_free_blocks());
    uart_puts(" head=");
    print_dec((int)g_jbd2.head);
    uart_puts(" tail=");
    print_dec((int)g_jbd2.tail);
    uart_puts(" pending=");
    print_dec((int)g_jbd2.pending_count);
    uart_puts("\n");
}

static bool ext4_jbd2_tx_commit(void) {
    if (!g_jbd2.enabled) {
        return true;
    }
    if (!g_jbd2.tx_active || !g_jbd2.write_ready) {
        return false;
    }
    g_jbd2.tx_enospc = false;
    if (g_jbd2.tx_revoke_overflow) {
        g_jbd2.tx_enospc = true;
        ext4_jbd2_log_enospc(g_jbd2.tx_revoke_count + 1U);
        return false;
    }
    if (g_jbd2.tx_dirty_count == 0U && g_jbd2.tx_revoke_count == 0U) {
        g_jbd2.tx_active = false;
        return true;
    }
    if (g_jbd2.pending_count >= EXT4_JBD2_MAX_PENDING_TX) {
        if (!ext4_jbd2_checkpoint_some(EXT4_JBD2_CHECKPOINT_BATCH) ||
            g_jbd2.pending_count >= EXT4_JBD2_MAX_PENDING_TX) {
            g_jbd2.tx_enospc = true;
            ext4_jbd2_log_enospc(1U);
            return false;
        }
    }

    uint32_t tag_bytes = ext4_jbd2_tx_tag_bytes();
    uint32_t tail_bytes = (g_jbd2.csum_v2 || g_jbd2.csum_v3) ? 4U : 0U;
    if (g_jbd2.block_size <= 12U + tail_bytes || tag_bytes == 0U) {
        return false;
    }
    uint32_t tags_per_desc = (g_jbd2.block_size - 12U - tail_bytes) / tag_bytes;
    if (tags_per_desc == 0U) {
        return false;
    }
    uint32_t revoke_entry_bytes = g_jbd2.feat_64bit ? 8U : 4U;
    uint32_t revoke_payload = g_jbd2.block_size - JBD2_REVOKE_ENTRIES_OFF - tail_bytes;
    uint32_t revoke_per_block = revoke_payload / revoke_entry_bytes;
    if (revoke_per_block == 0U) {
        revoke_per_block = 1U;
    }
    uint32_t desc_blocks = (g_jbd2.tx_dirty_count + tags_per_desc - 1U) / tags_per_desc;
    uint32_t revoke_blocks = (g_jbd2.tx_revoke_count == 0U) ? 0U :
                             (g_jbd2.tx_revoke_count + revoke_per_block - 1U) / revoke_per_block;
    uint32_t total_blocks = g_jbd2.tx_dirty_count + desc_blocks + revoke_blocks + 1U;
    uint32_t ring = g_jbd2.maxlen - g_jbd2.first;
    if (ring <= 1U || total_blocks >= ring) {
        g_jbd2.tx_enospc = true;
        ext4_jbd2_log_enospc(total_blocks);
        return false;
    }
    uint32_t tx_start = 0U;
    uint32_t tx_next_head = 0U;
    uint32_t reserve_tries = 0U;
    while (!ext4_jbd2_reserve_ring(total_blocks, &tx_start, &tx_next_head)) {
        if (g_jbd2.pending_count == 0U) {
            g_jbd2.tx_enospc = true;
            ext4_jbd2_log_enospc(total_blocks);
            return false;
        }
        if (!ext4_jbd2_checkpoint_until_free(total_blocks)) {
            return false;
        }
        reserve_tries++;
        if (reserve_tries > EXT4_JBD2_MAX_PENDING_TX) {
            g_jbd2.tx_enospc = true;
            ext4_jbd2_log_enospc(total_blocks);
            return false;
        }
    }

    uint8_t desc[4096];
    uint8_t data_blk[4096];
    uint8_t revoke_blk[4096];
    uint8_t seq_be[4];
    uint32_t tx_seq = g_jbd2.sequence;
    wr_be32(seq_be, tx_seq);

    g_jbd2.io_bypass = true;
    if (g_jbd2.pending_count == 0U) {
        g_jbd2.start = tx_start;
    }
    g_jbd2.head = tx_next_head;
    if (!ext4_jbd2_store_super()) {
        g_jbd2.io_bypass = false;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_jbd2.io_bypass = false;
        return false;
    }

    uint32_t cursor = tx_start;
    uint32_t idx = 0U;
    while (idx < g_jbd2.tx_dirty_count) {
        uint32_t count = g_jbd2.tx_dirty_count - idx;
        if (count > tags_per_desc) {
            count = tags_per_desc;
        }

        memset(desc, 0, g_jbd2.block_size);
        wr_be32(desc + JBD2_HDR_MAGIC_OFF, JBD2_MAGIC_NUMBER);
        wr_be32(desc + JBD2_HDR_TYPE_OFF, JBD2_DESCRIPTOR_BLOCK);
        wr_be32(desc + JBD2_HDR_SEQ_OFF, tx_seq);

        uint32_t off = 12U;
        uint32_t data_pos = ext4_jbd2_next_block(cursor, g_jbd2.first, g_jbd2.maxlen);
        if (data_pos == 0U) {
            g_jbd2.io_bypass = false;
            return false;
        }
        for (uint32_t j = 0U; j < count; j++, idx++) {
            ext4_jbd2_dirty_t *d = &g_jbd2.tx_dirty[idx];
            uint32_t flags = JBD2_FLAG_SAME_UUID;
            bool escape = rd_be32(d->data) == JBD2_MAGIC_NUMBER;
            if (escape) {
                flags |= JBD2_FLAG_ESCAPE;
            }
            if (j + 1U == count) {
                flags |= JBD2_FLAG_LAST_TAG;
            }

            uint32_t sum = crc32c_update(g_jbd2.csum_seed, seq_be, sizeof(seq_be));
            sum = crc32c_update(sum, d->data, g_jbd2.block_size);

            if (g_jbd2.csum_v3) {
                wr_be32(desc + off, (uint32_t)(d->fs_block & 0xFFFFFFFFU));
                wr_be32(desc + off + 4U, flags);
                wr_be32(desc + off + 8U, (uint32_t)(d->fs_block >> 32));
                wr_be32(desc + off + 12U, sum);
                off += 16U;
            } else {
                wr_be32(desc + off, (uint32_t)(d->fs_block & 0xFFFFFFFFU));
                wr_be16(desc + off + 4U, g_jbd2.csum_v2 ? (uint16_t)sum : 0U);
                wr_be16(desc + off + 6U, (uint16_t)(flags & 0xFFFFU));
                off += 8U;
                if (g_jbd2.feat_64bit) {
                    wr_be32(desc + off, (uint32_t)(d->fs_block >> 32));
                    off += 4U;
                }
            }

            memcpy(data_blk, d->data, g_jbd2.block_size);
            if (escape) {
                wr_be32(data_blk, 0U);
            }
            if (!ext4_jbd2_write_jblock(&g_jbd2.journal_meta, data_pos, data_blk)) {
                g_jbd2.io_bypass = false;
                return false;
            }
            data_pos = ext4_jbd2_next_block(data_pos, g_jbd2.first, g_jbd2.maxlen);
            if (data_pos == 0U) {
                g_jbd2.io_bypass = false;
                return false;
            }
        }
        if (g_jbd2.csum_v2 || g_jbd2.csum_v3) {
            ext4_jbd2_set_tail_checksum(desc);
        }
        if (!ext4_jbd2_write_jblock(&g_jbd2.journal_meta, cursor, desc)) {
            g_jbd2.io_bypass = false;
            return false;
        }
        cursor = data_pos;
    }

    uint32_t rev_idx = 0U;
    while (rev_idx < g_jbd2.tx_revoke_count) {
        uint32_t count = g_jbd2.tx_revoke_count - rev_idx;
        if (count > revoke_per_block) {
            count = revoke_per_block;
        }
        memset(revoke_blk, 0, g_jbd2.block_size);
        wr_be32(revoke_blk + JBD2_HDR_MAGIC_OFF, JBD2_MAGIC_NUMBER);
        wr_be32(revoke_blk + JBD2_HDR_TYPE_OFF, JBD2_REVOKE_BLOCK);
        wr_be32(revoke_blk + JBD2_HDR_SEQ_OFF, tx_seq);
        uint32_t off = JBD2_REVOKE_ENTRIES_OFF;
        for (uint32_t i = 0U; i < count; i++, rev_idx++) {
            uint64_t b = g_jbd2.tx_revokes[rev_idx];
            wr_be32(revoke_blk + off, (uint32_t)(b & 0xFFFFFFFFU));
            off += 4U;
            if (g_jbd2.feat_64bit) {
                wr_be32(revoke_blk + off, (uint32_t)(b >> 32));
                off += 4U;
            }
        }
        wr_be32(revoke_blk + JBD2_REVOKE_COUNT_OFF, off);
        if (g_jbd2.csum_v2 || g_jbd2.csum_v3) {
            ext4_jbd2_set_tail_checksum(revoke_blk);
        }
        if (!ext4_jbd2_write_jblock(&g_jbd2.journal_meta, cursor, revoke_blk)) {
            g_jbd2.io_bypass = false;
            return false;
        }
        cursor = ext4_jbd2_next_block(cursor, g_jbd2.first, g_jbd2.maxlen);
        if (cursor == 0U) {
            g_jbd2.io_bypass = false;
            return false;
        }
    }

    if (!ext4_jbd2_tx_emit_commit(tx_seq, &cursor)) {
        g_jbd2.io_bypass = false;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_jbd2.io_bypass = false;
        return false;
    }

    if (!ext4_jbd2_pending_push(tx_seq, tx_start, cursor,
                                g_jbd2.tx_dirty, g_jbd2.tx_dirty_count)) {
        g_jbd2.io_bypass = false;
        return false;
    }
    g_jbd2.head = cursor;
    g_jbd2.sequence = tx_seq + 1U;
    if (!ext4_jbd2_store_super()) {
        g_jbd2.io_bypass = false;
        return false;
    }
    if (block_cache_flush() != 0) {
        g_jbd2.io_bypass = false;
        return false;
    }
    g_jbd2.io_bypass = false;
    g_jbd2.tx_active = false;
    g_jbd2.tx_enospc = false;
    g_jbd2.tx_dirty_count = 0U;
    g_jbd2.tx_revoke_count = 0U;
    g_jbd2.tx_revoke_overflow = false;
    uint32_t min_free = ext4_jbd2_pressure_min_free_blocks();
    if (g_jbd2.pending_count >= EXT4_JBD2_CHECKPOINT_BATCH ||
        (min_free != 0U && ext4_jbd2_ring_free_blocks() < min_free)) {
        (void)ext4_jbd2_maybe_checkpoint(false);
    }
    return true;
}

static bool ext4_ordered_checkpoint(void) {
    return block_cache_flush() == 0;
}

static bool ext4_journal_setup(void) {
    uint64_t cap_bytes = block_cache_capacity_sectors() * (uint64_t)VIRTIO_BLK_SECTOR_SIZE;
    uint64_t fs_bytes = g_ext4.blocks_count * (uint64_t)g_ext4.block_size;

    memset(&g_journal, 0, sizeof(g_journal));
    if (cap_bytes < fs_bytes + (uint64_t)EXT4_JLOG_REGION_BYTES) {
        return true;
    }

    g_journal.enabled = true;
    g_journal.base_off = cap_bytes - (uint64_t)EXT4_JLOG_REGION_BYTES;
    g_journal.area_bytes = EXT4_JLOG_REGION_BYTES;
    return true;
}

static bool ext4_journal_replay_if_needed(void) {
    if (!g_journal.enabled) {
        return true;
    }

    ext4_jlog_hdr_t h;
    if (!ext4_raw_read_bytes(g_journal.base_off, &h, sizeof(h))) {
        return false;
    }
    if (h.magic != EXT4_JLOG_MAGIC) {
        return ext4_journal_clear();
    }
    if (h.state == EXT4_JLOG_STATE_CLEAN || h.rec_count == 0U || h.data_bytes == 0U) {
        return ext4_journal_clear();
    }
    if (h.state != EXT4_JLOG_STATE_COMMIT) {
        return ext4_journal_clear();
    }
    if (h.rec_count > EXT4_JLOG_MAX_RECORDS || h.data_bytes > EXT4_JLOG_DATA_BYTES) {
        return ext4_journal_clear();
    }

    uint64_t rec_base = g_journal.base_off + sizeof(ext4_jlog_hdr_t);
    uint64_t data_base = rec_base + (uint64_t)EXT4_JLOG_MAX_RECORDS * sizeof(ext4_jlog_rec_t);

    g_journal.replaying = true;
    for (uint32_t i = 0; i < h.rec_count; i++) {
        ext4_jlog_rec_t r;
        if (!ext4_raw_read_bytes(rec_base + (uint64_t)i * sizeof(ext4_jlog_rec_t), &r, sizeof(r))) {
            g_journal.replaying = false;
            return false;
        }
        if (r.len == 0U || r.data_off > h.data_bytes || (uint64_t)r.data_off + (uint64_t)r.len > h.data_bytes) {
            g_journal.replaying = false;
            return false;
        }
        if (!ext4_raw_read_bytes(data_base + r.data_off, g_journal.data, r.len)) {
            g_journal.replaying = false;
            return false;
        }
        if (!ext4_raw_write_bytes(r.byte_off, g_journal.data, r.len)) {
            g_journal.replaying = false;
            return false;
        }
    }
    g_journal.replaying = false;
    if (block_cache_flush() != 0) {
        return false;
    }
    return ext4_journal_clear();
}

static uint64_t ext4_group_desc_offset(uint32_t group) {
    uint32_t gd_table_block = (g_ext4.block_size == 1024U) ? 2U : 1U;
    return (uint64_t)gd_table_block * g_ext4.block_size + (uint64_t)group * g_ext4.desc_size;
}

static bool ext4_group_read_desc(uint32_t group, uint8_t *gd) {
    if (!gd || group >= g_ext4.groups_count) {
        return false;
    }
    if (g_ext4.desc_size < 32U || g_ext4.desc_size > g_ext4.block_size) {
        return false;
    }
    return ext4_disk_read_bytes(ext4_group_desc_offset(group), gd, g_ext4.desc_size);
}

static bool ext4_group_write_desc(uint32_t group, const uint8_t *gd) {
    if (!gd || group >= g_ext4.groups_count) {
        return false;
    }
    if (g_ext4.desc_size < 32U || g_ext4.desc_size > g_ext4.block_size) {
        return false;
    }
    return ext4_disk_write_bytes(ext4_group_desc_offset(group), gd, g_ext4.desc_size);
}

static uint64_t ext4_group_get_block_ref(const uint8_t *gd, uint32_t lo_off, uint32_t hi_off) {
    uint64_t v = rd_le32(gd + lo_off);
    if (g_ext4.has_64bit && g_ext4.desc_size >= 64U) {
        v |= ((uint64_t)rd_le32(gd + hi_off) << 32);
    }
    return v;
}

static uint32_t ext4_group_get_count16(const uint8_t *gd, uint32_t lo_off, uint32_t hi_off) {
    uint32_t v = rd_le16(gd + lo_off);
    if (g_ext4.has_64bit && g_ext4.desc_size >= 64U) {
        v |= ((uint32_t)rd_le16(gd + hi_off) << 16);
    }
    return v;
}

static void ext4_group_set_count16(uint8_t *gd, uint32_t lo_off, uint32_t hi_off, uint32_t v) {
    wr_le16(gd + lo_off, (uint16_t)(v & 0xFFFFU));
    if (g_ext4.has_64bit && g_ext4.desc_size >= 64U) {
        wr_le16(gd + hi_off, (uint16_t)((v >> 16) & 0xFFFFU));
    }
}

static bool ext4_inode_offset(uint32_t ino, uint64_t *off_out) {
    if (!off_out || ino == 0U || ino > g_ext4.inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1U) / g_ext4.inodes_per_group;
    uint32_t index = (ino - 1U) % g_ext4.inodes_per_group;

    uint8_t gd[4096];
    if (!ext4_group_read_desc(group, gd)) {
        return false;
    }
    uint64_t inode_table_block = ext4_group_get_block_ref(
        gd, EXT4_BG_INODE_TABLE_LO, EXT4_BG_INODE_TABLE_HI);
    if (inode_table_block == 0U) {
        return false;
    }

    *off_out = inode_table_block * (uint64_t)g_ext4.block_size +
               (uint64_t)index * g_ext4.inode_size;
    return true;
}

static bool ext4_load_meta(uint32_t ino, ext4_meta_t *out) {
    if (!out || ino == 0 || ino > g_ext4.inodes_count) {
        return false;
    }
    uint64_t inode_off = 0;
    if (!ext4_inode_offset(ino, &inode_off)) {
        return false;
    }
    uint8_t raw[4096];
    size_t rd = g_ext4.inode_size;
    if (rd > sizeof(raw)) {
        rd = sizeof(raw);
    }
    if (!ext4_disk_read_bytes(inode_off, raw, rd)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->ino = ino;
    out->mode = rd_le16(raw + EXT4_INODE_MODE);
    out->links = rd_le16(raw + EXT4_INODE_LINKS_COUNT);
    out->blocks_lo = rd_le32(raw + EXT4_INODE_BLOCKS_LO);
    out->flags = rd_le32(raw + EXT4_INODE_FLAGS);
    out->size = rd_le32(raw + EXT4_INODE_SIZE_LO);
    out->size |= ((uint64_t)rd_le32(raw + EXT4_INODE_SIZE_HIGH) << 32);
    for (int i = 0; i < 15; i++) {
        out->block[i] = rd_le32(raw + EXT4_INODE_BLOCK + (size_t)i * 4U);
    }
    return true;
}

static ext4_meta_t *ext4_meta_for_inode(const inode_t *inode) {
    if (!inode || !g_ext4.mounted || inode->fs_kind != FS_KIND_EXT4) {
        return 0;
    }
    if (inode == g_ext4.mountpoint) {
        return &g_ext4.root_meta;
    }
    for (int i = 0; i < EXT4_CACHE_MAX; i++) {
        if (g_ext4.cache_meta[i].valid && &g_ext4.cache_nodes[i] == inode) {
            return &g_ext4.cache_meta[i];
        }
    }
    return 0;
}

static bool ext4_is_dir_mode(uint16_t mode) {
    return (mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
}

static bool ext4_is_reg_mode(uint16_t mode) {
    return (mode & EXT4_S_IFMT) == EXT4_S_IFREG;
}

static bool ext4_is_lnk_mode(uint16_t mode) {
    return (mode & EXT4_S_IFMT) == EXT4_S_IFLNK;
}

static inode_type_t ext4_mode_to_inode_type(uint16_t mode) {
    if (ext4_is_dir_mode(mode)) {
        return INODE_DIR;
    }
    if (ext4_is_reg_mode(mode) || ext4_is_lnk_mode(mode)) {
        return INODE_FILE;
    }
    return INODE_DEV;
}

static int ext4_normalize_abs_path(const char *in, char *out, size_t outsz) {
    char comps[16][INODE_NAME_MAX + 1];
    int comp_count = 0;
    const char *p = in;

    if (!in || in[0] != '/' || outsz < 2) {
        return -1;
    }

    while (*p == '/') {
        p++;
    }

    while (*p) {
        char comp[INODE_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/') {
            if (n < INODE_NAME_MAX) {
                comp[n++] = *p;
            }
            p++;
        }
        comp[n] = '\0';
        while (*p == '/') {
            p++;
        }
        if (n == 0 || (n == 1 && comp[0] == '.')) {
            continue;
        }
        if (n == 2 && comp[0] == '.' && comp[1] == '.') {
            if (comp_count > 0) {
                comp_count--;
            }
            continue;
        }
        if (comp_count >= (int)(sizeof(comps) / sizeof(comps[0]))) {
            return -1;
        }
        strcpy(comps[comp_count++], comp);
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < comp_count; i++) {
        size_t n = strlen(comps[i]);
        if (pos + n + 1 >= outsz) {
            return -1;
        }
        if (pos > 1) {
            out[pos++] = '/';
        }
        memcpy(out + pos, comps[i], n);
        pos += n;
    }
    out[pos] = '\0';
    return 0;
}

static int ext4_inode_abs_path(const inode_t *node, char *out, size_t outsz) {
    const inode_t *stack[16];
    int n = 0;
    const inode_t *cur = node;

    if (!node || !out || outsz < 2) {
        return -1;
    }
    while (cur && cur->parent && n < (int)(sizeof(stack) / sizeof(stack[0]))) {
        stack[n++] = cur;
        cur = cur->parent;
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = n - 1; i >= 0; i--) {
        size_t seg = strlen(stack[i]->name);
        if (seg == 0) {
            continue;
        }
        if (pos > 1) {
            if (pos + 1 >= outsz) {
                return -1;
            }
            out[pos++] = '/';
        }
        if (pos + seg >= outsz) {
            return -1;
        }
        memcpy(out + pos, stack[i]->name, seg);
        pos += seg;
    }
    out[pos] = '\0';
    return 0;
}

static bool ext4_read_symlink_target(const ext4_meta_t *m, char *out, size_t outsz) {
    if (!m || !out || outsz < 2 || !ext4_is_lnk_mode(m->mode) || m->size == 0 ||
        m->size >= outsz) {
        return false;
    }

    if (!(m->flags & EXT4_EXTENTS_FL) && m->size <= sizeof(m->block)) {
        memcpy(out, (const uint8_t *)m->block, (size_t)m->size);
        out[m->size] = '\0';
        return true;
    }

    int n = ext4_read_file_at(m, 0, out, (size_t)m->size);
    if (n != (int)m->size) {
        return false;
    }
    out[m->size] = '\0';
    return true;
}

static inode_t *ext4_resolve_symlink(inode_t *dir, const ext4_meta_t *m) {
    char target[MAX_PATH];
    char base[MAX_PATH];
    char joined[MAX_PATH];
    char normalized[MAX_PATH];

    if (!dir || !m) {
        return 0;
    }
    if (!ext4_read_symlink_target(m, target, sizeof(target))) {
        return 0;
    }

    if (target[0] == '/') {
        if (ext4_normalize_abs_path(target, normalized, sizeof(normalized)) != 0) {
            return 0;
        }
        return fs_lookup(normalized);
    }

    if (ext4_inode_abs_path(dir, base, sizeof(base)) != 0) {
        return 0;
    }
    if (strcmp(base, "/") == 0) {
        joined[0] = '/';
        joined[1] = '\0';
    } else {
        strncpy(joined, base, sizeof(joined) - 1);
        joined[sizeof(joined) - 1] = '\0';
    }
    size_t len = strlen(joined);
    if (len + 1 >= sizeof(joined)) {
        return 0;
    }
    if (len == 0) {
        joined[len++] = '/';
    }
    if (joined[len - 1] != '/') {
        joined[len++] = '/';
    }
    joined[len] = '\0';
    size_t tlen = strlen(target);
    if (len + tlen >= sizeof(joined)) {
        return 0;
    }
    memcpy(joined + len, target, tlen);
    joined[len + tlen] = '\0';

    if (ext4_normalize_abs_path(joined, normalized, sizeof(normalized)) != 0) {
        return 0;
    }
    return fs_lookup(normalized);
}

static inode_t *ext4_cache_get(uint32_t ino, inode_t *parent, const char *name) {
    for (int i = 0; i < EXT4_CACHE_MAX; i++) {
        if (!g_ext4.cache_meta[i].valid) {
            continue;
        }
        if (g_ext4.cache_meta[i].ino == ino) {
            if (parent) {
                g_ext4.cache_nodes[i].parent = parent;
            }
            if (name) {
                strncpy(g_ext4.cache_nodes[i].name, name, INODE_NAME_MAX);
                g_ext4.cache_nodes[i].name[INODE_NAME_MAX] = '\0';
            }
            g_ext4.cache_nodes[i].fs_id = g_ext4.instance_id;
            return &g_ext4.cache_nodes[i];
        }
    }

    int slot = -1;
    for (int i = 0; i < EXT4_CACHE_MAX; i++) {
        if (!g_ext4.cache_meta[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0;
    }

    ext4_meta_t m;
    if (!ext4_load_meta(ino, &m)) {
        return 0;
    }

    inode_t *node = &g_ext4.cache_nodes[slot];
    memset(node, 0, sizeof(*node));
    if (name) {
        strncpy(node->name, name, INODE_NAME_MAX);
        node->name[INODE_NAME_MAX] = '\0';
    }
    node->type = ext4_mode_to_inode_type(m.mode);
    node->fs_kind = FS_KIND_EXT4;
    node->fs_id = g_ext4.instance_id;
    node->fs_ino = ino;
    node->parent = parent;
    node->writable = g_ext4.write_enabled &&
                     ext4_is_reg_mode(m.mode) &&
                     ((m.mode & 0222U) != 0U);
    node->executable = ext4_is_reg_mode(m.mode) && ((m.mode & 0111U) != 0U);
    node->size = (size_t)m.size;

    g_ext4.cache_meta[slot] = m;
    return node;
}

static uint32_t ext4_group_blocks_count(uint32_t group) {
    uint64_t group_first = (uint64_t)g_ext4.first_data_block +
                           (uint64_t)group * g_ext4.blocks_per_group;
    if (group_first >= g_ext4.blocks_count) {
        return 0;
    }
    uint64_t rem = g_ext4.blocks_count - group_first;
    uint64_t g = min_u64(rem, g_ext4.blocks_per_group);
    return (uint32_t)g;
}

static bool ext4_super_write_counts(void) {
    uint8_t sb[EXT4_SUPER_SIZE];
    if (!ext4_disk_read_bytes(EXT4_SUPER_OFFSET, sb, sizeof(sb))) {
        return false;
    }
    wr_le32(sb + EXT4_S_FREE_BLOCKS_COUNT_LO, (uint32_t)(g_ext4.free_blocks_count & 0xFFFFFFFFU));
    wr_le32(sb + EXT4_S_FREE_INODES_COUNT, g_ext4.free_inodes_count);
    if (g_ext4.has_64bit) {
        wr_le32(sb + EXT4_S_FREE_BLOCKS_COUNT_HI, (uint32_t)(g_ext4.free_blocks_count >> 32));
    }
    return ext4_disk_write_bytes(EXT4_SUPER_OFFSET, sb, sizeof(sb));
}

static bool ext4_group_adjust_free(uint32_t group, int dblocks, int dinodes) {
    uint8_t gd[4096];
    if (!ext4_group_read_desc(group, gd)) {
        return false;
    }
    uint32_t free_blocks = ext4_group_get_count16(
        gd, EXT4_BG_FREE_BLOCKS_COUNT_LO, EXT4_BG_FREE_BLOCKS_COUNT_HI);
    uint32_t free_inodes = ext4_group_get_count16(
        gd, EXT4_BG_FREE_INODES_COUNT_LO, EXT4_BG_FREE_INODES_COUNT_HI);

    if (dblocks < 0 && free_blocks < (uint32_t)(-dblocks)) {
        return false;
    }
    if (dinodes < 0 && free_inodes < (uint32_t)(-dinodes)) {
        return false;
    }
    free_blocks = (uint32_t)((int)free_blocks + dblocks);
    free_inodes = (uint32_t)((int)free_inodes + dinodes);
    ext4_group_set_count16(gd, EXT4_BG_FREE_BLOCKS_COUNT_LO, EXT4_BG_FREE_BLOCKS_COUNT_HI, free_blocks);
    ext4_group_set_count16(gd, EXT4_BG_FREE_INODES_COUNT_LO, EXT4_BG_FREE_INODES_COUNT_HI, free_inodes);
    return ext4_group_write_desc(group, gd);
}

static bool ext4_inode_store(const ext4_meta_t *m) {
    if (!m || !m->valid) {
        return false;
    }
    uint64_t inode_off = 0;
    if (!ext4_inode_offset(m->ino, &inode_off)) {
        return false;
    }
    uint8_t raw[4096];
    size_t rd = g_ext4.inode_size;
    if (rd > sizeof(raw)) {
        rd = sizeof(raw);
    }
    if (!ext4_disk_read_bytes(inode_off, raw, rd)) {
        return false;
    }

    wr_le16(raw + EXT4_INODE_MODE, m->mode);
    wr_le16(raw + EXT4_INODE_LINKS_COUNT, m->links);
    wr_le32(raw + EXT4_INODE_BLOCKS_LO, m->blocks_lo);
    wr_le32(raw + EXT4_INODE_FLAGS, m->flags);
    wr_le32(raw + EXT4_INODE_SIZE_LO, (uint32_t)(m->size & 0xFFFFFFFFU));
    wr_le32(raw + EXT4_INODE_SIZE_HIGH, (uint32_t)(m->size >> 32));
    for (int i = 0; i < 15; i++) {
        wr_le32(raw + EXT4_INODE_BLOCK + (size_t)i * 4U, m->block[i]);
    }
    return ext4_disk_write_bytes(inode_off, raw, rd);
}

static bool ext4_alloc_from_bitmap(uint64_t bitmap_block, uint32_t bits, uint32_t start_bit, uint32_t *idx_out) {
    uint8_t bm[4096];
    if (!idx_out || bits == 0U || start_bit >= bits || !ext4_read_block(bitmap_block, bm)) {
        return false;
    }
    for (uint32_t i = start_bit; i < bits; i++) {
        uint32_t byte = i >> 3;
        uint8_t bit = (uint8_t)(1U << (i & 7U));
        if ((bm[byte] & bit) != 0U) {
            continue;
        }
        bm[byte] |= bit;
        if (!ext4_write_block(bitmap_block, bm)) {
            return false;
        }
        *idx_out = i;
        return true;
    }
    return false;
}

static bool ext4_alloc_block(uint64_t *block_out) {
    if (!block_out || g_ext4.free_blocks_count == 0U) {
        return false;
    }
    for (uint32_t g = 0; g < g_ext4.groups_count; g++) {
        uint32_t group_blocks = ext4_group_blocks_count(g);
        if (group_blocks == 0U) {
            continue;
        }

        uint8_t gd[4096];
        if (!ext4_group_read_desc(g, gd)) {
            return false;
        }
        uint32_t free_blocks = ext4_group_get_count16(
            gd, EXT4_BG_FREE_BLOCKS_COUNT_LO, EXT4_BG_FREE_BLOCKS_COUNT_HI);
        if (free_blocks == 0U) {
            continue;
        }
        uint64_t bb = ext4_group_get_block_ref(gd, EXT4_BG_BLOCK_BITMAP_LO, EXT4_BG_BLOCK_BITMAP_HI);
        if (bb == 0U) {
            continue;
        }

        uint32_t idx = 0;
        if (!ext4_alloc_from_bitmap(bb, group_blocks, 0, &idx)) {
            continue;
        }

        if (!ext4_group_adjust_free(g, -1, 0)) {
            return false;
        }
        if (g_ext4.free_blocks_count == 0U) {
            return false;
        }
        g_ext4.free_blocks_count--;
        if (!ext4_super_write_counts()) {
            return false;
        }

        *block_out = (uint64_t)g_ext4.first_data_block +
                     (uint64_t)g * g_ext4.blocks_per_group + idx;
        return true;
    }
    return false;
}

static bool ext4_alloc_inode(uint32_t *ino_out) {
    if (!ino_out || g_ext4.free_inodes_count == 0U) {
        return false;
    }
    for (uint32_t g = 0; g < g_ext4.groups_count; g++) {
        uint8_t gd[4096];
        if (!ext4_group_read_desc(g, gd)) {
            return false;
        }
        uint32_t free_inodes = ext4_group_get_count16(
            gd, EXT4_BG_FREE_INODES_COUNT_LO, EXT4_BG_FREE_INODES_COUNT_HI);
        if (free_inodes == 0U) {
            continue;
        }
        uint64_t ib = ext4_group_get_block_ref(gd, EXT4_BG_INODE_BITMAP_LO, EXT4_BG_INODE_BITMAP_HI);
        if (ib == 0U) {
            continue;
        }

        uint32_t max_bits = g_ext4.inodes_per_group;
        uint64_t group_first_ino = (uint64_t)g * g_ext4.inodes_per_group + 1U;
        if (group_first_ino + max_bits - 1U > g_ext4.inodes_count) {
            max_bits = (uint32_t)(g_ext4.inodes_count - group_first_ino + 1U);
        }

        uint32_t start_bit = 0;
        if (group_first_ino <= g_ext4.first_ino) {
            uint64_t delta = g_ext4.first_ino - group_first_ino;
            if (delta >= max_bits) {
                continue;
            }
            start_bit = (uint32_t)delta;
        }

        uint32_t idx = 0;
        if (!ext4_alloc_from_bitmap(ib, max_bits, start_bit, &idx)) {
            continue;
        }

        if (!ext4_group_adjust_free(g, 0, -1)) {
            return false;
        }
        if (g_ext4.free_inodes_count == 0U) {
            return false;
        }
        g_ext4.free_inodes_count--;
        if (!ext4_super_write_counts()) {
            return false;
        }

        *ino_out = (uint32_t)group_first_ino + idx;
        return true;
    }
    return false;
}

static bool ext4_block_to_group_idx(uint64_t block, uint32_t *group_out, uint32_t *idx_out) {
    if (!group_out || !idx_out || block < g_ext4.first_data_block || block >= g_ext4.blocks_count) {
        return false;
    }
    uint64_t rel = block - g_ext4.first_data_block;
    uint32_t group = (uint32_t)(rel / g_ext4.blocks_per_group);
    uint32_t idx = (uint32_t)(rel % g_ext4.blocks_per_group);
    if (group >= g_ext4.groups_count) {
        return false;
    }
    if (idx >= ext4_group_blocks_count(group)) {
        return false;
    }
    *group_out = group;
    *idx_out = idx;
    return true;
}

static bool ext4_free_block(uint64_t block) {
    uint32_t group = 0, idx = 0;
    if (!ext4_block_to_group_idx(block, &group, &idx)) {
        return false;
    }

    uint8_t gd[4096];
    if (!ext4_group_read_desc(group, gd)) {
        return false;
    }
    uint64_t bb = ext4_group_get_block_ref(gd, EXT4_BG_BLOCK_BITMAP_LO, EXT4_BG_BLOCK_BITMAP_HI);
    if (bb == 0U) {
        return false;
    }

    uint8_t bm[4096];
    if (!ext4_read_block(bb, bm)) {
        return false;
    }
    uint32_t byte = idx >> 3;
    uint8_t bit = (uint8_t)(1U << (idx & 7U));
    if ((bm[byte] & bit) == 0U) {
        return true;
    }
    ext4_jbd2_note_revoke(block);
    bm[byte] &= (uint8_t)~bit;
    if (!ext4_write_block(bb, bm)) {
        return false;
    }

    if (!ext4_group_adjust_free(group, +1, 0)) {
        return false;
    }
    g_ext4.free_blocks_count++;
    return ext4_super_write_counts();
}

static bool ext4_free_inode_bit(uint32_t ino) {
    if (ino == 0U || ino > g_ext4.inodes_count || ino < g_ext4.first_ino) {
        return false;
    }
    uint32_t group = (ino - 1U) / g_ext4.inodes_per_group;
    uint32_t idx = (ino - 1U) % g_ext4.inodes_per_group;

    uint8_t gd[4096];
    if (!ext4_group_read_desc(group, gd)) {
        return false;
    }
    uint64_t ib = ext4_group_get_block_ref(gd, EXT4_BG_INODE_BITMAP_LO, EXT4_BG_INODE_BITMAP_HI);
    if (ib == 0U) {
        return false;
    }

    uint8_t bm[4096];
    if (!ext4_read_block(ib, bm)) {
        return false;
    }
    uint32_t byte = idx >> 3;
    uint8_t bit = (uint8_t)(1U << (idx & 7U));
    if ((bm[byte] & bit) == 0U) {
        return true;
    }
    bm[byte] &= (uint8_t)~bit;
    if (!ext4_write_block(ib, bm)) {
        return false;
    }

    if (!ext4_group_adjust_free(group, 0, +1)) {
        return false;
    }
    g_ext4.free_inodes_count++;
    return ext4_super_write_counts();
}

static bool ext4_free_indirect_subtree(uint64_t block, int depth) {
    if (block == 0U) {
        return true;
    }
    uint8_t blk[4096];
    if (!ext4_read_block(block, blk)) {
        return false;
    }
    uint32_t n = g_ext4.block_size / 4U;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t child = rd_le32(blk + (size_t)i * 4U);
        if (child == 0U) {
            continue;
        }
        if (depth == 1) {
            if (!ext4_free_block(child)) {
                return false;
            }
        } else {
            if (!ext4_free_indirect_subtree(child, depth - 1)) {
                return false;
            }
        }
    }
    return ext4_free_block(block);
}

static bool ext4_extent_free_tree_node(const uint8_t *node, bool free_data) {
    const ext4_extent_header_t *eh = (const ext4_extent_header_t *)node;
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_entries > eh->eh_max) {
        return false;
    }
    if (eh->eh_depth == 0U) {
        if (!free_data) {
            return true;
        }
        const ext4_extent_t *ex = (const ext4_extent_t *)(node + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint32_t len = ex[i].ee_len & 0x7FFFU;
            uint64_t pstart = ((uint64_t)ex[i].ee_start_hi << 32) | ex[i].ee_start_lo;
            for (uint32_t j = 0; j < len; j++) {
                if (!ext4_free_block(pstart + j)) {
                    return false;
                }
            }
        }
        return true;
    }

    const ext4_extent_idx_t *ix = (const ext4_extent_idx_t *)(node + sizeof(*eh));
    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        uint64_t child = ((uint64_t)ix[i].ei_leaf_hi << 32) | ix[i].ei_leaf_lo;
        if (child == 0U) {
            continue;
        }
        uint8_t buf[4096];
        if (!ext4_read_block(child, buf)) {
            return false;
        }
        if (!ext4_extent_free_tree_node(buf, free_data)) {
            return false;
        }
        if (!ext4_free_block(child)) {
            return false;
        }
    }
    return true;
}

static bool ext4_free_file_blocks(ext4_meta_t *m, bool free_data) {
    if (!m) {
        return false;
    }
    if (m->flags & EXT4_EXTENTS_FL) {
        uint8_t root[60];
        memcpy(root, m->block, sizeof(root));
        if (!ext4_extent_free_tree_node(root, free_data)) {
            return false;
        }
    } else if (free_data) {
        for (int i = 0; i < 12; i++) {
            if (m->block[i] && !ext4_free_block(m->block[i])) {
                return false;
            }
        }
        if (m->block[12] && !ext4_free_indirect_subtree(m->block[12], 1)) {
            return false;
        }
        if (m->block[13] && !ext4_free_indirect_subtree(m->block[13], 2)) {
            return false;
        }
        if (m->block[14] && !ext4_free_indirect_subtree(m->block[14], 3)) {
            return false;
        }
    }

    memset(m->block, 0, sizeof(m->block));
    if (m->flags & EXT4_EXTENTS_FL) {
        ext4_extent_header_t *eh = (ext4_extent_header_t *)m->block;
        eh->eh_magic = EXT4_EXT_MAGIC;
        eh->eh_entries = 0U;
        eh->eh_max = (uint16_t)((sizeof(m->block) - sizeof(*eh)) / 12U);
        eh->eh_depth = 0U;
        eh->eh_generation = 0U;
    }
    m->size = 0;
    m->blocks_lo = 0;
    return true;
}

static bool ext4_map_block_direct(const ext4_meta_t *m, uint32_t lblock, uint64_t *pblock) {
    if (!m || !pblock) {
        return false;
    }
    if (lblock < EXT4_NDIR_BLOCKS) {
        *pblock = (uint64_t)m->block[lblock];
        return true;
    }

    uint32_t n = g_ext4.block_size / 4U;
    if (n == 0U) {
        return false;
    }

    uint8_t blk[4096];
    lblock -= EXT4_NDIR_BLOCKS;
    if (lblock < n) {
        if (m->block[12] == 0U) {
            *pblock = 0;
            return true;
        }
        if (!ext4_read_block(m->block[12], blk)) {
            return false;
        }
        *pblock = rd_le32(blk + (size_t)lblock * 4U);
        return true;
    }

    lblock -= n;
    if (lblock < n * n) {
        if (m->block[13] == 0U) {
            *pblock = 0;
            return true;
        }
        uint32_t i1 = lblock / n;
        uint32_t i2 = lblock % n;
        if (!ext4_read_block(m->block[13], blk)) {
            return false;
        }
        uint64_t ind = rd_le32(blk + (size_t)i1 * 4U);
        if (ind == 0U) {
            *pblock = 0;
            return true;
        }
        if (!ext4_read_block(ind, blk)) {
            return false;
        }
        *pblock = rd_le32(blk + (size_t)i2 * 4U);
        return true;
    }

    lblock -= n * n;
    if (m->block[14] == 0U) {
        *pblock = 0;
        return true;
    }
    uint64_t cube = (uint64_t)n * (uint64_t)n * (uint64_t)n;
    if ((uint64_t)lblock >= cube) {
        *pblock = 0;
        return true;
    }

    uint32_t i1 = lblock / (n * n);
    uint32_t rem = lblock % (n * n);
    uint32_t i2 = rem / n;
    uint32_t i3 = rem % n;

    if (!ext4_read_block(m->block[14], blk)) {
        return false;
    }
    uint64_t dind = rd_le32(blk + (size_t)i1 * 4U);
    if (dind == 0U) {
        *pblock = 0;
        return true;
    }
    if (!ext4_read_block(dind, blk)) {
        return false;
    }
    uint64_t ind = rd_le32(blk + (size_t)i2 * 4U);
    if (ind == 0U) {
        *pblock = 0;
        return true;
    }
    if (!ext4_read_block(ind, blk)) {
        return false;
    }
    *pblock = rd_le32(blk + (size_t)i3 * 4U);
    return true;
}

static bool ext4_indirect_set(uint64_t block, uint32_t index, uint32_t value) {
    uint8_t buf[4096];
    if (index >= (g_ext4.block_size / 4U)) {
        return false;
    }
    if (!ext4_read_block(block, buf)) {
        return false;
    }
    wr_le32(buf + (size_t)index * 4U, value);
    return ext4_write_block(block, buf);
}

static bool ext4_alloc_zero_block(uint64_t *blk_out) {
    uint8_t z[4096];
    if (!blk_out || !ext4_alloc_block(blk_out)) {
        return false;
    }
    memset(z, 0, g_ext4.block_size);
    return ext4_write_block(*blk_out, z);
}

static bool ext4_set_block_direct(ext4_meta_t *m, uint32_t lblock, uint64_t pblock) {
    if (!m || pblock > 0xFFFFFFFFULL) {
        return false;
    }
    uint32_t n = g_ext4.block_size / 4U;
    if (n == 0U) {
        return false;
    }
    uint32_t p = (uint32_t)pblock;

    if (lblock < EXT4_NDIR_BLOCKS) {
        m->block[lblock] = p;
        return true;
    }

    lblock -= EXT4_NDIR_BLOCKS;
    if (lblock < n) {
        if (m->block[12] == 0U) {
            uint64_t ib = 0;
            if (!ext4_alloc_zero_block(&ib) || ib > 0xFFFFFFFFULL) {
                return false;
            }
            m->block[12] = (uint32_t)ib;
        }
        return ext4_indirect_set(m->block[12], lblock, p);
    }

    lblock -= n;
    if (lblock < n * n) {
        uint32_t i1 = lblock / n;
        uint32_t i2 = lblock % n;
        uint8_t blk[4096];

        if (m->block[13] == 0U) {
            uint64_t dib = 0;
            if (!ext4_alloc_zero_block(&dib) || dib > 0xFFFFFFFFULL) {
                return false;
            }
            m->block[13] = (uint32_t)dib;
        }
        if (!ext4_read_block(m->block[13], blk)) {
            return false;
        }
        uint32_t ind = rd_le32(blk + (size_t)i1 * 4U);
        if (ind == 0U) {
            uint64_t nb = 0;
            if (!ext4_alloc_zero_block(&nb) || nb > 0xFFFFFFFFULL) {
                return false;
            }
            wr_le32(blk + (size_t)i1 * 4U, (uint32_t)nb);
            if (!ext4_write_block(m->block[13], blk)) {
                return false;
            }
            ind = (uint32_t)nb;
        }
        return ext4_indirect_set(ind, i2, p);
    }

    lblock -= n * n;
    uint64_t cube = (uint64_t)n * (uint64_t)n * (uint64_t)n;
    if ((uint64_t)lblock >= cube) {
        return false;
    }

    uint32_t i1 = lblock / (n * n);
    uint32_t rem = lblock % (n * n);
    uint32_t i2 = rem / n;
    uint32_t i3 = rem % n;
    uint8_t blk[4096];
    uint8_t blk2[4096];

    if (m->block[14] == 0U) {
        uint64_t tib = 0;
        if (!ext4_alloc_zero_block(&tib) || tib > 0xFFFFFFFFULL) {
            return false;
        }
        m->block[14] = (uint32_t)tib;
    }
    if (!ext4_read_block(m->block[14], blk)) {
        return false;
    }
    uint32_t dind = rd_le32(blk + (size_t)i1 * 4U);
    if (dind == 0U) {
        uint64_t nb = 0;
        if (!ext4_alloc_zero_block(&nb) || nb > 0xFFFFFFFFULL) {
            return false;
        }
        wr_le32(blk + (size_t)i1 * 4U, (uint32_t)nb);
        if (!ext4_write_block(m->block[14], blk)) {
            return false;
        }
        dind = (uint32_t)nb;
    }

    if (!ext4_read_block(dind, blk2)) {
        return false;
    }
    uint32_t ind = rd_le32(blk2 + (size_t)i2 * 4U);
    if (ind == 0U) {
        uint64_t nb = 0;
        if (!ext4_alloc_zero_block(&nb) || nb > 0xFFFFFFFFULL) {
            return false;
        }
        wr_le32(blk2 + (size_t)i2 * 4U, (uint32_t)nb);
        if (!ext4_write_block(dind, blk2)) {
            return false;
        }
        ind = (uint32_t)nb;
    }

    return ext4_indirect_set(ind, i3, p);
}

static bool ext4_ptr_block_is_empty(uint64_t block) {
    uint8_t buf[4096];
    if (block == 0U || !ext4_read_block(block, buf)) {
        return false;
    }
    uint32_t n = g_ext4.block_size / 4U;
    for (uint32_t i = 0; i < n; i++) {
        if (rd_le32(buf + (size_t)i * 4U) != 0U) {
            return false;
        }
    }
    return true;
}

static bool ext4_clear_block_direct(ext4_meta_t *m, uint32_t lblock, uint64_t *old_block_out) {
    if (!m || !old_block_out) {
        return false;
    }
    *old_block_out = 0U;

    uint32_t n = g_ext4.block_size / 4U;
    if (n == 0U) {
        return false;
    }

    if (lblock < EXT4_NDIR_BLOCKS) {
        *old_block_out = m->block[lblock];
        m->block[lblock] = 0U;
        return true;
    }

    uint8_t b1[4096];
    uint8_t b2[4096];
    uint8_t b3[4096];

    lblock -= EXT4_NDIR_BLOCKS;
    if (lblock < n) {
        if (m->block[12] == 0U) {
            return true;
        }
        if (!ext4_read_block(m->block[12], b1)) {
            return false;
        }
        *old_block_out = rd_le32(b1 + (size_t)lblock * 4U);
        wr_le32(b1 + (size_t)lblock * 4U, 0U);
        if (!ext4_write_block(m->block[12], b1)) {
            return false;
        }
        if (ext4_ptr_block_is_empty(m->block[12])) {
            uint64_t old = m->block[12];
            m->block[12] = 0U;
            if (!ext4_free_block(old)) {
                return false;
            }
        }
        return true;
    }

    lblock -= n;
    if (lblock < n * n) {
        uint32_t i1 = lblock / n;
        uint32_t i2 = lblock % n;
        if (m->block[13] == 0U) {
            return true;
        }
        if (!ext4_read_block(m->block[13], b1)) {
            return false;
        }
        uint32_t ind = rd_le32(b1 + (size_t)i1 * 4U);
        if (ind == 0U) {
            return true;
        }
        if (!ext4_read_block(ind, b2)) {
            return false;
        }
        *old_block_out = rd_le32(b2 + (size_t)i2 * 4U);
        wr_le32(b2 + (size_t)i2 * 4U, 0U);
        if (!ext4_write_block(ind, b2)) {
            return false;
        }
        if (ext4_ptr_block_is_empty(ind)) {
            wr_le32(b1 + (size_t)i1 * 4U, 0U);
            if (!ext4_write_block(m->block[13], b1)) {
                return false;
            }
            if (!ext4_free_block(ind)) {
                return false;
            }
        }
        if (ext4_ptr_block_is_empty(m->block[13])) {
            uint64_t old = m->block[13];
            m->block[13] = 0U;
            if (!ext4_free_block(old)) {
                return false;
            }
        }
        return true;
    }

    lblock -= n * n;
    uint64_t cube = (uint64_t)n * (uint64_t)n * (uint64_t)n;
    if ((uint64_t)lblock >= cube) {
        return false;
    }

    uint32_t i1 = lblock / (n * n);
    uint32_t rem = lblock % (n * n);
    uint32_t i2 = rem / n;
    uint32_t i3 = rem % n;
    if (m->block[14] == 0U) {
        return true;
    }
    if (!ext4_read_block(m->block[14], b1)) {
        return false;
    }
    uint32_t dind = rd_le32(b1 + (size_t)i1 * 4U);
    if (dind == 0U) {
        return true;
    }
    if (!ext4_read_block(dind, b2)) {
        return false;
    }
    uint32_t ind = rd_le32(b2 + (size_t)i2 * 4U);
    if (ind == 0U) {
        return true;
    }
    if (!ext4_read_block(ind, b3)) {
        return false;
    }
    *old_block_out = rd_le32(b3 + (size_t)i3 * 4U);
    wr_le32(b3 + (size_t)i3 * 4U, 0U);
    if (!ext4_write_block(ind, b3)) {
        return false;
    }
    if (ext4_ptr_block_is_empty(ind)) {
        wr_le32(b2 + (size_t)i2 * 4U, 0U);
        if (!ext4_write_block(dind, b2)) {
            return false;
        }
        if (!ext4_free_block(ind)) {
            return false;
        }
    }
    if (ext4_ptr_block_is_empty(dind)) {
        wr_le32(b1 + (size_t)i1 * 4U, 0U);
        if (!ext4_write_block(m->block[14], b1)) {
            return false;
        }
        if (!ext4_free_block(dind)) {
            return false;
        }
    }
    if (ext4_ptr_block_is_empty(m->block[14])) {
        uint64_t old = m->block[14];
        m->block[14] = 0U;
        if (!ext4_free_block(old)) {
            return false;
        }
    }
    return true;
}

static uint8_t ext4_inode_filetype(const ext4_meta_t *m) {
    if (!m) {
        return 0U;
    }
    if (ext4_is_dir_mode(m->mode)) {
        return EXT4_FT_DIR;
    }
    if (ext4_is_lnk_mode(m->mode)) {
        return EXT4_FT_SYMLINK;
    }
    if (ext4_is_reg_mode(m->mode)) {
        return EXT4_FT_REG_FILE;
    }
    return 0U;
}

static void ext4_cache_invalidate_ino(uint32_t ino) {
    for (int i = 0; i < EXT4_CACHE_MAX; i++) {
        if (!g_ext4.cache_meta[i].valid || g_ext4.cache_meta[i].ino != ino) {
            continue;
        }
        pagecache_invalidate_inode(&g_ext4.cache_nodes[i]);
        memset(&g_ext4.cache_meta[i], 0, sizeof(g_ext4.cache_meta[i]));
        memset(&g_ext4.cache_nodes[i], 0, sizeof(g_ext4.cache_nodes[i]));
    }
}

static bool ext4_extent_map(const ext4_meta_t *m, uint32_t lblock, uint64_t *pblock) {
    if (!m || !pblock) {
        return false;
    }

    uint8_t node[4096];
    memset(node, 0, sizeof(node));
    memcpy(node, m->block, sizeof(m->block));
    const uint8_t *cur = node;

    for (;;) {
        const ext4_extent_header_t *eh = (const ext4_extent_header_t *)cur;
        if (eh->eh_magic != EXT4_EXT_MAGIC) {
            return false;
        }
        if (eh->eh_entries > eh->eh_max) {
            return false;
        }
        if (eh->eh_max == 0U ||
            ((uint32_t)sizeof(*eh) + (uint32_t)eh->eh_max * 12U) > g_ext4.block_size) {
            return false;
        }
        if (eh->eh_depth == 0) {
            const ext4_extent_t *ex = (const ext4_extent_t *)(cur + sizeof(*eh));
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint32_t start = ex[i].ee_block;
                uint32_t raw_len = ex[i].ee_len;
                uint32_t len = raw_len & 0x7FFFU;
                if (len == 0U) {
                    continue;
                }
                if (lblock < start || lblock >= start + len) {
                    continue;
                }
                if (raw_len & 0x8000U) {
                    *pblock = 0;
                    return true;
                }
                uint64_t pstart = ((uint64_t)ex[i].ee_start_hi << 32) | ex[i].ee_start_lo;
                *pblock = pstart + (lblock - start);
                return true;
            }
            *pblock = 0;
            return true;
        }

        const ext4_extent_idx_t *ix = (const ext4_extent_idx_t *)(cur + sizeof(*eh));
        int chosen = -1;
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (lblock < ix[i].ei_block) {
                break;
            }
            chosen = (int)i;
        }
        if (chosen < 0) {
            *pblock = 0;
            return true;
        }

        uint64_t leaf = ((uint64_t)ix[chosen].ei_leaf_hi << 32) | ix[chosen].ei_leaf_lo;
        if (leaf == 0U || !ext4_read_block(leaf, node)) {
            return false;
        }
        cur = node;
    }
}

static bool ext4_extent_collect_node(const uint8_t *node, size_t node_bytes,
                                     ext4_extent_run_t *runs, uint32_t *run_count) {
    if (!node || !runs || !run_count || node_bytes < sizeof(ext4_extent_header_t)) {
        return false;
    }

    const ext4_extent_header_t *eh = (const ext4_extent_header_t *)node;
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_entries > eh->eh_max || eh->eh_max == 0U) {
        return false;
    }
    if ((uint32_t)sizeof(*eh) + (uint32_t)eh->eh_max * 12U > node_bytes) {
        return false;
    }

    if (eh->eh_depth == 0U) {
        const ext4_extent_t *ex = (const ext4_extent_t *)(node + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint32_t raw_len = ex[i].ee_len;
            if (raw_len & 0x8000U) {
                return false;
            }
            uint32_t len = raw_len & EXT4_EXT_LEN_MASK;
            if (len == 0U) {
                continue;
            }
            if (*run_count >= EXT4_EXT_MUT_MAX_RUNS) {
                return false;
            }
            uint64_t pstart = ((uint64_t)ex[i].ee_start_hi << 32) | ex[i].ee_start_lo;
            if (pstart == 0U) {
                return false;
            }
            runs[*run_count].lblock = ex[i].ee_block;
            runs[*run_count].len = len;
            runs[*run_count].pblock = pstart;
            (*run_count)++;
        }
        return true;
    }

    const ext4_extent_idx_t *ix = (const ext4_extent_idx_t *)(node + sizeof(*eh));
    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        uint64_t child = ((uint64_t)ix[i].ei_leaf_hi << 32) | ix[i].ei_leaf_lo;
        if (child == 0U) {
            return false;
        }
        uint8_t buf[4096];
        if (!ext4_read_block(child, buf)) {
            return false;
        }
        if (!ext4_extent_collect_node(buf, g_ext4.block_size, runs, run_count)) {
            return false;
        }
    }
    return true;
}

static bool ext4_extent_collect_runs(const ext4_meta_t *m, ext4_extent_run_t *runs, uint32_t *run_count) {
    if (!m || !runs || !run_count || (m->flags & EXT4_EXTENTS_FL) == 0U) {
        return false;
    }

    *run_count = 0U;
    uint8_t root[60];
    memcpy(root, m->block, sizeof(root));
    if (!ext4_extent_collect_node(root, sizeof(root), runs, run_count)) {
        return false;
    }

    for (uint32_t i = 0; i < *run_count; i++) {
        uint64_t i_end = (uint64_t)runs[i].lblock + runs[i].len;
        if (runs[i].len == 0U || runs[i].len > EXT4_EXT_LEN_MASK || i_end > 0xFFFFFFFFULL) {
            return false;
        }
        if (i == 0U) {
            continue;
        }
        uint64_t prev_end = (uint64_t)runs[i - 1U].lblock + runs[i - 1U].len;
        if ((uint64_t)runs[i].lblock < prev_end) {
            return false;
        }
    }
    return true;
}

static bool ext4_extent_merge_runs(ext4_extent_run_t *runs, uint32_t *count_io) {
    if (!runs || !count_io) {
        return false;
    }
    uint32_t count = *count_io;
    if (count <= 1U) {
        return true;
    }

    uint32_t out = 0U;
    for (uint32_t i = 0; i < count; i++) {
        if (out == 0U) {
            runs[out++] = runs[i];
            continue;
        }
        ext4_extent_run_t *prev = &runs[out - 1U];
        ext4_extent_run_t *cur = &runs[i];
        uint64_t prev_l_end = (uint64_t)prev->lblock + prev->len;
        uint64_t prev_p_end = prev->pblock + prev->len;
        bool can_merge = prev_l_end == cur->lblock &&
                         prev_p_end == cur->pblock &&
                         (uint64_t)prev->len + cur->len <= EXT4_EXT_LEN_MASK;
        if (can_merge) {
            prev->len += cur->len;
        } else {
            runs[out++] = *cur;
        }
    }
    *count_io = out;
    return true;
}

static bool ext4_extent_free_new_nodes(const uint64_t *blocks, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (blocks[i] == 0U) {
            continue;
        }
        if (!ext4_free_block(blocks[i])) {
            return false;
        }
    }
    return true;
}

static bool ext4_extent_build_leaf_block(const ext4_extent_run_t *runs, uint32_t count, uint64_t *block_out) {
    if (!runs || count == 0U || !block_out) {
        return false;
    }
    if (count > (g_ext4.block_size - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t)) {
        return false;
    }

    uint64_t blk = 0;
    if (!ext4_alloc_block(&blk)) {
        return false;
    }

    uint8_t buf[4096];
    memset(buf, 0, g_ext4.block_size);
    ext4_extent_header_t *eh = (ext4_extent_header_t *)buf;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = (uint16_t)count;
    eh->eh_max = (uint16_t)((g_ext4.block_size - sizeof(*eh)) / sizeof(ext4_extent_t));
    eh->eh_depth = 0U;
    eh->eh_generation = 0U;

    ext4_extent_t *ex = (ext4_extent_t *)(buf + sizeof(*eh));
    for (uint32_t i = 0; i < count; i++) {
        ex[i].ee_block = runs[i].lblock;
        ex[i].ee_len = (uint16_t)runs[i].len;
        ex[i].ee_start_hi = (uint16_t)(runs[i].pblock >> 32);
        ex[i].ee_start_lo = (uint32_t)(runs[i].pblock & 0xFFFFFFFFU);
    }

    if (!ext4_write_block(blk, buf)) {
        (void)ext4_free_block(blk);
        return false;
    }
    *block_out = blk;
    return true;
}

static bool ext4_extent_build_index_block(const ext4_extent_node_ref_t *children, uint32_t count,
                                          uint16_t depth, uint64_t *block_out) {
    if (!children || count == 0U || !block_out || depth == 0U) {
        return false;
    }
    if (count > (g_ext4.block_size - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_idx_t)) {
        return false;
    }

    uint64_t blk = 0;
    if (!ext4_alloc_block(&blk)) {
        return false;
    }

    uint8_t buf[4096];
    memset(buf, 0, g_ext4.block_size);
    ext4_extent_header_t *eh = (ext4_extent_header_t *)buf;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = (uint16_t)count;
    eh->eh_max = (uint16_t)((g_ext4.block_size - sizeof(*eh)) / sizeof(ext4_extent_idx_t));
    eh->eh_depth = depth;
    eh->eh_generation = 0U;

    ext4_extent_idx_t *ix = (ext4_extent_idx_t *)(buf + sizeof(*eh));
    for (uint32_t i = 0; i < count; i++) {
        ix[i].ei_block = children[i].lblock;
        ix[i].ei_leaf_hi = (uint16_t)(children[i].block >> 32);
        ix[i].ei_leaf_lo = (uint32_t)(children[i].block & 0xFFFFFFFFU);
        ix[i].ei_unused = 0U;
    }

    if (!ext4_write_block(blk, buf)) {
        (void)ext4_free_block(blk);
        return false;
    }
    *block_out = blk;
    return true;
}

static bool ext4_extent_rebuild(ext4_meta_t *m, const ext4_extent_run_t *runs, uint32_t run_count) {
    if (!m || (m->flags & EXT4_EXTENTS_FL) == 0U) {
        return false;
    }

    uint8_t old_root[60];
    memcpy(old_root, m->block, sizeof(old_root));

    uint8_t new_root[60];
    memset(new_root, 0, sizeof(new_root));

    uint64_t new_meta_blocks[EXT4_EXT_MUT_MAX_NODES];
    uint32_t new_meta_count = 0U;
    memset(new_meta_blocks, 0, sizeof(new_meta_blocks));

    const uint16_t root_cap = (uint16_t)((sizeof(new_root) - sizeof(ext4_extent_header_t)) / 12U);
    const uint16_t node_cap = (uint16_t)((g_ext4.block_size - sizeof(ext4_extent_header_t)) / 12U);
    if (root_cap == 0U || node_cap == 0U) {
        return false;
    }

    ext4_extent_header_t *reh = (ext4_extent_header_t *)new_root;
    reh->eh_magic = EXT4_EXT_MAGIC;
    reh->eh_generation = 0U;

    if (run_count == 0U) {
        reh->eh_entries = 0U;
        reh->eh_max = root_cap;
        reh->eh_depth = 0U;
    } else if (run_count <= root_cap) {
        reh->eh_entries = (uint16_t)run_count;
        reh->eh_max = root_cap;
        reh->eh_depth = 0U;

        ext4_extent_t *ex = (ext4_extent_t *)(new_root + sizeof(*reh));
        for (uint32_t i = 0; i < run_count; i++) {
            ex[i].ee_block = runs[i].lblock;
            ex[i].ee_len = (uint16_t)runs[i].len;
            ex[i].ee_start_hi = (uint16_t)(runs[i].pblock >> 32);
            ex[i].ee_start_lo = (uint32_t)(runs[i].pblock & 0xFFFFFFFFU);
        }
    } else {
        ext4_extent_node_ref_t cur[EXT4_EXT_MUT_MAX_NODES];
        ext4_extent_node_ref_t next[EXT4_EXT_MUT_MAX_NODES];
        uint32_t cur_count = 0U;
        uint16_t child_depth = 0U;

        for (uint32_t i = 0; i < run_count;) {
            uint32_t chunk = run_count - i;
            if (chunk > node_cap) {
                chunk = node_cap;
            }
            if (cur_count >= EXT4_EXT_MUT_MAX_NODES) {
                (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                return false;
            }
            uint64_t blk = 0U;
            if (!ext4_extent_build_leaf_block(&runs[i], chunk, &blk)) {
                (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                return false;
            }
            if (new_meta_count >= EXT4_EXT_MUT_MAX_NODES) {
                (void)ext4_free_block(blk);
                (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                return false;
            }
            new_meta_blocks[new_meta_count++] = blk;
            cur[cur_count].lblock = runs[i].lblock;
            cur[cur_count].block = blk;
            cur_count++;
            i += chunk;
        }

        while (cur_count > root_cap) {
            uint32_t next_count = 0U;
            uint16_t parent_depth = (uint16_t)(child_depth + 1U);
            for (uint32_t i = 0; i < cur_count;) {
                uint32_t chunk = cur_count - i;
                if (chunk > node_cap) {
                    chunk = node_cap;
                }
                if (next_count >= EXT4_EXT_MUT_MAX_NODES) {
                    (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                    return false;
                }
                uint64_t blk = 0U;
                if (!ext4_extent_build_index_block(&cur[i], chunk, parent_depth, &blk)) {
                    (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                    return false;
                }
                if (new_meta_count >= EXT4_EXT_MUT_MAX_NODES) {
                    (void)ext4_free_block(blk);
                    (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
                    return false;
                }
                new_meta_blocks[new_meta_count++] = blk;
                next[next_count].lblock = cur[i].lblock;
                next[next_count].block = blk;
                next_count++;
                i += chunk;
            }

            for (uint32_t i = 0; i < next_count; i++) {
                cur[i] = next[i];
            }
            cur_count = next_count;
            child_depth = parent_depth;
        }

        reh->eh_entries = (uint16_t)cur_count;
        reh->eh_max = root_cap;
        reh->eh_depth = (uint16_t)(child_depth + 1U);
        ext4_extent_idx_t *rix = (ext4_extent_idx_t *)(new_root + sizeof(*reh));
        for (uint32_t i = 0; i < cur_count; i++) {
            rix[i].ei_block = cur[i].lblock;
            rix[i].ei_leaf_hi = (uint16_t)(cur[i].block >> 32);
            rix[i].ei_leaf_lo = (uint32_t)(cur[i].block & 0xFFFFFFFFU);
            rix[i].ei_unused = 0U;
        }
    }

    memcpy(m->block, new_root, sizeof(new_root));
    if (!ext4_inode_store(m)) {
        memcpy(m->block, old_root, sizeof(old_root));
        (void)ext4_extent_free_new_nodes(new_meta_blocks, new_meta_count);
        return false;
    }

    if (!ext4_extent_free_tree_node(old_root, false)) {
        return false;
    }
    return true;
}

static bool ext4_extent_assign_block(ext4_meta_t *m, uint32_t lblock, uint64_t pblock) {
    if (!m || pblock == 0U || pblock > 0xFFFFFFFFULL) {
        return false;
    }

    ext4_extent_run_t runs[EXT4_EXT_MUT_MAX_RUNS];
    uint32_t run_count = 0U;
    if (!ext4_extent_collect_runs(m, runs, &run_count)) {
        return false;
    }

    uint32_t pos = 0U;
    while (pos < run_count && runs[pos].lblock < lblock) {
        pos++;
    }
    if (pos > 0U) {
        uint64_t prev_end = (uint64_t)runs[pos - 1U].lblock + runs[pos - 1U].len;
        if ((uint64_t)lblock < prev_end) {
            return false;
        }
    }
    if (pos < run_count && runs[pos].lblock == lblock) {
        if (runs[pos].len != 0U) {
            return false;
        }
    }

    if (run_count >= EXT4_EXT_MUT_MAX_RUNS) {
        return false;
    }
    for (uint32_t i = run_count; i > pos; i--) {
        runs[i] = runs[i - 1U];
    }
    runs[pos].lblock = lblock;
    runs[pos].len = 1U;
    runs[pos].pblock = pblock;
    run_count++;

    if (!ext4_extent_merge_runs(runs, &run_count)) {
        return false;
    }
    return ext4_extent_rebuild(m, runs, run_count);
}

static bool ext4_extent_truncate_data(ext4_meta_t *m, uint64_t new_size) {
    if (!m) {
        return false;
    }

    ext4_extent_run_t runs[EXT4_EXT_MUT_MAX_RUNS];
    uint32_t run_count = 0U;
    if (!ext4_extent_collect_runs(m, runs, &run_count)) {
        return false;
    }

    uint32_t keep_blocks = (uint32_t)((new_size + g_ext4.block_size - 1U) / g_ext4.block_size);
    ext4_extent_run_t keep[EXT4_EXT_MUT_MAX_RUNS];
    uint32_t keep_count = 0U;

    for (uint32_t i = 0; i < run_count; i++) {
        uint64_t lstart = runs[i].lblock;
        uint64_t lend = lstart + runs[i].len;
        if (lend <= keep_blocks) {
            if (keep_count >= EXT4_EXT_MUT_MAX_RUNS) {
                return false;
            }
            keep[keep_count++] = runs[i];
            continue;
        }
        if (lstart >= keep_blocks) {
            for (uint32_t j = 0; j < runs[i].len; j++) {
                if (!ext4_free_block(runs[i].pblock + j)) {
                    return false;
                }
            }
            continue;
        }

        uint32_t keep_len = (uint32_t)(keep_blocks - lstart);
        if (keep_len == 0U || keep_len > runs[i].len) {
            return false;
        }
        for (uint32_t j = keep_len; j < runs[i].len; j++) {
            if (!ext4_free_block(runs[i].pblock + j)) {
                return false;
            }
        }
        if (keep_count >= EXT4_EXT_MUT_MAX_RUNS) {
            return false;
        }
        keep[keep_count] = runs[i];
        keep[keep_count].len = keep_len;
        keep_count++;
    }

    if (!ext4_extent_merge_runs(keep, &keep_count)) {
        return false;
    }
    return ext4_extent_rebuild(m, keep, keep_count);
}

static bool ext4_map_file_block(const ext4_meta_t *m, uint32_t lblock, uint64_t *pblock) {
    if (!m || !pblock) {
        return false;
    }
    if (m->flags & EXT4_EXTENTS_FL) {
        return ext4_extent_map(m, lblock, pblock);
    }
    return ext4_map_block_direct(m, lblock, pblock);
}

static int ext4_read_file_at(const ext4_meta_t *m, uint64_t off, void *buf, size_t len) {
    if (!m || !buf) {
        return -1;
    }
    if (off >= m->size) {
        return 0;
    }

    size_t remain = len;
    if (off + remain > m->size) {
        remain = (size_t)(m->size - off);
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < remain) {
        uint64_t cur = off + done;
        uint32_t lblock = (uint32_t)(cur / g_ext4.block_size);
        uint32_t in_block = (uint32_t)(cur % g_ext4.block_size);
        size_t chunk = g_ext4.block_size - in_block;
        if (chunk > (remain - done)) {
            chunk = remain - done;
        }

        uint64_t pblock = 0;
        if (!ext4_map_file_block(m, lblock, &pblock)) {
            return done ? (int)done : -1;
        }
        if (pblock == 0) {
            memset(dst + done, 0, chunk);
        } else {
            uint64_t byte_off = pblock * (uint64_t)g_ext4.block_size + in_block;
            if (!ext4_disk_read_bytes(byte_off, dst + done, chunk)) {
                return done ? (int)done : -1;
            }
        }
        done += chunk;
    }
    return (int)done;
}

static int ext4_dir_iter(const ext4_meta_t *dir_meta, uint64_t *off_io,
                         uint32_t *ino_out, char *name_out, uint8_t *ftype_out) {
    if (!dir_meta || !off_io || !ino_out || !name_out) {
        return -1;
    }
    uint64_t off = *off_io;

    while (off + 8U <= dir_meta->size) {
        uint8_t hdr[8];
        if (ext4_read_file_at(dir_meta, off, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
            return -1;
        }
        uint32_t ino = rd_le32(hdr + 0);
        uint16_t rec_len = rd_le16(hdr + 4);
        uint8_t name_len = hdr[6];
        uint8_t file_type = 0;
        if (g_ext4.has_filetype) {
            file_type = hdr[7];
        } else {
            name_len = (uint8_t)(rd_le16(hdr + 6) & 0xFFU);
        }

        if (rec_len < 8U) {
            return -1;
        }
        if (off + rec_len > dir_meta->size) {
            return -1;
        }
        if (name_len > rec_len - 8U) {
            return -1;
        }

        char name[256];
        memset(name, 0, sizeof(name));
        if (name_len > 0U) {
            size_t copy_len = name_len;
            if (copy_len > sizeof(name) - 1U) {
                copy_len = sizeof(name) - 1U;
            }
            if (ext4_read_file_at(dir_meta, off + 8U, name, copy_len) != (int)copy_len) {
                return -1;
            }
            name[copy_len] = '\0';
        }

        off += rec_len;
        *off_io = off;

        if (ino == 0U) {
            continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        *ino_out = ino;
        strcpy(name_out, name);
        if (ftype_out) {
            *ftype_out = file_type;
        }
        return 0;
    }
    return 1;
}

typedef struct {
    uint64_t pblock;
    uint32_t pos;
    uint16_t rec_len;
    uint32_t prev_pos;
    bool has_prev;
    uint32_t ino;
    uint8_t ftype;
} ext4_dir_hit_t;

static bool ext4_dir_find_entry(const ext4_meta_t *dir_meta, const char *name, ext4_dir_hit_t *hit) {
    if (!dir_meta || !name || !hit || !ext4_is_dir_mode(dir_meta->mode)) {
        return false;
    }
    size_t want_len = strlen(name);
    if (want_len == 0U || want_len > 255U) {
        return false;
    }

    uint8_t blk[4096];
    uint64_t blocks = (dir_meta->size + g_ext4.block_size - 1U) / g_ext4.block_size;
    for (uint64_t lb = 0; lb < blocks; lb++) {
        uint64_t pblock = 0;
        if (!ext4_map_file_block(dir_meta, (uint32_t)lb, &pblock) || pblock == 0U) {
            continue;
        }
        if (!ext4_read_block(pblock, blk)) {
            return false;
        }

        uint32_t pos = 0;
        uint32_t prev_pos = 0;
        bool has_prev = false;
        while (pos + 8U <= g_ext4.block_size) {
            uint32_t ino = rd_le32(blk + pos);
            uint16_t rec_len = rd_le16(blk + pos + 4U);
            uint8_t name_len = blk[pos + 6U];
            uint8_t ftype = g_ext4.has_filetype ? blk[pos + 7U] : 0U;
            if (rec_len < 8U || pos + rec_len > g_ext4.block_size) {
                return false;
            }
            if (name_len > rec_len - 8U) {
                return false;
            }

            if (ino != 0U && name_len == want_len &&
                memcmp(blk + pos + 8U, name, want_len) == 0) {
                memset(hit, 0, sizeof(*hit));
                hit->pblock = pblock;
                hit->pos = pos;
                hit->rec_len = rec_len;
                hit->prev_pos = prev_pos;
                hit->has_prev = has_prev;
                hit->ino = ino;
                hit->ftype = ftype;
                return true;
            }

            has_prev = true;
            prev_pos = pos;
            pos += rec_len;
        }
    }
    return false;
}

static bool ext4_dir_update_entry_ino(ext4_meta_t *dir_meta, const char *name, uint32_t new_ino) {
    ext4_dir_hit_t hit;
    if (!dir_meta || !name || !ext4_dir_find_entry(dir_meta, name, &hit)) {
        return false;
    }

    uint8_t blk[4096];
    if (!ext4_read_block(hit.pblock, blk)) {
        return false;
    }
    wr_le32(blk + hit.pos, new_ino);
    return ext4_write_block(hit.pblock, blk);
}

static bool ext4_dir_remove_entry(ext4_meta_t *dir_meta, const char *name, uint32_t *ino_out, uint8_t *ftype_out) {
    ext4_dir_hit_t hit;
    if (!dir_meta || !name || !ext4_dir_find_entry(dir_meta, name, &hit)) {
        return false;
    }

    uint8_t blk[4096];
    if (!ext4_read_block(hit.pblock, blk)) {
        return false;
    }

    if (hit.has_prev) {
        uint16_t prev_len = rd_le16(blk + hit.prev_pos + 4U);
        uint16_t merged = (uint16_t)(prev_len + hit.rec_len);
        wr_le16(blk + hit.prev_pos + 4U, merged);
    } else {
        wr_le32(blk + hit.pos, 0U);
    }

    if (!ext4_write_block(hit.pblock, blk)) {
        return false;
    }

    if (ino_out) {
        *ino_out = hit.ino;
    }
    if (ftype_out) {
        *ftype_out = hit.ftype;
    }
    return true;
}

static bool ext4_dir_is_empty(const ext4_meta_t *dir_meta) {
    if (!dir_meta || !ext4_is_dir_mode(dir_meta->mode)) {
        return false;
    }
    uint64_t off = 0;
    for (;;) {
        uint32_t child_ino = 0;
        char child_name[256];
        uint8_t ftype = 0;
        int rc = ext4_dir_iter(dir_meta, &off, &child_ino, child_name, &ftype);
        (void)ftype;
        if (rc == 1) {
            return true;
        }
        if (rc != 0) {
            return false;
        }
        return false;
    }
}

static bool ext4_prepare_writable_nonextent(ext4_meta_t *m) {
    if (!m || !ext4_is_reg_mode(m->mode)) {
        return false;
    }
    if ((m->flags & EXT4_EXTENTS_FL) == 0) {
        return true;
    }
    if (m->size == 0U) {
        m->flags &= ~EXT4_EXTENTS_FL;
        memset(m->block, 0, sizeof(m->block));
        m->blocks_lo = 0;
        return ext4_inode_store(m);
    }

    uint64_t n = g_ext4.block_size / 4U;
    uint64_t max_blocks = 12U + n + n * n + n * n * n;
    uint64_t blocks = (m->size + g_ext4.block_size - 1U) / g_ext4.block_size;
    if (blocks > max_blocks) {
        return false;
    }

    ext4_meta_t nm = *m;
    nm.flags &= ~EXT4_EXTENTS_FL;
    memset(nm.block, 0, sizeof(nm.block));
    nm.blocks_lo = (uint32_t)(((nm.size + 511U) / 512U) & 0xFFFFFFFFU);

    for (uint32_t lb = 0; lb < (uint32_t)blocks; lb++) {
        uint64_t pb = 0;
        if (!ext4_extent_map(m, lb, &pb)) {
            return false;
        }
        if (pb == 0U) {
            continue;
        }
        if (!ext4_set_block_direct(&nm, lb, pb)) {
            return false;
        }
    }

    uint8_t root[60];
    memcpy(root, m->block, sizeof(root));
    if (!ext4_extent_free_tree_node(root, false)) {
        return false;
    }

    *m = nm;
    return ext4_inode_store(m);
}

static bool ext4_dir_add_entry(ext4_meta_t *dir_meta, const char *name, uint32_t ino, uint8_t ftype) {
    if (!dir_meta || !name || !ext4_is_dir_mode(dir_meta->mode)) {
        return false;
    }
    size_t name_len = strlen(name);
    if (name_len == 0U || name_len > 255U) {
        return false;
    }

    uint16_t need = (uint16_t)(8U + align_up4((uint32_t)name_len));
    uint8_t blk[4096];
    uint64_t blocks = (dir_meta->size + g_ext4.block_size - 1U) / g_ext4.block_size;

    for (uint64_t lb = 0; lb < blocks; lb++) {
        uint64_t pblock = 0;
        if (!ext4_map_file_block(dir_meta, (uint32_t)lb, &pblock) || pblock == 0U) {
            return false;
        }
        if (!ext4_read_block(pblock, blk)) {
            return false;
        }

        uint32_t pos = 0;
        while (pos + 8U <= g_ext4.block_size) {
            uint32_t ino_cur = rd_le32(blk + pos);
            uint16_t rec_len = rd_le16(blk + pos + 4U);
            uint8_t name_cur = blk[pos + 6U];
            if (rec_len < 8U || pos + rec_len > g_ext4.block_size) {
                return false;
            }
            uint16_t ideal = (uint16_t)(8U + align_up4((uint32_t)name_cur));

            if (ino_cur == 0U) {
                if (rec_len >= need) {
                    wr_le32(blk + pos, ino);
                    wr_le16(blk + pos + 4U, rec_len);
                    blk[pos + 6U] = (uint8_t)name_len;
                    blk[pos + 7U] = g_ext4.has_filetype ? ftype : 0U;
                    memcpy(blk + pos + 8U, name, name_len);
                    return ext4_write_block(pblock, blk);
                }
            } else if (rec_len >= ideal && (uint16_t)(rec_len - ideal) >= need) {
                uint32_t npos = pos + ideal;
                uint16_t nrec = rec_len - ideal;
                wr_le16(blk + pos + 4U, ideal);
                wr_le32(blk + npos, ino);
                wr_le16(blk + npos + 4U, nrec);
                blk[npos + 6U] = (uint8_t)name_len;
                blk[npos + 7U] = g_ext4.has_filetype ? ftype : 0U;
                memcpy(blk + npos + 8U, name, name_len);
                return ext4_write_block(pblock, blk);
            }
            pos += rec_len;
        }
    }
    return false;
}

int ext4_write(inode_t *inode, size_t *offset, const void *buf, size_t len) {
    if (!inode || !offset || !buf || len == 0U || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || inode->type != INODE_FILE || !inode->writable) {
        return -1;
    }
    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m || !ext4_is_reg_mode(m->mode)) {
        return -1;
    }
    if ((uint64_t)inode->size > m->size) {
        m->size = (uint64_t)inode->size;
    }
    bool use_extents = (m->flags & EXT4_EXTENTS_FL) != 0U;
    if (!use_extents && !ext4_prepare_writable_nonextent(m)) {
        return -1;
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    uint8_t blk[4096];

    while (done < len) {
        uint64_t cur_off = (uint64_t)(*offset) + done;
        uint32_t lblock = (uint32_t)(cur_off / g_ext4.block_size);
        uint32_t in_block = (uint32_t)(cur_off % g_ext4.block_size);
        size_t chunk = g_ext4.block_size - in_block;
        if (chunk > len - done) {
            chunk = len - done;
        }

        bool fresh_block = false;
        uint64_t pblock = 0;
        if (use_extents) {
            if (!ext4_map_file_block(m, lblock, &pblock)) {
                break;
            }
            if (pblock == 0U) {
                uint64_t nb = 0;
                if (!ext4_alloc_block(&nb)) {
                    break;
                }
                pblock = nb;
                fresh_block = true;
            }
        } else {
            if (!ext4_map_block_direct(m, lblock, &pblock)) {
                break;
            }
            if (pblock == 0U) {
                uint64_t nb = 0;
                if (!ext4_alloc_block(&nb)) {
                    break;
                }
                pblock = nb;
                fresh_block = true;
            }
        }

        if (chunk == g_ext4.block_size && in_block == 0U) {
            if (!ext4_write_block(pblock, src + done)) {
                if (fresh_block) {
                    (void)ext4_free_block(pblock);
                }
                break;
            }
        } else {
            if (fresh_block) {
                memset(blk, 0, g_ext4.block_size);
            } else {
                if (!ext4_read_block(pblock, blk)) {
                    break;
                }
            }
            memcpy(blk + in_block, src + done, chunk);
            if (!ext4_write_block(pblock, blk)) {
                if (fresh_block) {
                    (void)ext4_free_block(pblock);
                }
                break;
            }
        }
        if (fresh_block) {
            if (!ext4_ordered_checkpoint()) {
                (void)ext4_free_block(pblock);
                break;
            }
            if (use_extents) {
                if (!ext4_extent_assign_block(m, lblock, pblock)) {
                    (void)ext4_free_block(pblock);
                    break;
                }
            } else {
                if (!ext4_set_block_direct(m, lblock, pblock)) {
                    (void)ext4_free_block(pblock);
                    break;
                }
            }
        }
        done += chunk;
    }

    if (done == 0U) {
        return -1;
    }

    uint64_t end = (uint64_t)(*offset) + done;
    if (end > m->size) {
        m->size = end;
    }
    m->blocks_lo = (uint32_t)(((m->size + 511U) / 512U) & 0xFFFFFFFFU);
    if (!ext4_ordered_checkpoint()) {
        return -1;
    }
    if (!ext4_inode_store(m)) {
        return -1;
    }
    inode->size = (size_t)m->size;
    *offset += done;
    if (!ext4_ordered_checkpoint()) {
        return -1;
    }
    return (int)done;
}

inode_t *ext4_create_file(inode_t *parent, const char *name) {
    if (!parent || !name || !g_ext4.mounted || !g_ext4.write_enabled ||
        parent->fs_kind != FS_KIND_EXT4 || parent->type != INODE_DIR) {
        return 0;
    }
    if (name[0] == '\0' || strchr(name, '/')) {
        return 0;
    }
    if (ext4_lookup_child_nofollow(parent, name)) {
        return 0;
    }

    ext4_meta_t *pm = ext4_meta_for_inode(parent);
    if (!pm || !ext4_is_dir_mode(pm->mode)) {
        return 0;
    }

    uint32_t ino = 0;
    if (!ext4_alloc_inode(&ino)) {
        return 0;
    }

    ext4_meta_t nm;
    memset(&nm, 0, sizeof(nm));
    nm.valid = true;
    nm.ino = ino;
    nm.mode = EXT4_S_IFREG | 0644U;
    nm.links = 1U;
    nm.blocks_lo = 0U;
    nm.flags = 0U;
    nm.size = 0U;
    memset(nm.block, 0, sizeof(nm.block));

    if (!ext4_inode_store(&nm)) {
        return 0;
    }
    if (!ext4_dir_add_entry(pm, name, ino, EXT4_FT_REG_FILE)) {
        return 0;
    }
    if (block_cache_flush() != 0) {
        return 0;
    }

    inode_t *node = ext4_cache_get(ino, parent, name);
    if (node) {
        node->writable = true;
        node->size = 0;
    }
    return node;
}

inode_t *ext4_create_dir(inode_t *parent, const char *name) {
    if (!parent || !name || !g_ext4.mounted || !g_ext4.write_enabled ||
        parent->fs_kind != FS_KIND_EXT4 || parent->type != INODE_DIR) {
        return 0;
    }
    if (name[0] == '\0' || strchr(name, '/')) {
        return 0;
    }
    if (ext4_lookup_child_nofollow(parent, name)) {
        return 0;
    }

    ext4_meta_t *pm = ext4_meta_for_inode(parent);
    if (!pm || !ext4_is_dir_mode(pm->mode)) {
        return 0;
    }

    uint32_t ino = 0U;
    if (!ext4_alloc_inode(&ino)) {
        return 0;
    }

    uint64_t dblk = 0U;
    if (!ext4_alloc_block(&dblk) || dblk > 0xFFFFFFFFULL) {
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    uint8_t blk[4096];
    memset(blk, 0, g_ext4.block_size);

    uint16_t dot_rec = (uint16_t)(8U + align_up4(1U));
    uint16_t dotdot_rec = (uint16_t)(g_ext4.block_size - dot_rec);

    wr_le32(blk + 0U, ino);
    wr_le16(blk + 4U, dot_rec);
    blk[6U] = 1U;
    blk[7U] = g_ext4.has_filetype ? EXT4_FT_DIR : 0U;
    blk[8U] = '.';

    wr_le32(blk + dot_rec + 0U, pm->ino);
    wr_le16(blk + dot_rec + 4U, dotdot_rec);
    blk[dot_rec + 6U] = 2U;
    blk[dot_rec + 7U] = g_ext4.has_filetype ? EXT4_FT_DIR : 0U;
    blk[dot_rec + 8U] = '.';
    blk[dot_rec + 9U] = '.';

    if (!ext4_write_block(dblk, blk)) {
        (void)ext4_free_block(dblk);
        (void)ext4_free_inode_bit(ino);
        return 0;
    }
    if (!ext4_ordered_checkpoint()) {
        (void)ext4_free_block(dblk);
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    ext4_meta_t nm;
    memset(&nm, 0, sizeof(nm));
    nm.valid = true;
    nm.ino = ino;
    nm.mode = EXT4_S_IFDIR | 0755U;
    nm.links = 2U;
    nm.blocks_lo = (uint32_t)(((uint64_t)g_ext4.block_size + 511U) / 512U);
    nm.flags = 0U;
    nm.size = g_ext4.block_size;
    memset(nm.block, 0, sizeof(nm.block));
    nm.block[0] = (uint32_t)dblk;

    if (!ext4_inode_store(&nm)) {
        (void)ext4_free_block(dblk);
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    if (!ext4_dir_add_entry(pm, name, ino, EXT4_FT_DIR)) {
        (void)ext4_free_block(dblk);
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    pm->links++;
    if (!ext4_inode_store(pm)) {
        (void)ext4_dir_remove_entry(pm, name, 0, 0);
        (void)ext4_free_block(dblk);
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    if (block_cache_flush() != 0) {
        return 0;
    }

    inode_t *node = ext4_cache_get(ino, parent, name);
    if (node) {
        node->size = g_ext4.block_size;
    }
    return node;
}

int ext4_link(inode_t *inode, inode_t *new_parent, const char *new_name) {
    if (!inode || !new_parent || !new_name || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || new_parent->fs_kind != FS_KIND_EXT4 ||
        inode->type != INODE_FILE || new_parent->type != INODE_DIR) {
        return -1;
    }
    if (inode->fs_ino == EXT4_ROOT_INO || new_name[0] == '\0' || strchr(new_name, '/')) {
        return -1;
    }
    if (strlen(new_name) > INODE_NAME_MAX) {
        return -1;
    }
    if (ext4_lookup_child_nofollow(new_parent, new_name)) {
        return -1;
    }

    ext4_meta_t local_meta;
    ext4_meta_t *im = ext4_meta_for_inode(inode);
    if (!im) {
        if (!ext4_load_meta(inode->fs_ino, &local_meta)) {
            return -1;
        }
        im = &local_meta;
    }
    ext4_meta_t *pm = ext4_meta_for_inode(new_parent);
    if (!pm || !ext4_is_dir_mode(pm->mode) || ext4_is_dir_mode(im->mode)) {
        return -1;
    }
    if (im->links == 0xFFFFU) {
        return -1;
    }

    uint8_t ftype = ext4_inode_filetype(im);
    if (!ext4_dir_add_entry(pm, new_name, im->ino, ftype)) {
        return -1;
    }

    im->links++;
    if (!ext4_inode_store(im)) {
        (void)ext4_dir_remove_entry(pm, new_name, 0, 0);
        return -1;
    }
    if (block_cache_flush() != 0) {
        return -1;
    }
    return 0;
}

inode_t *ext4_symlink(inode_t *parent, const char *name, const char *target) {
    if (!parent || !name || !target || !g_ext4.mounted || !g_ext4.write_enabled ||
        parent->fs_kind != FS_KIND_EXT4 || parent->type != INODE_DIR) {
        return 0;
    }
    if (name[0] == '\0' || strchr(name, '/') || strlen(name) > INODE_NAME_MAX) {
        return 0;
    }
    size_t target_len = strlen(target);
    if (target_len == 0U || target_len > 0xFFFFFFFFULL || target_len > g_ext4.block_size) {
        return 0;
    }
    if (ext4_lookup_child_nofollow(parent, name)) {
        return 0;
    }

    ext4_meta_t *pm = ext4_meta_for_inode(parent);
    if (!pm || !ext4_is_dir_mode(pm->mode)) {
        return 0;
    }

    uint32_t ino = 0U;
    uint64_t dblk = 0U;
    bool added = false;
    if (!ext4_alloc_inode(&ino)) {
        return 0;
    }

    ext4_meta_t nm;
    memset(&nm, 0, sizeof(nm));
    nm.valid = true;
    nm.ino = ino;
    nm.mode = EXT4_S_IFLNK | 0777U;
    nm.links = 1U;
    nm.flags = 0U;
    nm.size = target_len;

    if (target_len <= sizeof(nm.block)) {
        memcpy((uint8_t *)nm.block, target, target_len);
        nm.blocks_lo = 0U;
    } else {
        uint8_t blk[4096];
        memset(blk, 0, g_ext4.block_size);
        memcpy(blk, target, target_len);
        if (!ext4_alloc_block(&dblk) || dblk > 0xFFFFFFFFULL) {
            (void)ext4_free_inode_bit(ino);
            return 0;
        }
        if (!ext4_write_block(dblk, blk)) {
            (void)ext4_free_block(dblk);
            (void)ext4_free_inode_bit(ino);
            return 0;
        }
        if (!ext4_ordered_checkpoint()) {
            (void)ext4_free_block(dblk);
            (void)ext4_free_inode_bit(ino);
            return 0;
        }
        nm.block[0] = (uint32_t)dblk;
        nm.blocks_lo = (uint32_t)(((uint64_t)g_ext4.block_size + 511U) / 512U);
    }

    if (!ext4_inode_store(&nm)) {
        if (dblk != 0U) {
            (void)ext4_free_block(dblk);
        }
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    if (!ext4_dir_add_entry(pm, name, ino, EXT4_FT_SYMLINK)) {
        if (dblk != 0U) {
            (void)ext4_free_block(dblk);
        }
        (void)ext4_free_inode_bit(ino);
        return 0;
    }
    added = true;

    if (block_cache_flush() != 0) {
        if (added) {
            (void)ext4_dir_remove_entry(pm, name, 0, 0);
        }
        if (dblk != 0U) {
            (void)ext4_free_block(dblk);
        }
        (void)ext4_free_inode_bit(ino);
        return 0;
    }

    inode_t *node = ext4_cache_get(ino, parent, name);
    if (node) {
        node->writable = false;
        node->executable = false;
        node->size = target_len;
    }
    return node;
}

int ext4_readlink(inode_t *inode, char *buf, size_t buflen) {
    if (!inode || !buf || inode->fs_kind != FS_KIND_EXT4 || !g_ext4.mounted ||
        inode->type != INODE_FILE) {
        return -1;
    }

    ext4_meta_t local_meta;
    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m) {
        if (!ext4_load_meta(inode->fs_ino, &local_meta)) {
            return -1;
        }
        m = &local_meta;
    }
    if (!ext4_is_lnk_mode(m->mode)) {
        return -1;
    }
    if (buflen == 0U) {
        return 0;
    }

    size_t copy_len = (size_t)m->size;
    if (copy_len > buflen) {
        copy_len = buflen;
    }
    if (copy_len == 0U) {
        return 0;
    }

    if (ext4_is_inline_symlink(m)) {
        memcpy(buf, (const uint8_t *)m->block, copy_len);
        return (int)copy_len;
    }
    if (ext4_read_file_at(m, 0, buf, copy_len) != (int)copy_len) {
        return -1;
    }
    return (int)copy_len;
}

int ext4_lstat(inode_t *inode, fu_stat_t *st) {
    if (!inode || !st || inode->fs_kind != FS_KIND_EXT4 || !g_ext4.mounted) {
        return -1;
    }

    ext4_meta_t local_meta;
    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m) {
        if (!ext4_load_meta(inode->fs_ino, &local_meta)) {
            return -1;
        }
        m = &local_meta;
    }

    st->type = (uint32_t)ext4_mode_to_inode_type(m->mode);
    st->mode = (uint32_t)m->mode;
    st->size = m->size;
    st->nlink = (uint32_t)m->links;
    st->fs_kind = (uint32_t)FS_KIND_EXT4;
    return 0;
}

int ext4_chmod(inode_t *inode, uint32_t mode) {
    if (!inode || inode->fs_kind != FS_KIND_EXT4 || !g_ext4.mounted || !g_ext4.write_enabled) {
        return -1;
    }

    ext4_meta_t local_meta;
    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m) {
        if (!ext4_load_meta(inode->fs_ino, &local_meta)) {
            return -1;
        }
        m = &local_meta;
    }

    if (!ext4_is_reg_mode(m->mode) && !ext4_is_dir_mode(m->mode) && !ext4_is_lnk_mode(m->mode)) {
        return -1;
    }

    uint16_t old_mode = m->mode;
    uint16_t new_mode = (uint16_t)((old_mode & EXT4_S_IFMT) | (mode & 0777U));
    if (old_mode == new_mode) {
        return 0;
    }

    m->mode = new_mode;
    if (!ext4_inode_store(m)) {
        m->mode = old_mode;
        return -1;
    }
    if (block_cache_flush() != 0) {
        return -1;
    }

    inode->writable = g_ext4.write_enabled &&
                     ext4_is_reg_mode(new_mode) &&
                     ((new_mode & 0222U) != 0U);
    inode->executable = ext4_is_reg_mode(new_mode) && ((new_mode & 0111U) != 0U);
    return 0;
}

bool ext4_mount(inode_t *mountpoint, const inode_t *source_dev) {
    if (!mountpoint || mountpoint->type != INODE_DIR ||
        !source_dev || source_dev->type != INODE_DEV ||
        source_dev->dev_kind == DEV_NONE) {
        return false;
    }
    if (g_ext4.mounted) {
        uart_puts("[ext4] mount failed: already mounted\n");
        return false;
    }
    if (!block_cache_ready()) {
        uart_puts("[ext4] mount skipped: block cache unavailable\n");
        return false;
    }

    memset(&g_ext4, 0, sizeof(g_ext4));
    g_ext4.source_dev_kind = source_dev->dev_kind;
    g_ext4.source_lba_start = source_dev->dev_lba_start;
    g_ext4.source_lba_count = source_dev->dev_lba_count;
    if (g_ext4.source_lba_count == 0U) {
        return false;
    }

    uint8_t sb[EXT4_SUPER_SIZE];
    if (!ext4_disk_read_bytes(EXT4_SUPER_OFFSET, sb, sizeof(sb))) {
        uart_puts("[ext4] mount failed: cannot read superblock\n");
        return false;
    }

    uint16_t magic = rd_le16(sb + EXT4_S_MAGIC);
    if (magic != EXT4_SUPER_MAGIC) {
        uart_puts("[ext4] mount skipped: no ext4 signature\n");
        return false;
    }

    uint32_t compat = rd_le32(sb + EXT4_S_FEATURE_COMPAT);
    uint32_t incompat = rd_le32(sb + EXT4_S_FEATURE_INCOMPAT);
    uint32_t ro_compat = rd_le32(sb + EXT4_S_FEATURE_RO_COMPAT);
    uint32_t supported_incompat = EXT4_INCOMPAT_FILETYPE |
                                  EXT4_INCOMPAT_RECOVER |
                                  EXT4_INCOMPAT_EXTENTS |
                                  EXT4_INCOMPAT_64BIT |
                                  EXT4_INCOMPAT_FLEX_BG;
    uint32_t supported_ro_compat = EXT4_ROCOMPAT_SPARSE_SUPER |
                                   EXT4_ROCOMPAT_LARGE_FILE |
                                   EXT4_ROCOMPAT_BTREE_DIR |
                                   EXT4_ROCOMPAT_HUGE_FILE |
                                   EXT4_ROCOMPAT_GDT_CSUM |
                                   EXT4_ROCOMPAT_DIR_NLINK |
                                   EXT4_ROCOMPAT_EXTRA_ISIZE |
                                   EXT4_ROCOMPAT_METADATA_CSUM;
    uint32_t unsupported_incompat = incompat & ~supported_incompat;
    uint32_t unsupported_ro = ro_compat & ~supported_ro_compat;
    if (unsupported_incompat != 0U) {
        uart_puts("[ext4] mount failed: unsupported incompat features mask=");
        print_hex64((uint64_t)unsupported_incompat);
        uart_puts("\n");
        return false;
    }
    if (incompat & EXT4_INCOMPAT_META_BG) {
        uart_puts("[ext4] mount failed: META_BG not supported\n");
        return false;
    }
    if (ro_compat & EXT4_ROCOMPAT_BIGALLOC) {
        uart_puts("[ext4] mount failed: BIGALLOC not supported\n");
        return false;
    }

    g_ext4.feature_compat = compat;
    g_ext4.feature_incompat = incompat;
    g_ext4.feature_ro_compat = ro_compat;
    bool has_journal = (compat & EXT4_COMPAT_HAS_JOURNAL) != 0U;
    g_ext4.write_enabled = true;
    if (ro_compat & (EXT4_ROCOMPAT_GDT_CSUM | EXT4_ROCOMPAT_METADATA_CSUM)) {
        g_ext4.write_enabled = false;
    }
    if (unsupported_ro != 0U) {
        g_ext4.write_enabled = false;
        uart_puts("[ext4] mount forcing ro: unknown ro_compat mask=");
        print_hex64((uint64_t)unsupported_ro);
        uart_puts("\n");
    }

    uint32_t log_block = rd_le32(sb + EXT4_S_LOG_BLOCK_SIZE);
    if (log_block > 2U) {
        uart_puts("[ext4] mount failed: unsupported block size\n");
        return false;
    }
    g_ext4.block_size = 1024U << log_block;
    if (g_ext4.block_size < 1024U || g_ext4.block_size > 4096U) {
        uart_puts("[ext4] mount failed: invalid block size\n");
        return false;
    }
    if ((g_ext4.block_size % VIRTIO_BLK_SECTOR_SIZE) != 0U) {
        uart_puts("[ext4] mount failed: block size not sector aligned\n");
        return false;
    }
    g_ext4.sectors_per_block = g_ext4.block_size / VIRTIO_BLK_SECTOR_SIZE;
    g_ext4.first_data_block = rd_le32(sb + EXT4_S_FIRST_DATA_BLOCK);
    g_ext4.inodes_count = rd_le32(sb + EXT4_S_INODES_COUNT);
    if (g_ext4.inodes_count == 0U) {
        uart_puts("[ext4] mount failed: invalid inode count\n");
        return false;
    }
    g_ext4.inode_size = rd_le16(sb + EXT4_S_INODE_SIZE);
    g_ext4.first_ino = rd_le32(sb + EXT4_S_FIRST_INO);
    if (g_ext4.first_ino < 11U) {
        g_ext4.first_ino = 11U;
    }
    if (g_ext4.inode_size < 128U || g_ext4.inode_size > g_ext4.block_size ||
        g_ext4.inode_size > 4096U) {
        uart_puts("[ext4] mount failed: unsupported inode size\n");
        return false;
    }
    g_ext4.blocks_per_group = rd_le32(sb + EXT4_S_BLOCKS_PER_GROUP);
    g_ext4.inodes_per_group = rd_le32(sb + EXT4_S_INODES_PER_GROUP);
    if (g_ext4.blocks_per_group == 0U || g_ext4.inodes_per_group == 0U) {
        uart_puts("[ext4] mount failed: invalid group geometry\n");
        return false;
    }

    uint64_t blocks = rd_le32(sb + EXT4_S_BLOCKS_COUNT_LO);
    if (incompat & EXT4_INCOMPAT_64BIT) {
        blocks |= ((uint64_t)rd_le32(sb + EXT4_S_BLOCKS_COUNT_HI) << 32);
    }
    g_ext4.blocks_count = blocks;
    g_ext4.free_blocks_count = rd_le32(sb + EXT4_S_FREE_BLOCKS_COUNT_LO);
    if (incompat & EXT4_INCOMPAT_64BIT) {
        g_ext4.free_blocks_count |= ((uint64_t)rd_le32(sb + EXT4_S_FREE_BLOCKS_COUNT_HI) << 32);
    }
    g_ext4.free_inodes_count = rd_le32(sb + EXT4_S_FREE_INODES_COUNT);
    if (blocks <= g_ext4.first_data_block) {
        uart_puts("[ext4] mount failed: invalid block count\n");
        return false;
    }
    uint64_t data_blocks = blocks - g_ext4.first_data_block;
    g_ext4.groups_count = (uint32_t)((data_blocks + g_ext4.blocks_per_group - 1U) /
                                     g_ext4.blocks_per_group);
    g_ext4.desc_size = rd_le16(sb + EXT4_S_DESC_SIZE);
    if (g_ext4.desc_size == 0U) {
        g_ext4.desc_size = 32U;
    }
    g_ext4.has_filetype = (incompat & EXT4_INCOMPAT_FILETYPE) != 0U;
    g_ext4.has_64bit = (incompat & EXT4_INCOMPAT_64BIT) != 0U;

    memset(&g_journal, 0, sizeof(g_journal));
    memset(&g_jbd2, 0, sizeof(g_jbd2));
    if (has_journal) {
        if (!ext4_jbd2_replay_if_needed(sb)) {
            uart_puts("[ext4] mount failed: jbd2 replay failed\n");
            return false;
        }
        if (g_ext4.write_enabled && g_jbd2.write_ready) {
            g_ext4.write_enabled = true;
        } else {
            g_ext4.write_enabled = false;
        }
    } else {
        if (!ext4_journal_setup()) {
            uart_puts("[ext4] mount failed: journal setup failed\n");
            return false;
        }
        if (!ext4_journal_replay_if_needed()) {
            uart_puts("[ext4] mount failed: journal replay failed\n");
            return false;
        }
    }

    if (!ext4_load_meta(EXT4_ROOT_INO, &g_ext4.root_meta) || !ext4_is_dir_mode(g_ext4.root_meta.mode)) {
        uart_puts("[ext4] mount failed: root inode invalid\n");
        return false;
    }

    g_ext4.mounted = true;
    g_ext4.instance_id = ++g_ext4_instance_seq;
    if (g_ext4.instance_id == 0U) {
        g_ext4.instance_id = ++g_ext4_instance_seq;
    }
    g_ext4.mountpoint = mountpoint;
    mountpoint->fs_kind = FS_KIND_EXT4;
    mountpoint->fs_id = g_ext4.instance_id;
    mountpoint->fs_ino = EXT4_ROOT_INO;
    mountpoint->writable = g_ext4.write_enabled;
    mountpoint->executable = false;
    mountpoint->size = (size_t)g_ext4.root_meta.size;
    mountpoint->capacity = mountpoint->size;
    mountpoint->data = 0;

    char mount_path[MAX_PATH];
    if (ext4_inode_abs_path(mountpoint, mount_path, sizeof(mount_path)) != 0) {
        strcpy(mount_path, "(unknown)");
    }

    uart_puts("[ext4] mounted ");
    uart_puts(g_ext4.write_enabled ? "rw" : "ro");
    uart_puts(" at ");
    uart_puts(mount_path);
    uart_puts(" block=");
    print_dec((int)g_ext4.block_size);
    uart_puts(" inodes/group=");
    print_dec((int)g_ext4.inodes_per_group);
    if (g_ext4.feature_compat & EXT4_COMPAT_HAS_JOURNAL) {
        uart_puts(" jbd2=on");
    } else if (g_journal.enabled) {
        uart_puts(" jlog=on");
    } else {
        uart_puts(" jlog=off");
    }
    uart_puts("\n");
    return true;
}

bool ext4_unmount(inode_t *mountpoint) {
    if (!g_ext4.mounted || !mountpoint || mountpoint != g_ext4.mountpoint) {
        return false;
    }
    char mount_path[MAX_PATH];
    if (ext4_inode_abs_path(mountpoint, mount_path, sizeof(mount_path)) != 0) {
        strcpy(mount_path, "(unknown)");
    }
    if (g_journal.tx_active) {
        ext4_tx_abort();
    }
    if (!ext4_sync_filesystem()) {
        return false;
    }
    pagecache_invalidate_ext4_all();

    mountpoint->fs_kind = FS_KIND_MEM;
    mountpoint->fs_id = 0U;
    mountpoint->fs_ino = 0U;
    mountpoint->writable = false;
    mountpoint->executable = false;
    mountpoint->size = 0U;
    mountpoint->capacity = 0U;
    mountpoint->data = 0;

    memset(&g_ext4, 0, sizeof(g_ext4));
    memset(&g_journal, 0, sizeof(g_journal));
    memset(&g_jbd2, 0, sizeof(g_jbd2));
    uart_puts("[ext4] unmounted ");
    uart_puts(mount_path);
    uart_puts("\n");
    return true;
}

static inode_t *ext4_lookup_child_internal(inode_t *dir, const char *name, bool follow_symlink) {
    if (!g_ext4.mounted || !dir || !name || dir->fs_kind != FS_KIND_EXT4 || dir->type != INODE_DIR) {
        return 0;
    }

    ext4_meta_t *dm = ext4_meta_for_inode(dir);
    if (!dm) {
        return 0;
    }

    uint64_t off = 0;
    for (;;) {
        uint32_t child_ino = 0;
        char child_name[256];
        uint8_t ftype = 0;
        int rc = ext4_dir_iter(dm, &off, &child_ino, child_name, &ftype);
        if (rc != 0) {
            return 0;
        }
        (void)ftype;
        if (strcmp(child_name, name) == 0) {
            inode_t *child = ext4_cache_get(child_ino, dir, child_name);
            if (!child) {
                return 0;
            }

            if (!follow_symlink) {
                return child;
            }
            ext4_meta_t *cm = ext4_meta_for_inode(child);
            if (!cm || !ext4_is_lnk_mode(cm->mode)) {
                return child;
            }
            if (g_ext4_symlink_depth >= EXT4_SYMLINK_MAX_DEPTH) {
                return 0;
            }

            g_ext4_symlink_depth++;
            inode_t *resolved = ext4_resolve_symlink(dir, cm);
            g_ext4_symlink_depth--;
            return resolved;
        }
    }
}

inode_t *ext4_lookup_child(inode_t *dir, const char *name) {
    return ext4_lookup_child_internal(dir, name, true);
}

inode_t *ext4_lookup_child_nofollow(inode_t *dir, const char *name) {
    return ext4_lookup_child_internal(dir, name, false);
}

int ext4_read(inode_t *inode, size_t *offset, void *buf, size_t len) {
    if (!inode || !offset || !buf || inode->fs_kind != FS_KIND_EXT4 || !g_ext4.mounted) {
        return -1;
    }

    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m) {
        return -1;
    }

    if (inode->type == INODE_DIR) {
        if (len < sizeof(dirent_t)) {
            return -1;
        }
        uint64_t off = *offset;
        for (;;) {
            uint32_t child_ino = 0;
            char child_name[256];
            uint8_t ftype = 0;
            int rc = ext4_dir_iter(m, &off, &child_ino, child_name, &ftype);
            if (rc == 1) {
                return 0;
            }
            if (rc != 0) {
                return -1;
            }

            dirent_t ent;
            memset(&ent, 0, sizeof(ent));
            strncpy(ent.name, child_name, INODE_NAME_MAX);
            ent.name[INODE_NAME_MAX] = '\0';
            if (ftype == EXT4_FT_DIR) {
                ent.type = INODE_DIR;
            } else if (ftype == EXT4_FT_REG_FILE) {
                ent.type = INODE_FILE;
            } else if (ftype == 7U) {
                ent.type = INODE_FILE;
            } else {
                inode_t *child = ext4_cache_get(child_ino, inode, child_name);
                ent.type = child ? (uint32_t)child->type : INODE_FILE;
            }

            memcpy(buf, &ent, sizeof(ent));
            *offset = (size_t)off;
            return (int)sizeof(ent);
        }
    }

    if (inode->type != INODE_FILE) {
        return 0;
    }
    int n = ext4_read_file_at(m, *offset, buf, len);
    if (n > 0) {
        *offset += (size_t)n;
    }
    return n;
}

static bool ext4_is_inline_symlink(const ext4_meta_t *m) {
    if (!m || !ext4_is_lnk_mode(m->mode)) {
        return false;
    }
    return ((m->flags & EXT4_EXTENTS_FL) == 0U) && (m->size <= sizeof(m->block));
}

int ext4_unlink(inode_t *inode) {
    if (!inode || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || inode->type != INODE_FILE ||
        inode->fs_ino == EXT4_ROOT_INO || !inode->parent) {
        return -1;
    }
    if (inode->name[0] == '\0') {
        return -1;
    }

    inode_t *parent = inode->parent;
    if (parent->fs_kind != FS_KIND_EXT4 || parent->type != INODE_DIR) {
        return -1;
    }

    ext4_meta_t *pm = ext4_meta_for_inode(parent);
    if (!pm || !ext4_is_dir_mode(pm->mode)) {
        return -1;
    }

    ext4_meta_t local_meta;
    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m) {
        if (!ext4_load_meta(inode->fs_ino, &local_meta)) {
            return -1;
        }
        m = &local_meta;
    }
    if (ext4_is_dir_mode(m->mode)) {
        return -1;
    }

    uint32_t removed_ino = 0;
    if (!ext4_dir_remove_entry(pm, inode->name, &removed_ino, 0)) {
        return -1;
    }
    if (removed_ino != m->ino) {
        return -1;
    }

    if (m->links > 0U) {
        m->links--;
    }
    if (m->links == 0U) {
        pagecache_invalidate_inode(inode);
        if (ext4_is_inline_symlink(m)) {
            memset(m->block, 0, sizeof(m->block));
            m->size = 0U;
            m->blocks_lo = 0U;
        } else if (!ext4_free_file_blocks(m, true)) {
            return -1;
        }
        m->mode = 0U;
        m->flags = 0U;
        if (!ext4_inode_store(m)) {
            return -1;
        }
        if (!ext4_free_inode_bit(m->ino)) {
            return -1;
        }
        ext4_cache_invalidate_ino(m->ino);
        inode->parent = 0;
        inode->name[0] = '\0';
    } else {
        if (!ext4_inode_store(m)) {
            return -1;
        }
        inode->size = (size_t)m->size;
    }

    if (block_cache_flush() != 0) {
        return -1;
    }
    return 0;
}

int ext4_rmdir(inode_t *inode) {
    if (!inode || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || inode->type != INODE_DIR ||
        inode->fs_ino == EXT4_ROOT_INO || !inode->parent) {
        return -1;
    }
    if (inode->name[0] == '\0') {
        return -1;
    }

    inode_t *parent = inode->parent;
    if (parent->fs_kind != FS_KIND_EXT4 || parent->type != INODE_DIR) {
        return -1;
    }

    ext4_meta_t *pm = ext4_meta_for_inode(parent);
    ext4_meta_t *dm = ext4_meta_for_inode(inode);
    if (!pm || !dm || !ext4_is_dir_mode(pm->mode) || !ext4_is_dir_mode(dm->mode)) {
        return -1;
    }
    if (!ext4_dir_is_empty(dm)) {
        return -1;
    }

    uint32_t removed_ino = 0;
    if (!ext4_dir_remove_entry(pm, inode->name, &removed_ino, 0)) {
        return -1;
    }
    if (removed_ino != dm->ino) {
        return -1;
    }

    if (pm->links > 0U) {
        pm->links--;
    }
    if (!ext4_inode_store(pm)) {
        return -1;
    }

    dm->links = 0U;
    pagecache_invalidate_inode(inode);
    if (!ext4_free_file_blocks(dm, true)) {
        return -1;
    }
    dm->mode = 0U;
    dm->flags = 0U;
    if (!ext4_inode_store(dm)) {
        return -1;
    }
    if (!ext4_free_inode_bit(dm->ino)) {
        return -1;
    }

    ext4_cache_invalidate_ino(dm->ino);
    inode->parent = 0;
    inode->name[0] = '\0';

    if (block_cache_flush() != 0) {
        return -1;
    }
    return 0;
}

int ext4_rename(inode_t *inode, inode_t *new_parent, const char *new_name) {
    if (!inode || !new_parent || !new_name || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || new_parent->fs_kind != FS_KIND_EXT4 ||
        inode->fs_ino == EXT4_ROOT_INO || !inode->parent) {
        return -1;
    }
    if (new_name[0] == '\0' || strchr(new_name, '/') || strlen(new_name) > INODE_NAME_MAX) {
        return -1;
    }

    inode_t *old_parent = inode->parent;
    if (old_parent->fs_kind != FS_KIND_EXT4 || old_parent->type != INODE_DIR ||
        new_parent->type != INODE_DIR) {
        return -1;
    }

    if (old_parent == new_parent && strcmp(inode->name, new_name) == 0) {
        return 0;
    }

    if (ext4_lookup_child_nofollow(new_parent, new_name)) {
        return -1;
    }

    ext4_meta_t *im = ext4_meta_for_inode(inode);
    ext4_meta_t *opm = ext4_meta_for_inode(old_parent);
    ext4_meta_t *npm = ext4_meta_for_inode(new_parent);
    if (!im || !opm || !npm || !ext4_is_dir_mode(opm->mode) || !ext4_is_dir_mode(npm->mode)) {
        return -1;
    }

    if (ext4_is_dir_mode(im->mode)) {
        inode_t *p = new_parent;
        while (p) {
            if (p == inode) {
                return -1;
            }
            p = p->parent;
        }
    }

    char old_name[INODE_NAME_MAX + 1];
    strncpy(old_name, inode->name, INODE_NAME_MAX);
    old_name[INODE_NAME_MAX] = '\0';

    uint32_t removed_ino = 0;
    uint8_t removed_ftype = 0;
    if (!ext4_dir_remove_entry(opm, old_name, &removed_ino, &removed_ftype)) {
        return -1;
    }
    if (removed_ino != im->ino) {
        return -1;
    }
    if (removed_ftype == 0U) {
        removed_ftype = ext4_inode_filetype(im);
    }

    if (!ext4_dir_add_entry(npm, new_name, im->ino, removed_ftype)) {
        (void)ext4_dir_add_entry(opm, old_name, im->ino, removed_ftype);
        return -1;
    }

    if (ext4_is_dir_mode(im->mode) && old_parent != new_parent) {
        if (!ext4_dir_update_entry_ino(im, "..", npm->ino)) {
            (void)ext4_dir_remove_entry(npm, new_name, 0, 0);
            (void)ext4_dir_add_entry(opm, old_name, im->ino, removed_ftype);
            return -1;
        }
        if (opm->links > 0U) {
            opm->links--;
        }
        npm->links++;
        if (!ext4_inode_store(opm) || !ext4_inode_store(npm)) {
            return -1;
        }
    }

    inode->parent = new_parent;
    strncpy(inode->name, new_name, INODE_NAME_MAX);
    inode->name[INODE_NAME_MAX] = '\0';

    if (block_cache_flush() != 0) {
        return -1;
    }
    return 0;
}

int ext4_truncate(inode_t *inode, size_t new_size) {
    if (!inode || !g_ext4.mounted || !g_ext4.write_enabled ||
        inode->fs_kind != FS_KIND_EXT4 || inode->type != INODE_FILE || !inode->writable) {
        return -1;
    }

    ext4_meta_t *m = ext4_meta_for_inode(inode);
    if (!m || !ext4_is_reg_mode(m->mode)) {
        return -1;
    }
    if ((uint64_t)new_size == m->size) {
        return 0;
    }

    uint64_t old_size = m->size;
    if ((uint64_t)new_size < old_size) {
        if (new_size == 0U) {
            if (!ext4_free_file_blocks(m, true)) {
                return -1;
            }
        } else {
            uint32_t new_blocks = (uint32_t)(((uint64_t)new_size + g_ext4.block_size - 1U) / g_ext4.block_size);
            if (m->flags & EXT4_EXTENTS_FL) {
                if (!ext4_extent_truncate_data(m, (uint64_t)new_size)) {
                    return -1;
                }
            } else {
                uint32_t old_blocks = (uint32_t)((old_size + g_ext4.block_size - 1U) / g_ext4.block_size);
                for (uint32_t lb = new_blocks; lb < old_blocks; lb++) {
                    uint64_t old_block = 0U;
                    if (!ext4_clear_block_direct(m, lb, &old_block)) {
                        return -1;
                    }
                    if (old_block != 0U) {
                        /* Persist pointer-clears before releasing blocks back to allocator. */
                        if (!ext4_inode_store(m)) {
                            return -1;
                        }
                        if (!ext4_ordered_checkpoint()) {
                            return -1;
                        }
                        if (!ext4_free_block(old_block)) {
                            return -1;
                        }
                    }
                }
            }

            if (((uint64_t)new_size % g_ext4.block_size) != 0U && new_blocks > 0U) {
                uint64_t pblock = 0U;
                uint32_t keep_block = new_blocks - 1U;
                bool ok = false;
                if (m->flags & EXT4_EXTENTS_FL) {
                    ok = ext4_map_file_block(m, keep_block, &pblock);
                } else {
                    ok = ext4_map_block_direct(m, keep_block, &pblock);
                }
                if (!ok) {
                    return -1;
                }
                if (pblock != 0U) {
                    uint8_t blk[4096];
                    uint32_t cut = (uint32_t)((uint64_t)new_size % g_ext4.block_size);
                    if (!ext4_read_block(pblock, blk)) {
                        return -1;
                    }
                    memset(blk + cut, 0, g_ext4.block_size - cut);
                    if (!ext4_write_block(pblock, blk)) {
                        return -1;
                    }
                }
            }
        }
    }

    m->size = (uint64_t)new_size;
    m->blocks_lo = (uint32_t)(((m->size + 511U) / 512U) & 0xFFFFFFFFU);
    if (!ext4_inode_store(m)) {
        return -1;
    }
    inode->size = new_size;

    if (block_cache_flush() != 0) {
        return -1;
    }
    return 0;
}
