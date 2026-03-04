#define FUROS_TCC 1
#define FUROS_TCC_NO_RUN 1
#define ONE_SOURCE 1

#include "tcc_compat.h"
#include "../include/syscall.h"

/* ---------------- syscall glue ---------------- */
static long fu_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

static long fu_sys_brk(long addr) {
    return fu_syscall6(SYS_BRK, addr, 0, 0, 0, 0, 0);
}

int errno;

int open(const char *path, int flags, ...) {
    long rc = fu_syscall6(SYS_OPEN, (long)path, (long)flags, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return (int)rc;
}

int close(int fd) {
    long rc = fu_syscall6(SYS_CLOSE, (long)fd, 0, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return 0;
}

ssize_t read(int fd, void *buf, size_t count) {
    long rc;
    do {
        rc = fu_syscall6(SYS_READ, (long)fd, (long)buf, (long)count, 0, 0, 0);
    } while (rc == -2);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return (ssize_t)rc;
}

ssize_t write(int fd, const void *buf, size_t count) {
    size_t done = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (done < count) {
        long rc;
        do {
            rc = fu_syscall6(SYS_WRITE, (long)fd, (long)(p + done), (long)(count - done), 0, 0, 0);
        } while (rc == -2);
        if (rc < 0) {
            errno = (int)(-rc);
            return done ? (ssize_t)done : -1;
        }
        if (rc == 0) {
            break;
        }
        done += (size_t)rc;
    }
    return (ssize_t)done;
}

off_t lseek(int fd, off_t offset, int whence) {
    long rc = fu_syscall6(SYS_LSEEK, (long)fd, (long)offset, (long)whence, 0, 0, 0);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return (off_t)rc;
}

int unlink(const char *path) {
    long rc = fu_syscall6(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return 0;
}

int remove(const char *path) {
    return unlink(path);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    long rc = fu_syscall6(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)(-rc);
        return NULL;
    }
    return buf;
}

/* ---------------- basic libc ---------------- */

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i != 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (pa[i] < pb[i]) ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strcat(char *dst, const char *src) {
    char *p = dst + strlen(dst);
    while ((*p++ = *src++) != '\0') {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    if (c == '\0') {
        return (char *)s;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return (char *)haystack;
    }
    for (const char *h = haystack; *h; h++) {
        if (*h == *needle && strncmp(h, needle, nlen) == 0) {
            return (char *)h;
        }
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) {
                return (char *)s;
            }
        }
    }
    return NULL;
}

char *strerror(int errnum) {
    static char buf[32];
    (void)snprintf(buf, sizeof(buf), "err:%d", errnum);
    return buf;
}

static int fu_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int fu_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int fu_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    int neg = 0;
    unsigned long long v;

    while (fu_is_space(*p)) p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    v = 0;
    const char *start = p;
    for (;;) {
        int d = digit_val(*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        p++;
    }

    if (endptr) {
        *endptr = (char *)(p == start ? nptr : p);
    }
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    unsigned long long v;

    while (fu_is_space(*p)) p++;
    if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    v = 0;
    const char *start = p;
    for (;;) {
        int d = digit_val(*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        p++;
    }

    if (endptr) {
        *endptr = (char *)(p == start ? nptr : p);
    }
    return v;
}

static double pow10i(int exp) {
    double base = (exp < 0) ? 0.1 : 10.0;
    double out = 1.0;
    int n = exp < 0 ? -exp : exp;
    while (n) {
        if (n & 1) out *= base;
        base *= base;
        n >>= 1;
    }
    return out;
}

double strtod(const char *nptr, char **endptr) {
    const char *p = nptr;
    int neg = 0;
    double v = 0.0;
    double frac = 0.0;
    double scale = 1.0;
    int exp = 0;
    int exp_neg = 0;

    while (fu_is_space(*p)) p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    const char *start = p;
    while (fu_is_digit(*p)) {
        v = v * 10.0 + (double)(*p - '0');
        p++;
    }

    if (*p == '.') {
        p++;
        while (fu_is_digit(*p)) {
            frac = frac * 10.0 + (double)(*p - '0');
            scale *= 10.0;
            p++;
        }
    }

    v += frac / scale;

    if (*p == 'e' || *p == 'E') {
        const char *ep = p + 1;
        if (*ep == '+' || *ep == '-') {
            exp_neg = (*ep == '-');
            ep++;
        }
        if (fu_is_digit(*ep)) {
            p = ep;
            while (fu_is_digit(*p)) {
                exp = exp * 10 + (*p - '0');
                p++;
            }
            if (exp_neg) exp = -exp;
            v *= pow10i(exp);
        }
    }

    if (endptr) {
        *endptr = (char *)(p == start ? nptr : p);
    }
    return neg ? -v : v;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static void fu_exit_loop(void) __attribute__((noreturn));
static void fu_exit_loop(void) {
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void exit(int code) {
    fu_syscall6(SYS_EXIT, (long)code, 0, 0, 0, 0, 0);
    fu_exit_loop();
}

int system(const char *command) {
    (void)command;
    errno = ENOSYS;
    return -1;
}

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

/* ---------------- allocator ---------------- */
typedef struct bump_hdr {
    size_t size;
} bump_hdr_t;

static long heap_brk = -1;

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1U) & ~(a - 1U);
}

static void *sbrk(long increment) {
    if (heap_brk < 0) {
        long cur = fu_sys_brk(0);
        if (cur < 0) return (void *)-1;
        heap_brk = cur;
    }
    long next = heap_brk + increment;
    long rc = fu_sys_brk(next);
    if (rc < 0) {
        return (void *)-1;
    }
    long old = heap_brk;
    heap_brk = rc;
    return (void *)old;
}

void *malloc(size_t size) {
    if (size == 0U) {
        size = 1U;
    }
    size = align_up(size, 16U);
    size_t total = sizeof(bump_hdr_t) + size;
    bump_hdr_t *h = (bump_hdr_t *)sbrk((long)total);
    if (h == (void *)-1) {
        errno = ENOMEM;
        return NULL;
    }
    h->size = size;
    return (void *)(h + 1);
}

void free(void *ptr) {
    (void)ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0U) {
        return NULL;
    }

    size = align_up(size, 16U);
    bump_hdr_t *h = ((bump_hdr_t *)ptr) - 1;
    if (h->size >= size) {
        return ptr;
    }

    void *n = malloc(size);
    if (!n) {
        return NULL;
    }
    memcpy(n, ptr, h->size);
    return n;
}

void *calloc(size_t n, size_t size) {
    if (n == 0U || size == 0U) {
        return malloc(1);
    }
    if (n > ((size_t)-1) / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = n * size;
    void *p = malloc(total);
    if (!p) {
        return NULL;
    }
    memset(p, 0, total);
    return p;
}

/* ---------------- formatted output ---------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    FILE *file;
} out_ctx_t;

static void out_ch(out_ctx_t *o, char c) {
    if (o->file) {
        (void)fwrite(&c, 1, 1, o->file);
    } else if (o->buf && o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_strn(out_ctx_t *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out_ch(o, s[i]);
    }
}

static size_t utoa_base(unsigned long long v, unsigned base, int upper, char *tmp, size_t tmpsz) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t i = 0;
    if (base < 2 || base > 16 || tmpsz == 0) {
        return 0;
    }
    if (v == 0) {
        tmp[i++] = '0';
        return i;
    }
    while (v && i < tmpsz) {
        tmp[i++] = digits[v % base];
        v /= base;
    }
    return i;
}

static int do_vformat(out_ctx_t *o, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_ch(o, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_ch(o, '%');
            continue;
        }

        int left = 0;
        int zero = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') zero = 1;
            fmt++;
        }

        int width = 0;
        int precision = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (fu_is_digit(*fmt)) {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (fu_is_digit(*fmt)) {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int is_long = 0;
        int is_ll = 0;
        int is_z = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_ll = 1;
                fmt++;
            }
        } else if (*fmt == 'z') {
            is_z = 1;
            fmt++;
        }

        char spec = *fmt;
        char tmp[64];
        size_t n = 0;
        int neg = 0;
        unsigned long long uv = 0;

        switch (spec) {
        case 'd':
        case 'i': {
            long long sv;
            if (is_ll) sv = va_arg(ap, long long);
            else if (is_long) sv = va_arg(ap, long);
            else if (is_z) sv = (long long)va_arg(ap, ssize_t);
            else sv = va_arg(ap, int);
            if (sv < 0) {
                neg = 1;
                uv = (unsigned long long)(-sv);
            } else {
                uv = (unsigned long long)sv;
            }
            n = utoa_base(uv, 10, 0, tmp, sizeof(tmp));
            break;
        }
        case 'u':
            if (is_ll) uv = va_arg(ap, unsigned long long);
            else if (is_long) uv = va_arg(ap, unsigned long);
            else if (is_z) uv = (unsigned long long)va_arg(ap, size_t);
            else uv = va_arg(ap, unsigned int);
            n = utoa_base(uv, 10, 0, tmp, sizeof(tmp));
            break;
        case 'x':
        case 'X':
            if (is_ll) uv = va_arg(ap, unsigned long long);
            else if (is_long) uv = va_arg(ap, unsigned long);
            else if (is_z) uv = (unsigned long long)va_arg(ap, size_t);
            else uv = va_arg(ap, unsigned int);
            n = utoa_base(uv, 16, spec == 'X', tmp, sizeof(tmp));
            break;
        case 'p':
            uv = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            tmp[n++] = 'x';
            tmp[n++] = '0';
            n += utoa_base(uv, 16, 0, tmp + n, sizeof(tmp) - n);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            tmp[0] = c;
            n = 1;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t slen = strlen(s);
            if (precision >= 0 && (size_t)precision < slen) slen = (size_t)precision;
            int pad = width > (int)slen ? width - (int)slen : 0;
            if (!left) while (pad-- > 0) out_ch(o, ' ');
            out_strn(o, s, slen);
            if (left) while (pad-- > 0) out_ch(o, ' ');
            continue;
        }
        default:
            out_ch(o, '%');
            out_ch(o, spec ? spec : '?');
            continue;
        }

        size_t digits_n = n;
        if (spec == 'p' && digits_n >= 2 && tmp[0] == 'x' && tmp[1] == '0') {
            digits_n -= 2;
        }
        size_t need_zeros = 0;
        if (precision > 0 && (size_t)precision > digits_n) {
            need_zeros = (size_t)precision - digits_n;
            zero = 0;
        }

        size_t full = n + need_zeros + (neg ? 1U : 0U);
        int pad = width > (int)full ? width - (int)full : 0;
        char padc = (zero && !left && precision < 0) ? '0' : ' ';

        if (!left) {
            while (pad-- > 0) out_ch(o, padc);
        }
        if (neg) out_ch(o, '-');
        while (need_zeros-- > 0) out_ch(o, '0');

        if (spec == 'p' && n >= 2 && tmp[0] == 'x' && tmp[1] == '0') {
            out_ch(o, '0');
            out_ch(o, 'x');
            for (size_t i = n; i > 2; i--) out_ch(o, tmp[i - 1]);
        } else {
            for (size_t i = n; i > 0; i--) out_ch(o, tmp[i - 1]);
        }

        if (left) {
            while (pad-- > 0) out_ch(o, ' ');
        }
    }
    if (!o->file && o->buf && o->cap > 0) {
        size_t idx = (o->len < o->cap) ? o->len : (o->cap - 1);
        o->buf[idx] = '\0';
    }
    return (int)o->len;
}

/* ---------------- stdio ---------------- */

struct FU_FILE {
    int fd;
    int owned;
    int error;
    int eof;
    int has_ungot;
    unsigned char ungot;
};

static struct FU_FILE fu_stdin = {0, 0, 0, 0, 0, 0};
static struct FU_FILE fu_stdout = {1, 0, 0, 0, 0, 0};
static struct FU_FILE fu_stderr = {2, 0, 0, 0, 0, 0};

FILE *stdin = &fu_stdin;
FILE *stdout = &fu_stdout;
FILE *stderr = &fu_stderr;

static int mode_to_flags(const char *mode, int *append) {
    int flags = 0;
    int rd = 0;
    int wr = 0;
    *append = 0;

    if (!mode || !mode[0]) return -1;
    switch (mode[0]) {
    case 'r': rd = 1; break;
    case 'w': wr = 1; flags |= O_CREAT | O_TRUNC; break;
    case 'a': wr = 1; flags |= O_CREAT | O_APPEND; *append = 1; break;
    default: return -1;
    }

    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') {
            rd = 1;
            wr = 1;
        }
    }

    if (rd && wr) flags |= O_RDWR;
    else if (wr) flags |= O_WRONLY;
    else flags |= O_RDONLY;

    return flags;
}

