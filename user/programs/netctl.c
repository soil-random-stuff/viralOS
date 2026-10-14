/*
 * netctl: network adapters from the command line, in the manner of NetworkManager's nmcli.
 *
 *   netctl [general]                      is the computer online? (and the adapters, briefly)
 *   netctl device [status]                one line per adapter
 *   netctl device show [DEVICE]           everything about one (or every) adapter
 *   netctl device up|connect DEVICE       turn it on: connect when there's a cable
 *   netctl device down|disconnect DEVICE  disconnect it and keep it off
 *   netctl device renew DEVICE            ask DHCP for an address again
 *   netctl device modify DEVICE PROPERTY VALUE [PROPERTY VALUE]...
 *       ipv4.method     auto | manual     DHCP, or the address below
 *       ipv4.addresses  A.B.C.D/PREFIX    (manual)
 *       ipv4.gateway    A.B.C.D           (manual; "" for none)
 *       ipv4.dns        A.B.C.D[,A.B.C.D]    DNS servers (none = DHCP's)
 *
 * Words can be shortened as long as they stay unambiguous ("netctl d s eth0"). Changes take effect
 * at once and are saved in /etc/network.conf (one line per adapter, by its MAC address), which
 * the kernel reads when the adapter appears.
 */

#include <arpa/inet.h>
#include <netconf.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define CONFIG    NETCONF_FILE
#define MAX_IFS   16

static struct os_netif_info ifs[MAX_IFS];
static int if_count;

/* --- small helpers ------------------------------------------------------------------------------ */

