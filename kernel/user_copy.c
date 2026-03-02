#include "user_copy.h"
#include "config.h"

bool user_ptr_valid(uint64_t ptr, size_t len) {
    if (len == 0) {
        return true;
    }
    if (ptr < USER_VA_BASE) {
        return false;
    }
    if (ptr + len < ptr) {
        return false;
    }
    if (ptr + len > USER_VA_LIMIT) {
        return false;
    }
    return true;
}

int copy_from_user(void *dst, const void *user_src, size_t n) {
    if (!user_ptr_valid((uint64_t)user_src, n)) {
        return -1;
    }
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)user_src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return 0;
}

int copy_to_user(void *user_dst, const void *src, size_t n) {
    if (!user_ptr_valid((uint64_t)user_dst, n)) {
        return -1;
    }
    uint8_t *d = (uint8_t *)user_dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return 0;
}

int copy_user_str(char *dst, const char *user_src, size_t maxlen) {
    if (!user_ptr_valid((uint64_t)user_src, 1)) {
        return -1;
    }
    for (size_t i = 0; i < maxlen; i++) {
        uint64_t p = (uint64_t)(user_src + i);
        if (p >= USER_VA_LIMIT) {
            return -1;
        }
        dst[i] = user_src[i];
        if (dst[i] == '\0') {
            return 0;
        }
    }
    dst[maxlen - 1] = '\0';
    return 0;
}
