#!/usr/bin/env bash
set -euo pipefail

make -j4

STEP="${STEP:-0.02}"
OUT="$(mktemp)"
DISK="$(mktemp)"
OUT_AHCI=""
DISK_AHCI1=""
DISK_AHCI2=""
OUT_REPLAY1=""
OUT_REPLAY2=""
SEED=""
EXT4_EXPECT=0

if command -v mke2fs >/dev/null 2>&1; then
  SEED="$(mktemp -d)"
  mkdir -p "$SEED/docs"
  mkdir -p "$SEED/empty"
  printf "hello from ext4\n" > "$SEED/hello.txt"
  printf "FuriOS ext4 root\n" > "$SEED/docs/readme.txt"
  ln -sf docs/readme.txt "$SEED/readme-link"
  truncate -s 64M "$DISK"
  if mke2fs -q -F -t ext4 -b 4096 -m 0 \
      -O extent,filetype,^has_journal,^64bit,^metadata_csum,^dir_index \
      -d "$SEED" "$DISK" 16128; then
    EXT4_EXPECT=1
  fi
else
  truncate -s 64M "$DISK"
fi

trap 'rm -f "${OUT:-}" "${DISK:-}" "${OUT_AHCI:-}" "${DISK_AHCI1:-}" "${DISK_AHCI2:-}" "${OUT_REPLAY1:-}" "${OUT_REPLAY2:-}"; if [[ -n "${SEED:-}" ]]; then rm -rf "$SEED"; fi' EXIT

