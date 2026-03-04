#ifndef FUROS_INTTYPES_H
#define FUROS_INTTYPES_H

#include <stdint.h>

typedef long intptr_t;
typedef unsigned long uintptr_t;

#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"

#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"

#endif
