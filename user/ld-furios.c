#include "user.h"
#include "elf.h"
#include "config.h"
#include <stdbool.h>

#define PT_GNU_RELRO 0x6474E552U
#define PT_PHDR 6U
#define PT_TLS 7U

#define MAX_DSO 32
#define MAX_NEEDED 16
#define AUXV_MAX 48

#define DYN_LOAD_BASE 0x00500000UL
#define DYN_LOAD_LIMIT 0x00670000UL

typedef struct {
    uint64_t symtab;
    uint64_t strtab;
    uint64_t strsz;
    uint64_t syment;
    uint64_t sym_count;
    uint64_t gnu_hash;

    uint64_t rela;
    uint64_t relasz;
    uint64_t relaent;
    uint64_t rel;
    uint64_t relsz;
    uint64_t relent;

    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t pltrel;

    uint64_t init;
    uint64_t fini;
    uint64_t init_array;
    uint64_t init_arraysz;
    uint64_t fini_array;
    uint64_t fini_arraysz;

    uint32_t needed_count;
    uint32_t needed_off[MAX_NEEDED];
    bool have_runpath;
    bool have_rpath;
    uint32_t runpath_off;
    uint32_t rpath_off;
    uint64_t versym;
    uint64_t verdef;
    uint64_t verdefnum;
    uint64_t verneed;
    uint64_t verneednum;
    uint64_t flags;
    uint64_t flags1;
    bool textrel;

    uint64_t phdr_addr;
} dyn_info_t;

typedef struct {
    char path[MAX_PATH];
    uint8_t *image;
    size_t image_size;
    Elf64_Ehdr eh;

    uint64_t load_bias;
    uint64_t map_start;
    uint64_t map_end;

    uint64_t relro_addr;
    size_t relro_size;

    dyn_info_t dyn;
    uint8_t dep_count;
    int16_t dep_index[MAX_NEEDED];
    uint32_t tls_modid;
    uint64_t tls_offset;
    uint64_t tls_align;
    uint64_t tls_memsz;
    uint64_t tls_filesz;
    uint64_t tls_init_off;
    bool has_tls;

    bool is_main;
} dso_t;

static dso_t g_loader_objs[MAX_DSO];
static const char *g_ld_library_path;
static void *g_tls_block;
static size_t g_tls_block_size;

typedef struct {
    uint64_t arg;
    uint64_t entry;
} tlsdesc_t;

static uint64_t align_down_u64(uint64_t v, uint64_t a) {
    return v & ~(a - 1U);
}

static uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1U) & ~(a - 1U);
}

static void set_thread_pointer(void *tp) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp) : "memory");
}

static uint64_t tlsdesc_return(const tlsdesc_t *desc) {
    if (!desc) {
        return 0U;
    }
    return desc->arg;
}

static void write_str2(const char *s) {
    if (s) {
        (void)sys_write(2, s, strlen(s));
    }
}

static void loader_fail(const char *msg) {
    write_str2("ld-furios: ");
    write_str2(msg);
    write_str2("\n");
    sys_exit(127);
}

static void loader_fail_path(const char *msg, const char *path) {
    write_str2("ld-furios: ");
    write_str2(msg);
    if (path && path[0]) {
        write_str2(": ");
        write_str2(path);
    }
    write_str2("\n");
    sys_exit(127);
}

static int read_all(int fd, uint8_t *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        long n = read(fd, buf + done, (unsigned long)(len - done));
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int load_file_image(const char *path, uint8_t **out_buf, size_t *out_size) {
    if (!path || !out_buf || !out_size) {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    long end = lseek(fd, 0, SEEK_END);
    if (end <= 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    size_t sz = (size_t)end;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        close(fd);
        return -1;
    }

    if (read_all(fd, buf, sz) != 0) {
        free(buf);
        close(fd);
        return -1;
    }

    close(fd);
    *out_buf = buf;
    *out_size = sz;
    return 0;
}

static int validate_elf_image(const uint8_t *image, size_t image_size, Elf64_Ehdr *eh_out) {
    if (!image || image_size < sizeof(Elf64_Ehdr) || !eh_out) {
        return -1;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_machine != EM_AARCH64 ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phoff > image_size ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > image_size - eh->e_phoff) {
        return -1;
    }

    *eh_out = *eh;
    return 0;
}

static uint64_t elf_addr(const Elf64_Ehdr *eh, uint64_t load_bias, uint64_t vaddr) {
    if (eh->e_type == ET_DYN) {
        return load_bias + vaddr;
    }
    return vaddr;
}

static uint64_t elf_phdr_runtime_addr(const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                                      uint64_t load_bias, size_t image_size) {
    if (!eh || !ph || eh->e_phoff >= image_size) {
        return 0;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_PHDR) {
            return elf_addr(eh, load_bias, ph[i].p_vaddr);
        }
    }

    uint64_t phoff = eh->e_phoff;
    uint64_t phsize = (uint64_t)eh->e_phnum * eh->e_phentsize;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0U) {
            continue;
        }
        if (ph[i].p_offset > phoff) {
            continue;
        }
        if (phoff + phsize > ph[i].p_offset + ph[i].p_filesz) {
            continue;
        }
        return elf_addr(eh, load_bias, ph[i].p_vaddr + (phoff - ph[i].p_offset));
    }
    return 0;
}

static int map_elf_segments(const Elf64_Ehdr *eh, const uint8_t *image, size_t image_size,
                            uint64_t preferred_dyn_base,
                            uint64_t *load_bias_out,
                            uint64_t *map_start_out,
                            uint64_t *map_end_out,
                            uint64_t *dyn_addr_out, size_t *dyn_size_out,
                            uint64_t *relro_addr_out, size_t *relro_size_out,
                            uint64_t *phdr_addr_out) {
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(image + eh->e_phoff);
    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0U) {
            continue;
        }
        if (ph[i].p_filesz > ph[i].p_memsz) {
            return -1;
        }
        if (ph[i].p_offset > image_size ||
            ph[i].p_filesz > image_size - ph[i].p_offset) {
            return -1;
        }
        if (ph[i].p_vaddr < min_vaddr) {
            min_vaddr = ph[i].p_vaddr;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz > max_vaddr) {
            max_vaddr = ph[i].p_vaddr + ph[i].p_memsz;
        }
    }

    if (min_vaddr == (uint64_t)-1 || max_vaddr <= min_vaddr) {
        return -1;
    }

    uint64_t min_page = align_down_u64(min_vaddr, PAGE_SIZE);
    uint64_t max_page = align_up_u64(max_vaddr, PAGE_SIZE);
    uint64_t load_bias = 0;

    if (eh->e_type == ET_DYN) {
        if (preferred_dyn_base < USER_VA_BASE || (preferred_dyn_base & (PAGE_SIZE - 1U)) != 0U) {
            return -1;
        }
        load_bias = preferred_dyn_base - min_page;
    }

    uint64_t map_start = min_page + load_bias;
    uint64_t map_end = max_page + load_bias;
    uint64_t stack_floor = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    if (map_start < USER_VA_BASE || map_end > stack_floor || map_end <= map_start) {
        return -1;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0U) {
            continue;
        }

        uint64_t seg_start = align_down_u64(ph[i].p_vaddr + load_bias, PAGE_SIZE);
        uint64_t seg_end = align_up_u64(ph[i].p_vaddr + load_bias + ph[i].p_memsz, PAGE_SIZE);
        uint64_t seg_len = seg_end - seg_start;

        int seg_prot = 0;
        if (ph[i].p_flags & PF_R) {
            seg_prot |= PROT_READ;
        }
        if (ph[i].p_flags & PF_W) {
            seg_prot |= PROT_WRITE;
        }
        if (ph[i].p_flags & PF_X) {
            seg_prot |= PROT_EXEC;
        }
        int final_prot = seg_prot;
        if ((final_prot & PROT_EXEC) && (final_prot & PROT_WRITE)) {
            final_prot &= ~PROT_WRITE;
        }

        /* Keep writable mapping non-executable during load for W^X discipline. */
        int map_prot = PROT_READ | PROT_WRITE;
        void *mapped = mmap((void *)(uintptr_t)seg_start, (unsigned long)seg_len,
                            map_prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped == (void *)-1) {
            return -1;
        }

        uint64_t seg_va = ph[i].p_vaddr + load_bias;
        if (ph[i].p_filesz) {
            memcpy((void *)(uintptr_t)seg_va, image + ph[i].p_offset, (size_t)ph[i].p_filesz);
        }
        if (ph[i].p_memsz > ph[i].p_filesz) {
            uint64_t bss_addr = seg_va + ph[i].p_filesz;
            uint64_t bss_len = ph[i].p_memsz - ph[i].p_filesz;
            memset((void *)(uintptr_t)bss_addr, 0, (size_t)bss_len);
        }

        if (mprotect((void *)(uintptr_t)seg_start, (unsigned long)seg_len, final_prot) != 0) {
            return -1;
        }
    }

    *load_bias_out = load_bias;
    *map_start_out = map_start;
    *map_end_out = map_end;
    *dyn_addr_out = 0;
    *dyn_size_out = 0;
    *relro_addr_out = 0;
    *relro_size_out = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            *dyn_addr_out = elf_addr(eh, load_bias, ph[i].p_vaddr);
            *dyn_size_out = (size_t)ph[i].p_memsz;
        } else if (ph[i].p_type == PT_GNU_RELRO) {
            uint64_t rs = align_down_u64(elf_addr(eh, load_bias, ph[i].p_vaddr), PAGE_SIZE);
            uint64_t re = align_up_u64(elf_addr(eh, load_bias, ph[i].p_vaddr + ph[i].p_memsz), PAGE_SIZE);
            if (re > rs) {
                *relro_addr_out = rs;
                *relro_size_out = (size_t)(re - rs);
            }
        }
    }

    *phdr_addr_out = elf_phdr_runtime_addr(eh, ph, load_bias, image_size);

    return 0;
}