(
  emit() {
    echo "$1"
    sleep "$STEP"
  }
  emit_raw() {
    printf '%b' "$1"
    sleep "$STEP"
  }

  sleep 0.8
  emit 'ls'
  emit 'ls /dev'
  emit 'echo dev-null > /dev/null'
  emit 'echo dev-tty > /dev/tty'
  emit 'kill -15 1'
  emit 'kill 9999'
  emit 'cd /bin'
  emit 'ls'
  emit 'cd /'
  emit 'pwd'
  emit 'mkdir /tmpx'
  emit 'mkdir /tmpx/d'
  emit 'touch /tmpx/a'
  emit 'cp /etc/motd /tmpx/m'
  emit 'mv /tmpx/m /tmpx/m2'
  emit 'cat /tmpx/m2'
  emit 'rm -rf /tmpx'
  emit 'ls /tmpx'
  emit 'cat /etc/motd'

  if [[ "$EXT4_EXPECT" -eq 1 ]]; then
    emit 'ls /mnt'
    emit 'cat /mnt/hello.txt'
    emit 'ls /mnt/docs'
    emit 'cat /mnt/docs/readme.txt'
    emit 'cat /mnt/readme-link'
    emit 'ln /mnt/hello.txt /mnt/hello.hard'
    emit 'cat /mnt/hello.hard'
    emit 'ln -s docs/readme.txt /mnt/docs-link'
    emit 'cat /mnt/docs-link'
    emit 'rm /mnt/hello.hard'
    emit 'rm /mnt/docs-link'
    emit 'echo extent-native > /mnt/hello.txt'
    emit 'echo extent-tail >> /mnt/hello.txt'
    emit 'cat /mnt/hello.txt'
    emit 'echo ext4-write > /mnt/new.txt'
    emit 'echo ext4-append >> /mnt/new.txt'
    emit 'cat /mnt/new.txt'
    emit 'echo ext4-reset > /mnt/new.txt'
    emit 'cat /mnt/new.txt'
    emit 'mv /mnt/new.txt /mnt/new2.txt'
    emit 'cat /mnt/new2.txt'
    emit 'rm /mnt/new2.txt'
    emit 'rmdir /mnt/empty'
    emit 'mkdir /mnt/hi'
    emit 'touch /mnt/hi/file1'
    emit 'ls /mnt/hi'
    emit 'rm /mnt/hi/file1'
    emit 'rmdir /mnt/hi'
    emit 'ls /mnt'
    emit 'cd /mnt'
    emit 'umount /mnt'
    emit 'cd /'
    emit 'umount /mnt'
    emit 'mount /dev/vda /mnt ext4'
    emit 'ls /mnt'
  fi

  emit 'echo "hello world" > /q1'
  emit 'cat < /q1'
  emit 'echo foo\ bar'
  emit 'echo hi | cat'
  emit 'cat /etc/motd | cat > /p1'
  emit 'cat /p1'
  emit 'echo one two | cat | cat'
  emit 'echo append-one > /app'
  emit 'echo append-two >> /app'
  emit 'cat /app'
  emit 'umount /mnt'
  emit 'mkfs.ext4 -L FuriOSVOL -O extents,64bit,sparse_super -m 1 -E stride=8 /dev/vda'
  emit 'fsck.ext4 /dev/vda'
  emit 'mount /dev/vda /mnt ext4'
  emit 'ls /mnt'
  emit 'mkdir /mnt/mkfst'
  emit 'touch /mnt/mkfst/a'
  emit 'ls /mnt/mkfst'
  emit 'echo "int helper(void){return 42;}" > /mnt/helper.c'
  emit 'echo "extern long sys_write(int, const void*, unsigned long);" > /mnt/main.c'
  emit 'echo "int main(void){" >> /mnt/main.c'
  emit 'echo "  sys_write(1, \"multi-ok\", 8);" >> /mnt/main.c'
  emit 'echo "  return 0;" >> /mnt/main.c'
  emit 'echo "}" >> /mnt/main.c'
  emit 'echo "#include <unistd.h>" > /mnt/hello.c'
  emit 'echo "int main(void){ write(1, \"hello-from-tcc\\n\", 15); return 0; }" >> /mnt/hello.c'
  emit 'tcc -g -Wall -c /mnt/helper.c -o /mnt/helper.o'
  emit 'ar rcs /mnt/libhelper.a /mnt/helper.o'
  emit 'ranlib /mnt/libhelper.a'
  emit 'tcc /mnt/hello.c -o /mnt/hello.elf'
  emit 'chmod 755 /mnt/hello.elf'
  emit '/mnt/hello.elf'
  emit 'tcc -MD -MF /mnt/main.d -c /mnt/main.c -o /mnt/main_dep.o'
  emit 'tcc -c /mnt/main.c -o /mnt/main.o'
  emit 'cat /mnt/main.d'
  emit 'tcc /mnt/main.o -L/lib -lc -o /mnt/tccmulti.elf'
  emit 'chmod 755 /mnt/tccmulti.elf'
  emit '/mnt/tccmulti.elf'
  emit 'echo "long pick(void) __attribute__((weak));" > /mnt/wlib.c'
  emit 'echo "long pick(void){ return 1; }" >> /mnt/wlib.c'
  emit 'echo "long pick(void){ return 7; }" > /mnt/strong.c'
  emit 'echo "#include <unistd.h>" > /mnt/wmain.c'
  emit 'echo "extern long pick(void);" >> /mnt/wmain.c'
  emit 'echo "int main(void){" >> /mnt/wmain.c'
  emit 'echo "  char out[3];" >> /mnt/wmain.c'
  emit 'echo "  out[0] = \"W\"[0];" >> /mnt/wmain.c'
  emit 'echo "  out[1] = (char)(\"0\"[0] + (int)pick());" >> /mnt/wmain.c'
  emit 'echo "  out[2] = \"\\n\"[0];" >> /mnt/wmain.c'
  emit 'echo "  write(1, out, 3);" >> /mnt/wmain.c'
  emit 'echo "  return 0;" >> /mnt/wmain.c'
  emit 'echo "}" >> /mnt/wmain.c'
  emit 'tcc -c /mnt/wlib.c -o /mnt/wlib.o'
  emit 'ar rcs /mnt/libw.a /mnt/wlib.o'
  emit 'ranlib /mnt/libw.a'
  emit 'tcc -c /mnt/strong.c -o /mnt/strong.o'
  emit 'tcc -c /mnt/wmain.c -o /mnt/wmain.o'
  emit 'tcc /mnt/wmain.o /mnt/strong.o -L/mnt -lw -o /mnt/wtest.elf'
  emit 'chmod 755 /mnt/wtest.elf'
  emit '/mnt/wtest.elf'
  emit 'echo "__thread int tlsv = 41;" > /mnt/tls_def.c'
  emit 'echo "int tls_read(void){ return tlsv; }" >> /mnt/tls_def.c'
  emit 'echo "void tls_inc(void){ tlsv++; }" >> /mnt/tls_def.c'
  emit 'echo "#include <unistd.h>" > /mnt/tls_main.c'
  emit 'echo "extern int tls_read(void);" >> /mnt/tls_main.c'
  emit 'echo "extern void tls_inc(void);" >> /mnt/tls_main.c'
  emit 'echo "int main(void){" >> /mnt/tls_main.c'
  emit 'echo "  tls_inc();" >> /mnt/tls_main.c'
  emit 'echo "  if (tls_read() != 42) return 41;" >> /mnt/tls_main.c'
  emit 'echo "  write(1, \"tls-ok\", 6);" >> /mnt/tls_main.c'
  emit 'echo "  return 0;" >> /mnt/tls_main.c'
  emit 'echo "}" >> /mnt/tls_main.c'
  emit 'tcc -c /mnt/tls_def.c -o /mnt/tls_def.o'
  emit 'tcc -c /mnt/tls_main.c -o /mnt/tls_main.o'
  emit 'tcc /mnt/tls_main.o /mnt/tls_def.o -o /mnt/tls.elf'
  emit 'chmod 755 /mnt/tls.elf'
  emit '/mnt/tls.elf'
  emit 'echo "__thread int shared_tls = 100;" > /mnt/tls_a.c'
  emit 'echo "int shared_get(void){ return shared_tls; }" >> /mnt/tls_a.c'
  emit 'echo "void shared_inc(void){ shared_tls++; }" >> /mnt/tls_a.c'
  emit 'echo "__thread int own_tls = 7;" > /mnt/tls_b.c'
  emit 'echo "extern __thread int shared_tls;" >> /mnt/tls_b.c'
  emit 'echo "int own_b_get(void){ return own_tls; }" >> /mnt/tls_b.c'
  emit 'echo "int own_b_inc(void){ own_tls++; return own_tls; }" >> /mnt/tls_b.c'
  emit 'echo "int bump_from_b(void){ shared_tls += 2; return shared_tls; }" >> /mnt/tls_b.c'
  emit 'echo "#include <unistd.h>" > /mnt/tls_dso_main.c'
  emit 'echo "extern int shared_get(void);" >> /mnt/tls_dso_main.c'
  emit 'echo "extern int own_b_get(void);" >> /mnt/tls_dso_main.c'
  emit 'echo "extern int own_b_inc(void);" >> /mnt/tls_dso_main.c'
  emit 'echo "extern int bump_from_b(void);" >> /mnt/tls_dso_main.c'
  emit 'echo "int main(void){" >> /mnt/tls_dso_main.c'
  emit 'echo "  if (shared_get() != 100) return 51;" >> /mnt/tls_dso_main.c'
  emit 'echo "  if (own_b_get() != 7) return 52;" >> /mnt/tls_dso_main.c'
  emit 'echo "  if (bump_from_b() != 102) return 53;" >> /mnt/tls_dso_main.c'
  emit 'echo "  if (shared_get() != 102) return 54;" >> /mnt/tls_dso_main.c'
  emit 'echo "  if (own_b_inc() != 8) return 55;" >> /mnt/tls_dso_main.c'
  emit 'echo "  write(1, \"tls-dso-ok\", 10);" >> /mnt/tls_dso_main.c'
  emit 'echo "  return 0;" >> /mnt/tls_dso_main.c'
  emit 'echo "}" >> /mnt/tls_dso_main.c'
  emit 'tcc -fPIC -shared /mnt/tls_a.c -o /mnt/libtls_a.so'
  emit 'tcc -fPIC -shared /mnt/tls_b.c -o /mnt/libtls_b.so'
  emit 'cp /mnt/libtls_a.so /lib/libtls_a.so'
  emit 'cp /mnt/libtls_b.so /lib/libtls_b.so'
  emit 'tcc /mnt/tls_dso_main.c -L/mnt -ltls_a -ltls_b -o /mnt/tlsdso.elf'
  emit 'chmod 755 /mnt/tlsdso.elf'
  emit '/mnt/tlsdso.elf'
  emit 'echo "int bad(void){ _Thread_local int x; return x; }" > /mnt/tls_bad.c'
  emit 'tcc -c /mnt/tls_bad.c -o /mnt/tls_bad.o'
  emit 'echo "extern long sys_write(int, const void*, unsigned long);" > /mnt/ctor.c'
  emit 'echo "static void __attribute__((constructor)) c(void){" >> /mnt/ctor.c'
  emit 'echo "  sys_write(1, \"ctor \", 5);" >> /mnt/ctor.c'
  emit 'echo "}" >> /mnt/ctor.c'
  emit 'echo "static void __attribute__((destructor)) d(void){" >> /mnt/ctor.c'
  emit 'echo "  sys_write(1, \" dtor\", 5);" >> /mnt/ctor.c'
  emit 'echo "}" >> /mnt/ctor.c'
  emit 'echo "int main(void){" >> /mnt/ctor.c'
  emit 'echo "  sys_write(1, \"main\", 4);" >> /mnt/ctor.c'
  emit 'echo "  return 0;" >> /mnt/ctor.c'
  emit 'echo "}" >> /mnt/ctor.c'
  emit 'tcc /mnt/ctor.c -o /mnt/ctor.elf'
  emit 'chmod 755 /mnt/ctor.elf'
  emit '/mnt/ctor.elf'
  emit 'echo "#include <unistd.h>" > /mnt/rt.c'
  emit 'echo "#include <sys/mman.h>" >> /mnt/rt.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/rt.c'
  emit 'echo "extern long sys_write(int, const void*, unsigned long);" >> /mnt/rt.c'
  emit 'echo "int main(void){" >> /mnt/rt.c'
  emit 'echo "  int fds[2]; if (pipe(fds) != 0) return 1;" >> /mnt/rt.c'
  emit 'echo "  int pid = fork();" >> /mnt/rt.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/rt.c'
  emit 'echo "    close(fds[0]);" >> /mnt/rt.c'
  emit 'echo "    write(fds[1],\"Z\",1);" >> /mnt/rt.c'
  emit 'echo "    _exit(0);" >> /mnt/rt.c'
  emit 'echo "  }" >> /mnt/rt.c'
  emit 'echo "  close(fds[1]);" >> /mnt/rt.c'
  emit 'echo "  char ch = 0;" >> /mnt/rt.c'
  emit 'echo "  if (read(fds[0], &ch, 1) != 1) return 3;" >> /mnt/rt.c'
  emit 'echo "  int st = 0;" >> /mnt/rt.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 4;" >> /mnt/rt.c'
  emit 'echo "  void *p = mmap(0, 4096, PROT_READ|PROT_WRITE," >> /mnt/rt.c'
  emit 'echo "                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);" >> /mnt/rt.c'
  emit 'echo "  if (p == MAP_FAILED) return 5;" >> /mnt/rt.c'
  emit 'echo "  ((char*)p)[0] = \"M\"[0];" >> /mnt/rt.c'
  emit 'echo "  write(1, p, 1);" >> /mnt/rt.c'
  emit 'echo "  munmap(p, 4096);" >> /mnt/rt.c'
  emit 'echo "  if (ch != \"Z\"[0]) return 6;" >> /mnt/rt.c'
  emit 'echo "  sys_write(1, \"rt-ok-Z\", 7);" >> /mnt/rt.c'
  emit 'echo "  return 0;" >> /mnt/rt.c'
  emit 'echo "}" >> /mnt/rt.c'
  emit 'tcc -g /mnt/rt.c -o /mnt/rt.elf'
  emit 'chmod 755 /mnt/rt.elf'
  emit '/mnt/rt.elf'
  emit 'echo "#include <signal.h>" > /mnt/sigf.c'
  emit 'echo "#include <unistd.h>" >> /mnt/sigf.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/sigf.c'
  emit 'echo "volatile int hits = 0;" >> /mnt/sigf.c'
  emit 'echo "volatile int depth = 0;" >> /mnt/sigf.c'
  emit 'echo "volatile int maxdepth = 0;" >> /mnt/sigf.c'
  emit 'echo "volatile int calls = 0;" >> /mnt/sigf.c'
  emit 'echo "static void on_term(int s){" >> /mnt/sigf.c'
  emit 'echo "  (void)s;" >> /mnt/sigf.c'
  emit 'echo "  hits++;" >> /mnt/sigf.c'
  emit 'echo "}" >> /mnt/sigf.c'
  emit 'echo "static void on_int(int s){" >> /mnt/sigf.c'
  emit 'echo "  (void)s;" >> /mnt/sigf.c'
  emit 'echo "  depth++;" >> /mnt/sigf.c'
  emit 'echo "  if (depth > maxdepth) maxdepth = depth;" >> /mnt/sigf.c'
  emit 'echo "  calls++;" >> /mnt/sigf.c'
  emit 'echo "  if (calls == 1) kill(getpid(), SIGINT);" >> /mnt/sigf.c'
  emit 'echo "  for (volatile int i = 0; i < 20000; i++) {}" >> /mnt/sigf.c'
  emit 'echo "  depth--;" >> /mnt/sigf.c'
  emit 'echo "}" >> /mnt/sigf.c'
  emit 'echo "int main(void){" >> /mnt/sigf.c'
  emit 'echo "  sigaction_t sa; int st = 0;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_mask = 0; sa.sa_restorer = 0;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_handler = (unsigned long)on_term;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_flags = SA_RESETHAND;" >> /mnt/sigf.c'
  emit 'echo "  if (sigaction(SIGTERM, &sa, 0) != 0) return 20;" >> /mnt/sigf.c'
  emit 'echo "  if (kill(getpid(), SIGTERM) != 0) return 21;" >> /mnt/sigf.c'
  emit 'echo "  if (hits != 1) return 22;" >> /mnt/sigf.c'
  emit 'echo "  int pid = fork();" >> /mnt/sigf.c'
  emit 'echo "  if (pid == 0) { kill(getpid(), SIGTERM); _exit(0); }" >> /mnt/sigf.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 23;" >> /mnt/sigf.c'
  emit 'echo "  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) return 24;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_handler = (unsigned long)on_int;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_flags = SA_NODEFER;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_mask = 0;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_restorer = 0;" >> /mnt/sigf.c'
  emit 'echo "  if (sigaction(SIGINT, &sa, 0) != 0) return 25;" >> /mnt/sigf.c'
  emit 'echo "  if (kill(getpid(), SIGINT) != 0) return 26;" >> /mnt/sigf.c'
  emit 'echo "  if (calls < 2 || maxdepth < 2) return 27;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_handler = SIG_DFL;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_flags = SA_NOCLDWAIT;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_mask = 0; sa.sa_restorer = 0;" >> /mnt/sigf.c'
  emit 'echo "  if (sigaction(SIGCHLD, &sa, 0) != 0) return 28;" >> /mnt/sigf.c'
  emit 'echo "  pid = fork();" >> /mnt/sigf.c'
  emit 'echo "  if (pid == 0) _exit(0);" >> /mnt/sigf.c'
  emit 'echo "  if (waitpid(pid, &st, 0) >= 0) return 29;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_handler = SIG_IGN;" >> /mnt/sigf.c'
  emit 'echo "  sa.sa_flags = 0;" >> /mnt/sigf.c'
  emit 'echo "  if (sigaction(SIGCHLD, &sa, 0) != 0) return 30;" >> /mnt/sigf.c'
  emit 'echo "  pid = fork();" >> /mnt/sigf.c'
  emit 'echo "  if (pid == 0) _exit(0);" >> /mnt/sigf.c'
  emit 'echo "  if (waitpid(pid, &st, 0) >= 0) return 31;" >> /mnt/sigf.c'
  emit 'echo "  write(1, \"sigflag-ok\", 10);" >> /mnt/sigf.c'
  emit 'echo "  return 0;" >> /mnt/sigf.c'
  emit 'echo "}" >> /mnt/sigf.c'
  emit 'tcc /mnt/sigf.c -o /mnt/sigf.elf'
  emit 'chmod 755 /mnt/sigf.elf'
  emit '/mnt/sigf.elf'
  emit 'echo "extern long sys_write(int, const void*, unsigned long);" > /mnt/auxv.c'
  emit 'echo "typedef struct { unsigned long t; unsigned long v; } auxv_t;" >> /mnt/auxv.c'
  emit 'echo "int main(int argc, char **argv, char **envp, auxv_t *auxv){" >> /mnt/auxv.c'
  emit 'echo "  (void)argc; (void)argv;" >> /mnt/auxv.c'
  emit 'echo "  if (!envp || envp[0] != 0) return 11;" >> /mnt/auxv.c'
  emit 'echo "  unsigned long have = 0;" >> /mnt/auxv.c'
  emit 'echo "  auxv_t *a = auxv;" >> /mnt/auxv.c'
  emit 'echo "  while (a && a->t) {" >> /mnt/auxv.c'
  emit 'echo "    if (a->t == 5 && a->v) have |= 1;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 6 && a->v == 4096) have |= 2;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 9 && a->v) have |= 4;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 31 && a->v) have |= 8;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 7 && a->v) have |= 16;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 17 && a->v == 100) have |= 32;" >> /mnt/auxv.c'
  emit 'echo "    else if (a->t == 25 && a->v) have |= 64;" >> /mnt/auxv.c'
  emit 'echo "    a++;" >> /mnt/auxv.c'
  emit 'echo "  }" >> /mnt/auxv.c'
  emit 'echo "  if (have != 127) return 13;" >> /mnt/auxv.c'
  emit 'echo "  sys_write(1, \"auxv-ok\", 7);" >> /mnt/auxv.c'
  emit 'echo "  return 0;" >> /mnt/auxv.c'
  emit 'echo "}" >> /mnt/auxv.c'
  emit 'tcc /mnt/auxv.c -o /mnt/auxv.elf'
  emit 'chmod 755 /mnt/auxv.elf'
  emit '/mnt/auxv.elf'
  emit 'echo "int crash_here(void){ volatile int *p=(int*)0x123; return *p; }" > /mnt/trapdbg.c'
  emit 'echo "int main(void){ return crash_here(); }" >> /mnt/trapdbg.c'
  emit 'tcc -g /mnt/trapdbg.c -o /mnt/trapdbg.elf'
  emit 'chmod 755 /mnt/trapdbg.elf'
  emit '/mnt/trapdbg.elf'
  emit 'nano /mnt/nano_paste.txt'
  emit_raw '\x1b[200~paste-line-1\npaste-line-2\x1b[201~'
  emit_raw '\x0f'
  emit_raw '\x18'
  emit 'cat /mnt/nano_paste.txt'
  emit 'sleep 1'
  emit 'echo slept-ok'
  emit 'echo io-start > /stress'
  for i in $(seq 1 6); do
    emit "echo line-$i >> /stress"
  done
  emit 'cat /stress | cat > /stress2'
  emit 'cat /stress2'
  emit 'rm /p1'
  emit 'rm /q1'
  emit 'rm /app'
  emit 'rm /stress'
  emit 'rm /stress2'
  emit 'echo smoke-test'
  emit 'exit'
  sleep 0.4
) | timeout 70 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK",format=raw,id=vd0 \
      -device virtio-blk-device,drive=vd0 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT" 2>&1 || true

