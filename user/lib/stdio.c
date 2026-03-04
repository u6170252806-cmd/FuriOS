#include "../user.h"
#include <stdarg.h>
#include <stdbool.h>

static void out_ch(char **dst, size_t *rem, size_t *count, char c) {
    if (*dst && *rem > 1U) {
        **dst = c;
        (*dst)++;
        (*rem)--;
    }
    (*count)++;
}

static void out_str(char **dst, size_t *rem, size_t *count, const char *s) {
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        out_ch(dst, rem, count, *s++);
    }
}

static void out_u64(char **dst, size_t *rem, size_t *count, uint64_t v, unsigned base, bool upper) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t n = 0;
    if (base < 2U || base > 16U) {
        return;
    }
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < sizeof(tmp));
    while (n > 0) {
        out_ch(dst, rem, count, tmp[--n]);
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    char *dst = buf;
    size_t rem = n;
    size_t count = 0;

    if (!fmt) {
        if (buf && n) {
            buf[0] = '\0';
        }
        return 0;
    }

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_ch(&dst, &rem, &count, *p);
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }
        if (*p == '%') {
            out_ch(&dst, &rem, &count, '%');
            continue;
        }

        bool long_flag = false;
        bool longlong_flag = false;
        if (*p == 'l') {
            long_flag = true;
            p++;
            if (*p == 'l') {
                longlong_flag = true;
                p++;
            }
        }

        switch (*p) {
            case 'c': {
                int c = va_arg(ap, int);
                out_ch(&dst, &rem, &count, (char)c);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                out_str(&dst, &rem, &count, s);
                break;
            }
            case 'd':
            case 'i': {
                int64_t sv;
                if (longlong_flag) {
                    sv = va_arg(ap, long long);
                } else if (long_flag) {
                    sv = va_arg(ap, long);
                } else {
                    sv = va_arg(ap, int);
                }
                if (sv < 0) {
                    out_ch(&dst, &rem, &count, '-');
                    out_u64(&dst, &rem, &count, (uint64_t)(-sv), 10U, false);
                } else {
                    out_u64(&dst, &rem, &count, (uint64_t)sv, 10U, false);
                }
                break;
            }
            case 'u': {
                uint64_t uv;
                if (longlong_flag) {
                    uv = va_arg(ap, unsigned long long);
                } else if (long_flag) {
                    uv = va_arg(ap, unsigned long);
                } else {
                    uv = va_arg(ap, unsigned int);
                }
                out_u64(&dst, &rem, &count, uv, 10U, false);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t uv;
                if (longlong_flag) {
                    uv = va_arg(ap, unsigned long long);
                } else if (long_flag) {
                    uv = va_arg(ap, unsigned long);
                } else {
                    uv = va_arg(ap, unsigned int);
                }
                out_u64(&dst, &rem, &count, uv, 16U, *p == 'X');
                break;
            }
            case 'p': {
                uintptr_t pv = (uintptr_t)va_arg(ap, void *);
                out_str(&dst, &rem, &count, "0x");
                out_u64(&dst, &rem, &count, (uint64_t)pv, 16U, false);
                break;
            }
            default:
                out_ch(&dst, &rem, &count, '%');
                out_ch(&dst, &rem, &count, *p);
                break;
        }
    }

    if (buf && n) {
        *dst = '\0';
    }
    return (int)count;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return rc;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return rc;
}

int vprintf(const char *fmt, va_list ap) {
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        (void)sys_write(1, tmp, (unsigned long)((n < (int)sizeof(tmp)) ? n : (int)(sizeof(tmp) - 1)));
    }
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vprintf(fmt, ap);
    va_end(ap);
    return rc;
}

int putchar(int c) {
    char ch = (char)c;
    return sys_write(1, &ch, 1) == 1 ? c : -1;
}

int getchar(void) {
    char ch = 0;
    long rc = sys_read(0, &ch, 1);
    if (rc <= 0) {
        return -1;
    }
    return (unsigned char)ch;
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
    printf("assert: %s (%s:%u %s)\n",
           assertion ? assertion : "?",
           file ? file : "?",
           line,
           function ? function : "?");
    abort();
}
