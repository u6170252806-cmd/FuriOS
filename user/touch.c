#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: touch <path>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_CREAT | O_WRONLY);
        if (fd < 0) {
            puts("touch: cannot touch ");
            puts(argv[i]);
            puts("\n");
            rc = 1;
            continue;
        }
        sys_close(fd);
    }
    return rc;
}
