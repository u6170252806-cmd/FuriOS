#ifndef FUROS_TCC_COMPAT_H
#define FUROS_TCC_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define alloca __builtin_alloca

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef long ssize_t;
typedef long off_t;
typedef long time_t;

struct timeval {
    long tv_sec;
    long tv_usec;
};
typedef struct timeval timeval;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef struct FU_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 0x3
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_BINARY 0

#define EINTR 4
#define EINVAL 22
#define ENOSYS 38
#define ENOMEM 12
#define ENOENT 2

extern int errno;

typedef int jmp_buf[1];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
int remove(const char *path);
char *getcwd(char *buf, size_t size);
int gettimeofday(struct timeval *tv, void *tz);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void exit(int code) __attribute__((noreturn));
int atoi(const char *s);
char *getenv(const char *name);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
char *strerror(int errnum);

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int ungetc(int c, FILE *stream);
int putchar(int c);
int puts(const char *s);
char *fgets(char *s, int size, FILE *stream);
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

void qsort(void *base, size_t nitems, size_t size,
           int (*compar)(const void *, const void *));

time_t time(time_t *tloc);
struct tm *localtime(const time_t *timer);
double ldexp(double x, int exp);
long double ldexpl(long double x, int exp);
int mprotect(void *addr, size_t len, int prot);
long sysconf(int name);
char *realpath(const char *path, char *resolved_path);
int system(const char *command);
void bzero(void *s, size_t n);

#endif
