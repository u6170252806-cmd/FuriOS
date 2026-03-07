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
    char cmd[96];
    int n = 0;
    if (argc == 3 && strcmp(argv[2], "dhcp") == 0) {
        const char *dhcp_argv[] = { "dhcp", argv[1], 0 };
        if (execv("/bin/dhcp", dhcp_argv) != 0) {
            puts("ifconfig: dhcp exec failed\n");
            return 1;
        }
        return 0;
    }

    if (argc == 1) {
        n = snprintf(cmd, sizeof(cmd), "ifconfig\n");
    } else if (argc == 3 && (strcmp(argv[2], "down") == 0 || strcmp(argv[2], "up") == 0)) {
        n = snprintf(cmd, sizeof(cmd), "ifconfig %s %s\n", argv[1], argv[2]);
    } else if (argc == 4) {
        n = snprintf(cmd, sizeof(cmd), "ifconfig %s %s %s\n", argv[1], argv[2], argv[3]);
    } else if (argc == 5) {
        n = snprintf(cmd, sizeof(cmd), "ifconfig %s %s %s %s\n",
                     argv[1], argv[2], argv[3], argv[4]);
    } else {
        puts("usage: ifconfig [if ip mask [gw] | if up | if down | if dhcp]\n");
        return 1;
    }

    if (n <= 0 || (unsigned long)n >= sizeof(cmd)) {
        puts("ifconfig: command too long\n");
        return 1;
    }

    int fd = open("/dev/net0", O_RDWR);
    if (fd < 0) {
        puts("ifconfig: /dev/net0 unavailable\n");
        return 1;
    }
    if (write(fd, cmd, (unsigned long)n) != n) {
        puts("ifconfig: write failed\n");
        close(fd);
        return 1;
    }
    (void)print_net0_response(fd);
    close(fd);
    return 0;
}
