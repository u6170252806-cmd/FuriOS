#include "user.h"

static void path_join(const char *base, const char *name, char *out, int out_len) {
    int i = 0;
    while (base[i] && i < out_len - 1) {
        out[i] = base[i];
        i++;
    }
    if (i > 0 && out[i - 1] != '/' && i < out_len - 1) {
        out[i++] = '/';
    }
    int j = 0;
    while (name[j] && i < out_len - 1) {
        out[i++] = name[j++];
    }
    out[i] = '\0';
}

static int rm_path(const char *path, int recursive, int force) {
    if (strcmp(path, "/") == 0) {
        if (!force) {
            puts("rm: refusing to remove /\n");
        }
        return -1;
    }

    if (sys_unlink(path) == 0) {
        return 0;
    }

    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        if (!force) {
            puts("rm: cannot remove ");
            puts(path);
            puts("\n");
        }
        return -1;
    }

    if (!recursive) {
        sys_close(fd);
        if (!force) {
            puts("rm: is a directory ");
            puts(path);
            puts("\n");
        }
        return -1;
    }

    dirent_t ents[64];
    int ent_count = 0;
    int rc = 0;
    while (ent_count < (int)(sizeof(ents) / sizeof(ents[0]))) {
        long n = sys_read(fd, &ents[ent_count], sizeof(ents[ent_count]));
        if (n == 0) {
            break;
        }
        if (n != (long)sizeof(ents[ent_count])) {
            rc = -1;
            break;
        }
        ent_count++;
    }
    sys_close(fd);

    for (int i = 0; i < ent_count; i++) {
        dirent_t *ent = &ents[i];
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }

        char child[128];
        path_join(path, ent->name, child, (int)sizeof(child));

        if (ent->type == DIRENT_DIR) {
            if (rm_path(child, recursive, force) != 0 && !force) {
                rc = -1;
            }
        } else {
            if (sys_unlink(child) != 0 && !force) {
                puts("rm: cannot remove ");
                puts(child);
                puts("\n");
                rc = -1;
            }
        }
    }

    if (sys_rmdir(path) != 0) {
        if (!force) {
            puts("rm: cannot remove ");
            puts(path);
            puts("\n");
        }
        return -1;
    }

    return rc;
}

int main(int argc, char **argv) {
    int recursive = 0;
    int force = 0;
    int start = 1;

    while (start < argc && argv[start][0] == '-') {
        if (strcmp(argv[start], "--") == 0) {
            start++;
            break;
        }
        for (int i = 1; argv[start][i]; i++) {
            if (argv[start][i] == 'r' || argv[start][i] == 'R') {
                recursive = 1;
            } else if (argv[start][i] == 'f') {
                force = 1;
            } else {
                puts("usage: rm [-r] [-f] <path>...\n");
                return 1;
            }
        }
        start++;
    }

    if (start >= argc) {
        puts("usage: rm [-r] [-f] <path>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        if (rm_path(argv[i], recursive, force) != 0 && !force) {
            rc = 1;
        }
    }
    return rc;
}
