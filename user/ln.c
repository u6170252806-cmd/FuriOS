#include "user.h"

int main(int argc, char **argv) {
    int symbolic = 0;
    const char *target;
    const char *link_path;
    int rc;

    if (argc == 4 && strcmp(argv[1], "-s") == 0) {
        symbolic = 1;
        target = argv[2];
        link_path = argv[3];
    } else if (argc == 3) {
        target = argv[1];
        link_path = argv[2];
    } else {
        puts("usage: ln [-s] <target> <linkpath>\n");
        return 1;
    }

    if (symbolic) {
        rc = sys_symlink(target, link_path);
    } else {
        rc = sys_link(target, link_path);
    }
    if (rc != 0) {
        puts("ln: failed\n");
        return 1;
    }
    return 0;
}
