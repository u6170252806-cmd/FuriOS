#include "user.h"
#include "elf.h"

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} fu_elf64_shdr_t;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} fu_elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} fu_elf64_rela_t;

typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} fu_elf64_dyn_t;

#define FU_SHT_SYMTAB 2U
#define FU_SHT_STRTAB 3U
#define FU_SHT_RELA 4U
#define FU_SHT_DYNSYM 11U

static int read_fully(int fd, void *buf, unsigned long len) {
    unsigned long done = 0;
    while (done < len) {
        long n = read(fd, (char *)buf + done, len - done);
        if (n <= 0) {
            return -1;
        }
        done += (unsigned long)n;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        puts("usage: elfinfo <path>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        puts("elfinfo: open failed\n");
        return 1;
    }

    fu_stat_t st;
    if (fstat(fd, &st) == 0) {
        printf("size=%lu mode=0%o\n",
               (unsigned long)st.size,
               (unsigned)(st.mode & 07777U));
    }

    Elf64_Ehdr eh;
    if (read_fully(fd, &eh, sizeof(eh)) != 0) {
        puts("elfinfo: short read ehdr\n");
        close(fd);
        return 1;
    }

    printf("type=%u machine=%u class=%u entry=0x%lx phoff=%lu phnum=%u phentsz=%u\n",
           (unsigned)eh.e_type,
           (unsigned)eh.e_machine,
           (unsigned)eh.e_ident[EI_CLASS],
           (unsigned long)eh.e_entry,
           (unsigned long)eh.e_phoff,
           (unsigned)eh.e_phnum,
           (unsigned)eh.e_phentsize);
    printf("shoff=%lu shnum=%u shentsz=%u\n",
           (unsigned long)eh.e_shoff,
           (unsigned)eh.e_shnum,
           (unsigned)eh.e_shentsize);

    if (eh.e_phentsize != sizeof(Elf64_Phdr)) {
        puts("elfinfo: unexpected phdr size\n");
        close(fd);
        return 1;
    }

    if (lseek(fd, (long)eh.e_phoff, SEEK_SET) < 0) {
        puts("elfinfo: seek phoff failed\n");
        close(fd);
        return 1;
    }

    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        if (read_fully(fd, &ph, sizeof(ph)) != 0) {
            puts("elfinfo: short read phdr\n");
            close(fd);
            return 1;
        }
        printf("ph[%u]: type=%u flags=%u off=0x%lx vaddr=0x%lx filesz=0x%lx memsz=0x%lx align=0x%lx\n",
               i,
               (unsigned)ph.p_type,
               (unsigned)ph.p_flags,
               (unsigned long)ph.p_offset,
               (unsigned long)ph.p_vaddr,
               (unsigned long)ph.p_filesz,
               (unsigned long)ph.p_memsz,
               (unsigned long)ph.p_align);

        if (ph.p_type == PT_DYNAMIC && ph.p_filesz > 0 &&
            ph.p_filesz <= 4096 && (ph.p_filesz % sizeof(fu_elf64_dyn_t)) == 0) {
            if (lseek(fd, (long)ph.p_offset, SEEK_SET) >= 0) {
                unsigned long n = (unsigned long)(ph.p_filesz / sizeof(fu_elf64_dyn_t));
                printf("dynamic[%u]: count=%lu\n", i, n);
                for (unsigned long di = 0; di < n; di++) {
                    fu_elf64_dyn_t d;
                    if (read_fully(fd, &d, sizeof(d)) != 0) {
                        break;
                    }
                    if (d.d_tag == DT_NULL ||
                        d.d_tag == DT_RELA ||
                        d.d_tag == DT_RELASZ ||
                        d.d_tag == DT_RELAENT ||
                        d.d_tag == DT_REL ||
                        d.d_tag == DT_RELSZ ||
                        d.d_tag == DT_RELENT ||
                        d.d_tag == DT_JMPREL ||
                        d.d_tag == DT_PLTREL ||
                        d.d_tag == DT_PLTRELSZ ||
                        d.d_tag == DT_SYMTAB ||
                        d.d_tag == DT_STRTAB ||
                        d.d_tag == DT_HASH ||
                        d.d_tag == DT_GNU_HASH) {
                        printf("  dyn[%lu]: tag=%ld val=0x%lx\n",
                               di, (long)d.d_tag, (unsigned long)d.d_un.d_val);
                    }
                    if (d.d_tag == DT_NULL) {
                        break;
                    }
                }
            }
            if (lseek(fd, (long)eh.e_phoff + (long)((i + 1U) * sizeof(Elf64_Phdr)), SEEK_SET) < 0) {
                puts("elfinfo: seek after dynamic failed\n");
                close(fd);
                return 1;
            }
        }
    }

    if (eh.e_shentsize == sizeof(fu_elf64_shdr_t) && eh.e_shoff > 0 && eh.e_shnum > 0) {
        fu_elf64_shdr_t shdrs[128];
        if (eh.e_shnum > (sizeof(shdrs) / sizeof(shdrs[0]))) {
            puts("elfinfo: too many sections\n");
            close(fd);
            return 1;
        }
        if (lseek(fd, (long)eh.e_shoff, SEEK_SET) < 0) {
            puts("elfinfo: seek shoff failed\n");
            close(fd);
            return 1;
        }
        for (unsigned i = 0; i < eh.e_shnum; i++) {
            fu_elf64_shdr_t sh;
            if (read_fully(fd, &sh, sizeof(sh)) != 0) {
                puts("elfinfo: short read shdr\n");
                close(fd);
                return 1;
            }
            shdrs[i] = sh;
            printf("sh[%u]: type=%u off=0x%lx size=0x%lx addr=0x%lx flags=0x%lx align=0x%lx\n",
                   i,
                   (unsigned)sh.sh_type,
                   (unsigned long)sh.sh_offset,
                   (unsigned long)sh.sh_size,
                   (unsigned long)sh.sh_addr,
                   (unsigned long)sh.sh_flags,
                   (unsigned long)sh.sh_addralign);
        }

        for (unsigned i = 0; i < eh.e_shnum; i++) {
            fu_elf64_shdr_t *symsh = &shdrs[i];
            if (symsh->sh_type != FU_SHT_DYNSYM && symsh->sh_type != FU_SHT_SYMTAB) {
                continue;
            }
            if (symsh->sh_entsize != sizeof(fu_elf64_sym_t) || symsh->sh_link >= eh.e_shnum) {
                continue;
            }
            fu_elf64_shdr_t *strsh = &shdrs[symsh->sh_link];
            if (strsh->sh_type != FU_SHT_STRTAB || strsh->sh_size == 0) {
                continue;
            }
            if (strsh->sh_size > 8192 || symsh->sh_size > 32768) {
                continue;
            }
            char strtab[8192];
            if (lseek(fd, (long)strsh->sh_offset, SEEK_SET) < 0 ||
                read_fully(fd, strtab, (unsigned long)strsh->sh_size) != 0) {
                continue;
            }
            unsigned long nsyms = (unsigned long)(symsh->sh_size / sizeof(fu_elf64_sym_t));
            if (lseek(fd, (long)symsh->sh_offset, SEEK_SET) < 0) {
                continue;
            }
            printf("symbols[%u]: count=%lu kind=%s\n",
                   i, nsyms, symsh->sh_type == FU_SHT_DYNSYM ? "dynsym" : "symtab");
            for (unsigned long si = 0; si < nsyms; si++) {
                fu_elf64_sym_t s;
                if (read_fully(fd, &s, sizeof(s)) != 0) {
                    break;
                }
                unsigned st_type = ELF64_ST_TYPE(s.st_info);
                if (st_type != STT_TLS) {
                    continue;
                }
                const char *name = "<bad>";
                if (s.st_name < strsh->sh_size) {
                    name = &strtab[s.st_name];
                }
                printf("  sym[%lu]: name=%s type=%u bind=%u shndx=%u value=0x%lx size=0x%lx\n",
                       si, name, st_type, (unsigned)ELF64_ST_BIND(s.st_info),
                       (unsigned)s.st_shndx, (unsigned long)s.st_value, (unsigned long)s.st_size);
            }
        }

        for (unsigned i = 0; i < eh.e_shnum; i++) {
            fu_elf64_shdr_t *relsh = &shdrs[i];
            if (relsh->sh_type != FU_SHT_RELA ||
                relsh->sh_entsize != sizeof(fu_elf64_rela_t) ||
                relsh->sh_link >= eh.e_shnum) {
                continue;
            }
            unsigned long nrel = (unsigned long)(relsh->sh_size / sizeof(fu_elf64_rela_t));
            if (nrel > 128 || relsh->sh_size > 16384) {
                continue;
            }
            printf("rela[%u]: count=%lu link=%u info=%u\n",
                   i, nrel, (unsigned)relsh->sh_link, (unsigned)relsh->sh_info);
            if (lseek(fd, (long)relsh->sh_offset, SEEK_SET) < 0) {
                continue;
            }
            for (unsigned long ri = 0; ri < nrel; ri++) {
                fu_elf64_rela_t r;
                if (read_fully(fd, &r, sizeof(r)) != 0) {
                    break;
                }
                printf("  rel[%lu]: off=0x%lx type=%u sym=%u add=0x%lx\n",
                       ri,
                       (unsigned long)r.r_offset,
                       (unsigned)ELF64_R_TYPE(r.r_info),
                       (unsigned)ELF64_R_SYM(r.r_info),
                       (unsigned long)r.r_addend);
            }
        }
    }

    close(fd);
    return 0;
}
