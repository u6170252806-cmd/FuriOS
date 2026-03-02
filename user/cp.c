#include "user.h"

static int write_all(int fd, const char *buf, long len) {
    long off = 0;
    while (off < len) {
        long n = sys_write(fd, buf + off, (unsigned long)(len - off));
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        puts("usage: cp <src> <dst>\n");
        return 1;
    }

    int in_fd = sys_open(argv[1], O_RDONLY);
    if (in_fd < 0) {
        puts("cp: cannot open source\n");
        return 1;
    }

    int out_fd = sys_open(argv[2], O_CREAT | O_WRONLY | O_TRUNC);
    if (out_fd < 0) {
        puts("cp: cannot open destination\n");
        sys_close(in_fd);
        return 1;
    }

    int rc = 0;
    char buf[256];
    for (;;) {
        long n = sys_read(in_fd, buf, sizeof(buf));
        if (n < 0) {
            rc = 1;
            puts("cp: read failed\n");
            break;
        }
        if (n == 0) {
            break;
        }
        if (write_all(out_fd, buf, n) != 0) {
            rc = 1;
            puts("cp: write failed\n");
            break;
        }
    }

    sys_close(out_fd);
    sys_close(in_fd);
    return rc;
}
