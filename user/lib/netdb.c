#include "../user.h"

#define DNS_PORT_DEFAULT 53U
#define DNS_SERVER_DEFAULT_BE 0x0A000203U
#define DNS_MAX_NAME 255U
#define DNS_MAX_PACKET 512U
#define DNS_POLL_TICKS 10U
#define DNS_MAX_POLLS 20U
#define DNS_CACHE_MAX 8U
#define HOSTS_LINE_MAX 512U
#define NETDB_EAGAIN 11
#define NETDB_EINVAL 22
#define NETDB_ENOENT 2
#define NETDB_ETIMEDOUT 110

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_hdr_t;

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} sockaddr_in;

typedef struct {
    int valid;
    char name[DNS_MAX_NAME];
    uint32_t server_ip_be;
    uint16_t server_port;
    uint32_t addr_be;
} resolver_cache_entry_t;

static resolver_cache_entry_t g_dns_cache[DNS_CACHE_MAX];
static uint32_t g_dns_cache_next = 0U;

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8) |
           ((v & 0x00FF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static uint16_t htons16(uint16_t v) {
    return bswap16(v);
}

static uint16_t ntohs16(uint16_t v) {
    return bswap16(v);
}

static uint32_t htonl32(uint32_t v) {
    return bswap32(v);
}

static uint32_t ntohl32(uint32_t v) {
    return bswap32(v);
}

static void trim_ws(char **start, char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t' || **start == '\r' || **start == '\n')) {
        (*start)++;
    }
    while (*end > *start &&
           ((*(*end - 1) == ' ') || (*(*end - 1) == '\t') || (*(*end - 1) == '\r') || (*(*end - 1) == '\n'))) {
        (*end)--;
    }
}

static char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static size_t host_key_length(const char *name) {
    size_t len = 0U;
    if (!name) {
        return 0U;
    }
    len = strlen(name);
    while (len > 0U && name[len - 1U] == '.') {
        len--;
    }
    return len;
}

static int host_key_copy(char *dst, size_t cap, const char *name) {
    size_t len;
    if (!dst || cap == 0U || !name) {
        return -1;
    }
    len = host_key_length(name);
    if (len == 0U || len >= cap) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        dst[i] = ascii_tolower(name[i]);
    }
    dst[len] = '\0';
    return 0;
}

static int hostname_equal(const char *a, const char *b) {
    size_t alen = host_key_length(a);
    size_t blen = host_key_length(b);
    if (alen != blen) {
        return 0;
    }
    for (size_t i = 0; i < alen; i++) {
        if (ascii_tolower(a[i]) != ascii_tolower(b[i])) {
            return 0;
        }
    }
    return 1;
}

static int dns_cache_lookup(const char *name, uint32_t server_ip_be, uint16_t server_port, uint32_t *out_be) {
    char key[DNS_MAX_NAME];
    if (!out_be || host_key_copy(key, sizeof(key), name) != 0) {
        return -1;
    }
    for (uint32_t i = 0U; i < DNS_CACHE_MAX; i++) {
        const resolver_cache_entry_t *ent = &g_dns_cache[i];
        if (!ent->valid) {
            continue;
        }
        if (ent->server_ip_be != server_ip_be || ent->server_port != server_port) {
            continue;
        }
        if (strcmp(ent->name, key) != 0) {
            continue;
        }
        *out_be = ent->addr_be;
        return 0;
    }
    return -1;
}

static void dns_cache_store(const char *name, uint32_t server_ip_be, uint16_t server_port, uint32_t addr_be) {
    char key[DNS_MAX_NAME];
    if (host_key_copy(key, sizeof(key), name) != 0) {
        return;
    }
    for (uint32_t i = 0U; i < DNS_CACHE_MAX; i++) {
        resolver_cache_entry_t *ent = &g_dns_cache[i];
        if (!ent->valid) {
            continue;
        }
        if (ent->server_ip_be != server_ip_be || ent->server_port != server_port) {
            continue;
        }
        if (strcmp(ent->name, key) != 0) {
            continue;
        }
        ent->addr_be = addr_be;
        return;
    }
    resolver_cache_entry_t *slot = &g_dns_cache[g_dns_cache_next % DNS_CACHE_MAX];
    g_dns_cache_next++;
    memset(slot, 0, sizeof(*slot));
    slot->valid = 1;
    slot->server_ip_be = server_ip_be;
    slot->server_port = server_port;
    slot->addr_be = addr_be;
    strcpy(slot->name, key);
}

