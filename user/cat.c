#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: cat <path> | cat -\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = -1;
        if (strcmp(argv[i], "-") == 0) {
            fd = 0;
        } else {
            fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) {
                puts("cat: open failed\n");
                return 1;
            }
        }

        char buf[128];
        for (;;) {
            long n = sys_read(fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            (void)sys_write(1, buf, (unsigned long)n);
        }

        if (fd > 0) {
            sys_close(fd);
        }
    }
    return 0;
}
