#include "user.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        puts("usage: ar <opflags> <archive> [files...]\n");
        return 1;
    }

    if (argc + 2 >= 64) {
        puts("ar: too many arguments\n");
        return 1;
    }

    const char *exec_argv[64];
    int n = 0;
    exec_argv[n++] = "tcc";
    exec_argv[n++] = "-ar";
    for (int i = 1; i < argc; i++) {
        exec_argv[n++] = argv[i];
    }
    exec_argv[n] = 0;

    if (sys_exec("/bin/tcc", exec_argv) < 0) {
        puts("ar: exec /bin/tcc failed\n");
        return 1;
    }
    return 0;
}
