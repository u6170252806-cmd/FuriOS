#include "user.h"

int main(int argc, char **argv) {
    const char *path = ".";
    if (argc > 1) {
        path = argv[1];
    }

    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        puts("ls: open failed\n");
        return 1;
    }

    dirent_t ent;
    while (sys_read(fd, &ent, sizeof(ent)) == (long)sizeof(ent)) {
        puts(ent.name);
        puts("\n");
    }

    sys_close(fd);
    return 0;
}