static void strncpy_safe(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool starts(const char *word, const char *full) {   /* an abbreviation of `full`? */
    size_t n = strlen(word);
    return n > 0 && strncmp(word, full, n) == 0;
}

static const char *ip_text(uint32_t a, char *buf) {   /* network byte order */
    struct in_addr in = { a };
    inet_ntop(AF_INET, &in, buf, INET_ADDRSTRLEN);
    return buf;
}

static int prefix_of(uint32_t netmask) {
    uint32_t m = ntohl(netmask);
    int bits = 0;
    while (m & 0x80000000u) {
        bits++;
        m <<= 1;
    }
    return bits;
}

static void mac_text(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *size_text(uint64_t bytes, char *buf, size_t size) {
    if (bytes < 1024) {
        snprintf(buf, size, "%lu B", (unsigned long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, size, "%.1f KiB", (double)bytes / 1024);
    } else if (bytes < 1024ull * 1024 * 1024) {
        snprintf(buf, size, "%.1f MiB", (double)bytes / (1024 * 1024));
    } else {
        snprintf(buf, size, "%.2f GiB", (double)bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

static const char *state_text(const struct os_netif_info *i) {
    switch (i->state) {
    case OS_NETIF_UNAVAILABLE:  return "unavailable";
    case OS_NETIF_DISCONNECTED: return "disconnected";
    case OS_NETIF_CONNECTING:   return "connecting (getting IP configuration)";
    case OS_NETIF_CONNECTED:    return "connected";
    default:                    return "unknown";
    }
}

static const char *short_state(const struct os_netif_info *i) {
    return i->state == OS_NETIF_CONNECTING ? "connecting" : state_text(i);
}

static const char *type_text(const struct os_netif_info *i) {
    return i->flags & OS_NETIF_LOOPBACK ? "loopback" : "ethernet";
}

static void load_adapters(void) {
    if_count = 0;
    while (if_count < MAX_IFS && netif_info(if_count, &ifs[if_count]) == 0) {
        if_count++;
    }
}

static struct os_netif_info *find(const char *name) {
    for (int i = 0; i < if_count; i++) {
        if (strcmp(ifs[i].name, name) == 0) {
            return &ifs[i];
        }
    }
    printf("Error: Device '%s' not found. (See: netctl device)\n", name);
    return NULL;
}

/* --- showing ------------------------------------------------------------------------------------ */

static void address_column(const struct os_netif_info *i, char *out, size_t size) {
    char ip[INET_ADDRSTRLEN];
    if (i->address) {
        snprintf(out, size, "%s/%d", ip_text(i->address, ip), prefix_of(i->netmask));
    } else {
        snprintf(out, size, "--");
    }
}

static int cmd_device_status(void) {
    printf("%-8s %-9s %-13s %-19s %s\n", "DEVICE", "TYPE", "STATE", "ADDRESS", "DRIVER");
    for (int k = 0; k < if_count; k++) {
        const struct os_netif_info *i = &ifs[k];
        char addr[32];
        address_column(i, addr, sizeof(addr));
        printf("%-8s %-9s %-13s %-19s %s\n", i->name, type_text(i), short_state(i), addr, i->driver);
    }
    return 0;
}

static void field(const char *name, const char *value) {
    printf("%-26s %s\n", name, value);
}

static void show_one(const struct os_netif_info *i) {
    char buf[96], ip[INET_ADDRSTRLEN];
    field("GENERAL.DEVICE:", i->name);
    field("GENERAL.TYPE:", type_text(i));
    field("GENERAL.MODEL:", i->model);
    field("GENERAL.DRIVER:", i->driver);
    if (!(i->flags & OS_NETIF_LOOPBACK)) {
        mac_text(i->mac, buf);
        field("GENERAL.HWADDR:", buf);
    }
    snprintf(buf, sizeof(buf), "%u", i->mtu);
    field("GENERAL.MTU:", buf);
    field("GENERAL.STATE:", state_text(i));
    field("GENERAL.AUTOCONNECT:", i->flags & OS_NETIF_ENABLED ? "yes" : "no (netctl device up turns it on)");
    if (!(i->flags & OS_NETIF_LOOPBACK)) {
        field("WIRED-PROPERTIES.CARRIER:", i->state == OS_NETIF_UNAVAILABLE ? "off" : "on");
        if (i->speed_mbps) {
            snprintf(buf, sizeof(buf), "%u Mb/s, %s duplex", i->speed_mbps, i->full_duplex ? "full" : "half");
        } else {
            snprintf(buf, sizeof(buf), i->state == OS_NETIF_UNAVAILABLE ? "--" : "unknown");
        }
        field("WIRED-PROPERTIES.SPEED:", buf);
        field("IP4.METHOD:", i->method == OS_NETIF_AUTO ? "auto (DHCP)" : "manual");
    }
    if (i->address) {
        snprintf(buf, sizeof(buf), "%s/%d", ip_text(i->address, ip), prefix_of(i->netmask));
        field("IP4.ADDRESS[1]:", buf);
        field("IP4.GATEWAY:", i->gateway ? ip_text(i->gateway, ip) : "--");
        if (i->flags & OS_NETIF_DEFAULT_ROUTE) {
            field("IP4.ROUTE[1]:", "default (the internet goes out here)");
        }
        for (int d = 0; d < 2; d++) {
            if (i->dns[d]) {
                snprintf(buf, sizeof(buf), "IP4.DNS[%d]:", d + 1);
                field(buf, ip_text(i->dns[d], ip));
            }
        }
    }
    if (i->dhcp_server) {
        field("DHCP4.SERVER:", ip_text(i->dhcp_server, ip));
        unsigned h = i->lease_seconds / 3600, m = i->lease_seconds / 60 % 60;
        snprintf(buf, sizeof(buf), "%u s (%uh %02um)", i->lease_seconds, h, m);
        field("DHCP4.LEASE_TIME:", buf);
    }
    if (!(i->flags & OS_NETIF_LOOPBACK)) {
        char size[24];
        snprintf(buf, sizeof(buf), "%lu packets, %s, %lu errors, %lu dropped", (unsigned long)i->rx_packets,
                 size_text(i->rx_bytes, size, sizeof(size)), (unsigned long)i->rx_errors, (unsigned long)i->rx_dropped);
        field("STATISTICS.RX:", buf);
        snprintf(buf, sizeof(buf), "%lu packets, %s, %lu errors, %lu dropped", (unsigned long)i->tx_packets,
                 size_text(i->tx_bytes, size, sizeof(size)), (unsigned long)i->tx_errors, (unsigned long)i->tx_dropped);
        field("STATISTICS.TX:", buf);
    }
}

static int cmd_device_show(const char *name) {
    bool first = true;
    for (int k = 0; k < if_count; k++) {
        if (name && strcmp(ifs[k].name, name) != 0) {
            continue;
        }
        if (!first) {
            printf("\n");
        }
        show_one(&ifs[k]);
        first = false;
    }
    if (first && name) {
        find(name);
        return 10;
    }
    return 0;
}

static int cmd_general(void) {
    const struct os_netif_info *route = NULL;
    bool any = false, connecting = false;
    for (int k = 0; k < if_count; k++) {
        if (ifs[k].flags & OS_NETIF_LOOPBACK) {
            continue;
        }
        any = true;
        connecting |= ifs[k].state == OS_NETIF_CONNECTING;
        if (ifs[k].flags & OS_NETIF_DEFAULT_ROUTE) {
            route = &ifs[k];
        }
    }
    char host[64] = "viralos";
    int fd = open(_PATH_HOSTNAME, O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, host, sizeof(host) - 1);
        close(fd);
        host[n > 0 ? n : 0] = '\0';
        for (char *c = host; *c; c++) {
            if (*c == '\r' || *c == '\n' || *c == ' ') {
                *c = '\0';
                break;
            }
        }
    }
    const char *state = route ? (route->gateway ? "connected" : "connected (local only)") :
                        connecting ? "connecting" : any ? "disconnected" : "no network adapters";
    printf("%-14s %s\n", "STATE:", state);
    printf("%-14s %s\n", "HOSTNAME:", host);
    if (route) {
        char ip[INET_ADDRSTRLEN];
        printf("%-14s %s via %s\n", "DEFAULT ROUTE:", route->name, route->gateway ? ip_text(route->gateway, ip) : "--");
        for (int d = 0; d < 2; d++) {
            if (route->dns[d]) {
                printf("%-14s %s\n", d == 0 ? "DNS:" : "", ip_text(route->dns[d], ip));
            }
        }
    }
    printf("\n");
    return cmd_device_status();
}

/* --- settings ----------------------------------------------------------------------------------- */

static int set_power(const char *name, bool on) {
    struct os_netif_info *i = find(name);
    if (!i) {
        return 10;
    }
    if (i->flags & OS_NETIF_LOOPBACK) {
        printf("Error: the loopback device is always on.\n");
        return 1;
    }
    int r = netconf_set_power(i, on);
    if (r < 0) {
        printf("Error: %s\n", strerror(r));
        return 1;
    }
    if (!on) {
        printf("Device '%s' successfully disconnected.\n", name);
        return 0;
    }
    /* Wait a little for an address, as nmcli does. */
    for (int t = 0; t < 100; t++) {
        netif_info((int)(i - ifs), i);
        if (i->state == OS_NETIF_CONNECTED || i->state == OS_NETIF_UNAVAILABLE) {
            break;
        }
        sleep_ms(100);
    }
    if (i->state == OS_NETIF_CONNECTED) {
        char addr[32];
        address_column(i, addr, sizeof(addr));
        printf("Device '%s' successfully activated (%s).\n", name, addr);
    } else if (i->state == OS_NETIF_UNAVAILABLE) {
        printf("Device '%s' is on, and will connect when a cable is plugged in.\n", name);
    } else {
        printf("Device '%s' is on, still waiting for an address (DHCP).\n", name);
    }
    return 0;
}

static int cmd_renew(const char *name) {
    struct os_netif_info *i = find(name);
    if (!i) {
        return 10;
    }
    int r = netconf_renew(i);
    if (r == -EINVAL) {
        printf("Error: '%s' doesn't use DHCP (ipv4.method manual) or is off.\n", name);
        return 1;
    }
    if (r < 0) {
        printf("Error: %s\n", strerror(r));
        return 1;
    }
    for (int t = 0; t < 100; t++) {
        sleep_ms(100);
        netif_info((int)(i - ifs), i);
        if (i->state == OS_NETIF_CONNECTED || i->state == OS_NETIF_UNAVAILABLE) {
            break;
        }
    }
    char addr[32];
    address_column(i, addr, sizeof(addr));
    printf("%s: %s, %s\n", name, short_state(i), addr);
    return 0;
}

static int cmd_modify(const char *name, int argc, char **argv) {
    struct os_netif_info *i = find(name);
    if (!i) {
        return 10;
    }
    if (i->flags & OS_NETIF_LOOPBACK) {
        printf("Error: the loopback device can't be changed.\n");
        return 1;
    }
    if (argc == 0 || argc % 2) {
        printf("Error: give PROPERTY VALUE pairs (ipv4.method, ipv4.addresses, ipv4.gateway, ipv4.dns).\n");
        return 2;
    }
    struct netconf s;
    netconf_load(i->mac, &s);
    for (int k = 0; k < argc; k += 2) {
        const char *prop = argv[k];
        /* Strip quotes the shell left on (it passes '' and "" through as they are); "none" = empty. */
        char value_buf[128];
        strncpy_safe(value_buf, argv[k + 1], sizeof(value_buf));
        char *value = value_buf;
        size_t vlen = strlen(value);
        if (vlen >= 2 && (value[0] == '\'' || value[0] == '"') && value[vlen - 1] == value[0]) {
            value[vlen - 1] = '\0';
            value++;
        }
        if (strcmp(value, "none") == 0) {
            value[0] = '\0';
        }
        if (strcmp(prop, "ipv4.method") == 0) {
            if (strcmp(value, "auto") == 0 || strcmp(value, "dhcp") == 0) {
                s.manual = false;
            } else if (strcmp(value, "manual") == 0 || strcmp(value, "static") == 0) {
                s.manual = true;
            } else {
                printf("Error: ipv4.method is 'auto' or 'manual', not '%s'.\n", value);
                return 2;
            }
        } else if (strcmp(prop, "ipv4.addresses") == 0 || strcmp(prop, "ipv4.address") == 0) {
            if (!netconf_parse_cidr(value, &s.address, &s.netmask)) {
                printf("Error: '%s' isn't an address like 192.168.1.20/24.\n", value);
                return 2;
            }
        } else if (strcmp(prop, "ipv4.gateway") == 0) {
            if (!value[0]) {
                s.gateway = 0;
            } else if (!netconf_parse_address(value, &s.gateway)) {
                printf("Error: '%s' isn't an address like 192.168.1.1.\n", value);
                return 2;
            }
        } else if (strcmp(prop, "ipv4.dns") == 0) {
            s.dns[0] = s.dns[1] = 0;
            char copy[64];
            strncpy_safe(copy, value, sizeof(copy));
            int d = 0;
            for (char *t = copy; *t && d < 2;) {
                while (*t == ' ' || *t == ',') {
                    t++;
                }
                char *end = t;
                while (*end && *end != ' ' && *end != ',') {
                    end++;
                }
                char more = *end;
                *end = '\0';
                if (*t && !netconf_parse_address(t, &s.dns[d++])) {
                    printf("Error: '%s' isn't a DNS server address.\n", t);
                    return 2;
                }
                if (!more) {
                    break;
                }
                t = end + 1;
            }
        } else {
            printf("Error: unknown property '%s' (ipv4.method, ipv4.addresses, ipv4.gateway, ipv4.dns).\n", prop);
            return 2;
        }
    }
    if (s.manual && !s.address) {
        printf("Error: ipv4.method manual needs ipv4.addresses too, e.g.\n"
               "  netctl device modify %s ipv4.method manual ipv4.addresses 192.168.1.20/24 ipv4.gateway 192.168.1.1\n", name);
        return 2;
    }
    int r = netconf_apply(i, &s);
    if (r < 0) {
        printf("Error: %s\n", strerror(r));
        return 1;
    }
    for (int t = 0; t < 50 && !s.manual; t++) {
        netif_info((int)(i - ifs), i);
        if (i->state != OS_NETIF_CONNECTING) {
            break;
        }
        sleep_ms(100);
    }
    netif_info((int)(i - ifs), i);
    char addr[32];
    address_column(i, addr, sizeof(addr));
    printf("%s: %s, %s (%s)\n", name, short_state(i), addr, s.manual ? "manual" : "DHCP");
    return 0;
}

static void usage(void) {
    printf("Usage: netctl [OBJECT] [COMMAND] [ARGUMENTS]\n"
           "  netctl [general]                     overall state, default route, DNS, adapters\n"
           "  netctl device [status]               the adapters, one per line\n"
           "  netctl device show [DEVICE]          all the details\n"
           "  netctl device up|connect DEVICE      turn on and connect\n"
           "  netctl device down|disconnect DEVICE disconnect and stay off\n"
           "  netctl device renew DEVICE           get a new DHCP lease\n"
           "  netctl device modify DEVICE PROPERTY VALUE ...\n"
           "      ipv4.method auto|manual          DHCP, or a fixed address:\n"
           "      ipv4.addresses A.B.C.D/PREFIX\n"
           "      ipv4.gateway A.B.C.D\n"
           "      ipv4.dns A.B.C.D,A.B.C.D         (none = the ones DHCP gives)\n"
           "Words may be shortened: netctl d s eth0\n"
           "Settings are saved in " CONFIG ".\n");
}

int main(int argc, char **argv) {
    load_adapters();
    if (argc < 2) {
        return cmd_general();
    }
    const char *object = argv[1];
    if (strcmp(object, "help") == 0 || strcmp(object, "-h") == 0 || strcmp(object, "--help") == 0) {
        usage();
        return 0;
    }
    if (starts(object, "general")) {
        return cmd_general();
    }
    if (!starts(object, "device")) {
        printf("Error: unknown object '%s' (try: netctl help)\n", object);
        return 2;
    }
    const char *cmd = argc > 2 ? argv[2] : "status";
    const char *dev = argc > 3 ? argv[3] : NULL;
    if (starts(cmd, "status")) {
        return cmd_device_status();
    }
    if (starts(cmd, "show")) {
        return cmd_device_show(dev);
    }
    if (!dev) {
        printf("Error: which device? (see: netctl device)\n");
        return 2;
    }
    if (strcmp(cmd, "up") == 0 || starts(cmd, "connect")) {
        return set_power(dev, true);
    }
    if (strcmp(cmd, "down") == 0 || starts(cmd, "disconnect")) {
        return set_power(dev, false);
    }
    if (starts(cmd, "renew")) {
        return cmd_renew(dev);
    }
    if (starts(cmd, "modify")) {
        return cmd_modify(dev, argc - 4, argv + 4);
    }
    printf("Error: unknown command '%s' (try: netctl help)\n", cmd);
    return 2;
}
