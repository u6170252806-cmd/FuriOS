#!/usr/bin/env bash
set -euo pipefail

make -j4

STEP="${STEP:-0.02}"
OUT="$(mktemp)"
DISK="$(mktemp)"
OUT_AHCI=""
OUT_NANO=""
DISK_AHCI1=""
DISK_AHCI2=""
OUT_REPLAY1=""
OUT_REPLAY2=""
SEED=""
EXT4_EXPECT=0
DNS_PID=""
TCP_PID=""
UDP_PID=""

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

trap 'if [[ -n "${DNS_PID:-}" ]]; then kill "${DNS_PID}" >/dev/null 2>&1 || true; fi; if [[ -n "${TCP_PID:-}" ]]; then kill "${TCP_PID}" >/dev/null 2>&1 || true; fi; if [[ -n "${UDP_PID:-}" ]]; then kill "${UDP_PID}" >/dev/null 2>&1 || true; fi; rm -f "${OUT:-}" "${OUT_NANO:-}" "${DISK:-}" "${OUT_AHCI:-}" "${DISK_AHCI1:-}" "${DISK_AHCI2:-}" "${OUT_REPLAY1:-}" "${OUT_REPLAY2:-}"; if [[ -n "${SEED:-}" ]]; then rm -rf "$SEED"; fi' EXIT

python3 - <<'PY' &
import socket, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 15353))
target = b"\x06furios\x04test\x00"
answer_ip = bytes([10, 0, 2, 2])
while True:
    data, addr = sock.recvfrom(512)
    if len(data) < 12:
        continue
    qdcount = struct.unpack("!H", data[4:6])[0]
    if qdcount != 1:
        continue
    off = 12
    while off < len(data):
        n = data[off]
        off += 1
        if n == 0:
            break
        if off + n > len(data):
            off = None
            break
        off += n
    if off is None or off + 4 > len(data):
        continue
    qname = data[12:off]
    question = data[12:off + 4]
    flags = 0x8180 if qname == target else 0x8183
    ancount = 1 if qname == target else 0
    header = data[0:2] + struct.pack("!HHHHH", flags, 1, ancount, 0, 0)
    reply = header + question
    if ancount:
        reply += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 30, 4) + answer_ip
    sock.sendto(reply, addr)
PY
DNS_PID=$!

python3 - <<'PY' &
import socket

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 18080))
srv.listen(8)
while True:
    conn, _ = srv.accept()
    try:
        conn.recv(1024)
        conn.sendall(b"host-tcp-ok\n")
    finally:
        conn.close()
PY
TCP_PID=$!

python3 - <<'PY' &
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 18081))
while True:
    data, addr = sock.recvfrom(2048)
    sock.sendto(b"host-udp-ok\n", addr)
PY
UDP_PID=$!

sleep 0.2

