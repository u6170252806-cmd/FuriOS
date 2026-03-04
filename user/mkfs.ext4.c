#include "user.h"

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0U;
    if (!s || !*s || !out) {
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        uint32_t d = (uint32_t)(*s - '0');
        if (v > (0xFFFFFFFFU - d) / 10U) {
            return -1;
        }
        v = v * 10U + d;
    }
    *out = v;
    return 0;
}

static int hex_nybble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int parse_uuid(const char *s, uint8_t out[16]) {
    char hex[32];
    int n = 0;
    if (!s || !out) {
        return -1;
    }
    for (; *s; s++) {
        if (*s == '-') {
            continue;
        }
        if (n >= 32) {
            return -1;
        }
        if (hex_nybble(*s) < 0) {
            return -1;
        }
        hex[n++] = *s;
    }
    if (n != 32) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        int hi = hex_nybble(hex[i * 2]);
        int lo = hex_nybble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void set_feature(uint32_t *mask, uint32_t bit, int on) {
    if (on) {
        *mask |= bit;
    } else {
        *mask &= ~bit;
    }
}

static int parse_features(char *list, uint32_t *mask) {
    char *p = list;
    while (*p) {
        while (*p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *tok = p;
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            *p++ = '\0';
        }
        int on = 1;
        if (tok[0] == '^') {
            on = 0;
            tok++;
        }
        if (strcmp(tok, "none") == 0) {
            if (on) {
                *mask = 0U;
            }
            continue;
        }
        if (strcmp(tok, "baseline") == 0) {
            uint32_t preset = MKFS_EXT4_FEAT_EXTENTS |
                              MKFS_EXT4_FEAT_SPARSE_SUPER |
                              MKFS_EXT4_FEAT_HAS_JOURNAL;
            if (on) {
                *mask |= preset;
            } else {
                *mask &= ~preset;
            }
            continue;
        }
        if (strcmp(tok, "kernelrw") == 0) {
            uint32_t preset = MKFS_EXT4_FEAT_EXTENTS |
                              MKFS_EXT4_FEAT_64BIT |
                              MKFS_EXT4_FEAT_SPARSE_SUPER |
                              MKFS_EXT4_FEAT_HAS_JOURNAL;
            if (on) {
                *mask |= preset;
            } else {
                *mask &= ~preset;
            }
            continue;
        }
        if (strcmp(tok, "modern") == 0) {
            uint32_t preset = MKFS_EXT4_FEAT_EXTENTS |
                              MKFS_EXT4_FEAT_64BIT |
                              MKFS_EXT4_FEAT_METADATA_CSUM |
                              MKFS_EXT4_FEAT_SPARSE_SUPER |
                              MKFS_EXT4_FEAT_HAS_JOURNAL;
            if (on) {
                *mask |= preset;
            } else {
                *mask &= ~preset;
            }
            continue;
        }
        if (strcmp(tok, "extents") == 0) {
            set_feature(mask, MKFS_EXT4_FEAT_EXTENTS, on);
            continue;
        }
        if (strcmp(tok, "64bit") == 0) {
            set_feature(mask, MKFS_EXT4_FEAT_64BIT, on);
            continue;
        }
        if (strcmp(tok, "metadata_csum") == 0) {
            set_feature(mask, MKFS_EXT4_FEAT_METADATA_CSUM, on);
            continue;
        }
        if (strcmp(tok, "sparse_super") == 0) {
            set_feature(mask, MKFS_EXT4_FEAT_SPARSE_SUPER, on);
            continue;
        }
        if (strcmp(tok, "has_journal") == 0) {
            set_feature(mask, MKFS_EXT4_FEAT_HAS_JOURNAL, on);
            continue;
        }
        return -1;
    }
    return 0;
}

static int parse_extended(char *list, uint16_t *stride_out, uint32_t *journal_blocks_out) {
    char *p = list;
    while (*p) {
        while (*p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *tok = p;
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            *p++ = '\0';
        }
        if (strncmp(tok, "stride=", 7) == 0) {
            uint32_t v = 0U;
            if (parse_u32(tok + 7, &v) != 0 || v > 65535U) {
                return -1;
            }
            *stride_out = (uint16_t)v;
            continue;
        }
        if (strncmp(tok, "journal_blocks=", 15) == 0) {
            uint32_t v = 0U;
            if (parse_u32(tok + 15, &v) != 0 || v == 0U) {
                return -1;
            }
            *journal_blocks_out = v;
            continue;
        }
        return -1;
    }
    return 0;
}

static void usage(void) {
    puts("usage: mkfs.ext4 [-L label] [-U uuid|random] [-m reserved_pct] ");
    puts("[-i bytes-per-inode] [-T default|small|largefile|largefile4] ");
    puts("[-K|--strict-kernel] ");
    puts("[-E stride=N,journal_blocks=N] [-O feat[,feat...]] <device>\n");
}

int main(int argc, char **argv) {
    fu_mkfs_ext4_opts_t opts;
    unsigned long mkfs_flags = 0;
    memset(&opts, 0, sizeof(opts));
    opts.size = sizeof(opts);
    opts.feature_flags = MKFS_EXT4_FEAT_EXTENTS |
                         MKFS_EXT4_FEAT_64BIT |
                         MKFS_EXT4_FEAT_SPARSE_SUPER |
                         MKFS_EXT4_FEAT_HAS_JOURNAL;
    opts.profile = MKFS_EXT4_PROFILE_DEFAULT;
    opts.inode_size = 128U;

    char *target = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            memset(opts.label, 0, sizeof(opts.label));
            const char *src = argv[++i];
            for (size_t j = 0; j < sizeof(opts.label) - 1U && src[j]; j++) {
                opts.label[j] = src[j];
            }
            opts.opt_flags |= MKFS_EXT4_OPT_LABEL_SET;
            continue;
        }
        if (strcmp(argv[i], "-U") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            const char *v = argv[++i];
            if (strcmp(v, "random") == 0) {
                opts.opt_flags &= ~MKFS_EXT4_OPT_UUID_SET;
            } else {
                if (parse_uuid(v, opts.uuid) != 0) {
                    puts("mkfs.ext4: invalid uuid\n");
                    return 1;
                }
                opts.opt_flags |= MKFS_EXT4_OPT_UUID_SET;
            }
            continue;
        }
        if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            uint32_t pct = 0U;
            if (parse_u32(argv[++i], &pct) != 0 || pct > 99U) {
                puts("mkfs.ext4: invalid -m value\n");
                return 1;
            }
            opts.reserved_pct = (uint16_t)pct;
            continue;
        }
        if (strcmp(argv[i], "-O") == 0) {
            if (i + 1 >= argc || parse_features(argv[++i], &opts.feature_flags) != 0) {
                puts("mkfs.ext4: invalid -O list\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            uint32_t bpi = 0U;
            if (parse_u32(argv[++i], &bpi) != 0 || bpi < 1024U) {
                puts("mkfs.ext4: invalid -i value\n");
                return 1;
            }
            opts.bytes_per_inode = bpi;
            continue;
        }
        if (strcmp(argv[i], "-T") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            const char *profile = argv[++i];
            if (strcmp(profile, "default") == 0) {
                opts.profile = MKFS_EXT4_PROFILE_DEFAULT;
            } else if (strcmp(profile, "small") == 0) {
                opts.profile = MKFS_EXT4_PROFILE_SMALL;
            } else if (strcmp(profile, "largefile") == 0) {
                opts.profile = MKFS_EXT4_PROFILE_LARGEFILE;
            } else if (strcmp(profile, "largefile4") == 0) {
                opts.profile = MKFS_EXT4_PROFILE_LARGEFILE4;
            } else {
                puts("mkfs.ext4: invalid -T profile\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-K") == 0 || strcmp(argv[i], "--strict-kernel") == 0) {
            mkfs_flags |= MKFS_EXT4_F_STRICT_KERNEL;
            continue;
        }
        if (strcmp(argv[i], "-E") == 0) {
            if (i + 1 >= argc || parse_extended(argv[++i], &opts.stride, &opts.journal_blocks) != 0) {
                puts("mkfs.ext4: invalid -E list\n");
                return 1;
            }
            continue;
        }
        if (argv[i][0] == '-') {
            usage();
            return 1;
        }
        if (target) {
            usage();
            return 1;
        }
        target = argv[i];
    }

    if (!target) {
        usage();
        return 1;
    }

    if (sys_mkfsext4(target, mkfs_flags, &opts) != 0) {
        puts("mkfs.ext4: failed\n");
        return 1;
    }

    puts("mkfs.ext4: formatted\n");
    return 0;
}
