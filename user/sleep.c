#include "user.h"
#include "config.h"

static int parse_u64(const char *s, unsigned long *out) {
    unsigned long v = 0;
    if (!s || !*s) {
        return -1;
    }
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') {
            return -1;
        }
        unsigned long d = (unsigned long)(c - '0');
        unsigned long next = v * 10UL + d;
        if (next < v) {
            return -1;
        }
        v = next;
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        puts("usage: sleep <seconds>\n");
        return 1;
    }

    unsigned long seconds = 0;
    if (parse_u64(argv[1], &seconds) != 0) {
        puts("sleep: invalid number\n");
        return 1;
    }

    unsigned long ticks = seconds * (unsigned long)TIMER_HZ;
    if (seconds != 0 && ticks / seconds != (unsigned long)TIMER_HZ) {
        puts("sleep: overflow\n");
        return 1;
    }

    if (sys_sleep(ticks) < 0) {
        puts("sleep: failed\n");
        return 1;
    }
    return 0;
}