FILE *fdopen(int fd, const char *mode) {
    int append = 0;
    if (mode_to_flags(mode, &append) < 0) {
        errno = EINVAL;
        return NULL;
    }
    struct FU_FILE *f = (struct FU_FILE *)calloc(1, sizeof(struct FU_FILE));
    if (!f) return NULL;
    f->fd = fd;
    f->owned = 0;
    if (append) (void)lseek(fd, 0, SEEK_END);
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int append = 0;
    int flags = mode_to_flags(mode, &append);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, flags);
    if (fd < 0) {
        return NULL;
    }
    struct FU_FILE *f = (struct FU_FILE *)calloc(1, sizeof(struct FU_FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->owned = 1;
    if (append) (void)lseek(fd, 0, SEEK_END);
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (!stream) {
        return fopen(path, mode);
    }
    (void)fflush(stream);
    int append = 0;
    int flags = mode_to_flags(mode, &append);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    if (stream->owned) {
        (void)close(stream->fd);
    }
    stream->fd = fd;
    stream->owned = 1;
    stream->error = 0;
    stream->eof = 0;
    stream->has_ungot = 0;
    if (append) (void)lseek(fd, 0, SEEK_END);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    int rc = 0;
    if (stream->owned) {
        rc = close(stream->fd);
    }
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return rc < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    size_t total = size * nmemb;
    unsigned char *out = (unsigned char *)ptr;
    size_t nread = 0;

    if (stream->has_ungot && total > 0) {
        out[0] = stream->ungot;
        stream->has_ungot = 0;
        nread = 1;
    }

    while (nread < total) {
        ssize_t rc = read(stream->fd, out + nread, total - nread);
        if (rc < 0) {
            stream->error = 1;
            break;
        }
        if (rc == 0) {
            stream->eof = 1;
            break;
        }
        nread += (size_t)rc;
    }
    return nread / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    size_t total = size * nmemb;
    const unsigned char *in = (const unsigned char *)ptr;
    size_t nw = 0;
    while (nw < total) {
        ssize_t rc = write(stream->fd, in + nw, total - nw);
        if (rc <= 0) {
            stream->error = 1;
            break;
        }
        nw += (size_t)rc;
    }
    return nw / size;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    stream->has_ungot = 0;
    return lseek(stream->fd, (off_t)offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

int fgetc(FILE *stream) {
    unsigned char c;
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    if (stream->has_ungot) {
        stream->has_ungot = 0;
        return (int)stream->ungot;
    }
    ssize_t rc = read(stream->fd, &c, 1);
    if (rc == 1) {
        return (int)c;
    }
    if (rc == 0) stream->eof = 1;
    else stream->error = 1;
    return EOF;
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) {
        return EOF;
    }
    stream->has_ungot = 1;
    stream->ungot = (unsigned char)c;
    return c;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    ssize_t rc = write(stream->fd, &ch, 1);
    if (rc == 1) {
        return c & 0xFF;
    }
    stream->error = 1;
    return EOF;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) {
        errno = EINVAL;
        return EOF;
    }
    size_t n = strlen(s);
    return (fwrite(s, 1, n, stream) == n) ? (int)n : EOF;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    if (!s) s = "(null)";
    size_t n = strlen(s);
    if (fwrite(s, 1, n, stdout) != n) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return (int)(n + 1);
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) {
        errno = EINVAL;
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) {
                return NULL;
            }
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    s[i] = '\0';
    return s;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    out_ctx_t out;
    out.buf = NULL;
    out.cap = 0;
    out.len = 0;
    out.file = stream ? stream : stdout;
    return do_vformat(&out, fmt, ap);
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(stream, fmt, ap);
    va_end(ap);
    return rc;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    out_ctx_t out;
    out.buf = buf;
    out.cap = n;
    out.len = 0;
    out.file = NULL;
    return do_vformat(&out, fmt, ap);
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

/* ---------------- misc stubs ---------------- */

static void qsort_swap(unsigned char *a, unsigned char *b, size_t size) {
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void qsort_impl(unsigned char *base, size_t n, size_t size,
                       int (*cmp)(const void *, const void *)) {
    if (n < 2) return;
    size_t pivot = n / 2;
    qsort_swap(base + pivot * size, base + (n - 1) * size, size);

    size_t store = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (cmp(base + i * size, base + (n - 1) * size) < 0) {
            qsort_swap(base + i * size, base + store * size, size);
            store++;
        }
    }
    qsort_swap(base + store * size, base + (n - 1) * size, size);

    qsort_impl(base, store, size, cmp);
    qsort_impl(base + (store + 1) * size, n - store - 1, size, cmp);
}

void qsort(void *base, size_t nitems, size_t size,
           int (*compar)(const void *, const void *)) {
    if (!base || !compar || size == 0) return;
    qsort_impl((unsigned char *)base, nitems, size, compar);
}

int setjmp(jmp_buf env) {
    (void)env;
    return 0;
}

void longjmp(jmp_buf env, int val) {
    (void)env;
    (void)val;
    exit(1);
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    tv->tv_sec = 0;
    tv->tv_usec = 0;
    return 0;
}

time_t time(time_t *tloc) {
    time_t t = 0;
    if (tloc) {
        *tloc = t;
    }
    return t;
}

struct tm *localtime(const time_t *timer) {
    static struct tm tm_zero;
    (void)timer;
    memset(&tm_zero, 0, sizeof(tm_zero));
    tm_zero.tm_year = 70;
    tm_zero.tm_mday = 1;
    return &tm_zero;
}

double ldexp(double x, int exp) {
    if (exp == 0 || x == 0.0) {
        return x;
    }
    double f = 1.0;
    int n = exp > 0 ? exp : -exp;
    while (n--) {
        f *= 2.0;
    }
    return exp > 0 ? (x * f) : (x / f);
}

long double ldexpl(long double x, int exp) {
    return (long double)ldexp((double)x, exp);
}

int mprotect(void *addr, size_t len, int prot) {
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}

long sysconf(int name) {
    (void)name;
    return 4096;
}

char *realpath(const char *path, char *resolved_path) {
    static char buf[512];
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    char *dst = resolved_path ? resolved_path : buf;
    if (path[0] == '/') {
        strncpy(dst, path, 511);
        dst[511] = '\0';
        return dst;
    }
    if (!getcwd(dst, 480)) {
        return NULL;
    }
    size_t n = strlen(dst);
    if (n && dst[n - 1] != '/') {
        dst[n++] = '/';
    }
    strncpy(dst + n, path, 511 - n);
    dst[511] = '\0';
    return dst;
}

void __clear_cache(void *begin, void *end) {
    __asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");
    (void)begin;
    (void)end;
}

void __assert_fail(const char *assertion, const char *file,
                   unsigned int line, const char *function) {
    (void)fprintf(stderr, "assert: %s (%s:%u %s)\n",
                  assertion ? assertion : "?",
                  file ? file : "?", line,
                  function ? function : "?");
    exit(1);
}

/* TinyCC implementation */
#include "../third_party/tinycc/tcc.c"
