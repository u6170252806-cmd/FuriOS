#include "user.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        puts("usage: mv <src> <dst>\n");
        return 1;
    }

    if (sys_rename(argv[1], argv[2]) < 0) {
        puts("mv: rename failed\n");
        return 1;
    }

    return 0;
}
