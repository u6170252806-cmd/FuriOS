#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: mkdir <dir>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i]) < 0) {
            puts("mkdir: cannot create ");
            puts(argv[i]);
            puts("\n");
            rc = 1;
        }
    }
    return rc;
}
