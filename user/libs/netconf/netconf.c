/* Network adapter settings (see <netconf.h>). */

#include <arpa/inet.h>
#include <netconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void copy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void netconf_mac_text(const uint8_t mac[6], char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *netconf_ip_text(uint32_t address, char *out) {
    struct in_addr in = { address };
    inet_ntop(AF_INET, &in, out, INET_ADDRSTRLEN);
    return out;
}

int netconf_prefix(uint32_t netmask) {
    uint32_t m = ntohl(netmask);
    int bits = 0;
    while (m & 0x80000000u) {
        bits++;
        m <<= 1;
    }
    return bits;
}

bool netconf_parse_address(const char *s, uint32_t *out) {
    struct in_addr a;
    if (!inet_aton(s, &a)) {
        return false;
    }
    *out = a.s_addr;
    return true;
}

bool netconf_parse_cidr(const char *s, uint32_t *address, uint32_t *netmask) {
    char ip[32];
    const char *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (n >= sizeof(ip)) {
        return false;
    }
    memcpy(ip, s, n);
    ip[n] = '\0';
    int prefix = 24;
    if (slash) {
        char *end;
        prefix = (int)strtol(slash + 1, &end, 10);
        if (*end || slash[1] == '\0' || prefix < 1 || prefix > 32) {
            return false;
        }
    }
    *netmask = htonl(prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
    return netconf_parse_address(ip, address);
}

/* --- the file ---------------------------------------------------------------------------------- */

static char *read_config(void) {
    int fd = open(NETCONF_FILE, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 1024 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        long n = read(fd, buf + len, cap - len - 1);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

/* The next line of a buffer (cut in place); *next moves past it. NULL at the end. */
static char *next_line(char **next) {
    char *line = *next;
    if (!line || !*line) {
        return NULL;
    }
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
        *next = nl + 1;
    } else {
        *next = NULL;
    }
    return line;
}

static bool line_is_for(const char *line, const char *mac) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    for (int i = 0; i < 17; i++) {
        char a = line[i];
        if (a >= 'A' && a <= 'F') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != mac[i]) {
            return false;
        }
    }
    return line[17] == ' ' || line[17] == '\t' || line[17] == '\0' || line[17] == '\r';
}

void netconf_load(const uint8_t mac[6], struct netconf *s) {
    memset(s, 0, sizeof(*s));
    char text[18];
    netconf_mac_text(mac, text);
    char *buf = read_config();
    if (!buf) {
        return;
    }
    char *cursor = buf;
    for (char *line; (line = next_line(&cursor));) {
        if (!line_is_for(line, text)) {
            continue;
        }
        char *w = line;
        while (*w == ' ' || *w == '\t') {
            w++;
        }
        w += 17;
        while (*w) {
            while (*w == ' ' || *w == '\t' || *w == '\r') {
                w++;
            }
            if (!*w || *w == '#') {
                break;
            }
            char *word = w;
            while (*w && *w != ' ' && *w != '\t' && *w != '\r') {
                w++;
            }
            char saved = *w;
            *w = '\0';
            if (strcmp(word, "manual") == 0) {
                s->manual = true;
            } else if (strcmp(word, "auto") == 0) {
                s->manual = false;
            } else if (strcmp(word, "off") == 0) {
                s->off = true;
            } else if (strncmp(word, "address=", 8) == 0) {
                netconf_parse_cidr(word + 8, &s->address, &s->netmask);
            } else if (strncmp(word, "gateway=", 8) == 0) {
                netconf_parse_address(word + 8, &s->gateway);
            } else if (strncmp(word, "dns=", 4) == 0) {
                char *d = word + 4, *comma = strchr(d, ',');
                if (comma) {
                    *comma = '\0';
                    netconf_parse_address(comma + 1, &s->dns[1]);
                }
                netconf_parse_address(d, &s->dns[0]);
            }
            *w = saved;
        }
    }
    free(buf);
}