static uint64_t gnu_hash_sym_count(uint64_t gnu_hash_addr) {
    if (gnu_hash_addr == 0U) {
        return 0U;
    }

    const uint32_t *hdr = (const uint32_t *)(uintptr_t)gnu_hash_addr;
    uint32_t nbuckets = hdr[0];
    uint32_t symoffset = hdr[1];
    uint32_t bloom_size = hdr[2];

    if (nbuckets == 0U || bloom_size == 0U) {
        return 0U;
    }

    const uint64_t *bloom = (const uint64_t *)(const void *)(hdr + 4);
    const uint32_t *buckets = (const uint32_t *)(const void *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;

    uint64_t max_sym = symoffset;
    const uint32_t scan_cap = 1U << 20;

    for (uint32_t b = 0; b < nbuckets; b++) {
        uint32_t idx = buckets[b];
        if (idx < symoffset) {
            continue;
        }

        uint32_t steps = 0;
        uint32_t cur = idx;
        for (;;) {
            uint32_t chain_idx = cur - symoffset;
            if (chain_idx > scan_cap) {
                return max_sym;
            }

            uint32_t h = chain[chain_idx];
            if ((uint64_t)cur + 1U > max_sym) {
                max_sym = (uint64_t)cur + 1U;
            }
            if (h & 1U) {
                break;
            }
            cur++;
            steps++;
            if (steps > scan_cap) {
                return max_sym;
            }
        }
    }

    return max_sym;
}

static int parse_dynamic(const Elf64_Ehdr *eh, uint64_t load_bias, uint64_t dyn_addr,
                         size_t dyn_size, dyn_info_t *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->relaent = sizeof(Elf64_Rela);
    out->relent = sizeof(Elf64_Rel);
    out->syment = sizeof(Elf64_Sym);
    out->pltrel = DT_RELA;

    if (dyn_addr == 0U || dyn_size < sizeof(Elf64_Dyn)) {
        return 0;
    }

    const Elf64_Dyn *dyn = (const Elf64_Dyn *)(uintptr_t)dyn_addr;
    size_t count = dyn_size / sizeof(Elf64_Dyn);
    uint64_t hash_addr = 0;
    bool needed_overflow = false;

    for (size_t i = 0; i < count; i++) {
        uint64_t val = dyn[i].d_un;
        switch (dyn[i].d_tag) {
            case DT_NULL:
                i = count;
                break;
            case DT_STRTAB:
                out->strtab = elf_addr(eh, load_bias, val);
                break;
            case DT_STRSZ:
                out->strsz = val;
                break;
            case DT_SYMTAB:
                out->symtab = elf_addr(eh, load_bias, val);
                break;
            case DT_SYMENT:
                out->syment = val;
                break;
            case DT_RELA:
                out->rela = elf_addr(eh, load_bias, val);
                break;
            case DT_RELASZ:
                out->relasz = val;
                break;
            case DT_RELAENT:
                out->relaent = val;
                break;
            case DT_REL:
                out->rel = elf_addr(eh, load_bias, val);
                break;
            case DT_RELSZ:
                out->relsz = val;
                break;
            case DT_RELENT:
                out->relent = val;
                break;
            case DT_JMPREL:
                out->jmprel = elf_addr(eh, load_bias, val);
                break;
            case DT_PLTRELSZ:
                out->pltrelsz = val;
                break;
            case DT_PLTREL:
                out->pltrel = val;
                break;
            case DT_INIT:
                out->init = elf_addr(eh, load_bias, val);
                break;
            case DT_FINI:
                out->fini = elf_addr(eh, load_bias, val);
                break;
            case DT_INIT_ARRAY:
                out->init_array = elf_addr(eh, load_bias, val);
                break;
            case DT_INIT_ARRAYSZ:
                out->init_arraysz = val;
                break;
            case DT_FINI_ARRAY:
                out->fini_array = elf_addr(eh, load_bias, val);
                break;
            case DT_FINI_ARRAYSZ:
                out->fini_arraysz = val;
                break;
            case DT_HASH:
                hash_addr = elf_addr(eh, load_bias, val);
                break;
            case DT_GNU_HASH:
                out->gnu_hash = elf_addr(eh, load_bias, val);
                break;
            case DT_NEEDED:
                if (out->needed_count < MAX_NEEDED) {
                    out->needed_off[out->needed_count++] = (uint32_t)val;
                } else {
                    needed_overflow = true;
                }
                break;
            case DT_RUNPATH:
                out->have_runpath = true;
                out->runpath_off = (uint32_t)val;
                break;
            case DT_RPATH:
                out->have_rpath = true;
                out->rpath_off = (uint32_t)val;
                break;
            case DT_TEXTREL:
                out->textrel = true;
                break;
            case DT_FLAGS:
                out->flags = val;
                if ((val & DF_TEXTREL) != 0U) {
                    out->textrel = true;
                }
                break;
            case DT_FLAGS_1:
                out->flags1 = val;
                break;
            case DT_VERSYM:
                out->versym = elf_addr(eh, load_bias, val);
                break;
            case DT_VERDEF:
                out->verdef = elf_addr(eh, load_bias, val);
                break;
            case DT_VERDEFNUM:
                out->verdefnum = val;
                break;
            case DT_VERNEED:
                out->verneed = elf_addr(eh, load_bias, val);
                break;
            case DT_VERNEEDNUM:
                out->verneednum = val;
                break;
            default:
                break;
        }
    }

    if (needed_overflow) {
        return -1;
    }

    if (hash_addr != 0U) {
        const uint32_t *hash = (const uint32_t *)(uintptr_t)hash_addr;
        out->sym_count = hash[1];
    } else if (out->gnu_hash != 0U) {
        out->sym_count = gnu_hash_sym_count(out->gnu_hash);
    }

    return 0;
}

static int path_exists_readable(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static int build_lib_path(char *out, size_t outsz, const char *dir, const char *name) {
    if (!out || !dir || !name) {
        return -1;
    }
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    if (dl + 1U + nl + 1U > outsz) {
        return -1;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1U, name, nl);
    out[dl + 1U + nl] = '\0';
    return 0;
}

static const char *env_lookup(char **envp, const char *name) {
    if (!envp || !name) {
        return 0;
    }
    size_t nl = strlen(name);
    for (char **p = envp; *p; p++) {
        const char *ent = *p;
        if (strncmp(ent, name, nl) == 0 && ent[nl] == '=') {
            return ent + nl + 1U;
        }
    }
    return 0;
}

static void path_dirname(const char *path, char *out, size_t outsz) {
    if (!out || outsz < 2U) {
        return;
    }
    out[0] = '.';
    out[1] = '\0';
    if (!path || path[0] == '\0') {
        return;
    }

    const char *slash = strrchr(path, '/');
    if (!slash) {
        return;
    }
    if (slash == path) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    size_t len = (size_t)(slash - path);
    if (len >= outsz) {
        return;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static int expand_origin_component(const char *component, const dso_t *requester,
                                   char *out, size_t outsz) {
    char origin[MAX_PATH];
    const char *base = ".";
    size_t w = 0U;

    if (!component || !out || outsz < 2U) {
        return -1;
    }
    if (requester) {
        path_dirname(requester->path, origin, sizeof(origin));
        base = origin;
    }

    for (size_t i = 0; component[i] != '\0';) {
        if (component[i] == '$' &&
            strncmp(component + i, "$ORIGIN", 7U) == 0) {
            size_t bl = strlen(base);
            if (w + bl >= outsz) {
                return -1;
            }
            memcpy(out + w, base, bl);
            w += bl;
            i += 7U;
            continue;
        }
        if (w + 1U >= outsz) {
            return -1;
        }
        out[w++] = component[i++];
    }
    out[w] = '\0';
    return 0;
}

static const char *dso_dyn_string(const dso_t *obj, uint32_t off) {
    if (!obj || !obj->dyn.strtab || off >= obj->dyn.strsz) {
        return 0;
    }
    return (const char *)(uintptr_t)(obj->dyn.strtab + (uint64_t)off);
}

static const char *dso_runpath(const dso_t *obj) {
    if (!obj || !obj->dyn.have_runpath) {
        return 0;
    }
    return dso_dyn_string(obj, obj->dyn.runpath_off);
}

static const char *dso_rpath(const dso_t *obj) {
    if (!obj || !obj->dyn.have_rpath) {
        return 0;
    }
    return dso_dyn_string(obj, obj->dyn.rpath_off);
}

static int resolve_needed_from_list(const char *list, const char *name,
                                    const dso_t *requester,
                                    char *path_out, size_t outsz) {
    if (!list || !name || !path_out || outsz < 2U) {
        return -1;
    }

    const char *p = list;
    while (*p) {
        const char *start = p;
        while (*p && *p != ':') {
            p++;
        }
        size_t dl = (size_t)(p - start);
        char comp[MAX_PATH];
        char dir[MAX_PATH];

        if (dl == 0U) {
            comp[0] = '.';
            comp[1] = '\0';
        } else if (dl < sizeof(comp)) {
            memcpy(comp, start, dl);
            comp[dl] = '\0';
        } else {
            comp[0] = '\0';
        }

        if (comp[0] != '\0' &&
            expand_origin_component(comp, requester, dir, sizeof(dir)) == 0 &&
            dir[0] != '\0' &&
            build_lib_path(path_out, outsz, dir, name) == 0 &&
            path_exists_readable(path_out)) {
            return 0;
        }

        if (*p == ':') {
            p++;
        }
    }
    return -1;
}

static int resolve_needed_path(const dso_t *requester, const char *name,
                               char *path_out, size_t outsz) {
    if (!name || !path_out || outsz < 2U || name[0] == '\0') {
        return -1;
    }

    if (strchr(name, '/')) {
        if (strlen(name) + 1U > outsz) {
            return -1;
        }
        strcpy(path_out, name);
        return path_exists_readable(path_out) ? 0 : -1;
    }

    const char *runpath = requester ? dso_runpath(requester) : 0;
    const char *rpath = (requester && !runpath) ? dso_rpath(requester) : 0;

    if (rpath &&
        resolve_needed_from_list(rpath, name, requester, path_out, outsz) == 0) {
        return 0;
    }
    if (resolve_needed_from_list(g_ld_library_path, name, requester, path_out, outsz) == 0) {
        return 0;
    }
    if (runpath &&
        resolve_needed_from_list(runpath, name, requester, path_out, outsz) == 0) {
        return 0;
    }
    if (build_lib_path(path_out, outsz, "/lib", name) == 0 && path_exists_readable(path_out)) {
        return 0;
    }
    if (build_lib_path(path_out, outsz, "/usr/lib", name) == 0 && path_exists_readable(path_out)) {
        return 0;
    }
    return -1;
}

static const char *needed_name_at(const dso_t *obj, uint32_t needed_idx) {
    if (!obj || needed_idx >= obj->dyn.needed_count || !obj->dyn.strtab) {
        return 0;
    }
    uint32_t off = obj->dyn.needed_off[needed_idx];
    if (off >= obj->dyn.strsz) {
        return 0;
    }
    return (const char *)(uintptr_t)(obj->dyn.strtab + off);
}

static int dso_find_by_path(dso_t *objs, int count, const char *path) {
    for (int i = 0; i < count; i++) {
        if (strcmp(objs[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int dso_load(dso_t *obj, const char *path, uint64_t preferred_dyn_base, bool is_main) {
    if (!obj || !path) {
        return -1;
    }

    memset(obj, 0, sizeof(*obj));
    if (strlen(path) + 1U > sizeof(obj->path)) {
        return -1;
    }
    strcpy(obj->path, path);
    obj->is_main = is_main;
    obj->dep_count = 0U;
    for (size_t i = 0; i < MAX_NEEDED; i++) {
        obj->dep_index[i] = -1;
    }

    if (load_file_image(path, &obj->image, &obj->image_size) != 0) {
        return -1;
    }
    if (validate_elf_image(obj->image, obj->image_size, &obj->eh) != 0) {
        return -1;
    }

    const Elf64_Phdr *ph = 0;
    uint64_t dyn_addr = 0;
    size_t dyn_size = 0;
    if (map_elf_segments(&obj->eh, obj->image, obj->image_size,
                         preferred_dyn_base,
                         &obj->load_bias,
                         &obj->map_start,
                         &obj->map_end,
                         &dyn_addr,
                         &dyn_size,
                         &obj->relro_addr,
                         &obj->relro_size,
                         &obj->dyn.phdr_addr) != 0) {
        return -1;
    }

    if (parse_dynamic(&obj->eh, obj->load_bias, dyn_addr, dyn_size, &obj->dyn) != 0) {
        return -1;
    }

    if (obj->dyn.strtab && obj->dyn.strsz) {
        for (uint32_t i = 0; i < obj->dyn.needed_count; i++) {
            if (obj->dyn.needed_off[i] >= obj->dyn.strsz) {
                return -1;
            }
        }
        if ((obj->dyn.have_runpath && obj->dyn.runpath_off >= obj->dyn.strsz) ||
            (obj->dyn.have_rpath && obj->dyn.rpath_off >= obj->dyn.strsz)) {
            return -1;
        }
    } else if (obj->dyn.needed_count > 0U ||
               obj->dyn.have_runpath ||
               obj->dyn.have_rpath ||
               obj->dyn.verdef ||
               obj->dyn.verneed) {
        return -1;
    }
    if (obj->dyn.versym && !obj->dyn.symtab) {
        return -1;
    }
    if (obj->dyn.textrel) {
        loader_fail_path("text relocations unsupported (W^X)", obj->path);
    }

    obj->dyn.phdr_addr = elf_phdr_runtime_addr(&obj->eh,
                                               (const Elf64_Phdr *)(obj->image + obj->eh.e_phoff),
                                               obj->load_bias,
                                               obj->image_size);

    ph = (const Elf64_Phdr *)(const void *)(obj->image + obj->eh.e_phoff);
    for (uint16_t i = 0; i < obj->eh.e_phnum; i++) {
        if (ph[i].p_type != PT_TLS || ph[i].p_memsz == 0U) {
            continue;
        }
        if (ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_offset > obj->image_size ||
            ph[i].p_filesz > obj->image_size - ph[i].p_offset) {
            return -1;
        }
        obj->has_tls = true;
        obj->tls_align = ph[i].p_align ? ph[i].p_align : 1U;
        obj->tls_memsz = ph[i].p_memsz;
        obj->tls_filesz = ph[i].p_filesz;
        obj->tls_init_off = ph[i].p_offset;
        break;
    }

    return 0;
}

static int dso_resolve_dependencies(dso_t *objs, int obj_count) {
    for (int i = 0; i < obj_count; i++) {
        dso_t *obj = &objs[i];
        obj->dep_count = 0U;

        for (uint32_t n = 0; n < obj->dyn.needed_count; n++) {
            const char *name = needed_name_at(obj, n);
            if (!name || name[0] == '\0') {
                return -1;
            }
            char dep_path[MAX_PATH];
            if (resolve_needed_path(obj, name, dep_path, sizeof(dep_path)) != 0) {
                return -1;
            }
            int dep_idx = dso_find_by_path(objs, obj_count, dep_path);
            if (dep_idx < 0) {
                return -1;
            }
            obj->dep_index[obj->dep_count++] = (int16_t)dep_idx;
        }
    }
    return 0;
}

static int dso_init_tls_runtime(dso_t *objs, int obj_count) {
    uint64_t tls_total = 0U;
    uint32_t next_modid = 1U;

    g_tls_block = 0;
    g_tls_block_size = 0U;

    for (int i = 0; i < obj_count; i++) {
        dso_t *obj = &objs[i];
        obj->tls_modid = 0U;
        obj->tls_offset = 0U;
        if (!obj->has_tls || obj->tls_memsz == 0U) {
            continue;
        }
        uint64_t align = obj->tls_align ? obj->tls_align : 1U;
        if (align & (align - 1U)) {
            return -1;
        }
        tls_total = align_up_u64(tls_total, align);
        obj->tls_offset = tls_total;
        obj->tls_modid = next_modid++;
        tls_total += obj->tls_memsz;
    }

    if (tls_total == 0U) {
        set_thread_pointer(0);
        return 0;
    }

    size_t map_len = (size_t)align_up_u64(tls_total, PAGE_SIZE);
    void *blk = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == (void *)-1) {
        return -1;
    }
    memset(blk, 0, map_len);

    for (int i = 0; i < obj_count; i++) {
        dso_t *obj = &objs[i];
        if (!obj->has_tls || obj->tls_memsz == 0U || obj->tls_filesz == 0U) {
            continue;
        }
        if (obj->tls_init_off > obj->image_size ||
            obj->tls_filesz > obj->image_size - obj->tls_init_off) {
            return -1;
        }
        memcpy((uint8_t *)blk + obj->tls_offset,
               obj->image + obj->tls_init_off,
               (size_t)obj->tls_filesz);
    }

    g_tls_block = blk;
    g_tls_block_size = map_len;
    set_thread_pointer(blk);
    return 0;
}

static int dso_build_init_order(dso_t *objs, int obj_count, int idx, uint8_t *state,
                                int *order, int *order_count) {
    if (idx < 0 || idx >= obj_count || !state || !order || !order_count) {
        return -1;
    }
    if (state[idx] == 2U) {
        return 0;
    }
    if (state[idx] == 1U) {
        return -1;
    }

    state[idx] = 1U;
    dso_t *obj = &objs[idx];
    for (uint8_t n = 0; n < obj->dep_count; n++) {
        int dep = obj->dep_index[n];
        if (dep < 0 || dep >= obj_count) {
            return -1;
        }
        if (dso_build_init_order(objs, obj_count, dep, state, order, order_count) != 0) {
            return -1;
        }
    }

    state[idx] = 2U;
    if (idx != 0) {
        if (*order_count >= MAX_DSO) {
            return -1;
        }
        order[*order_count] = idx;
        (*order_count)++;
    }
    return 0;
}

static const Elf64_Sym *dso_sym_at(const dso_t *obj, uint32_t sym_idx) {
    if (!obj || !obj->dyn.symtab || obj->dyn.syment < sizeof(Elf64_Sym)) {
        return 0;
    }
    if (obj->dyn.sym_count && sym_idx >= obj->dyn.sym_count) {
        return 0;
    }
    return (const Elf64_Sym *)(uintptr_t)(obj->dyn.symtab + (uint64_t)sym_idx * obj->dyn.syment);
}

static const char *dso_sym_name(const dso_t *obj, const Elf64_Sym *sym) {
    if (!obj || !sym || !obj->dyn.strtab || sym->st_name >= obj->dyn.strsz) {
        return "";
    }
    return (const char *)(uintptr_t)(obj->dyn.strtab + sym->st_name);
}

static uint64_t dso_sym_addr(const dso_t *obj, const Elf64_Sym *sym) {
    if (!obj || !sym) {
        return 0;
    }
    return sym->st_value + ((obj->eh.e_type == ET_DYN) ? obj->load_bias : 0U);
}

static uint16_t dso_sym_version_index(const dso_t *obj, uint32_t sym_idx, bool *hidden_out) {
    if (hidden_out) {
        *hidden_out = false;
    }
    if (!obj || !obj->dyn.versym) {
        return VER_NDX_GLOBAL;
    }
    if (obj->dyn.sym_count != 0U && sym_idx >= obj->dyn.sym_count) {
        return VER_NDX_GLOBAL;
    }
    const Elf64_Versym *vs = (const Elf64_Versym *)(uintptr_t)obj->dyn.versym;
    uint16_t raw = vs[sym_idx];
    if (hidden_out) {
        *hidden_out = (raw & VERSYM_HIDDEN) != 0U;
    }
    return (uint16_t)(raw & VERSYM_VERSION);
}

static const char *dso_verdef_name_by_index(const dso_t *obj, uint16_t ver_idx) {
    if (!obj || ver_idx <= VER_NDX_GLOBAL || !obj->dyn.verdef || obj->dyn.verdefnum == 0U ||
        !obj->dyn.strtab) {
        return 0;
    }
    uint64_t cur = obj->dyn.verdef;
    for (uint64_t i = 0; i < obj->dyn.verdefnum; i++) {
        const Elf64_Verdef *vd = (const Elf64_Verdef *)(uintptr_t)cur;
        if (vd->vd_ndx == ver_idx) {
            const Elf64_Verdaux *aux = (const Elf64_Verdaux *)(uintptr_t)(cur + vd->vd_aux);
            if ((uint64_t)aux->vda_name >= obj->dyn.strsz) {
                return 0;
            }
            return (const char *)(uintptr_t)(obj->dyn.strtab + aux->vda_name);
        }
        if (vd->vd_next == 0U) {
            break;
        }
        cur += vd->vd_next;
    }
    return 0;
}

static const char *dso_verneed_name_by_index(const dso_t *obj, uint16_t ver_idx) {
    if (!obj || ver_idx <= VER_NDX_GLOBAL || !obj->dyn.verneed || obj->dyn.verneednum == 0U ||
        !obj->dyn.strtab) {
        return 0;
    }
    uint64_t cur_need = obj->dyn.verneed;
    for (uint64_t i = 0; i < obj->dyn.verneednum; i++) {
        const Elf64_Verneed *vn = (const Elf64_Verneed *)(uintptr_t)cur_need;
        uint64_t cur_aux = cur_need + vn->vn_aux;
        for (uint16_t n = 0; n < vn->vn_cnt; n++) {
            const Elf64_Vernaux *vna = (const Elf64_Vernaux *)(uintptr_t)cur_aux;
            uint16_t other = (uint16_t)(vna->vna_other & VERSYM_VERSION);
            if (other == ver_idx) {
                if ((uint64_t)vna->vna_name >= obj->dyn.strsz) {
                    return 0;
                }
                return (const char *)(uintptr_t)(obj->dyn.strtab + vna->vna_name);
            }
            if (vna->vna_next == 0U) {
                break;
            }
            cur_aux += vna->vna_next;
        }
        if (vn->vn_next == 0U) {
            break;
        }
        cur_need += vn->vn_next;
    }
    return 0;
}

static const char *dso_required_version_name(const dso_t *obj, uint32_t sym_idx, bool *hidden_out) {
    uint16_t ver_idx = dso_sym_version_index(obj, sym_idx, hidden_out);
    if (ver_idx <= VER_NDX_GLOBAL) {
        return 0;
    }
    return dso_verneed_name_by_index(obj, ver_idx);
}

static int lookup_global_symbol(dso_t *objs, int obj_count, const char *name,
                                const char *req_version, bool req_hidden,
                                uint64_t *addr_out, size_t *size_out,
                                int *def_obj_idx_out, const Elf64_Sym **def_sym_out) {
    uint64_t weak_addr = 0;
    size_t weak_size = 0;
    int weak_obj_idx = -1;
    const Elf64_Sym *weak_sym = 0;
    bool have_weak = false;

    for (int i = 0; i < obj_count; i++) {
        dso_t *obj = &objs[i];
        if (!obj->dyn.symtab || !obj->dyn.strtab || obj->dyn.sym_count == 0U) {
            continue;
        }

        for (uint32_t s = 0; s < obj->dyn.sym_count; s++) {
            const Elf64_Sym *sym = dso_sym_at(obj, s);
            if (!sym || sym->st_shndx == SHN_UNDEF) {
                continue;
            }
            uint8_t bind = ELF64_ST_BIND(sym->st_info);
            if (bind == STB_LOCAL) {
                continue;
            }
            const char *sym_name = dso_sym_name(obj, sym);
            if (!sym_name || strcmp(sym_name, name) != 0) {
                continue;
            }

            bool provider_hidden = false;
            uint16_t provider_ver = dso_sym_version_index(obj, s, &provider_hidden);
            if (req_version && req_version[0] != '\0') {
                const char *prov_name = dso_verdef_name_by_index(obj, provider_ver);
                if (!prov_name || strcmp(prov_name, req_version) != 0) {
                    continue;
                }
                if (req_hidden && !provider_hidden) {
                    continue;
                }
            } else if (provider_hidden) {
                continue;
            }

            uint64_t addr = dso_sym_addr(obj, sym);
            size_t size = (size_t)sym->st_size;
            if (bind == STB_WEAK) {
                if (!have_weak) {
                    have_weak = true;
                    weak_addr = addr;
                    weak_size = size;
                    weak_obj_idx = i;
                    weak_sym = sym;
                }
                continue;
            }

            *addr_out = addr;
            *size_out = size;
            if (def_obj_idx_out) {
                *def_obj_idx_out = i;
            }
            if (def_sym_out) {
                *def_sym_out = sym;
            }
            return 0;
        }
    }

    if (have_weak) {
        *addr_out = weak_addr;
        *size_out = weak_size;
        if (def_obj_idx_out) {
            *def_obj_idx_out = weak_obj_idx;
        }
        if (def_sym_out) {
            *def_sym_out = weak_sym;
        }
        return 0;
    }
    return -1;
}

static int resolve_symbol_for_reloc(dso_t *objs, int obj_count, int obj_idx,
                                    uint32_t sym_idx, uint64_t *addr_out,
                                    size_t *size_out, const char **name_out,
                                    bool *undefined_weak_out,
                                    int *def_obj_idx_out,
                                    const Elf64_Sym **def_sym_out) {
    dso_t *obj = &objs[obj_idx];
    const Elf64_Sym *sym = dso_sym_at(obj, sym_idx);
    if (!sym) {
        return -1;
    }

    const char *name = dso_sym_name(obj, sym);
    if (name_out) {
        *name_out = name;
    }

    if (sym->st_shndx != SHN_UNDEF) {
        *addr_out = dso_sym_addr(obj, sym);
        *size_out = (size_t)sym->st_size;
        if (undefined_weak_out) {
            *undefined_weak_out = false;
        }
        if (def_obj_idx_out) {
            *def_obj_idx_out = obj_idx;
        }
        if (def_sym_out) {
            *def_sym_out = sym;
        }
        return 0;
    }

    uint8_t bind = ELF64_ST_BIND(sym->st_info);
    if (bind == STB_WEAK) {
        *addr_out = 0U;
        *size_out = (size_t)sym->st_size;
        if (undefined_weak_out) {
            *undefined_weak_out = true;
        }
        if (def_obj_idx_out) {
            *def_obj_idx_out = -1;
        }
        if (def_sym_out) {
            *def_sym_out = sym;
        }
        return 0;
    }

    if (!name || name[0] == '\0') {
        return -1;
    }

    bool req_hidden = false;
    const char *req_version = dso_required_version_name(obj, sym_idx, &req_hidden);
    if (lookup_global_symbol(objs, obj_count, name, req_version, req_hidden,
                             addr_out, size_out, def_obj_idx_out, def_sym_out) != 0) {
        return -1;
    }

    if (undefined_weak_out) {
        *undefined_weak_out = false;
    }
    return 0;
}

static int tls_symbol_modid(const dso_t *objs, int def_obj_idx, bool undef_weak,
                            uint64_t *modid_out) {
    if (!modid_out) {
        return -1;
    }
    if (undef_weak || def_obj_idx < 0) {
        *modid_out = 0U;
        return 0;
    }
    const dso_t *def_obj = &objs[def_obj_idx];
    if (!def_obj->has_tls || def_obj->tls_modid == 0U) {
        return -1;
    }
    *modid_out = def_obj->tls_modid;
    return 0;
}

static int tls_symbol_dtprel(const Elf64_Sym *def_sym, int64_t addend, bool undef_weak,
                             uint64_t *out) {
    if (!out) {
        return -1;
    }
    if (undef_weak || !def_sym) {
        *out = 0U;
        return 0;
    }
    *out = (uint64_t)((int64_t)def_sym->st_value + addend);
    return 0;
}

static int tls_symbol_tprel(const dso_t *objs, int def_obj_idx, const Elf64_Sym *def_sym,
                            int64_t addend, bool undef_weak, uint64_t *out) {
    if (!out) {
        return -1;
    }
    if (undef_weak || !def_sym || def_obj_idx < 0) {
        *out = 0U;
        return 0;
    }
    const dso_t *def_obj = &objs[def_obj_idx];
    if (!def_obj->has_tls || def_obj->tls_modid == 0U) {
        return -1;
    }
    *out = (uint64_t)((int64_t)def_sym->st_value + addend + (int64_t)def_obj->tls_offset);
    return 0;
}

static int apply_rela_table(dso_t *objs, int obj_count, int obj_idx,
                            uint64_t rela_addr, uint64_t rela_size, uint64_t rela_ent) {
    dso_t *obj = &objs[obj_idx];
    if (rela_size == 0U) {
        return 0;
    }
    if (rela_ent < sizeof(Elf64_Rela) || (rela_size % rela_ent) != 0U) {
        return -1;
    }

    for (uint64_t off = 0; off < rela_size; off += rela_ent) {
        const Elf64_Rela *rela = (const Elf64_Rela *)(uintptr_t)(rela_addr + off);
        uint64_t reloc_addr = (obj->eh.e_type == ET_DYN) ?
                              (obj->load_bias + rela->r_offset) :
                              rela->r_offset;
        uint64_t *where = (uint64_t *)(uintptr_t)reloc_addr;
        uint32_t rtype = ELF64_R_TYPE(rela->r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela->r_info);

        switch (rtype) {
            case R_AARCH64_NONE:
                break;
            case R_AARCH64_RELATIVE:
                *where = ((obj->eh.e_type == ET_DYN) ? obj->load_bias : 0U) +
                         (uint64_t)rela->r_addend;
                break;
            case R_AARCH64_IRELATIVE: {
                uint64_t resolver_addr = ((obj->eh.e_type == ET_DYN) ? obj->load_bias : 0U) +
                                         (uint64_t)rela->r_addend;
                uint64_t (*resolver)(void) = (uint64_t (*)(void))(uintptr_t)resolver_addr;
                *where = resolver();
                break;
            }
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_size;
                (void)undef_weak;
                (void)def_obj_idx;
                (void)def_sym;
                *where = sym_addr + (uint64_t)rela->r_addend;
                break;
            }
            case R_AARCH64_TLS_DTPMOD64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t modid = 0U;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                (void)def_sym;
                if (tls_symbol_modid(objs, def_obj_idx, undef_weak, &modid) != 0) {
                    return -1;
                }
                *where = modid;
                break;
            }
            case R_AARCH64_TLS_DTPREL64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t dtprel = 0U;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                (void)def_obj_idx;
                if (tls_symbol_dtprel(def_sym, rela->r_addend, undef_weak, &dtprel) != 0) {
                    return -1;
                }
                *where = dtprel;
                break;
            }
            case R_AARCH64_TLS_TPREL64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t tprel = 0U;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                if (tls_symbol_tprel(objs, def_obj_idx, def_sym, rela->r_addend,
                                     undef_weak, &tprel) != 0) {
                    return -1;
                }
                *where = tprel;
                break;
            }
            case R_AARCH64_TLSDESC: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t tprel = 0U;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                if (tls_symbol_tprel(objs, def_obj_idx, def_sym, rela->r_addend,
                                     undef_weak, &tprel) != 0) {
                    return -1;
                }
                tlsdesc_t *td = (tlsdesc_t *)(uintptr_t)reloc_addr;
                td->arg = tprel;
                td->entry = (uint64_t)(uintptr_t)&tlsdesc_return;
                break;
            }
            case R_AARCH64_ABS32:
            case R_AARCH64_ABS16:
            case R_AARCH64_PREL64:
            case R_AARCH64_PREL32:
            case R_AARCH64_PREL16: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_size;
                (void)def_obj_idx;
                (void)def_sym;
                uint64_t abs_val = sym_addr + (uint64_t)rela->r_addend;
                int64_t rel_val = (int64_t)abs_val - (int64_t)reloc_addr;
                if (rtype == R_AARCH64_ABS32) {
                    *(uint32_t *)(uintptr_t)where = (uint32_t)abs_val;
                } else if (rtype == R_AARCH64_ABS16) {
                    *(uint16_t *)(uintptr_t)where = (uint16_t)abs_val;
                } else if (rtype == R_AARCH64_PREL64) {
                    *(uint64_t *)(uintptr_t)where = (uint64_t)rel_val;
                } else if (rtype == R_AARCH64_PREL32) {
                    *(uint32_t *)(uintptr_t)where = (uint32_t)rel_val;
                } else {
                    *(uint16_t *)(uintptr_t)where = (uint16_t)rel_val;
                }
                (void)undef_weak;
                break;
            }
            case R_AARCH64_COPY: {
                uint64_t src_addr = 0;
                size_t src_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &src_addr, &src_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved COPY symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)def_obj_idx;
                (void)def_sym;
                if (!undef_weak && src_addr != 0U) {
                    const Elf64_Sym *dst_sym = dso_sym_at(obj, sym_idx);
                    size_t copy_len = dst_sym ? (size_t)dst_sym->st_size : 0U;
                    if (copy_len == 0U) {
                        copy_len = src_size;
                    }
                    if (copy_len != 0U) {
                        memcpy((void *)(uintptr_t)where, (const void *)(uintptr_t)src_addr, copy_len);
                    }
                }
                break;
            }
            default:
                write_str2("ld-furios: unsupported relocation type\n");
                return -1;
        }
    }

    return 0;
}

static int apply_rel_table(dso_t *objs, int obj_count, int obj_idx,
                           uint64_t rel_addr, uint64_t rel_size, uint64_t rel_ent) {
    dso_t *obj = &objs[obj_idx];
    if (rel_size == 0U) {
        return 0;
    }
    if (rel_ent < sizeof(Elf64_Rel) || (rel_size % rel_ent) != 0U) {
        return -1;
    }

    uint64_t base = (obj->eh.e_type == ET_DYN) ? obj->load_bias : 0U;
    for (uint64_t off = 0; off < rel_size; off += rel_ent) {
        const Elf64_Rel *rel = (const Elf64_Rel *)(uintptr_t)(rel_addr + off);
        uint64_t reloc_addr = (obj->eh.e_type == ET_DYN) ?
                              (obj->load_bias + rel->r_offset) :
                              rel->r_offset;
        uint64_t *where64 = (uint64_t *)(uintptr_t)reloc_addr;
        uint32_t rtype = ELF64_R_TYPE(rel->r_info);
        uint32_t sym_idx = ELF64_R_SYM(rel->r_info);

        switch (rtype) {
            case R_AARCH64_NONE:
                break;
            case R_AARCH64_RELATIVE:
                *where64 = base + *where64;
                break;
            case R_AARCH64_IRELATIVE: {
                uint64_t resolver_addr = base + *where64;
                uint64_t (*resolver)(void) = (uint64_t (*)(void))(uintptr_t)resolver_addr;
                *where64 = resolver();
                break;
            }
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_size;
                (void)undef_weak;
                (void)def_obj_idx;
                (void)def_sym;
                *where64 = sym_addr + *where64;
                break;
            }
            case R_AARCH64_TLS_DTPMOD64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t modid = 0U;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                (void)def_sym;
                if (tls_symbol_modid(objs, def_obj_idx, undef_weak, &modid) != 0) {
                    return -1;
                }
                *where64 = modid;
                break;
            }
            case R_AARCH64_TLS_DTPREL64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t dtprel = 0U;
                int64_t add = *(int64_t *)(uintptr_t)where64;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                (void)def_obj_idx;
                if (tls_symbol_dtprel(def_sym, add, undef_weak, &dtprel) != 0) {
                    return -1;
                }
                *where64 = dtprel;
                break;
            }
            case R_AARCH64_TLS_TPREL64: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t tprel = 0U;
                int64_t add = *(int64_t *)(uintptr_t)where64;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                if (tls_symbol_tprel(objs, def_obj_idx, def_sym, add,
                                     undef_weak, &tprel) != 0) {
                    return -1;
                }
                *where64 = tprel;
                break;
            }
            case R_AARCH64_TLSDESC: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                uint64_t tprel = 0U;
                tlsdesc_t *td = (tlsdesc_t *)(uintptr_t)reloc_addr;
                int64_t add = (int64_t)td->arg;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved TLS symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_addr;
                (void)sym_size;
                if (tls_symbol_tprel(objs, def_obj_idx, def_sym, add,
                                     undef_weak, &tprel) != 0) {
                    return -1;
                }
                td->arg = tprel;
                td->entry = (uint64_t)(uintptr_t)&tlsdesc_return;
                break;
            }
            case R_AARCH64_ABS32:
            case R_AARCH64_ABS16:
            case R_AARCH64_PREL64:
            case R_AARCH64_PREL32:
            case R_AARCH64_PREL16: {
                uint64_t sym_addr = 0;
                size_t sym_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &sym_addr, &sym_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)sym_size;
                (void)undef_weak;
                (void)def_obj_idx;
                (void)def_sym;

                if (rtype == R_AARCH64_ABS32) {
                    uint32_t add = *(uint32_t *)(uintptr_t)reloc_addr;
                    *(uint32_t *)(uintptr_t)reloc_addr = (uint32_t)(sym_addr + add);
                } else if (rtype == R_AARCH64_ABS16) {
                    uint16_t add = *(uint16_t *)(uintptr_t)reloc_addr;
                    *(uint16_t *)(uintptr_t)reloc_addr = (uint16_t)(sym_addr + add);
                } else if (rtype == R_AARCH64_PREL64) {
                    int64_t add = *(int64_t *)(uintptr_t)reloc_addr;
                    int64_t rel_val = ((int64_t)sym_addr + add) - (int64_t)reloc_addr;
                    *(uint64_t *)(uintptr_t)reloc_addr = (uint64_t)rel_val;
                } else if (rtype == R_AARCH64_PREL32) {
                    int32_t add = *(int32_t *)(uintptr_t)reloc_addr;
                    int64_t rel_val = ((int64_t)sym_addr + (int64_t)add) - (int64_t)reloc_addr;
                    *(uint32_t *)(uintptr_t)reloc_addr = (uint32_t)rel_val;
                } else {
                    int16_t add = *(int16_t *)(uintptr_t)reloc_addr;
                    int64_t rel_val = ((int64_t)sym_addr + (int64_t)add) - (int64_t)reloc_addr;
                    *(uint16_t *)(uintptr_t)reloc_addr = (uint16_t)rel_val;
                }
                break;
            }
            case R_AARCH64_COPY: {
                uint64_t src_addr = 0;
                size_t src_size = 0;
                const char *sym_name = "";
                bool undef_weak = false;
                int def_obj_idx = -1;
                const Elf64_Sym *def_sym = 0;
                if (resolve_symbol_for_reloc(objs, obj_count, obj_idx, sym_idx,
                                             &src_addr, &src_size, &sym_name,
                                             &undef_weak, &def_obj_idx,
                                             &def_sym) != 0) {
                    write_str2("ld-furios: unresolved COPY symbol: ");
                    write_str2(sym_name);
                    write_str2("\n");
                    return -1;
                }
                (void)def_obj_idx;
                (void)def_sym;
                if (!undef_weak && src_addr != 0U) {
                    const Elf64_Sym *dst_sym = dso_sym_at(obj, sym_idx);
                    size_t copy_len = dst_sym ? (size_t)dst_sym->st_size : 0U;
                    if (copy_len == 0U) {
                        copy_len = src_size;
                    }
                    if (copy_len != 0U) {
                        memcpy((void *)(uintptr_t)where64,
                               (const void *)(uintptr_t)src_addr, copy_len);
                    }
                }
                break;
            }
            default:
                write_str2("ld-furios: unsupported relocation type\n");
                return -1;
        }
    }
    return 0;
}

