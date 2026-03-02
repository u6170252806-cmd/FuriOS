#include "user.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        puts("usage: umount <target>\n");
        return 1;
    }
    if (sys_umount(argv[1], 0) != 0) {
        puts("umount: failed\n");
        return 1;
    }
    return 0;
}
