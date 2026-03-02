#include "user.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char cwd[128];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) {
        puts("pwd: failed\n");
        return 1;
    }
    puts(cwd);
    puts("\n");
    return 0;
}