static void run_init_for_dso(const dso_t *obj) {
    if (!obj) {
        return;
    }
    if (obj->dyn.init) {
        void (*fn)(void) = (void (*)(void))(uintptr_t)obj->dyn.init;
        fn();
    }
    if (obj->dyn.init_array && obj->dyn.init_arraysz >= sizeof(uint64_t)) {
        uint64_t *arr = (uint64_t *)(uintptr_t)obj->dyn.init_array;
        size_t cnt = (size_t)(obj->dyn.init_arraysz / sizeof(uint64_t));
        for (size_t i = 0; i < cnt; i++) {
            if (arr[i]) {
                void (*fn)(void) = (void (*)(void))(uintptr_t)arr[i];
                fn();
            }
        }
    }
}

static void run_fini_for_dso(const dso_t *obj) {
    if (!obj) {
        return;
    }
    if (obj->dyn.fini_array && obj->dyn.fini_arraysz >= sizeof(uint64_t)) {
        uint64_t *arr = (uint64_t *)(uintptr_t)obj->dyn.fini_array;
        size_t cnt = (size_t)(obj->dyn.fini_arraysz / sizeof(uint64_t));
        for (size_t i = cnt; i > 0; i--) {
            uint64_t v = arr[i - 1U];
            if (v) {
                void (*fn)(void) = (void (*)(void))(uintptr_t)v;
                fn();
            }
        }
    }
    if (obj->dyn.fini) {
        void (*fn)(void) = (void (*)(void))(uintptr_t)obj->dyn.fini;
        fn();
    }
}