(
  emit() {
    printf '%s\n' "$1"
    sleep "$STEP"
  }
  emit_raw() {
    printf '%b' "$1"
    sleep "$STEP"
  }

  sleep 0.8
  emit 'ls'
  emit 'ls /dev'
  emit 'ifconfig'
  emit 'ping 10.0.2.2 1'
  emit 'route'
  emit 'ifconfig lo 127.0.0.1 255.0.0.0'
  emit 'ifconfig lo down'
  emit 'ifconfig lo 127.0.0.1 255.0.0.0'
  emit 'route add 10.10.0.0 255.255.0.0 0.0.0.0 lo'
  emit 'route add default 10.0.0.1 lo'
  emit 'route'
  emit 'route get 127.0.0.1'
  emit 'arp'
  emit 'arp flush'
  emit 'ifconfig eth0 dhcp'
  emit 'ifconfig'
  emit 'cat /etc/resolv.conf'
  emit 'cat /etc/hosts'
  emit 'ping localhost 1'
  emit 'echo nameserver 10.0.2.2:15353 > /etc/resolv.conf'
  emit 'nslookup furios.test'
  emit 'route get 10.0.2.2'
  emit 'echo guest-hosts | nc host.qemu 18080'
  emit 'echo guest-tcp | nc furios.test 18080'
  emit 'echo guest-udp | nc -u furios.test 18081'
  emit 'route del default'
  emit 'route del 10.10.0.0 255.255.0.0'
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
  emit 'echo tmpx-nested > /tmpx/a'
  emit 'cat /tmpx/a'
  emit 'cp /etc/motd /tmpx/m'
  emit 'mv /tmpx/m /tmpx/m2'
  emit 'cat /tmpx/m2'
  emit 'rm -rf /tmpx'
  emit 'ls /tmpx'
  emit 'cat /etc/motd'
  emit 'ping 127.0.0.1 1'

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
  emit 'echo append-one > /mnt/app'
  emit 'echo append-two >> /mnt/app'
  emit 'cat /mnt/app'
  emit 'umount /mnt'
  emit 'mkfs.ext4 -L FuriOSVOL -O extents,64bit,sparse_super -m 1 -E stride=8 /dev/vda'
  emit 'fsck.ext4 /dev/vda'
  emit 'mount /dev/vda /mnt ext4'
  emit 'ls /mnt'
  emit 'mkdir /mnt/mkfst'
  emit 'touch /mnt/mkfst/a'
  emit 'ls /mnt/mkfst'
  emit 'mkdir /mnt/cachechk'
  emit 'echo cache-bravo > /mnt/cachechk/b'
  emit 'mv /mnt/cachechk/b /mnt/cachechk/b2'
  emit 'echo cache-gamma > /mnt/cachechk/c'
  emit 'umount /mnt'
  emit 'mount /dev/vda /mnt ext4'
  emit 'cat /mnt/cachechk/b2'
  emit 'cat /mnt/cachechk/c'
  emit 'echo "int helper(void){return 42;}" > /mnt/helper.c'
  emit 'echo "extern long sys_write(int, const void*, unsigned long);" > /mnt/main.c'
  emit 'echo "int main(void){" >> /mnt/main.c'
  emit 'echo "  sys_write(1, \"multi-ok\", 8);" >> /mnt/main.c'
  emit 'echo "  return 0;" >> /mnt/main.c'
  emit 'echo "}" >> /mnt/main.c'
  emit 'echo "#include <unistd.h>" > /mnt/hello.c'
  emit 'echo "int main(void){ write(1, \"hello-from-tcc\\n\", 15); return 0; }" >> /mnt/hello.c'
  emit 'echo "#include <unistd.h>" > /mnt/tcp.c'
  emit 'echo "#include <sys/socket.h>" >> /mnt/tcp.c'
  emit 'echo "#include <netinet/in.h>" >> /mnt/tcp.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/tcp.c'
  emit 'echo "int main(void){" >> /mnt/tcp.c'
  emit 'echo "  int l = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcp.c'
  emit 'echo "  sockaddr_in a; unsigned long alen = sizeof(a);" >> /mnt/tcp.c'
  emit 'echo "  sockaddr_in p; unsigned long plen = sizeof(p);" >> /mnt/tcp.c'
  emit 'echo "  char buf[4]; int st = 0;" >> /mnt/tcp.c'
  emit 'echo "  if (l < 0) return 1;" >> /mnt/tcp.c'
  emit 'echo "  a.sin_family = AF_INET;" >> /mnt/tcp.c'
  emit 'echo "  a.sin_port = 0;" >> /mnt/tcp.c'
  emit 'echo "  a.sin_addr = htonl(" >> /mnt/tcp.c'
  emit 'echo "      INADDR_LOOPBACK);" >> /mnt/tcp.c'
  emit 'echo "  if (bind(l, (sockaddr*)&a, sizeof(a)) != 0) return 2;" >> /mnt/tcp.c'
  emit 'echo "  if (getsockname(l, (sockaddr*)&a, &alen) != 0) return 3;" >> /mnt/tcp.c'
  emit 'echo "  if (listen(l, 1) != 0) return 4;" >> /mnt/tcp.c'
  emit 'echo "  int pid = fork();" >> /mnt/tcp.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/tcp.c'
  emit 'echo "    int c = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcp.c'
  emit 'echo "    if (c < 0) _exit(11);" >> /mnt/tcp.c'
  emit 'echo "    if (connect(c, (sockaddr*)&a, sizeof(a)) != 0) _exit(12);" >> /mnt/tcp.c'
  emit 'echo "    if (getsockname(c, (sockaddr*)&p, &plen) != 0) _exit(13);" >> /mnt/tcp.c'
  emit 'echo "    if (getpeername(c, (sockaddr*)&p, &plen) != 0) _exit(14);" >> /mnt/tcp.c'
  emit 'echo "    if (write(c, \"tcp\", 3) != 3) _exit(15);" >> /mnt/tcp.c'
  emit 'echo "    if (shutdown(c, SHUT_WR) != 0) _exit(16);" >> /mnt/tcp.c'
  emit 'echo "    _exit(0);" >> /mnt/tcp.c'
  emit 'echo "  }" >> /mnt/tcp.c'
  emit 'echo "  if (pid < 0) return 5;" >> /mnt/tcp.c'
  emit 'echo "  int s = accept(l, (sockaddr*)&p, &plen);" >> /mnt/tcp.c'
  emit 'echo "  if (s < 0) return 6;" >> /mnt/tcp.c'
  emit 'echo "  if (getsockname(s, (sockaddr*)&p, &plen) != 0) return 7;" >> /mnt/tcp.c'
  emit 'echo "  if (getpeername(s, (sockaddr*)&p, &plen) != 0) return 8;" >> /mnt/tcp.c'
  emit 'echo "  if (read(s, buf, 3) != 3) return 9;" >> /mnt/tcp.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 10;" >> /mnt/tcp.c'
  emit 'echo "  if (buf[0] != \"t\"[0]) return 11;" >> /mnt/tcp.c'
  emit 'echo "  if (buf[1] != \"c\"[0]) return 12;" >> /mnt/tcp.c'
  emit 'echo "  if (buf[2] != \"p\"[0]) return 13;" >> /mnt/tcp.c'
  emit 'echo "  write(1, \"tcp-ok\\n\", 7);" >> /mnt/tcp.c'
  emit 'echo "  return 0;" >> /mnt/tcp.c'
  emit 'echo "}" >> /mnt/tcp.c'
  emit 'echo "#include <unistd.h>" > /mnt/tcpnb.c'
  emit 'echo "#include <poll.h>" >> /mnt/tcpnb.c'
  emit 'echo "#include <sys/socket.h>" >> /mnt/tcpnb.c'
  emit 'echo "#include <netinet/in.h>" >> /mnt/tcpnb.c'
  emit 'echo "int main(void){" >> /mnt/tcpnb.c'
  emit 'echo "  int l = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0);" >> /mnt/tcpnb.c'
  emit 'echo "  sockaddr_in a; unsigned long alen = sizeof(a);" >> /mnt/tcpnb.c'
  emit 'echo "  sockaddr_in p; unsigned long plen = sizeof(p);" >> /mnt/tcpnb.c'
  emit 'echo "  pollfd fd;" >> /mnt/tcpnb.c'
  emit 'echo "  if (l < 0) return 1;" >> /mnt/tcpnb.c'
  emit 'echo "  a.sin_family = AF_INET;" >> /mnt/tcpnb.c'
  emit 'echo "  a.sin_port = 0;" >> /mnt/tcpnb.c'
  emit 'echo "  a.sin_addr = htonl(" >> /mnt/tcpnb.c'
  emit 'echo "      INADDR_LOOPBACK);" >> /mnt/tcpnb.c'
  emit 'echo "  if (bind(l, (sockaddr*)&a, sizeof(a)) != 0) return 2;" >> /mnt/tcpnb.c'
  emit 'echo "  if (getsockname(l, (sockaddr*)&a, &alen) != 0) return 3;" >> /mnt/tcpnb.c'
  emit 'echo "  if (listen(l, SOMAXCONN) != 0) return 4;" >> /mnt/tcpnb.c'
  emit 'echo "  fd.fd = l; fd.events = POLLIN; fd.revents = 0;" >> /mnt/tcpnb.c'
  emit 'echo "  if (poll(&fd, 1, 0) != 0) return 5;" >> /mnt/tcpnb.c'
  emit 'echo "  if (accept(l, 0, 0) >= 0) return 6;" >> /mnt/tcpnb.c'
  emit 'echo "  int pid = fork();" >> /mnt/tcpnb.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/tcpnb.c'
  emit 'echo "    int c = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcpnb.c'
  emit 'echo "    if (connect(c, (sockaddr*)&a, sizeof(a)) != 0) _exit(1);" >> /mnt/tcpnb.c'
  emit 'echo "    _exit(0);" >> /mnt/tcpnb.c'
  emit 'echo "  }" >> /mnt/tcpnb.c'
  emit 'echo "  if (pid < 0) return 7;" >> /mnt/tcpnb.c'
  emit 'echo "  if (poll(&fd, 1, 200) != 1) return 8;" >> /mnt/tcpnb.c'
  emit 'echo "  if (accept(l, (sockaddr*)&p, &plen) < 0) return 9;" >> /mnt/tcpnb.c'
  emit 'echo "  write(1, \"tcp-nb-ok\\n\", 10);" >> /mnt/tcpnb.c'
  emit 'echo "  return 0;" >> /mnt/tcpnb.c'
  emit 'echo "}" >> /mnt/tcpnb.c'
  emit 'echo "#include <unistd.h>" > /mnt/tcpclose.c'
  emit 'echo "#include <sys/socket.h>" >> /mnt/tcpclose.c'
  emit 'echo "#include <netinet/in.h>" >> /mnt/tcpclose.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/tcpclose.c'
  emit 'echo "int main(void){" >> /mnt/tcpclose.c'
  emit 'echo "  int l = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcpclose.c'
  emit 'echo "  sockaddr_in a; unsigned long alen = sizeof(a);" >> /mnt/tcpclose.c'
  emit 'echo "  sockaddr_in p; unsigned long plen = sizeof(p);" >> /mnt/tcpclose.c'
  emit 'echo "  char buf[4]; int st = 0;" >> /mnt/tcpclose.c'
  emit 'echo "  if (l < 0) return 1;" >> /mnt/tcpclose.c'
  emit 'echo "  a.sin_family = AF_INET;" >> /mnt/tcpclose.c'
  emit 'echo "  a.sin_port = 0;" >> /mnt/tcpclose.c'
  emit 'echo "  a.sin_addr = htonl(INADDR_LOOPBACK);" >> /mnt/tcpclose.c'
  emit 'echo "  if (bind(l, (sockaddr*)&a, sizeof(a)) != 0) return 2;" >> /mnt/tcpclose.c'
  emit 'echo "  if (getsockname(l, (sockaddr*)&a, &alen) != 0) return 3;" >> /mnt/tcpclose.c'
  emit 'echo "  if (listen(l, 1) != 0) return 4;" >> /mnt/tcpclose.c'
  emit 'echo "  int pid = fork();" >> /mnt/tcpclose.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/tcpclose.c'
  emit 'echo "    int c = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcpclose.c'
  emit 'echo "    char in[4];" >> /mnt/tcpclose.c'
  emit 'echo "    if (c < 0) _exit(11);" >> /mnt/tcpclose.c'
  emit 'echo "    if (connect(c, (sockaddr*)&a, sizeof(a)) != 0) _exit(12);" >> /mnt/tcpclose.c'
  emit 'echo "    if (write(c, \"hi\", 2) != 2) _exit(13);" >> /mnt/tcpclose.c'
  emit 'echo "    if (shutdown(c, SHUT_WR) != 0) _exit(14);" >> /mnt/tcpclose.c'
  emit 'echo "    if (read(c, in, 1) != 1 || in[0] != \"!\"[0]) _exit(15);" >> /mnt/tcpclose.c'
  emit 'echo "    if (read(c, in, 1) != 0) _exit(16);" >> /mnt/tcpclose.c'
  emit 'echo "    _exit(0);" >> /mnt/tcpclose.c'
  emit 'echo "  }" >> /mnt/tcpclose.c'
  emit 'echo "  if (pid < 0) return 5;" >> /mnt/tcpclose.c'
  emit 'echo "  int s = accept(l, (sockaddr*)&p, &plen);" >> /mnt/tcpclose.c'
  emit 'echo "  if (s < 0) return 6;" >> /mnt/tcpclose.c'
  emit 'echo "  if (read(s, buf, 2) != 2) return 7;" >> /mnt/tcpclose.c'
  emit 'echo "  if (read(s, buf, 1) != 0) return 8;" >> /mnt/tcpclose.c'
  emit 'echo "  if (write(s, \"!\", 1) != 1) return 9;" >> /mnt/tcpclose.c'
  emit 'echo "  close(s);" >> /mnt/tcpclose.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 10;" >> /mnt/tcpclose.c'
  emit 'echo "  write(1, \"tcp-close-ok\\n\", 13);" >> /mnt/tcpclose.c'
  emit 'echo "  return 0;" >> /mnt/tcpclose.c'
  emit 'echo "}" >> /mnt/tcpclose.c'
  emit 'echo "#include <unistd.h>" > /mnt/udp.c'
  emit 'echo "#include <sys/socket.h>" >> /mnt/udp.c'
  emit 'echo "#include <netinet/in.h>" >> /mnt/udp.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/udp.c'
  emit 'echo "int main(void){" >> /mnt/udp.c'
  emit 'echo "  int s = socket(AF_INET, SOCK_DGRAM, 0);" >> /mnt/udp.c'
  emit 'echo "  sockaddr_in a; unsigned long alen = sizeof(a);" >> /mnt/udp.c'
  emit 'echo "  sockaddr_in src; unsigned long slen = sizeof(src);" >> /mnt/udp.c'
  emit 'echo "  char buf[4]; int st = 0;" >> /mnt/udp.c'
  emit 'echo "  if (s < 0) return 1;" >> /mnt/udp.c'
  emit 'echo "  a.sin_family = AF_INET;" >> /mnt/udp.c'
  emit 'echo "  a.sin_port = 0;" >> /mnt/udp.c'
  emit 'echo "  a.sin_addr = htonl(" >> /mnt/udp.c'
  emit 'echo "      INADDR_LOOPBACK);" >> /mnt/udp.c'
  emit 'echo "  if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) return 2;" >> /mnt/udp.c'
  emit 'echo "  if (getsockname(s, (sockaddr*)&a, &alen) != 0) return 3;" >> /mnt/udp.c'
  emit 'echo "  int pid = fork();" >> /mnt/udp.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/udp.c'
  emit 'echo "    int c = socket(AF_INET, SOCK_DGRAM, 0);" >> /mnt/udp.c'
  emit 'echo "    if (c < 0) _exit(11);" >> /mnt/udp.c'
  emit 'echo "    long n = sendto(c, \"udp\", 3, 0," >> /mnt/udp.c'
  emit 'echo "                   (sockaddr*)&a, sizeof(a));" >> /mnt/udp.c'
  emit 'echo "    if (n != 3) _exit(12);" >> /mnt/udp.c'
  emit 'echo "    _exit(0);" >> /mnt/udp.c'
  emit 'echo "  }" >> /mnt/udp.c'
  emit 'echo "  if (pid < 0) return 4;" >> /mnt/udp.c'
  emit 'echo "  if (recvfrom(s, buf, 3, 0," >> /mnt/udp.c'
  emit 'echo "               (sockaddr*)&src, &slen) != 3) return 5;" >> /mnt/udp.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 6;" >> /mnt/udp.c'
  emit 'echo "  if (buf[0] != \"u\"[0]) return 7;" >> /mnt/udp.c'
  emit 'echo "  if (buf[1] != \"d\"[0]) return 8;" >> /mnt/udp.c'
  emit 'echo "  if (buf[2] != \"p\"[0]) return 9;" >> /mnt/udp.c'
  emit 'echo "  write(1, \"udp-ok\\n\", 7);" >> /mnt/udp.c'
  emit 'echo "  return 0;" >> /mnt/udp.c'
  emit 'echo "}" >> /mnt/udp.c'
  emit 'echo "#include <unistd.h>" > /mnt/nctcp.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/nctcp.c'
  emit 'echo "static int slurp(int fd, char *buf, int cap){" >> /mnt/nctcp.c'
  emit 'echo "  int off = 0;" >> /mnt/nctcp.c'
  emit 'echo "  for (;;) {" >> /mnt/nctcp.c'
  emit 'echo "    long n;" >> /mnt/nctcp.c'
  emit 'echo "    if (off >= cap) break;" >> /mnt/nctcp.c'
  emit 'echo "    n = read(fd, buf + off, (unsigned long)(cap - off));" >> /mnt/nctcp.c'
  emit 'echo "    if (n <= 0) break;" >> /mnt/nctcp.c'
  emit 'echo "    off += (int)n;" >> /mnt/nctcp.c'
  emit 'echo "  }" >> /mnt/nctcp.c'
  emit 'echo "  return off;" >> /mnt/nctcp.c'
  emit 'echo "}" >> /mnt/nctcp.c'
  emit 'echo "int main(void){" >> /mnt/nctcp.c'
  emit 'echo "  int si[2], so[2], ci[2]; int st = 0;" >> /mnt/nctcp.c'
  emit 'echo "  char sb[64];" >> /mnt/nctcp.c'
  emit 'echo "  if (pipe(si) || pipe(so) || pipe(ci)) return 1;" >> /mnt/nctcp.c'
  emit 'echo "  int sp = fork();" >> /mnt/nctcp.c'
  emit 'echo "  if (sp == 0) {" >> /mnt/nctcp.c'
  emit 'echo "    char *av[] = {\"nc\", \"-l\", \"2345\", 0};" >> /mnt/nctcp.c'
  emit 'echo "    dup2(si[0], 0);" >> /mnt/nctcp.c'
  emit 'echo "    dup2(so[1], 1);" >> /mnt/nctcp.c'
  emit 'echo "    close(si[1]);" >> /mnt/nctcp.c'
  emit 'echo "    close(so[0]);" >> /mnt/nctcp.c'
  emit 'echo "    close(ci[0]);" >> /mnt/nctcp.c'
  emit 'echo "    close(ci[1]);" >> /mnt/nctcp.c'
  emit 'echo "    execv(\"/bin/nc\", av); _exit(11);" >> /mnt/nctcp.c'
  emit 'echo "  }" >> /mnt/nctcp.c'
  emit 'echo "  if (sp < 0) return 2;" >> /mnt/nctcp.c'
  emit 'echo "  int cp = fork();" >> /mnt/nctcp.c'
  emit 'echo "  if (cp == 0) {" >> /mnt/nctcp.c'
  emit 'echo "    char *av[] = {\"nc\", \"127.0.0.1\", \"2345\", 0};" >> /mnt/nctcp.c'
  emit 'echo "    dup2(ci[0], 0);" >> /mnt/nctcp.c'
  emit 'echo "    close(ci[1]);" >> /mnt/nctcp.c'
  emit 'echo "    close(si[0]);" >> /mnt/nctcp.c'
  emit 'echo "    close(si[1]);" >> /mnt/nctcp.c'
  emit 'echo "    close(so[0]);" >> /mnt/nctcp.c'
  emit 'echo "    close(so[1]);" >> /mnt/nctcp.c'
  emit 'echo "    execv(\"/bin/nc\", av); _exit(12);" >> /mnt/nctcp.c'
  emit 'echo "  }" >> /mnt/nctcp.c'
  emit 'echo "  if (cp < 0) return 3;" >> /mnt/nctcp.c'
  emit 'echo "  close(si[0]);" >> /mnt/nctcp.c'
  emit 'echo "  close(so[1]);" >> /mnt/nctcp.c'
  emit 'echo "  close(ci[0]);" >> /mnt/nctcp.c'
  emit 'echo "  close(si[1]);" >> /mnt/nctcp.c'
  emit 'echo "  write(ci[1], \"from-client\\n\", 12);" >> /mnt/nctcp.c'
  emit 'echo "  close(ci[1]);" >> /mnt/nctcp.c'
  emit 'echo "  int sn = slurp(so[0], sb, 63);" >> /mnt/nctcp.c'
  emit 'echo "  if (waitpid(sp, &st, 0) < 0) return 4;" >> /mnt/nctcp.c'
  emit 'echo "  if (waitpid(cp, &st, 0) < 0) return 5;" >> /mnt/nctcp.c'
  emit 'echo "  if (sn < 12) return 6;" >> /mnt/nctcp.c'
  emit 'echo "  if (sb[0] != \"f\"[0]) return 7;" >> /mnt/nctcp.c'
  emit 'echo "  write(1, \"nc-tcp-ok\\n\", 10);" >> /mnt/nctcp.c'
  emit 'echo "  return 0;" >> /mnt/nctcp.c'
  emit 'echo "}" >> /mnt/nctcp.c'
  emit 'echo "#include <unistd.h>" > /mnt/ncudp.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/ncudp.c'
  emit 'echo "#include <signal.h>" >> /mnt/ncudp.c'
  emit 'echo "int main(void){" >> /mnt/ncudp.c'
  emit 'echo "  int si[2], so[2], ci[2]; int st = 0;" >> /mnt/ncudp.c'
  emit 'echo "  char sb[64];" >> /mnt/ncudp.c'
  emit 'echo "  if (pipe(si) || pipe(so) || pipe(ci)) return 1;" >> /mnt/ncudp.c'
  emit 'echo "  int sp = fork();" >> /mnt/ncudp.c'
  emit 'echo "  if (sp == 0) {" >> /mnt/ncudp.c'
  emit 'echo "    char *av[] = {\"nc\", \"-u\", \"-l\", \"2346\", 0};" >> /mnt/ncudp.c'
  emit 'echo "    dup2(si[0], 0);" >> /mnt/ncudp.c'
  emit 'echo "    dup2(so[1], 1);" >> /mnt/ncudp.c'
  emit 'echo "    close(si[1]);" >> /mnt/ncudp.c'
  emit 'echo "    close(so[0]);" >> /mnt/ncudp.c'
  emit 'echo "    close(ci[0]);" >> /mnt/ncudp.c'
  emit 'echo "    close(ci[1]);" >> /mnt/ncudp.c'
  emit 'echo "    execv(\"/bin/nc\", av); _exit(11);" >> /mnt/ncudp.c'
  emit 'echo "  }" >> /mnt/ncudp.c'
  emit 'echo "  if (sp < 0) return 2;" >> /mnt/ncudp.c'
  emit 'echo "  sleep(20);" >> /mnt/ncudp.c'
  emit 'echo "  int cp = fork();" >> /mnt/ncudp.c'
  emit 'echo "  if (cp == 0) {" >> /mnt/ncudp.c'
  emit 'echo "    char *av[] = {\"nc\", \"-u\", \"127.0.0.1\", \"2346\", 0};" >> /mnt/ncudp.c'
  emit 'echo "    dup2(ci[0], 0);" >> /mnt/ncudp.c'
  emit 'echo "    close(ci[1]);" >> /mnt/ncudp.c'
  emit 'echo "    close(si[0]);" >> /mnt/ncudp.c'
  emit 'echo "    close(si[1]);" >> /mnt/ncudp.c'
  emit 'echo "    close(so[0]);" >> /mnt/ncudp.c'
  emit 'echo "    close(so[1]);" >> /mnt/ncudp.c'
  emit 'echo "    execv(\"/bin/nc\", av); _exit(12);" >> /mnt/ncudp.c'
  emit 'echo "  }" >> /mnt/ncudp.c'
  emit 'echo "  if (cp < 0) return 3;" >> /mnt/ncudp.c'
  emit 'echo "  close(si[0]);" >> /mnt/ncudp.c'
  emit 'echo "  close(so[1]);" >> /mnt/ncudp.c'
  emit 'echo "  close(ci[0]);" >> /mnt/ncudp.c'
  emit 'echo "  close(si[1]);" >> /mnt/ncudp.c'
  emit 'echo "  write(ci[1], \"udp-one\\n\", 8);" >> /mnt/ncudp.c'
  emit 'echo "  close(ci[1]);" >> /mnt/ncudp.c'
  emit 'echo "  long sn = read(so[0], sb, 63);" >> /mnt/ncudp.c'
  emit 'echo "  kill(sp, SIGTERM);" >> /mnt/ncudp.c'
  emit 'echo "  kill(cp, SIGTERM);" >> /mnt/ncudp.c'
  emit 'echo "  if (waitpid(sp, &st, 0) < 0) return 4;" >> /mnt/ncudp.c'
  emit 'echo "  if (waitpid(cp, &st, 0) < 0) return 5;" >> /mnt/ncudp.c'
  emit 'echo "  if (sn < 8) return 6;" >> /mnt/ncudp.c'
  emit 'echo "  if (sb[0] != \"u\"[0]) return 7;" >> /mnt/ncudp.c'
  emit 'echo "  write(1, \"nc-udp-ok\\n\", 10);" >> /mnt/ncudp.c'
  emit 'echo "  return 0;" >> /mnt/ncudp.c'
  emit 'echo "}" >> /mnt/ncudp.c'
  emit 'tcc -g -Wall -c /mnt/helper.c -o /mnt/helper.o'
  emit 'ar rcs /mnt/libhelper.a /mnt/helper.o'
  emit 'ranlib /mnt/libhelper.a'
  emit 'tcc /mnt/hello.c -o /mnt/hello.elf'
  emit 'chmod 755 /mnt/hello.elf'
  emit '/mnt/hello.elf'
  emit 'tcc /mnt/tcp.c -o /mnt/tcp.elf'
  emit 'chmod 755 /mnt/tcp.elf'
  emit '/mnt/tcp.elf'
  emit 'tcc /mnt/tcpnb.c -o /mnt/tcpnb.elf'
  emit 'chmod 755 /mnt/tcpnb.elf'
  emit '/mnt/tcpnb.elf'
  emit 'tcc /mnt/tcpclose.c -o /mnt/tcpclose.elf'
  emit 'chmod 755 /mnt/tcpclose.elf'
  emit '/mnt/tcpclose.elf'
  emit 'tcc /mnt/udp.c -o /mnt/udp.elf'
  emit 'chmod 755 /mnt/udp.elf'
  emit '/mnt/udp.elf'
  emit 'tcc /mnt/nctcp.c -o /mnt/nctcp.elf'
  emit 'chmod 755 /mnt/nctcp.elf'
  emit '/mnt/nctcp.elf'
  emit 'tcc /mnt/ncudp.c -o /mnt/ncudp.elf'
  emit 'chmod 755 /mnt/ncudp.elf'
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
  emit 'mkdir /mnt/libdyn'
  emit 'cp /mnt/libtls_a.so /mnt/libdyn/libtls_a.so'
  emit 'cp /mnt/libtls_b.so /mnt/libdyn/libtls_b.so'
  emit 'tcc /mnt/tls_dso_main.c -Wl,-rpath,/mnt/libdyn -L/mnt/libdyn -ltls_a -ltls_b -o /mnt/tlsdso.elf'
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
  emit 'echo "#include <signal.h>" > /mnt/sig.c'
  emit 'echo "#include <unistd.h>" >> /mnt/sig.c'
  emit 'echo "volatile int hits = 0;" >> /mnt/sig.c'
  emit 'echo "static void h(int s){ (void)s; hits++; }" >> /mnt/sig.c'
  emit 'echo "int main(void){" >> /mnt/sig.c'
  emit 'echo "  sigaction_t sa;" >> /mnt/sig.c'
  emit 'echo "  sa.sa_mask = 0; sa.sa_restorer = 0;" >> /mnt/sig.c'
  emit 'echo "  sa.sa_handler = (unsigned long)h; sa.sa_flags = 0;" >> /mnt/sig.c'
  emit 'echo "  if (sigaction(SIGTERM, &sa, 0) != 0) return 1;" >> /mnt/sig.c'
  emit 'echo "  if (kill(getpid(), SIGTERM) != 0) return 2;" >> /mnt/sig.c'
  emit 'echo "  sa.sa_handler = SIG_IGN;" >> /mnt/sig.c'
  emit 'echo "  if (sigaction(SIGINT, &sa, 0) != 0) return 3;" >> /mnt/sig.c'
  emit 'echo "  if (kill(getpid(), SIGINT) != 0) return 4;" >> /mnt/sig.c'
  emit 'echo "  if (hits != 1) return 5;" >> /mnt/sig.c'
  emit 'echo "  write(1, \"sig-ok\\n\", 7);" >> /mnt/sig.c'
  emit 'echo "  return 0;" >> /mnt/sig.c'
  emit 'echo "}" >> /mnt/sig.c'
  emit 'tcc /mnt/sig.c -o /mnt/sig.elf'
  emit 'chmod 755 /mnt/sig.elf'
  emit '/mnt/sig.elf'
  emit 'echo "#include <unistd.h>" > /mnt/sigrt.c'
  emit 'echo "#include <signal.h>" >> /mnt/sigrt.c'
  emit 'echo "#include <errno.h>" >> /mnt/sigrt.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/sigrt.c'
  emit 'echo "int main(void){" >> /mnt/sigrt.c'
  emit 'echo "  sigaction_t sa;" >> /mnt/sigrt.c'
  emit 'echo "  int fds[2];" >> /mnt/sigrt.c'
  emit 'echo "  int st = 0;" >> /mnt/sigrt.c'
  emit 'echo "  int rc = 0;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_handler = SIG_IGN;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_flags = 0;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_restorer = 0;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_mask = 0;" >> /mnt/sigrt.c'
  emit 'echo "  if (sigaction(SIGPIPE, &sa, 0) != 0) return 1;" >> /mnt/sigrt.c'
  emit 'echo "  if (pipe(fds) != 0) return 2;" >> /mnt/sigrt.c'
  emit 'echo "  close(fds[0]);" >> /mnt/sigrt.c'
  emit 'echo "  errno = 0;" >> /mnt/sigrt.c'
  emit 'echo "  if (write(fds[1], \"x\", 1) != -1) return 3;" >> /mnt/sigrt.c'
  emit 'echo "  if (errno != EPIPE) return 4;" >> /mnt/sigrt.c'
  emit 'echo "  close(fds[1]);" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_handler = SIG_DFL;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_flags = SA_NOCLDWAIT;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_restorer = 0;" >> /mnt/sigrt.c'
  emit 'echo "  sa.sa_mask = 0;" >> /mnt/sigrt.c'
  emit 'echo "  if (sigaction(SIGCHLD, &sa, 0) != 0) return 5;" >> /mnt/sigrt.c'
  emit 'echo "  int pid = fork();" >> /mnt/sigrt.c'
  emit 'echo "  if (pid == 0) _exit(7);" >> /mnt/sigrt.c'
  emit 'echo "  if (pid < 0) return 6;" >> /mnt/sigrt.c'
  emit 'echo "  for (int i = 0; i < 32; i++) {" >> /mnt/sigrt.c'
  emit 'echo "    rc = waitpid(pid, &st, WNOHANG);" >> /mnt/sigrt.c'
  emit 'echo "    if (rc == -1) break;" >> /mnt/sigrt.c'
  emit 'echo "    if (rc == pid) return 8;" >> /mnt/sigrt.c'
  emit 'echo "    sleep(1);" >> /mnt/sigrt.c'
  emit 'echo "  }" >> /mnt/sigrt.c'
  emit 'echo "  if (rc != -1 || errno != ECHILD) return 9;" >> /mnt/sigrt.c'
  emit 'echo "  write(1, \"sigrt-ok\\n\", 9);" >> /mnt/sigrt.c'
  emit 'echo "  return 0;" >> /mnt/sigrt.c'
  emit 'echo "}" >> /mnt/sigrt.c'
  emit 'tcc /mnt/sigrt.c -o /mnt/sigrt.elf'
  emit 'chmod 755 /mnt/sigrt.elf'
  emit '/mnt/sigrt.elf'
  emit 'echo "#include <unistd.h>" > /mnt/waitflow.c'
  emit 'echo "#include <signal.h>" >> /mnt/waitflow.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/waitflow.c'
  emit 'echo "int main(void){" >> /mnt/waitflow.c'
  emit 'echo "  int st = 0;" >> /mnt/waitflow.c'
  emit 'echo "  int seen_cont = 0;" >> /mnt/waitflow.c'
  emit 'echo "  int pid = fork();" >> /mnt/waitflow.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/waitflow.c'
  emit 'echo "    for (;;) sleep(1);" >> /mnt/waitflow.c'
  emit 'echo "  }" >> /mnt/waitflow.c'
  emit 'echo "  if (pid < 0) return 1;" >> /mnt/waitflow.c'
  emit 'echo "  sleep(1);" >> /mnt/waitflow.c'
  emit 'echo "  if (kill(pid, SIGSTOP) != 0) return 2;" >> /mnt/waitflow.c'
  emit 'echo "  if (waitpid(pid, &st, WUNTRACED) != pid) return 3;" >> /mnt/waitflow.c'
  emit 'echo "  if (!WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) return 4;" >> /mnt/waitflow.c'
  emit 'echo "  if (kill(pid, SIGCONT) != 0) return 5;" >> /mnt/waitflow.c'
  emit 'echo "  for (int i = 0; i < 8; i++) {" >> /mnt/waitflow.c'
  emit 'echo "    int rc = waitpid(pid, &st, WCONTINUED | WNOHANG);" >> /mnt/waitflow.c'
  emit 'echo "    if (rc == pid) {" >> /mnt/waitflow.c'
  emit 'echo "      if (!WIFCONTINUED(st)) return 6;" >> /mnt/waitflow.c'
  emit 'echo "      seen_cont = 1;" >> /mnt/waitflow.c'
  emit 'echo "      break;" >> /mnt/waitflow.c'
  emit 'echo "    }" >> /mnt/waitflow.c'
  emit 'echo "    sleep(1);" >> /mnt/waitflow.c'
  emit 'echo "  }" >> /mnt/waitflow.c'
  emit 'echo "  if (!seen_cont) return 7;" >> /mnt/waitflow.c'
  emit 'echo "  if (kill(pid, SIGTERM) != 0) return 8;" >> /mnt/waitflow.c'
  emit 'echo "  if (waitpid(pid, &st, 0) != pid) return 9;" >> /mnt/waitflow.c'
  emit 'echo "  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) return 10;" >> /mnt/waitflow.c'
  emit 'echo "  write(1, \"waitflow-ok\\n\", 12);" >> /mnt/waitflow.c'
  emit 'echo "  return 0;" >> /mnt/waitflow.c'
  emit 'echo "}" >> /mnt/waitflow.c'
  emit 'tcc /mnt/waitflow.c -o /mnt/waitflow.elf'
  emit 'chmod 755 /mnt/waitflow.elf'
  emit '/mnt/waitflow.elf'
  emit 'echo "#include <unistd.h>" > /mnt/mmsync.c'
  emit 'echo "#include <fcntl.h>" >> /mnt/mmsync.c'
  emit 'echo "#include <sys/mman.h>" >> /mnt/mmsync.c'
  emit 'echo "int main(void){" >> /mnt/mmsync.c'
  emit 'echo "  int fd = open(\"/mnt/mmsync.txt\"," >> /mnt/mmsync.c'
  emit 'echo "                O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/mmsync.c'
  emit 'echo "  char buf[4];" >> /mnt/mmsync.c'
  emit 'echo "  if (fd < 0) return 1;" >> /mnt/mmsync.c'
  emit 'echo "  if (write(fd, \"aaaa\", 4) != 4) return 2;" >> /mnt/mmsync.c'
  emit 'echo "  char *p = (char*)mmap(0, 4096," >> /mnt/mmsync.c'
  emit 'echo "      PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);" >> /mnt/mmsync.c'
  emit 'echo "  if (p == (char*)MAP_FAILED) return 3;" >> /mnt/mmsync.c'
  emit 'echo "  p[0] = \"b\"[0];" >> /mnt/mmsync.c'
  emit 'echo "  if (msync(p, 4096, MS_SYNC) != 0) return 4;" >> /mnt/mmsync.c'
  emit 'echo "  if (lseek(fd, 0, 0) != 0) return 5;" >> /mnt/mmsync.c'
  emit 'echo "  if (read(fd, buf, 4) != 4) return 6;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[0] != \"b\"[0]) return 7;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[1] != \"a\"[0]) return 7;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[2] != \"a\"[0]) return 7;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[3] != \"a\"[0]) return 7;" >> /mnt/mmsync.c'
  emit 'echo "  p[1] = \"c\"[0];" >> /mnt/mmsync.c'
  emit 'echo "  if (fsync(fd) != 0) return 8;" >> /mnt/mmsync.c'
  emit 'echo "  int fd2 = open(\"/mnt/mmsync.txt\", O_RDONLY);" >> /mnt/mmsync.c'
  emit 'echo "  if (fd2 < 0) return 9;" >> /mnt/mmsync.c'
  emit 'echo "  if (read(fd2, buf, 4) != 4) return 10;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[0] != \"b\"[0]) return 11;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[1] != \"c\"[0]) return 11;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[2] != \"a\"[0]) return 11;" >> /mnt/mmsync.c'
  emit 'echo "  if (buf[3] != \"a\"[0]) return 11;" >> /mnt/mmsync.c'
  emit 'echo "  write(1, \"mmsync-ok\\n\", 10);" >> /mnt/mmsync.c'
  emit 'echo "  return 0;" >> /mnt/mmsync.c'
  emit 'echo "}" >> /mnt/mmsync.c'
  emit 'tcc /mnt/mmsync.c -o /mnt/mmsync.elf'
  emit 'chmod 755 /mnt/mmsync.elf'
  emit '/mnt/mmsync.elf'
  emit 'echo "#include <unistd.h>" > /mnt/mmtrunc.c'
  emit 'echo "#include <fcntl.h>" >> /mnt/mmtrunc.c'
  emit 'echo "#include <sys/mman.h>" >> /mnt/mmtrunc.c'
  emit 'echo "int main(void){" >> /mnt/mmtrunc.c'
  emit 'echo "  int fd;" >> /mnt/mmtrunc.c'
  emit 'echo "  int fd2;" >> /mnt/mmtrunc.c'
  emit 'echo "  int fd3;" >> /mnt/mmtrunc.c'
  emit 'echo "  char *p;" >> /mnt/mmtrunc.c'
  emit 'echo "  fd = open(\"/mnt/mmtrunc.txt\", O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/mmtrunc.c'
  emit 'echo "  if (fd < 0) return 1;" >> /mnt/mmtrunc.c'
  emit 'echo "  if (write(fd, \"abcdef\", 6) != 6) return 2;" >> /mnt/mmtrunc.c'
  emit 'echo "  p = (char*)mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);" >> /mnt/mmtrunc.c'
  emit 'echo "  if (p == (char*)MAP_FAILED) return 3;" >> /mnt/mmtrunc.c'
  emit 'echo "  fd2 = open(\"/mnt/mmtrunc.txt\", O_RDWR|O_TRUNC);" >> /mnt/mmtrunc.c'
  emit 'echo "  if (fd2 < 0) return 4;" >> /mnt/mmtrunc.c'
  emit 'echo "  close(fd2);" >> /mnt/mmtrunc.c'
  emit 'echo "  fd3 = open(\"/mnt/mmtrunc.txt\", O_RDWR);" >> /mnt/mmtrunc.c'
  emit 'echo "  if (fd3 < 0) return 5;" >> /mnt/mmtrunc.c'
  emit 'echo "  if (write(fd3, \"xy\", 2) != 2) return 6;" >> /mnt/mmtrunc.c'
  emit 'echo "  if (fsync(fd3) != 0) return 7;" >> /mnt/mmtrunc.c'
  emit 'echo "  if (p[0] != \"x\"[0] || p[1] != \"y\"[0]) return 8;" >> /mnt/mmtrunc.c'
  emit 'echo "  if (p[2] != 0 || p[3] != 0 || p[4] != 0 || p[5] != 0) return 9;" >> /mnt/mmtrunc.c'
  emit 'echo "  write(1, \"mmtrunc-ok\\n\", 11);" >> /mnt/mmtrunc.c'
  emit 'echo "  return 0;" >> /mnt/mmtrunc.c'
  emit 'echo "}" >> /mnt/mmtrunc.c'
  emit 'tcc /mnt/mmtrunc.c -o /mnt/mmtrunc.elf'
  emit 'chmod 755 /mnt/mmtrunc.elf'
  emit '/mnt/mmtrunc.elf'
  emit 'echo "#include <unistd.h>" > /mnt/mmgrow.c'
  emit 'echo "#include <fcntl.h>" >> /mnt/mmgrow.c'
  emit 'echo "#include <sys/mman.h>" >> /mnt/mmgrow.c'
  emit 'echo "int main(void){" >> /mnt/mmgrow.c'
  emit 'echo "  int fd = open(\"/mnt/mmgrow.txt\", O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/mmgrow.c'
  emit 'echo "  int fd2;" >> /mnt/mmgrow.c'
  emit 'echo "  int fd3;" >> /mnt/mmgrow.c'
  emit 'echo "  char buf[4];" >> /mnt/mmgrow.c'
  emit 'echo "  char *p;" >> /mnt/mmgrow.c'
  emit 'echo "  long end;" >> /mnt/mmgrow.c'
  emit 'echo "  if (fd < 0) return 1;" >> /mnt/mmgrow.c'
  emit 'echo "  if (write(fd, \"abcdef\", 6) != 6) return 2;" >> /mnt/mmgrow.c'
  emit 'echo "  p = (char*)mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);" >> /mnt/mmgrow.c'
  emit 'echo "  if (p == (char*)MAP_FAILED) return 3;" >> /mnt/mmgrow.c'
  emit 'echo "  p[0] = \"A\"[0];" >> /mnt/mmgrow.c'
  emit 'echo "  fd2 = open(\"/mnt/mmgrow.txt\", O_RDWR|O_TRUNC);" >> /mnt/mmgrow.c'
  emit 'echo "  if (fd2 < 0) return 4;" >> /mnt/mmgrow.c'
  emit 'echo "  close(fd2);" >> /mnt/mmgrow.c'
  emit 'echo "  fd3 = open(\"/mnt/mmgrow.txt\", O_RDWR);" >> /mnt/mmgrow.c'
  emit 'echo "  if (fd3 < 0) return 5;" >> /mnt/mmgrow.c'
  emit 'echo "  if (write(fd3, \"xy\", 2) != 2) return 6;" >> /mnt/mmgrow.c'
  emit 'echo "  if (fsync(fd3) != 0) return 7;" >> /mnt/mmgrow.c'
  emit 'echo "  close(fd3);" >> /mnt/mmgrow.c'
  emit 'echo "  p[4] = \"Z\"[0];" >> /mnt/mmgrow.c'
  emit 'echo "  if (munmap(p, 4096) != 0) return 8;" >> /mnt/mmgrow.c'
  emit 'echo "  end = lseek(fd, 0, 2);" >> /mnt/mmgrow.c'
  emit 'echo "  if (end != 2) return 9;" >> /mnt/mmgrow.c'
  emit 'echo "  if (lseek(fd, 0, 0) != 0) return 10;" >> /mnt/mmgrow.c'
  emit 'echo "  if (read(fd, buf, 4) != 2) return 11;" >> /mnt/mmgrow.c'
  emit 'echo "  if (buf[0] != \"x\"[0] || buf[1] != \"y\"[0]) return 12;" >> /mnt/mmgrow.c'
  emit 'echo "  write(1, \"mmgrow-ok\\n\", 10);" >> /mnt/mmgrow.c'
  emit 'echo "  return 0;" >> /mnt/mmgrow.c'
  emit 'echo "}" >> /mnt/mmgrow.c'
  emit 'tcc /mnt/mmgrow.c -o /mnt/mmgrow.elf'
  emit 'chmod 755 /mnt/mmgrow.elf'
  emit '/mnt/mmgrow.elf'
  emit 'echo "#include <unistd.h>" > /mnt/mmcohere.c'
  emit 'echo "#include <fcntl.h>" >> /mnt/mmcohere.c'
  emit 'echo "#include <sys/mman.h>" >> /mnt/mmcohere.c'
  emit 'echo "int main(void){" >> /mnt/mmcohere.c'
  emit 'echo "  int fd = open(\"/mnt/mmcohere.txt\", O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/mmcohere.c'
  emit 'echo "  char buf[4];" >> /mnt/mmcohere.c'
  emit 'echo "  char *p;" >> /mnt/mmcohere.c'
  emit 'echo "  if (fd < 0) return 1;" >> /mnt/mmcohere.c'
  emit 'echo "  if (write(fd, \"abcd\", 4) != 4) return 2;" >> /mnt/mmcohere.c'
  emit 'echo "  p = (char*)mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);" >> /mnt/mmcohere.c'
  emit 'echo "  if (p == (char*)MAP_FAILED) return 3;" >> /mnt/mmcohere.c'
  emit 'echo "  p[0] = \"x\"[0];" >> /mnt/mmcohere.c'
  emit 'echo "  if (lseek(fd, 0, 0) != 0) return 4;" >> /mnt/mmcohere.c'
  emit 'echo "  if (read(fd, buf, 4) != 4) return 5;" >> /mnt/mmcohere.c'
  emit 'echo "  if (buf[0] != \"x\"[0] || buf[1] != \"b\"[0] || buf[2] != \"c\"[0] || buf[3] != \"d\"[0]) return 6;" >> /mnt/mmcohere.c'
  emit 'echo "  if (lseek(fd, 1, 0) != 1) return 7;" >> /mnt/mmcohere.c'
  emit 'echo "  if (write(fd, \"y\", 1) != 1) return 8;" >> /mnt/mmcohere.c'
  emit 'echo "  if (p[1] != \"y\"[0]) return 9;" >> /mnt/mmcohere.c'
  emit 'echo "  if (fsync(fd) != 0) return 10;" >> /mnt/mmcohere.c'
  emit 'echo "  write(1, \"mmcohere-ok\\n\", 12);" >> /mnt/mmcohere.c'
  emit 'echo "  return 0;" >> /mnt/mmcohere.c'
  emit 'echo "}" >> /mnt/mmcohere.c'
  emit 'tcc /mnt/mmcohere.c -o /mnt/mmcohere.elf'
  emit 'chmod 755 /mnt/mmcohere.elf'
  emit '/mnt/mmcohere.elf'
  emit 'echo "#include <unistd.h>" > /mnt/reusecache.c'
  emit 'echo "#include <fcntl.h>" >> /mnt/reusecache.c'
  emit 'echo "int main(void){" >> /mnt/reusecache.c'
  emit 'echo "  static char big[4096];" >> /mnt/reusecache.c'
  emit 'echo "  static char buf[4096];" >> /mnt/reusecache.c'
  emit 'echo "  int fd;" >> /mnt/reusecache.c'
  emit 'echo "  int i;" >> /mnt/reusecache.c'
  emit 'echo "  for (i = 0; i < 4096; i++) big[i] = \"A\"[0];" >> /mnt/reusecache.c'
  emit 'echo "  fd = open(\"/mnt/reusecache-old\", O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/reusecache.c'
  emit 'echo "  if (fd < 0) return 1;" >> /mnt/reusecache.c'
  emit 'echo "  if (write(fd, big, 4096) != 4096) return 2;" >> /mnt/reusecache.c'
  emit 'echo "  close(fd);" >> /mnt/reusecache.c'
  emit 'echo "  if (unlink(\"/mnt/reusecache-old\") != 0) return 3;" >> /mnt/reusecache.c'
  emit 'echo "  fd = open(\"/mnt/reusecache-new\", O_CREAT|O_RDWR|O_TRUNC);" >> /mnt/reusecache.c'
  emit 'echo "  if (fd < 0) return 4;" >> /mnt/reusecache.c'
  emit 'echo "  if (lseek(fd, 4095, 0) != 4095) return 5;" >> /mnt/reusecache.c'
  emit 'echo "  if (write(fd, \"Z\", 1) != 1) return 6;" >> /mnt/reusecache.c'
  emit 'echo "  if (fsync(fd) != 0) return 7;" >> /mnt/reusecache.c'
  emit 'echo "  if (lseek(fd, 0, 0) != 0) return 8;" >> /mnt/reusecache.c'
  emit 'echo "  if (read(fd, buf, 4096) != 4096) return 9;" >> /mnt/reusecache.c'
  emit 'echo "  if (buf[0] != 0 || buf[4095] != \"Z\"[0]) return 10;" >> /mnt/reusecache.c'
  emit 'echo "  write(1, \"reusecache-ok\\n\", 14);" >> /mnt/reusecache.c'
  emit 'echo "  return 0;" >> /mnt/reusecache.c'
  emit 'echo "}" >> /mnt/reusecache.c'
  emit 'tcc /mnt/reusecache.c -o /mnt/reusecache.elf'
  emit 'chmod 755 /mnt/reusecache.elf'
  emit '/mnt/reusecache.elf'
  emit 'echo "#include <unistd.h>" > /mnt/tcpstress.c'
  emit 'echo "#include <sys/socket.h>" >> /mnt/tcpstress.c'
  emit 'echo "#include <netinet/in.h>" >> /mnt/tcpstress.c'
  emit 'echo "#include <sys/wait.h>" >> /mnt/tcpstress.c'
  emit 'echo "int main(void){" >> /mnt/tcpstress.c'
  emit 'echo "  int l = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcpstress.c'
  emit 'echo "  sockaddr_in a; unsigned long alen = sizeof(a);" >> /mnt/tcpstress.c'
  emit 'echo "  sockaddr_in p; unsigned long plen = sizeof(p);" >> /mnt/tcpstress.c'
  emit 'echo "  int st = 0; char ch = 0;" >> /mnt/tcpstress.c'
  emit 'echo "  if (l < 0) return 1;" >> /mnt/tcpstress.c'
  emit 'echo "  a.sin_family = AF_INET;" >> /mnt/tcpstress.c'
  emit 'echo "  a.sin_port = 0;" >> /mnt/tcpstress.c'
  emit 'echo "  a.sin_addr = htonl(INADDR_LOOPBACK);" >> /mnt/tcpstress.c'
  emit 'echo "  if (bind(l, (sockaddr*)&a, sizeof(a)) != 0) return 2;" >> /mnt/tcpstress.c'
  emit 'echo "  if (getsockname(l, (sockaddr*)&a, &alen) != 0) return 3;" >> /mnt/tcpstress.c'
  emit 'echo "  if (listen(l, 2) != 0) return 4;" >> /mnt/tcpstress.c'
  emit 'echo "  int pid = fork();" >> /mnt/tcpstress.c'
  emit 'echo "  if (pid == 0) {" >> /mnt/tcpstress.c'
  emit 'echo "    for (int i = 0; i < 8; i++) {" >> /mnt/tcpstress.c'
  emit 'echo "      int c = socket(AF_INET, SOCK_STREAM, 0);" >> /mnt/tcpstress.c'
  emit 'echo "      if (c < 0) _exit(10 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      int cr = connect(c, (sockaddr*)&a," >> /mnt/tcpstress.c'
  emit 'echo "                       sizeof(a));" >> /mnt/tcpstress.c'
  emit 'echo "      if (cr != 0) _exit(20 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      if (write(c, \"x\", 1) != 1) _exit(30 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      if (shutdown(c, SHUT_WR) != 0) _exit(40 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      if (read(c, &ch, 1) != 1) _exit(50 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      if (ch != \"y\"[0]) _exit(50 + i);" >> /mnt/tcpstress.c'
  emit 'echo "      close(c);" >> /mnt/tcpstress.c'
  emit 'echo "    }" >> /mnt/tcpstress.c'
  emit 'echo "    _exit(0);" >> /mnt/tcpstress.c'
  emit 'echo "  }" >> /mnt/tcpstress.c'
  emit 'echo "  if (pid < 0) return 5;" >> /mnt/tcpstress.c'
  emit 'echo "  for (int i = 0; i < 8; i++) {" >> /mnt/tcpstress.c'
  emit 'echo "    int s = accept(l, (sockaddr*)&p, &plen);" >> /mnt/tcpstress.c'
  emit 'echo "    if (s < 0) return 6;" >> /mnt/tcpstress.c'
  emit 'echo "    if (read(s, &ch, 1) != 1) return 7;" >> /mnt/tcpstress.c'
  emit 'echo "    if (ch != \"x\"[0]) return 7;" >> /mnt/tcpstress.c'
  emit 'echo "    if (write(s, \"y\", 1) != 1) return 8;" >> /mnt/tcpstress.c'
  emit 'echo "    close(s);" >> /mnt/tcpstress.c'
  emit 'echo "  }" >> /mnt/tcpstress.c'
  emit 'echo "  if (waitpid(pid, &st, 0) < 0) return 9;" >> /mnt/tcpstress.c'
  emit 'echo "  write(1, \"tcpstress-ok\\n\", 13);" >> /mnt/tcpstress.c'
  emit 'echo "  return 0;" >> /mnt/tcpstress.c'
  emit 'echo "}" >> /mnt/tcpstress.c'
  emit 'tcc /mnt/tcpstress.c -o /mnt/tcpstress.elf'
  emit 'chmod 755 /mnt/tcpstress.elf'
  emit '/mnt/tcpstress.elf'
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
  emit 'sleep 1'
  emit 'echo slept-ok'
  emit 'echo io-start > /mnt/stress'
  for i in $(seq 1 6); do
    emit "echo line-$i >> /mnt/stress"
  done
  emit 'cat /mnt/stress | cat > /mnt/stress2'
  emit 'cat /mnt/stress2'
  emit 'rm /p1'
  emit 'rm /q1'
  emit 'rm /mnt/stress'
  emit 'rm /mnt/stress2'
  emit 'echo smoke-test'
  emit 'exit'
  sleep 0.4
) | timeout 150 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -nic user,model=rtl8139 \
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
grep -q "net0" "$OUT"
grep -q "vda" "$OUT"
grep -q "\[rtl8139\] ready" "$OUT"
grep -q "init" "$OUT"
grep -q "echo" "$OUT"
grep -q "FuriOS EL0 userspace online" "$OUT"
grep -q "loopback up" "$OUT"
grep -q "dev=rtl8139" "$OUT"
grep -q "route: ok" "$OUT"
grep -q "arp: ok" "$OUT"
grep -q "10.0.2.2 52:55:0a:00:02:02 eth0" "$OUT"
grep -q "dhcp: lease 10.0.2." "$OUT"
grep -q "nameserver 10.0.2.3" "$OUT"
grep -q "host.qemu" "$OUT"
grep -q "server 10.0.2.2:15353" "$OUT"
grep -q "name furios.test" "$OUT"
grep -q "address 10.0.2.2" "$OUT"
grep -q "dst=10.0.2.2 via=10.0.2.2 dev=eth0 src=10.0.2." "$OUT"
grep -q "host-tcp-ok" "$OUT"
grep -q "host-udp-ok" "$OUT"
grep -q "default 10.0.0.1 lo" "$OUT"
grep -q "pong 127.0.0.1" "$OUT"

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
  if rg -q "rmdir: failed /mnt/empty|rm: cannot remove /mnt/new2.txt|mkdir: cannot create /mnt/hi" "$OUT"; then
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
grep -q "tmpx-nested" "$OUT"
grep -q "hello world" "$OUT"
grep -q "foo bar" "$OUT"
grep -q "hi" "$OUT"
grep -q "one two" "$OUT"
grep -q "append-one" "$OUT"
grep -q "append-two" "$OUT"
grep -q "mkfs.ext4: formatted" "$OUT"
grep -q "fsck.ext4: clean" "$OUT"
grep -q "mkfst" "$OUT"
rg -q "^cache-bravo$" "$OUT"
rg -q "^cache-gamma$" "$OUT"
grep -q "hello-from-tcc" "$OUT"
rg -q "^tcp-ok$" "$OUT"
rg -q "^tcp-close-ok$" "$OUT"
grep -q "sh\$ /mnt/tccmulti.elf" "$OUT"
grep -q "W7" "$OUT"
rg -q "^tls-ok" "$OUT"
grep -q "tls-dso-ok" "$OUT"
rg -q "^tcp-nb-ok$" "$OUT"
rg -q "^udp-ok$" "$OUT"
rg -q "^nc-tcp-ok$" "$OUT"
rg -q "^tcpstress-ok$" "$OUT"
grep -q "block-scope thread-local must be static or extern" "$OUT"
grep -q "sh\$ /mnt/ctor.elf" "$OUT"
grep -q "sh\$ /mnt/rt.elf" "$OUT"
rg -q "^sig-ok$" "$OUT"
rg -q "^sigrt-ok$" "$OUT"
rg -q "^waitflow-ok$" "$OUT"
rg -q "^mmsync-ok$" "$OUT"
rg -q "^mmtrunc-ok$" "$OUT"
rg -q "^mmgrow-ok$" "$OUT"
rg -q "^mmcohere-ok$" "$OUT"
rg -q "^reusecache-ok$" "$OUT"
grep -q "auxv-ok" "$OUT"
grep -q "comm=trapdbg.elf" "$OUT"
grep -q "sym=crash_here+" "$OUT"
grep -q "main.c" "$OUT"
grep -q "slept-ok" "$OUT"
grep -q "line-6" "$OUT"
if rg -q "exec failed: /mnt/tcp.elf|exec failed: /mnt/tcpnb.elf|exec failed: /mnt/tcpclose.elf|exec failed: /mnt/udp.elf|exec failed: /mnt/nctcp.elf|exec failed: /mnt/ncudp.elf|exec failed: /mnt/tccmulti.elf|exec failed: /mnt/tls.elf|exec failed: /mnt/tlsdso.elf|exec failed: /mnt/ctor.elf|exec failed: /mnt/rt.elf|exec failed: /mnt/sig.elf|exec failed: /mnt/sigrt.elf|exec failed: /mnt/waitflow.elf|exec failed: /mnt/mmsync.elf|exec failed: /mnt/mmtrunc.elf|exec failed: /mnt/mmgrow.elf|exec failed: /mnt/mmcohere.elf|exec failed: /mnt/reusecache.elf|exec failed: /mnt/tcpstress.elf|exec failed: /mnt/auxv.elf" "$OUT"; then
  echo "[test] FAIL: toolchain runtime compatibility flow failed" >&2
  exit 1