int fu_parse_ipv4(const char *src, uint32_t *out_be) {
    uint32_t part = 0U;
    uint32_t count = 0U;
    uint32_t ip = 0U;
    int have_digit = 0;

    if (!src || !*src || !out_be) {
        return -1;
    }
    for (; *src; src++) {
        char c = *src;
        if (c >= '0' && c <= '9') {
            have_digit = 1;
            part = part * 10U + (uint32_t)(c - '0');
            if (part > 255U) {
                return -1;
            }
            continue;
        }
        if (c != '.' || !have_digit || count >= 3U) {
            return -1;
        }
        ip = (ip << 8) | part;
        part = 0U;
        have_digit = 0;
        count++;
    }
    if (!have_digit || count != 3U) {
        return -1;
    }
    *out_be = (ip << 8) | part;
    return 0;
}

void fu_format_ipv4(uint32_t ip_be, char *buf, unsigned long size) {
    uint8_t b0 = (uint8_t)((ip_be >> 24) & 0xFFU);
    uint8_t b1 = (uint8_t)((ip_be >> 16) & 0xFFU);
    uint8_t b2 = (uint8_t)((ip_be >> 8) & 0xFFU);
    uint8_t b3 = (uint8_t)(ip_be & 0xFFU);
    if (!buf || size == 0U) {
        return;
    }
    (void)snprintf(buf, (size_t)size, "%u.%u.%u.%u",
                   (unsigned)b0, (unsigned)b1, (unsigned)b2, (unsigned)b3);
}

int inet_aton(const char *src, in_addr *out) {
    uint32_t ip_be = 0U;
    if (fu_parse_ipv4(src, &ip_be) != 0) {
        return 0;
    }
    if (out) {
        out->s_addr = htonl32(ip_be);
    }
    return 1;
}

unsigned long inet_addr(const char *src) {
    in_addr addr;
    if (!inet_aton(src, &addr)) {
        return (unsigned long)0xFFFFFFFFUL;
    }
    return (unsigned long)addr.s_addr;
}

char *inet_ntoa(in_addr in) {
    static char buf[16];
    fu_format_ipv4(ntohl32(in.s_addr), buf, sizeof(buf));
    return buf;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET || !dst) {
        errno = NETDB_EINVAL;
        return -1;
    }
    return inet_aton(src, (in_addr *)dst) ? 1 : 0;
}

const char *inet_ntop(int af, const void *src, char *dst, unsigned long size) {
    const in_addr *addr = (const in_addr *)src;
    if (af != AF_INET || !src || !dst || size < 16U) {
        errno = NETDB_EINVAL;
        return 0;
    }
    fu_format_ipv4(ntohl32(addr->s_addr), dst, size);
    return dst;
}

static int parse_nameserver_line(char *line, uint32_t *ip_be, uint16_t *port_out) {
    const char *prefix = "nameserver";
    size_t prefix_len = 10U;
    char *start = line;
    char *end = line + strlen(line);
    trim_ws(&start, &end);
    if ((size_t)(end - start) <= prefix_len || strncmp(start, prefix, prefix_len) != 0) {
        return -1;
    }
    start += prefix_len;
    trim_ws(&start, &end);
    if (start >= end) {
        return -1;
    }

    char token[48];
    size_t n = (size_t)(end - start);
    if (n >= sizeof(token)) {
        n = sizeof(token) - 1U;
    }
    memcpy(token, start, n);
    token[n] = '\0';

    char *colon = strrchr(token, ':');
    uint16_t port = DNS_PORT_DEFAULT;
    if (colon) {
        uint32_t v = 0U;
        *colon++ = '\0';
        if (!*colon) {
            return -1;
        }
        while (*colon) {
            if (*colon < '0' || *colon > '9') {
                return -1;
            }
            v = v * 10U + (uint32_t)(*colon - '0');
            if (v > 65535U) {
                return -1;
            }
            colon++;
        }
        port = (uint16_t)v;
    }

    if (fu_parse_ipv4(token, ip_be) != 0) {
        return -1;
    }
    if (port_out) {
        *port_out = port;
    }
    return 0;
}

