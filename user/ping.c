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
        v = v * 10U + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        puts("usage: ping <host> [count]\n");
        return 1;
    }

    uint32_t count = 4U;
    if (argc == 3 && parse_u32(argv[2], &count) != 0) {
        puts("ping: bad count\n");
        return 1;
    }
    if (count == 0U) {
        return 0;
    }

    int fd = open("/dev/net0", O_RDWR);
    if (fd < 0) {
        puts("ping: /dev/net0 unavailable\n");
        return 1;
    }

    char cmd[64];
    char resp[128];
    uint32_t ip_be = 0U;
    char ip_text[16];

    if (fu_resolve_name_ipv4(argv[1], &ip_be) != 0) {
        puts("ping: resolve failed\n");
        close(fd);
        return 1;
    }
    fu_format_ipv4(ip_be, ip_text, sizeof(ip_text));

    for (uint32_t i = 0; i < count; i++) {
        int n = snprintf(cmd, sizeof(cmd), "ping %s\n", ip_text);
        if (n <= 0 || write(fd, cmd, (unsigned long)n) != n) {
            puts("ping: write failed\n");
            close(fd);
            return 1;
        }
        long r = read(fd, resp, sizeof(resp) - 1U);
        if (r <= 0) {
            puts("ping: read failed\n");
            close(fd);
            return 1;
        }
        resp[r] = '\0';
        puts(resp);
        if (i + 1U < count) {
            sys_sleep(100U);
        }
    }
    close(fd);
    return 0;
}
