#include "user.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: ranlib <archive>...\n");
        return 1;
    }

    if (argc + 4 >= 64) {
        puts("ranlib: too many arguments\n");
        return 1;
    }

    const char *exec_argv[64];
    int n = 0;
    exec_argv[n++] = "tcc";
    exec_argv[n++] = "-ar";
    exec_argv[n++] = "s";
    for (int i = 1; i < argc; i++) {
        exec_argv[n++] = argv[i];
    }
    exec_argv[n] = 0;

    if (sys_exec("/bin/tcc", exec_argv) < 0) {
        puts("ranlib: exec /bin/tcc failed\n");
        return 1;
    }
    return 0;
}
