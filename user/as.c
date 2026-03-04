#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: as <input.s> [options]\n");
        return 1;
    }

    if (argc + 3 >= 64) {
        puts("as: too many arguments\n");
        return 1;
    }

    int need_compile = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-E") == 0 ||
            strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            need_compile = 0;
            break;
        }
    }

    const char *exec_argv[64];
    int n = 0;
    exec_argv[n++] = "tcc";
    if (need_compile) {
        exec_argv[n++] = "-c";
    }
    for (int i = 1; i < argc; i++) {
        exec_argv[n++] = argv[i];
    }
    exec_argv[n] = 0;

    if (sys_exec("/bin/tcc", exec_argv) < 0) {
        puts("as: exec /bin/tcc failed\n");
        return 1;
    }
    return 0;
}