static int hosts_line_lookup(char *line, const char *name, uint32_t *out_be) {
    char *hash;
    char *start;
    char *end;
    char *cur;
    char *tok_end;
    char ip_text[48];
    uint32_t addr_be = 0U;

    if (!line || !name || !out_be) {
        return -1;
    }
    hash = strchr(line, '#');
    if (hash) {
        *hash = '\0';
    }
    start = line;
    end = line + strlen(line);
    trim_ws(&start, &end);
    if (start >= end) {
        return -1;
    }

    cur = start;
    while (cur < end && *cur != ' ' && *cur != '\t') {
        cur++;
    }
    if ((size_t)(cur - start) >= sizeof(ip_text)) {
        return -1;
    }
    memcpy(ip_text, start, (size_t)(cur - start));
    ip_text[cur - start] = '\0';
    if (fu_parse_ipv4(ip_text, &addr_be) != 0) {
        return -1;
    }

    while (cur < end) {
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            cur++;
        }
        if (cur >= end) {
            break;
        }
        tok_end = cur;
        while (tok_end < end && *tok_end != ' ' && *tok_end != '\t') {
            tok_end++;
        }
        char saved = *tok_end;
        *tok_end = '\0';
        if (hostname_equal(cur, name)) {
            *out_be = addr_be;
            *tok_end = saved;
            return 0;
        }
        *tok_end = saved;
        cur = tok_end;
    }
    return -1;
}

static int hosts_lookup(const char *name, uint32_t *out_be) {
    int fd;
    char chunk[128];
    char line[HOSTS_LINE_MAX];
    size_t line_len = 0U;

    if (!name || !out_be) {
        errno = NETDB_EINVAL;
        return -1;
    }

    fd = open("/etc/hosts", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    for (;;) {
        long n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line[line_len] = '\0';
                if (hosts_line_lookup(line, name, out_be) == 0) {
                    close(fd);
                    return 0;
                }
                line_len = 0U;
                continue;
            }
            if (line_len + 1U < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }

    if (line_len > 0U) {
        line[line_len] = '\0';
        if (hosts_line_lookup(line, name, out_be) == 0) {
            close(fd);
            return 0;
        }
    }
    close(fd);
    errno = NETDB_ENOENT;
    return -1;
}

int fu_resolver_nameserver(uint32_t *ip_be, uint16_t *port) {
    char buf[256];
    long n;
    int fd;

    if (!ip_be || !port) {
        errno = NETDB_EINVAL;
        return -1;
    }
    *ip_be = DNS_SERVER_DEFAULT_BE;
    *port = DNS_PORT_DEFAULT;

    fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1U);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    char *cur = buf;
    while (*cur) {
        char *line = cur;
        char *nl = strchr(cur, '\n');
        if (nl) {
            *nl = '\0';
            cur = nl + 1;
        } else {
            cur += strlen(cur);
        }
        if (parse_nameserver_line(line, ip_be, port) == 0) {
            return 0;
        }
    }
    return 0;
}

