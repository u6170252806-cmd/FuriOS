/* FuriOS TinyCC port configuration */
#ifndef TINYCC_CONFIG_H
#define TINYCC_CONFIG_H

#define TCC_VERSION "0.9.28-furios"

#define CC_NAME CC_gcc
#define GCC_MAJOR 15
#define GCC_MINOR 2

#if !(TCC_TARGET_I386 || TCC_TARGET_X86_64 || TCC_TARGET_ARM || TCC_TARGET_ARM64 || TCC_TARGET_RISCV64 || TCC_TARGET_C67)
#define TCC_TARGET_ARM64 1
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_BACKTRACE 0
#endif

#ifndef CONFIG_TCCDIR
#define CONFIG_TCCDIR "/"
#endif

#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_DWARF_VERSION 2

#define CONFIG_TCC_LIBPATHS "/lib:/usr/lib"
#define CONFIG_TCC_SYSINCLUDEPATHS "/include:/usr/include"
#define CONFIG_TCC_CRTPREFIX "/lib:/usr/lib"
#define CONFIG_TCC_ELFINTERP "/lib/ld-furios.so"
#define CONFIG_TCC_CRTPREFIX "/lib:/usr/lib"
#define CONFIG_TCC_CRT1 "/lib/crt1.o"
#define CONFIG_TCC_CRTI "/lib/crti.o"
#define CONFIG_TCC_CRTN "/lib/crtn.o"
#define CONFIG_TCC_LIBTCC1 "libtcc1.a"
#define CONFIG_TCC_SO 0

#endif