int netconf_save(const uint8_t mac[6], const char *name, const struct netconf *s) {
    char text[18], line[256], ip[INET_ADDRSTRLEN];
    netconf_mac_text(mac, text);
    int n = snprintf(line, sizeof(line), "%s  %s", text, s->manual ? "manual" : "auto");
    if (s->manual && s->address) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  address=%s/%d", netconf_ip_text(s->address, ip),
                      netconf_prefix(s->netmask));
    }
    if (s->manual && s->gateway) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  gateway=%s", netconf_ip_text(s->gateway, ip));
    }
    if (s->dns[0]) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  dns=%s", netconf_ip_text(s->dns[0], ip));
        if (s->dns[1]) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, ",%s", netconf_ip_text(s->dns[1], ip));
        }
    }
    if (s->off) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  off");
    }
    snprintf(line + n, sizeof(line) - (size_t)n, "    # %s", name);

    char *old = read_config();
    int fd = open(NETCONF_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        free(old);
        return fd;
    }
    bool written = false;
    if (!old || !old[0]) {
        dprintf(fd, "# Network adapter settings: one line per adapter, by its MAC address.\n"
                    "#   MAC  auto|manual  [address=A.B.C.D/prefix] [gateway=A.B.C.D] [dns=A.B.C.D,A.B.C.D] [off]\n");
    } else {
        char *cursor = old;
        for (char *l; (l = next_line(&cursor));) {
            if (line_is_for(l, text)) {
                if (!written) {
                    dprintf(fd, "%s\n", line);
                    written = true;
                }
            } else {
                dprintf(fd, "%s\n", l);
            }
        }
    }
    if (!written) {
        dprintf(fd, "%s\n", line);
    }
    close(fd);
    free(old);
    return 0;
}

/* --- the running adapter ------------------------------------------------------------------------- */

static void config_for(const struct os_netif_info *i, uint32_t op, struct os_netif_config *c) {
    memset(c, 0, sizeof(*c));
    copy(c->name, i->name, sizeof(c->name));
    c->op = op;
}

int netconf_apply(const struct os_netif_info *i, const struct netconf *s) {
    struct os_netif_config c;
    config_for(i, OS_NETIF_SET_CONFIG, &c);
    c.method = s->manual ? OS_NETIF_MANUAL : OS_NETIF_AUTO;
    c.address = s->address;
    c.netmask = s->netmask;
    c.gateway = s->gateway;
    c.dns[0] = s->dns[0];
    c.dns[1] = s->dns[1];
    int r = netif_set(&c);
    if (r == 0) {
        struct netconf saved;
        netconf_load(i->mac, &saved);
        struct netconf now = *s;
        now.off = saved.off;
        r = netconf_save(i->mac, i->name, &now);
    }
    return r;
}

int netconf_set_power(const struct os_netif_info *i, bool on) {
    struct os_netif_config c;
    config_for(i, on ? OS_NETIF_SET_UP : OS_NETIF_SET_DOWN, &c);
    int r = netif_set(&c);
    if (r == 0) {
        struct netconf s;
        netconf_load(i->mac, &s);
        s.off = !on;
        r = netconf_save(i->mac, i->name, &s);
    }
    return r;
}

int netconf_renew(const struct os_netif_info *i) {
    struct os_netif_config c;
    config_for(i, OS_NETIF_SET_RENEW, &c);
    return netif_set(&c);
}

bool netconf_online(const struct os_netif_info *i) {
    return i->state == OS_NETIF_CONNECTED && i->gateway && !(i->flags & OS_NETIF_LOOPBACK);
}

const char *netconf_state_text(const struct os_netif_info *i) {
    switch (i->state) {
    case OS_NETIF_UNAVAILABLE:  return "Cable unplugged";
    case OS_NETIF_DISCONNECTED: return "Disconnected";
    case OS_NETIF_CONNECTING:   return "Connecting...";
    case OS_NETIF_CONNECTED:    return i->gateway ? "Connected, Internet access" : "Connected, no internet";
    default:                    return "Unknown";
    }
}