static int call_entry(uint64_t entry, int argc, char **argv, char **envp, Elf64_auxv_t *auxv) {
    register uint64_t x0 __asm__("x0") = (uint64_t)argc;
    register char **x1 __asm__("x1") = argv;
    register char **x2 __asm__("x2") = envp;
    register Elf64_auxv_t *x3 __asm__("x3") = auxv;
    register uint64_t x16 __asm__("x16") = entry;
    __asm__ volatile("blr x16"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x16)
                     : "memory", "x30");
    return (int)x0;
}

static Elf64_auxv_t *auxv_from_envp(char **envp) {
    if (!envp) {
        return 0;
    }
    while (*envp) {
        envp++;
    }
    return (Elf64_auxv_t *)(void *)(envp + 1);
}

static uint64_t auxv_lookup(const Elf64_auxv_t *auxv, uint64_t key, uint64_t fallback) {
    if (!auxv) {
        return fallback;
    }
    for (const Elf64_auxv_t *it = auxv; it->a_type != AT_NULL; it++) {
        if (it->a_type == key) {
            return it->a_val;
        }
    }
    return fallback;
}

static bool auxv_overridden_key(uint64_t key) {
    switch (key) {
        case AT_PHDR:
        case AT_PHENT:
        case AT_PHNUM:
        case AT_PAGESZ:
        case AT_BASE:
        case AT_ENTRY:
        case AT_UID:
        case AT_EUID:
        case AT_GID:
        case AT_EGID:
        case AT_SECURE:
        case AT_EXECFN:
            return true;
        default:
            return false;
    }
}