static int dns_encode_name(const char *name, uint8_t *buf, size_t cap, size_t *off) {
    const char *cur = name;
    if (!name || !*name || !buf || !off) {
        return -1;
    }
    while (*cur) {
        const char *dot = strchr(cur, '.');
        size_t len = dot ? (size_t)(dot - cur) : strlen(cur);
        if (len == 0U || len > 63U || *off + 1U + len >= cap) {
            return -1;
        }
        buf[(*off)++] = (uint8_t)len;
        memcpy(buf + *off, cur, len);
        *off += len;
        if (!dot) {
            break;
        }
        cur = dot + 1;
    }
    if (*off >= cap) {
        return -1;
    }
    buf[(*off)++] = 0U;
    return 0;
}

static int dns_skip_name(const uint8_t *pkt, size_t pkt_len, size_t *off) {
    size_t pos;
    uint32_t loops = 0U;
    if (!pkt || !off || *off >= pkt_len) {
        return -1;
    }
    pos = *off;
    while (pos < pkt_len) {
        uint8_t len = pkt[pos++];
        if (len == 0U) {
            *off = pos;
            return 0;
        }
        if ((len & 0xC0U) == 0xC0U) {
            if (pos >= pkt_len) {
                return -1;
            }
            *off = pos + 1U;
            return 0;
        }
        if ((len & 0xC0U) != 0U || pos + len > pkt_len) {
            return -1;
        }
        pos += len;
        if (++loops > 128U) {
            return -1;
        }
    }
    return -1;
}

