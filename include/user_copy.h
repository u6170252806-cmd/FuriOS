#ifndef FUROS_USER_COPY_H
#define FUROS_USER_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool user_ptr_valid(uint64_t ptr, size_t len);
int copy_from_user(void *dst, const void *user_src, size_t n);
int copy_to_user(void *user_dst, const void *src, size_t n);
int copy_user_str(char *dst, const char *user_src, size_t maxlen);

#endif