cat "$OUT"

grep -q "FuriOS aarch64 boot" "$OUT"
grep -q "sh\$ ls" "$OUT"
grep -q "sh\$ ls /dev" "$OUT"
grep -q "null" "$OUT"
grep -q "zero" "$OUT"
grep -q "tty" "$OUT"
grep -q "vda" "$OUT"
grep -q "init" "$OUT"
grep -q "echo" "$OUT"
grep -q "FuriOS EL0 userspace online" "$OUT"

if [[ "$EXT4_EXPECT" -eq 1 ]]; then
  grep -q "\[ext4\] mounted rw at /mnt" "$OUT"
  grep -q "hello from ext4" "$OUT"
  grep -q "FuriOS ext4 root" "$OUT"
  grep -q "sh\$ cat /mnt/readme-link" "$OUT"
  grep -q "sh\$ cat /mnt/hello.hard" "$OUT"
  grep -q "sh\$ cat /mnt/docs-link" "$OUT"
  rg -q "^extent-native$" "$OUT"
  rg -q "^extent-tail$" "$OUT"
  grep -q "ext4-write" "$OUT"
  grep -q "ext4-append" "$OUT"
  grep -q "ext4-reset" "$OUT"
  grep -q "\[ext4\] unmounted /mnt" "$OUT"
  if rg -q "mv: rename failed|rmdir: failed /mnt/empty|rm: cannot remove /mnt/new2.txt|mkdir: cannot create /mnt/hi" "$OUT"; then
    echo "[test] FAIL: ext4 mutation flow failed" >&2
    exit 1
  fi
  if rg -q "^mount: failed$" "$OUT"; then
    echo "[test] FAIL: mount/umount flow failed" >&2
    exit 1
  fi
  grep -q "umount: failed" "$OUT"
