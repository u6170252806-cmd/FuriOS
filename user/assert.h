#ifndef FUROS_ASSERT_H
#define FUROS_ASSERT_H

void __assert_fail(const char *assertion, const char *file,
                   unsigned int line, const char *function);

#ifndef NDEBUG
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#else
#define assert(expr) ((void)0)
#endif

#endif