static void auxv_push(Elf64_auxv_t *auxv, size_t *count, uint64_t key, uint64_t val) {
    if (!auxv || !count || *count >= AUXV_MAX) {
        loader_fail("auxv overflow");
    }
    auxv[*count].a_type = key;
    auxv[*count].a_val = val;
    (*count)++;
}

static void auxv_copy_passthrough(Elf64_auxv_t *dst, size_t *count, const Elf64_auxv_t *src) {
    if (!dst || !count || !src) {
        return;
    }
    for (const Elf64_auxv_t *it = src; it->a_type != AT_NULL; it++) {
        if (auxv_overridden_key(it->a_type)) {
            continue;
        }
        auxv_push(dst, count, it->a_type, it->a_val);
    }
}

int main(int argc, char **argv, char **envp) {
    if (argc < 2 || !argv || !argv[1]) {
        loader_fail("usage: /lib/ld-furios.so <program> [argv0 ...]");
    }

    char *empty_env[1] = {0};
    char **pass_env = envp ? envp : empty_env;
    g_ld_library_path = env_lookup(pass_env, "LD_LIBRARY_PATH");

    const char *prog = argv[1];
    char *fallback_argv[2];
    int prog_argc;
    char **prog_argv;

    if (argc > 2 && argv[2]) {
        prog_argc = argc - 2;
        prog_argv = &argv[2];
    } else {
        fallback_argv[0] = (char *)prog;
        fallback_argv[1] = 0;
        prog_argc = 1;
        prog_argv = fallback_argv;
    }

    dso_t *objs = g_loader_objs;
    int obj_count = 0;
    memset(objs, 0, sizeof(g_loader_objs));

    uint64_t next_dyn_base = DYN_LOAD_BASE;

    if (dso_load(&objs[obj_count], prog, next_dyn_base, true) != 0) {
        loader_fail_path("failed to load target", prog);
    }
    if (objs[obj_count].map_end > next_dyn_base) {
        next_dyn_base = align_up_u64(objs[obj_count].map_end + PAGE_SIZE, PAGE_SIZE);
    }
    obj_count++;

    for (int scan = 0; scan < obj_count; scan++) {
        dso_t *cur = &objs[scan];
        for (uint32_t n = 0; n < cur->dyn.needed_count; n++) {
            const char *name = (const char *)(uintptr_t)(cur->dyn.strtab + cur->dyn.needed_off[n]);
            char dep_path[MAX_PATH];
            if (resolve_needed_path(cur, name, dep_path, sizeof(dep_path)) != 0) {
                loader_fail_path("DT_NEEDED not found", name);
            }

            if (dso_find_by_path(objs, obj_count, dep_path) >= 0) {
                continue;
            }
            if (obj_count >= MAX_DSO) {
                loader_fail("too many shared objects");
            }
            if (next_dyn_base < DYN_LOAD_BASE || next_dyn_base >= DYN_LOAD_LIMIT) {
                loader_fail("shared object VA space exhausted");
            }

            if (dso_load(&objs[obj_count], dep_path, next_dyn_base, false) != 0) {
                loader_fail_path("failed to load shared object", dep_path);
            }
            next_dyn_base = align_up_u64(objs[obj_count].map_end + PAGE_SIZE, PAGE_SIZE);
            if (next_dyn_base >= DYN_LOAD_LIMIT) {
                loader_fail("shared object VA space exhausted");
            }
            obj_count++;
        }
    }

    if (dso_resolve_dependencies(objs, obj_count) != 0) {
        loader_fail("failed to resolve DT_NEEDED dependency graph");
    }
    if (dso_init_tls_runtime(objs, obj_count) != 0) {
        loader_fail("failed to initialize static TLS runtime");
    }

    for (int i = 0; i < obj_count; i++) {
        dso_t *obj = &objs[i];
        if (obj->dyn.rela && obj->dyn.relasz) {
            if (apply_rela_table(objs, obj_count, i,
                                 obj->dyn.rela, obj->dyn.relasz, obj->dyn.relaent) != 0) {
                loader_fail_path("RELA relocation failed", obj->path);
            }
        }
        if (obj->dyn.rel && obj->dyn.relsz) {
            if (apply_rel_table(objs, obj_count, i,
                                obj->dyn.rel, obj->dyn.relsz, obj->dyn.relent) != 0) {
                loader_fail_path("REL relocation failed", obj->path);
            }
        }
        if (obj->dyn.jmprel && obj->dyn.pltrelsz) {
            if (obj->dyn.pltrel == DT_RELA) {
                if (apply_rela_table(objs, obj_count, i,
                                     obj->dyn.jmprel, obj->dyn.pltrelsz, obj->dyn.relaent) != 0) {
                    loader_fail_path("PLT RELA relocation failed", obj->path);
                }
            } else if (obj->dyn.pltrel == DT_REL) {
                if (apply_rel_table(objs, obj_count, i,
                                    obj->dyn.jmprel, obj->dyn.pltrelsz, obj->dyn.relent) != 0) {
                    loader_fail_path("PLT REL relocation failed", obj->path);
                }
            } else {
                loader_fail_path("unsupported PLT relocation encoding", obj->path);
            }
        }
    }

    for (int i = 0; i < obj_count; i++) {
        if (objs[i].relro_addr && objs[i].relro_size) {
            if (mprotect((void *)(uintptr_t)objs[i].relro_addr,
                         (unsigned long)objs[i].relro_size,
                         PROT_READ) != 0) {
                loader_fail_path("PT_GNU_RELRO protect failed", objs[i].path);
            }
        }
    }

    int init_order[MAX_DSO];
    int init_count = 0;
    uint8_t init_state[MAX_DSO];
    memset(init_order, 0, sizeof(init_order));
    memset(init_state, 0, sizeof(init_state));
    if (dso_build_init_order(objs, obj_count, 0, init_state, init_order, &init_count) != 0) {
        loader_fail("failed to build dependency init order");
    }
    for (int i = 0; i < obj_count; i++) {
        if (init_state[i] == 0U) {
            if (dso_build_init_order(objs, obj_count, i, init_state, init_order, &init_count) != 0) {
                loader_fail("failed to finalize init order");
            }
        }
    }

    for (int i = 0; i < init_count; i++) {
        run_init_for_dso(&objs[init_order[i]]);
    }

    dso_t *main_obj = &objs[0];
    uint64_t entry_addr = elf_addr(&main_obj->eh, main_obj->load_bias, main_obj->eh.e_entry);

    Elf64_auxv_t *in_auxv = auxv_from_envp(pass_env);
    uint64_t at_base = align_down_u64((uint64_t)(uintptr_t)&main, PAGE_SIZE);
    uint64_t at_uid = auxv_lookup(in_auxv, AT_UID, 0U);
    uint64_t at_euid = auxv_lookup(in_auxv, AT_EUID, at_uid);
    uint64_t at_gid = auxv_lookup(in_auxv, AT_GID, 0U);
    uint64_t at_egid = auxv_lookup(in_auxv, AT_EGID, at_gid);
    uint64_t at_secure = auxv_lookup(in_auxv, AT_SECURE, 0U);

    Elf64_auxv_t auxv[AUXV_MAX];
    size_t auxc = 0;
    auxv_copy_passthrough(auxv, &auxc, in_auxv);
    auxv_push(auxv, &auxc, AT_PHDR, main_obj->dyn.phdr_addr);
    auxv_push(auxv, &auxc, AT_PHENT, main_obj->eh.e_phentsize);
    auxv_push(auxv, &auxc, AT_PHNUM, main_obj->eh.e_phnum);
    auxv_push(auxv, &auxc, AT_PAGESZ, PAGE_SIZE);
    auxv_push(auxv, &auxc, AT_BASE, at_base);
    auxv_push(auxv, &auxc, AT_ENTRY, entry_addr);
    auxv_push(auxv, &auxc, AT_UID, at_uid);
    auxv_push(auxv, &auxc, AT_EUID, at_euid);
    auxv_push(auxv, &auxc, AT_GID, at_gid);
    auxv_push(auxv, &auxc, AT_EGID, at_egid);
    auxv_push(auxv, &auxc, AT_SECURE, at_secure);
    auxv_push(auxv, &auxc, AT_EXECFN, (uint64_t)(uintptr_t)prog);
    auxv_push(auxv, &auxc, AT_NULL, 0U);

    int rc = call_entry(entry_addr, prog_argc, prog_argv, pass_env, auxv);

    for (int i = init_count - 1; i >= 0; i--) {
        run_fini_for_dso(&objs[init_order[i]]);
    }

    sys_exit(rc);
    return rc;
}
