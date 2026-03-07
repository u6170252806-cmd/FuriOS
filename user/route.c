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
    char cmd[128];
    int n = 0;

    if (argc == 1) {
        n = snprintf(cmd, sizeof(cmd), "route\n");
    } else if (argc == 5 &&
               strcmp(argv[1], "add") == 0 &&
               strcmp(argv[2], "default") == 0) {
        n = snprintf(cmd, sizeof(cmd), "route add default %s %s\n", argv[3], argv[4]);
    } else if (argc == 3 &&
               strcmp(argv[1], "del") == 0 &&
               strcmp(argv[2], "default") == 0) {
        n = snprintf(cmd, sizeof(cmd), "route del default\n");
    } else if (argc == 3 && strcmp(argv[1], "get") == 0) {
        n = snprintf(cmd, sizeof(cmd), "route get %s\n", argv[2]);
    } else if (argc == 6 && strcmp(argv[1], "add") == 0) {
        n = snprintf(cmd, sizeof(cmd), "route add %s %s %s %s\n",
                     argv[2], argv[3], argv[4], argv[5]);
    } else if (argc == 4 && strcmp(argv[1], "del") == 0) {
        n = snprintf(cmd, sizeof(cmd), "route del %s %s\n", argv[2], argv[3]);
    } else {
        puts("usage: route [get <ip> | add <prefix> <mask> <gw> <if> | add default <gw> <if> | del <prefix> <mask> | del default]\n");
        return 1;
    }

    if (n <= 0 || (unsigned long)n >= sizeof(cmd)) {
        puts("route: command too long\n");
        return 1;
    }

    int fd = open("/dev/net0", O_RDWR);
    if (fd < 0) {
        puts("route: /dev/net0 unavailable\n");
        return 1;
    }
    if (write(fd, cmd, (unsigned long)n) != n) {
        puts("route: write failed\n");
        close(fd);
        return 1;
    }
    (void)print_net0_response(fd);
    close(fd);
    return 0;
}