fi
if rg -q "touch: cannot touch /tmpx/a|redirect output failed|cp: cannot open destination|mv: rename failed" "$OUT"; then
  echo "[test] FAIL: memfs nested create/move flow failed" >&2
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

OUT_NANO="$(mktemp)"
(
  emit_raw() {
    printf '%b' "$1"
    sleep "$STEP"
  }
  MULTI_PASTE_BODY=""
  for i in $(seq 1 16); do
    MULTI_PASTE_BODY="${MULTI_PASTE_BODY}paste-burst-${i}\n"
  done
  LONG_PASTE_TAIL="$(printf 'paste-tail-%0240d' 0)"

  sleep 0.8
  printf 'nano /nano_paste.txt\n'
  sleep 0.05
  emit_raw '\x1b[200~paste-line-1\npaste-line-2\n'
  emit_raw "$MULTI_PASTE_BODY"
  emit_raw "$LONG_PASTE_TAIL"
  emit_raw '\x1b[201~'
  sleep 0.3
  emit_raw '\x0f'
  sleep 0.25
  emit_raw '\x18'
  sleep 0.25
  printf 'cat /nano_paste.txt\n'
  sleep 0.05
  printf 'echo AFTER1\n'
  sleep 0.05
  printf 'echo AFTER2\n'
  sleep 0.05
  printf 'exit\n'
  sleep 0.2
) | timeout 30 qemu-system-aarch64 \
      -machine virt,virtualization=on,gic-version=2 \
      -cpu cortex-a53 \
      -m 256M \
      -smp 1 \
      -nographic \
      -monitor none \
      -nic none \
      -device loader,file=build/kernel.elf,cpu-num=0 >"$OUT_NANO" 2>&1 || true

