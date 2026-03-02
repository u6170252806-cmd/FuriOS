#include "../user.h"

void putc(char c) {
    (void)sys_write(1, &c, 1);
}

void puts(const char *s) {
    (void)sys_write(1, s, strlen(s));
}

int readline(char *buf, int maxlen) {
    int n = 0;
    while (n < maxlen - 1) {
        char c = 0;
        long r = sys_read(0, &c, 1);
        if (r <= 0) {
            break;
        }
        if (c == '\r') {
            c = '\n';
        }
        if (c == '\n') {
            putc('\n');
            break;
        }
        if (c == 0x7f || c == 0x08) {
            if (n > 0) {
                n--;
                puts("\b \b");
            }
            continue;
        }
        buf[n++] = c;
        putc(c);
    }
    buf[n] = '\0';
    return n;
}

int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        *p++ = '\0';
    }
    argv[argc] = 0;
    return argc;
}