fi

grep -q "smoke-test" "$OUT"
grep -q "kill: failed" "$OUT"
grep -q "ls: open failed" "$OUT"
grep -q "hello world" "$OUT"
grep -q "foo bar" "$OUT"
grep -q "hi" "$OUT"
grep -q "one two" "$OUT"
grep -q "append-one" "$OUT"
grep -q "append-two" "$OUT"
grep -q "mkfs.ext4: formatted" "$OUT"
grep -q "fsck.ext4: clean" "$OUT"
grep -q "mkfst" "$OUT"
grep -q "hello-from-tcc" "$OUT"
grep -q "sh\$ /mnt/tccmulti.elf" "$OUT"
grep -q "W7" "$OUT"
rg -q "^tls-ok" "$OUT"
grep -q "tls-dso-ok" "$OUT"
grep -q "block-scope thread-local must be static or extern" "$OUT"
grep -q "sh\$ /mnt/ctor.elf" "$OUT"
grep -q "sh\$ /mnt/rt.elf" "$OUT"
grep -q "sigflag-ok" "$OUT"
grep -q "auxv-ok" "$OUT"
grep -q "comm=trapdbg.elf" "$OUT"
grep -q "sym=crash_here+" "$OUT"
grep -q "paste-line-1" "$OUT"
grep -q "paste-line-2" "$OUT"
grep -q "main.c" "$OUT"
grep -q "slept-ok" "$OUT"
grep -q "line-6" "$OUT"
if rg -q "exec failed: /mnt/tccmulti.elf|exec failed: /mnt/tls.elf|exec failed: /mnt/tlsdso.elf|exec failed: /mnt/ctor.elf|exec failed: /mnt/rt.elf|exec failed: /mnt/sigf.elf|exec failed: /mnt/auxv.elf" "$OUT"; then
  echo "[test] FAIL: toolchain runtime compatibility flow failed" >&2
  exit 1