static int dns_extract_a(const uint8_t *pkt, size_t pkt_len, uint16_t expect_id, uint32_t *out_be) {
    const dns_hdr_t *hdr;
    size_t off;
    uint16_t qdcount;
    uint16_t ancount;

    if (!pkt || pkt_len < sizeof(dns_hdr_t) || !out_be) {
        return -1;
    }
    hdr = (const dns_hdr_t *)pkt;
    if (ntohs16(hdr->id) != expect_id) {
        return -1;
    }
    if ((ntohs16(hdr->flags) & 0x8000U) == 0U) {
        return -1;
    }
    if ((ntohs16(hdr->flags) & 0x000FU) != 0U) {
        return -1;
    }
    qdcount = ntohs16(hdr->qdcount);
    ancount = ntohs16(hdr->ancount);
    off = sizeof(dns_hdr_t);

    while (qdcount-- > 0U) {
        if (dns_skip_name(pkt, pkt_len, &off) != 0 || off + 4U > pkt_len) {
            return -1;
        }
        off += 4U;
    }
    while (ancount-- > 0U) {
        uint16_t type;
        uint16_t klass;
        uint16_t rdlen;
        if (dns_skip_name(pkt, pkt_len, &off) != 0 || off + 10U > pkt_len) {
            return -1;
        }
        type = (uint16_t)(((uint16_t)pkt[off] << 8) | pkt[off + 1U]);
        klass = (uint16_t)(((uint16_t)pkt[off + 2U] << 8) | pkt[off + 3U]);
        rdlen = (uint16_t)(((uint16_t)pkt[off + 8U] << 8) | pkt[off + 9U]);
        off += 10U;
        if (off + rdlen > pkt_len) {
            return -1;
        }
        if (type == 1U && klass == 1U && rdlen == 4U) {
            *out_be = ((uint32_t)pkt[off] << 24) |
                      ((uint32_t)pkt[off + 1U] << 16) |
                      ((uint32_t)pkt[off + 2U] << 8) |
                      (uint32_t)pkt[off + 3U];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

static int dns_query_a(uint32_t server_ip_be, uint16_t server_port, const char *name, uint32_t *out_be) {
    uint8_t pkt[DNS_MAX_PACKET];
    uint8_t resp[DNS_MAX_PACKET];
    dns_hdr_t *hdr = (dns_hdr_t *)pkt;
    sockaddr_in addr;
    unsigned long addrlen = sizeof(addr);
    size_t off = sizeof(dns_hdr_t);
    static uint16_t next_id = 0x4400U;
    uint16_t query_id;
    int fd;
    fu_pollfd_t pfd;

    if (!name || !out_be) {
        errno = NETDB_EINVAL;
        return -1;
    }

    memset(pkt, 0, sizeof(pkt));
    query_id = ++next_id;
    hdr->id = htons16(query_id);
    hdr->flags = htons16(0x0100U);
    hdr->qdcount = htons16(1U);
    if (dns_encode_name(name, pkt, sizeof(pkt), &off) != 0 || off + 4U > sizeof(pkt)) {
        errno = NETDB_EINVAL;
        return -1;
    }
    pkt[off++] = 0U;
    pkt[off++] = 1U;
    pkt[off++] = 0U;
    pkt[off++] = 1U;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons16(server_port);
    addr.sin_addr = htonl32(server_ip_be);

    if (sendto(fd, pkt, (unsigned long)off, 0, (const fu_sockaddr_t *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (uint32_t tries = 0; tries < DNS_MAX_POLLS; tries++) {
        int rc = poll(&pfd, 1, DNS_POLL_TICKS);
        if (rc < 0) {
            close(fd);
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        if (pfd.revents & POLLIN) {
            long n = recvfrom(fd, resp, sizeof(resp), 0, (fu_sockaddr_t *)&addr, &addrlen);
            if (n < 0) {
                if (errno == NETDB_EAGAIN) {
                    continue;
                }
                close(fd);
                return -1;
            }
            close(fd);
            if (dns_extract_a(resp, (size_t)n, query_id, out_be) == 0) {
                return 0;
            }
            errno = NETDB_ENOENT;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
    }
    close(fd);
    errno = NETDB_ETIMEDOUT;
    return -1;
}

int fu_resolve_name_ipv4_at(const char *name, uint32_t server_ip, uint16_t server_port, uint32_t *out_be) {
    if (!name || !*name || !out_be) {
        errno = NETDB_EINVAL;
        return -1;
    }
    if (fu_parse_ipv4(name, out_be) == 0) {
        return 0;
    }
    if (dns_cache_lookup(name, server_ip, server_port, out_be) == 0) {
        return 0;
    }
    if (dns_query_a(server_ip, server_port, name, out_be) == 0) {
        dns_cache_store(name, server_ip, server_port, *out_be);
        return 0;
    }
    return -1;
}

int fu_resolve_name_ipv4(const char *name, uint32_t *out_be) {
    uint32_t server_ip = DNS_SERVER_DEFAULT_BE;
    uint16_t server_port = DNS_PORT_DEFAULT;
    if (!name || !*name || !out_be) {
        errno = NETDB_EINVAL;
        return -1;
    }
    if (fu_parse_ipv4(name, out_be) == 0) {
        return 0;
    }
    if (dns_cache_lookup(name, 0U, 0U, out_be) == 0) {
        return 0;
    }
    if (hosts_lookup(name, out_be) == 0) {
        dns_cache_store(name, 0U, 0U, *out_be);
        return 0;
    }
    (void)fu_resolver_nameserver(&server_ip, &server_port);
    return fu_resolve_name_ipv4_at(name, server_ip, server_port, out_be);
}

hostent *gethostbyname(const char *name) {
    static hostent ent;
    static char *aliases[1];
    static char *addr_list[2];
    static char name_buf[DNS_MAX_NAME];
    static uint32_t addr_be;
    static in_addr addr_net;

    if (!name || fu_resolve_name_ipv4(name, &addr_be) != 0) {
        return 0;
    }
    memset(&ent, 0, sizeof(ent));
    memset(name_buf, 0, sizeof(name_buf));
    size_t copy_len = strlen(name);
    if (copy_len >= sizeof(name_buf)) {
        copy_len = sizeof(name_buf) - 1U;
    }
    memcpy(name_buf, name, copy_len);
    name_buf[copy_len] = '\0';
    addr_net.s_addr = htonl32(addr_be);
    aliases[0] = 0;
    addr_list[0] = (char *)&addr_net;
    addr_list[1] = 0;
    ent.h_name = name_buf;
    ent.h_aliases = aliases;
    ent.h_addrtype = AF_INET;
    ent.h_length = 4;
    ent.h_addr_list = addr_list;
    return &ent;
}
