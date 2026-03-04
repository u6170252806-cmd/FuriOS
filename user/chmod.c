#include "user.h"

static int parse_mode_octal(const char *s, uint32_t *mode_out) {
    uint32_t mode = 0U;
    if (!s || !*s || !mode_out) {
        return -1;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7') {
            return -1;
        }
        mode = (mode << 3) | (uint32_t)(*p - '0');
        if (mode > 07777U) {
            return -1;
        }
    }
    *mode_out = mode;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        puts("usage: chmod <mode-octal> <path>...\n");
        return 1;
    }

    uint32_t mode = 0U;
    if (parse_mode_octal(argv[1], &mode) != 0) {
        puts("chmod: invalid mode\n");
        return 1;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (sys_chmod(argv[i], mode) != 0) {
            puts("chmod: failed ");
            puts(argv[i]);
            putc('\n');
            rc = 1;
        }
    }
    return rc;
}