fi
if rg -q "\\[trap\\] sync pid=.*comm=tls\\.elf" "$OUT"; then
  echo "[test] FAIL: TLS runtime execution trapped" >&2
  exit 1
fi
if rg -q "fork failed|\[panic\]|no runnable" "$OUT"; then
  echo "[test] FAIL: runtime error detected" >&2
  exit 1
fi

OUT_REPLAY1="$(mktemp)"
(
  emit() {
    echo "$1"
    sleep "$STEP"
  }
  sleep 0.8
  emit 'echo replay-one > /mnt/replay.txt'
  emit 'echo replay-two >> /mnt/replay.txt'
  emit 'cat /mnt/replay.txt'
  sleep 2.0
) | timeout 12 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK",format=raw,id=vd0 \
      -device virtio-blk-device,drive=vd0 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT_REPLAY1" 2>&1 || true

OUT_REPLAY2="$(mktemp)"
(
  emit() {
    echo "$1"
    sleep "$STEP"
  }
  sleep 0.8
  emit 'cat /mnt/replay.txt'
  emit 'exit'
  sleep 0.3
) | timeout 16 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK",format=raw,id=vd0 \
      -device virtio-blk-device,drive=vd0 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT_REPLAY2" 2>&1 || true

cat "$OUT_REPLAY2"
grep -q "replay-one" "$OUT_REPLAY2"
grep -q "replay-two" "$OUT_REPLAY2"
if rg -q "mount: failed|cat: open failed|\\[panic\\]|no runnable" "$OUT_REPLAY2"; then
  echo "[test] FAIL: ext4 replay durability gate failed" >&2
  exit 1
