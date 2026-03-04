#include "user.h"

static int parse_pid(const char *s, int *out) {
    if (!s || !*s) {
        return -1;
    }
    long v = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') {
            return -1;
        }
        long d = (long)(c - '0');
        long next = v * 10L + d;
        if (next < v) {
            return -1;
        }
        v = next;
    }
    if (v <= 0 || v > 0x7FFFFFFFL) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    int sig = SIGTERM;
    const char *pid_arg = 0;
    if (argc == 2) {
        pid_arg = argv[1];
    } else if (argc == 3 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-9") == 0 || strcmp(argv[1], "-KILL") == 0) {
            sig = SIGKILL;
        } else if (strcmp(argv[1], "-1") == 0 || strcmp(argv[1], "-HUP") == 0) {
            sig = SIGHUP;
        } else if (strcmp(argv[1], "-2") == 0 || strcmp(argv[1], "-INT") == 0) {
            sig = SIGINT;
        } else if (strcmp(argv[1], "-3") == 0 || strcmp(argv[1], "-QUIT") == 0) {
            sig = SIGQUIT;
        } else if (strcmp(argv[1], "-18") == 0 || strcmp(argv[1], "-CONT") == 0) {
            sig = SIGCONT;
        } else if (strcmp(argv[1], "-19") == 0 || strcmp(argv[1], "-STOP") == 0) {
            sig = SIGSTOP;
        } else if (strcmp(argv[1], "-15") == 0 || strcmp(argv[1], "-TERM") == 0) {
            sig = SIGTERM;
        } else {
            puts("usage: kill [-1|-2|-3|-9|-15|-18|-19] <pid>\n");
            return 1;
        }
        pid_arg = argv[2];
    } else {
        puts("usage: kill [-1|-2|-3|-9|-15|-18|-19] <pid>\n");
        return 1;
    }

    int pid = 0;
    if (parse_pid(pid_arg, &pid) != 0) {
        puts("kill: invalid pid\n");
        return 1;
    }

    if (sys_kill(pid, sig) != 0) {
        puts("kill: failed\n");
        return 1;
    }
    return 0;
}
