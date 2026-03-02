#include "user.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    puts("[init] pid=");
    {
        int pid = sys_getpid();
        char d[12];
        int i = 0;
        if (pid == 0) {
            d[i++] = '0';
        } else {
            int x = pid;
            char tmp[12];
            int t = 0;
            while (x > 0 && t < 11) {
                tmp[t++] = (char)('0' + (x % 10));
                x /= 10;
            }
            while (t > 0) {
                d[i++] = tmp[--t];
            }
        }
        d[i++] = '\n';
        d[i] = '\0';
        puts(d);
    }

    for (;;) {
        int pid = sys_fork();
        if (pid < 0) {
            puts("[init] fork failed\n");
            sys_yield();
            continue;
        }
        if (pid == 0) {
            const char *argv_sh[] = {"/bin/sh", 0};
            int rc = sys_exec("/bin/sh", argv_sh);
            if (rc < 0) {
                puts("[init] exec /bin/sh failed\n");
                sys_exit(1);
            }
            sys_exit(0);
        }

        int status = 0;
        (void)sys_waitpid(pid, &status, 0);
        puts("[init] shell exited, restarting\n");
    }

    return 0;
}