cat "$OUT_NANO"
grep -q "paste-line-1" "$OUT_NANO"
grep -q "paste-line-2" "$OUT_NANO"
grep -q "paste-burst-16" "$OUT_NANO"
grep -q "paste-tail-" "$OUT_NANO"
grep -q "AFTER1" "$OUT_NANO"
grep -q "AFTER2" "$OUT_NANO"
if rg -q "parse error|\\[panic\\]|no runnable" "$OUT_NANO"; then
  echo "[test] FAIL: nano interactive paste flow failed" >&2
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
  emit 'echo move-me > /mnt/replay-move.txt'
  emit 'echo gone > /mnt/replay-gone.txt'
  emit 'echo stale-old > /mnt/replay-reuse-old'
  emit 'rm /mnt/replay-reuse-old'
  emit 'echo fresh-new > /mnt/replay-reuse-new'
  emit 'mv /mnt/replay-move.txt /mnt/replay-moved.txt'
  emit 'rm /mnt/replay-gone.txt'
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
  emit 'cat /mnt/replay-moved.txt'
  emit 'cat /mnt/replay-reuse-new'
  emit 'ls /mnt'
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
grep -q "move-me" "$OUT_REPLAY2"
grep -q "fresh-new" "$OUT_REPLAY2"
if rg -q "^replay-gone.txt$" "$OUT_REPLAY2"; then
  echo "[test] FAIL: ext4 replay unlink gate failed" >&2
  exit 1
fi
if rg -q "^replay-reuse-old$" "$OUT_REPLAY2"; then
  echo "[test] FAIL: ext4 replay reuse/unlink gate failed" >&2
  exit 1
fi
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
