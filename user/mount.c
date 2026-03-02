#include "user.h"

int main(int argc, char **argv) {
    const char *source = 0;
    const char *target = 0;
    const char *fstype = "ext4";

    if (argc == 3) {
        source = argv[1];
        target = argv[2];
    } else if (argc == 4) {
        source = argv[1];
        target = argv[2];
        fstype = argv[3];
    } else {
        puts("usage: mount <source> <target> [fstype]\n");
        return 1;
    }

    if (sys_mount(source, target, fstype, 0, 0) != 0) {
        puts("mount: failed\n");
        return 1;
    }
    return 0;
}
