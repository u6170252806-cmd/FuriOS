#include "user.h"

static int print_net0_response(int fd) {
    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf) - 1U)) > 0) {
        buf[n] = '\0';
        puts(buf);
    }
    return 0;
}

int main(int argc, char **argv) {
    char cmd[32];
    int n = 0;

    if (argc == 1) {
        n = snprintf(cmd, sizeof(cmd), "arp\n");
    } else if (argc == 2 && strcmp(argv[1], "flush") == 0) {
        n = snprintf(cmd, sizeof(cmd), "arp flush\n");
    } else {
        puts("usage: arp [flush]\n");
        return 1;
    }

    if (n <= 0 || (unsigned long)n >= sizeof(cmd)) {
        puts("arp: command too long\n");
        return 1;
    }

    int fd = open("/dev/net0", O_RDWR);
    if (fd < 0) {
        puts("arp: /dev/net0 unavailable\n");
        return 1;
    }
    if (write(fd, cmd, (unsigned long)n) != n) {
        puts("arp: write failed\n");
        close(fd);
        return 1;
    }
    (void)print_net0_response(fd);
    close(fd);
    return 0;
}
