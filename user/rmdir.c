#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: rmdir <dir>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_rmdir(argv[i]) < 0) {
            puts("rmdir: failed ");
            puts(argv[i]);
            puts("\n");
            rc = 1;
        }
    }
    return rc;
}