fi

OUT_AHCI="$(mktemp)"
DISK_AHCI1="$(mktemp)"
DISK_AHCI2="$(mktemp)"
truncate -s 64M "$DISK_AHCI1"
truncate -s 64M "$DISK_AHCI2"

# MBR: one Linux partition at LBA 2048, size 32768 sectors.
printf '\x00\x00\x00\x00\x83\x00\x00\x00\x00\x08\x00\x00\x00\x80\x00\x00' \
  | dd of="$DISK_AHCI1" bs=1 seek=446 conv=notrunc status=none
printf '\x55\xAA' | dd of="$DISK_AHCI1" bs=1 seek=510 conv=notrunc status=none

(
  emit() {
    echo "$1"
    sleep "$STEP"
  }

  sleep 0.8
  emit 'ls /dev'
  emit 'exit'
  sleep 0.3
) | timeout 18 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -drive if=none,file="$DISK_AHCI1",format=raw,id=sd0 \
      -drive if=none,file="$DISK_AHCI2",format=raw,id=sd1 \
      -device ich9-ahci,id=ahci0 \
      -device ide-hd,drive=sd0,bus=ahci0.0 \
      -device ide-hd,drive=sd1,bus=ahci0.1 \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT_AHCI" 2>&1 || true

cat "$OUT_AHCI"
grep -q "\[ahci\] ready" "$OUT_AHCI"
grep -q "mode=irq" "$OUT_AHCI"
grep -q "sda" "$OUT_AHCI"
grep -q "sdb" "$OUT_AHCI"
grep -q "sda1" "$OUT_AHCI"
if rg -q "^vda$" "$OUT_AHCI"; then
  echo "[test] FAIL: /dev/vda should not exist without virtio disk" >&2
  exit 1
fi
if rg -q "fork failed|\[panic\]|no runnable" "$OUT_AHCI"; then
  echo "[test] FAIL: AHCI runtime error detected" >&2
  exit 1
fi

echo "[test] PASS"
