#include "user.h"

static int parse_server(const char *text, uint32_t *ip_be, uint16_t *port) {
    char buf[48];
    char *colon;
    uint32_t v = 0U;
    size_t n;

    if (!text || !ip_be || !port) {
        return -1;
    }
    n = strlen(text);
    if (n == 0U || n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, text, n);
    buf[n] = '\0';

    colon = strrchr(buf, ':');
    if (colon) {
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
        *port = (uint16_t)v;
    }
    return fu_parse_ipv4(buf, ip_be);
}

int main(int argc, char **argv) {
    uint32_t server_ip = 0U;
    uint32_t answer_ip = 0U;
    uint16_t server_port = 53U;
    char server_text[16];
    char answer_text[16];
    int rc;

    if (argc < 2 || argc > 3) {
        puts("usage: nslookup <name> [server[:port]]\n");
        return 1;
    }

    if (argc == 3) {
        if (parse_server(argv[2], &server_ip, &server_port) != 0) {
            puts("nslookup: bad server\n");
            return 1;
        }
    } else if (fu_resolver_nameserver(&server_ip, &server_port) != 0) {
        puts("nslookup: resolver unavailable\n");
        return 1;
    }

    rc = fu_resolve_name_ipv4_at(argv[1], server_ip, server_port, &answer_ip);
    if (rc != 0) {
        puts("nslookup: query failed\n");
        return 1;
    }

    fu_format_ipv4(server_ip, server_text, sizeof(server_text));
    fu_format_ipv4(answer_ip, answer_text, sizeof(answer_text));
    printf("server %s:%u\n", server_text, (unsigned)server_port);
    printf("name %s\n", argv[1]);
    printf("address %s\n", answer_text);
    return 0;
}
