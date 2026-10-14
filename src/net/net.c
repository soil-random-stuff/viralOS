/*
 * Network adapters: each card a driver registers (net/netdev.h) becomes an lwIP interface
 * ("eth0", ...; lwIP calls it "en0"), joined to the network as its settings say:
 *
 *   /etc/network.conf, written by netctl: one line per adapter, by MAC address,
 *       52:54:00:12:34:56  auto|manual  [address=A.B.C.D/prefix] [gateway=A.B.C.D] [dns=A.B.C.D,...] [off]
 *   "auto" asks DHCP for an address; "manual" uses the one given; "off" keeps it disconnected.
 *   An adapter without a line uses DHCP.
 *
 * The "net" thread runs the drivers' poll() (after their interrupts, or every tick for cards
 * without one) and hands received frames to lwIP's tcpip thread. Everything that touches lwIP's
 * state from outside that thread takes its core lock (LOCK_TCPIP_CORE).
 */

#include "net/net.h"
#include "net/netdev.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"

#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/sync.h"
#include <paths.h>

#define MAX_ADAPTERS 8
#define CONFIG       _PATH_NETWORK_CONF
#define POLL_MS_NO_IRQ 10      /* cards without an interrupt: every tick */
#define POLL_MS_IDLE   500     /* the others: now and then, for their link state */

struct adapter {
    struct net_device *dev;
    struct netif netif;
    char hostname[32];
    /* settings */
    bool enabled;
    uint32_t method;                /* OS_NETIF_AUTO / MANUAL */
    uint32_t address, netmask, gateway, dns[2];   /* network byte order */
    bool dhcp_running;
    bool was_connected;             /* (for the log) */
    uint8_t tx_frame[NET_FRAME_MAX];
};

static struct adapter adapters[MAX_ADAPTERS];
static int adapter_count;
static struct wait_queue wake = WAIT_QUEUE_INIT;
static volatile bool wake_pending;
static bool stack_ready;

/* --- small helpers ------------------------------------------------------------------------------ */

static void format_ip(char *out, uint32_t a) {   /* network byte order */
    const uint8_t *b = (const uint8_t *)&a;
    char *p = out;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        if (v >= 100) {
            *p++ = (char)('0' + v / 100);
        }
        if (v >= 10) {
            *p++ = (char)('0' + v / 10 % 10);
        }
        *p++ = (char)('0' + v % 10);
        if (i < 3) {
            *p++ = '.';
        }
    }
    *p = '\0';
}

static int prefix_of(uint32_t netmask) {
    int bits = 0;
    for (; netmask; netmask &= netmask - 1) {   /* (no libgcc for __builtin_popcount) */
        bits++;
    }
    return bits;
}

/* "A.B.C.D" -> network byte order; false if it isn't one. */
static bool parse_ip(const char *s, uint32_t *out) {
    ip4_addr_t a;
    if (!ip4addr_aton(s, &a)) {
        return false;
    }
    *out = ip4_addr_get_u32(&a);
    return true;
}

static void read_hostname(char *buf, size_t size) {
    strlcpy(buf, "viralos", size);
    struct vnode *f;
    if (vfs_resolve(NULL, _PATH_HOSTNAME, &f) != 0) {
        return;
    }
    int64_t n = vfs_read(f, 0, buf, size - 1);
    vfs_put(f);
    buf[n > 0 ? n : 0] = '\0';
    for (char *c = buf; *c; c++) {
        if (*c == '\n' || *c == '\r' || *c == ' ') {
            *c = '\0';
            break;
        }
    }
    if (!buf[0]) {
        strlcpy(buf, "viralos", size);
    }
}

/* --- settings ----------------------------------------------------------------------------------- */

static void format_mac(char *out, const uint8_t *mac) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3] = hex[mac[i] >> 4];
        out[i * 3 + 1] = hex[mac[i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
}

static bool same_mac(const char *a, const char *b) {
    for (int i = 0; i < 17; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'F') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return a[17] == '\0' || a[17] == ' ' || a[17] == '\t';
}

/* This adapter's line in /etc/network.conf, if there is one. */
static void load_settings(struct adapter *a) {
    a->enabled = true;
    a->method = OS_NETIF_AUTO;
    struct vnode *f;
    if (vfs_resolve(NULL, CONFIG, &f) != 0) {
        return;
    }
    char *buf = kmalloc(8192);
    if (!buf) {
        vfs_put(f);
        return;
    }
    int64_t n = vfs_read(f, 0, buf, 8191);
    vfs_put(f);
    buf[n > 0 ? n : 0] = '\0';
    char mac[18];
    format_mac(mac, a->dev->mac);

    for (char *line = buf; line && *line;) {
        char *next = line;
        while (*next && *next != '\n') {
            next++;
        }
        if (*next) {
            *next++ = '\0';
        } else {
            next = NULL;
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line && *line != '#' && strlen(line) >= 17 && same_mac(line, mac)) {
            /* Words after the MAC address. */
            char *w = line + 17;
            while (*w) {
                while (*w == ' ' || *w == '\t' || *w == '\r') {
                    *w++ = '\0';
                }
                if (!*w) {
                    break;
                }
                char *word = w;
                while (*w && *w != ' ' && *w != '\t' && *w != '\r') {
                    w++;
                }
                if (*w) {
                    *w++ = '\0';
                }
                if (strcmp(word, "auto") == 0) {
                    a->method = OS_NETIF_AUTO;
                } else if (strcmp(word, "manual") == 0) {
                    a->method = OS_NETIF_MANUAL;
                } else if (strcmp(word, "off") == 0) {
                    a->enabled = false;
                } else if (strncmp(word, "address=", 8) == 0) {
                    char *slash = word + 8;
                    while (*slash && *slash != '/') {
                        slash++;
                    }
                    int prefix = 24;
                    if (*slash) {
                        *slash++ = '\0';
                        prefix = 0;
                        while (*slash >= '0' && *slash <= '9') {
                            prefix = prefix * 10 + (*slash++ - '0');
                        }
                    }
                    if (parse_ip(word + 8, &a->address) && prefix >= 0 && prefix <= 32) {
                        a->netmask = lwip_htonl(prefix ? 0xFFFFFFFFu << (32 - prefix) : 0);
                    }
                } else if (strncmp(word, "gateway=", 8) == 0) {
                    parse_ip(word + 8, &a->gateway);
                } else if (strncmp(word, "dns=", 4) == 0) {
                    char *d = word + 4;
                    for (int i = 0; i < 2 && *d; i++) {
                        char *comma = d;
                        while (*comma && *comma != ',') {
                            comma++;
                        }
                        bool more = *comma == ',';
                        *comma = '\0';
                        parse_ip(d, &a->dns[i]);
                        d = more ? comma + 1 : comma;
                    }
                }
            }
            break;
        }
        line = next;
    }
    kfree(buf);
}

/* --- lwIP's side of a card ----------------------------------------------------------------------- */

static err_t link_output(struct netif *netif, struct pbuf *p) {
    struct adapter *a = netif->state;
    struct net_device *dev = a->dev;
    if (p->tot_len > NET_FRAME_MAX) {
        dev->tx_dropped++;
        return ERR_IF;
    }
    /* (Always under the core lock, so one frame buffer per card is enough.) */
    uint16_t len = pbuf_copy_partial(p, a->tx_frame, p->tot_len, 0);
    if (dev->transmit(dev, a->tx_frame, len) < 0) {
        dev->tx_dropped++;
        return ERR_IF;
    }
    return ERR_OK;
}

static err_t adapter_init(struct netif *netif) {
    struct adapter *a = netif->state;
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, a->dev->mac, 6);
    netif->mtu = NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    netif_set_hostname(netif, a->hostname);
    return ERR_OK;
}

/* The adapter carrying the internet: the first one connected with a gateway (else just connected). */
static void choose_default(void) {
    struct adapter *best = NULL;
    for (int i = 0; i < adapter_count; i++) {
        struct adapter *a = &adapters[i];
        struct netif *n = &a->netif;
        if (!netif_is_up(n) || !netif_is_link_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) {
            continue;
        }
        if (!best || (ip4_addr_isany_val(*netif_ip4_gw(&best->netif)) && !ip4_addr_isany_val(*netif_ip4_gw(n)))) {
            best = a;
        }
    }
    netif_set_default(best ? &best->netif : NULL);
    /* DNS servers set by hand win over DHCP's. */
    if (best && best->dns[0]) {
        for (int i = 0; i < 2; i++) {
            ip_addr_t d;
            ip_addr_set_ip4_u32_val(d, best->dns[i]);
            dns_setserver((u8_t)i, best->dns[i] ? &d : IP_ADDR_ANY);
        }
    }
}

/* Runs in the tcpip thread whenever an address or the link changes. */
static void status_changed(struct netif *netif) {
    struct adapter *a = netif->state;
    bool connected = netif_is_up(netif) && netif_is_link_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif));
    choose_default();
    if (connected && !a->was_connected) {
        char ip[16], gw[16];
        format_ip(ip, ip4_addr_get_u32(netif_ip4_addr(netif)));
        format_ip(gw, ip4_addr_get_u32(netif_ip4_gw(netif)));
        kprintf("[net] %s: connected, %s/%d, gateway %s (%s)\n", a->dev->name, ip,
                prefix_of(ip4_addr_get_u32(netif_ip4_netmask(netif))), gw,
                a->method == OS_NETIF_AUTO ? "DHCP" : "manual");
    } else if (!connected && a->was_connected) {
        kprintf("[net] %s: disconnected\n", a->dev->name);
    }
    a->was_connected = connected;
    net_socket_event();
}

/* Bring the adapter to what its settings say. Core lock held. */
static void apply_settings(struct adapter *a) {
    struct netif *n = &a->netif;
    if (a->dhcp_running) {
        dhcp_release_and_stop(n);
        a->dhcp_running = false;
    }
    netif_set_addr(n, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    if (!a->enabled) {
        netif_set_down(n);
        status_changed(n);
        return;
    }
    netif_set_up(n);
    if (a->method == OS_NETIF_MANUAL) {
        ip4_addr_t ip, mask, gw;
        ip4_addr_set_u32(&ip, a->address);
        ip4_addr_set_u32(&mask, a->netmask);
        ip4_addr_set_u32(&gw, a->gateway);
        netif_set_addr(n, &ip, &mask, &gw);
    } else {
        dhcp_start(n);   /* (it waits for the link by itself) */
        a->dhcp_running = true;
    }
    status_changed(n);
}

static void link_callback_fn(void *arg) {
    struct adapter *a = arg;
    if (a->dev->link_up) {
        netif_set_link_up(&a->netif);   /* DHCP starts over by itself */
    } else {
        netif_set_link_down(&a->netif);
    }
}

void net_link_changed(struct net_device *dev) {
    struct adapter *a = dev->stack;
    if (!a) {
        return;
    }
    if (dev->link_up) {
        kprintf("[net] %s: link up, %u Mbit/s %s duplex\n", dev->name, dev->speed_mbps,
                dev->full_duplex ? "full" : "half");
    } else {
        kprintf("[net] %s: link down (cable unplugged?)\n", dev->name);
    }
    tcpip_try_callback(link_callback_fn, a);
}

void net_register(struct net_device *dev) {
    if (adapter_count == MAX_ADAPTERS || !stack_ready) {
        return;
    }
    struct adapter *a = &adapters[adapter_count];
    memset(a, 0, sizeof(*a));
    a->dev = dev;
    dev->index = adapter_count;
    dev->name[0] = 'e';
    dev->name[1] = 't';
    dev->name[2] = 'h';
    dev->name[3] = (char)('0' + adapter_count);
    dev->name[4] = '\0';
    read_hostname(a->hostname, sizeof(a->hostname));
    load_settings(a);

    LOCK_TCPIP_CORE();
    if (!netif_add(&a->netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, a, adapter_init, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        kprintf("[WARN] net: can't add %s\n", dev->name);
        return;
    }
    netif_set_status_callback(&a->netif, status_changed);
    netif_set_link_callback(&a->netif, status_changed);
    dev->stack = a;
    adapter_count++;
    if (dev->link_up) {
        netif_set_link_up(&a->netif);
    }
    apply_settings(a);
    UNLOCK_TCPIP_CORE();

    char mac[18];
    format_mac(mac, dev->mac);
    kprintf("[net] %s: %s (%s), MAC %s, %s\n", dev->name, dev->model, dev->driver, mac,
            !a->enabled ? "turned off" : a->method == OS_NETIF_AUTO ? "DHCP" : "manual address");
    net_wake();
}

/* --- receiving: the "net" thread ----------------------------------------------------------------- */

void net_receive(struct net_device *dev, const void *frame, size_t len) {
    struct adapter *a = dev->stack;
    if (!a || len < 14 || len > NET_FRAME_MAX + 4) {
        dev->rx_errors++;
        return;
    }
    dev->rx_packets++;
    dev->rx_bytes += len;
    if (!netif_is_up(&a->netif)) {
        return;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (!p) {
        dev->rx_dropped++;
        return;
    }
    pbuf_take(p, frame, (u16_t)len);
    if (a->netif.input(p, &a->netif) != ERR_OK) {
        pbuf_free(p);
        dev->rx_dropped++;
    }
}

void net_wake(void) {
    wake_pending = true;
    wait_queue_wake_one(&wake);
}

static void net_thread(void *arg) {
    (void)arg;
    for (;;) {
        bool polling = false;
        for (int i = 0; i < adapter_count; i++) {
            struct net_device *dev = adapters[i].dev;
            dev->poll(dev);
            polling |= !dev->has_interrupt;
        }
        uint64_t irq = irq_save();
        if (!wake_pending) {
            wait_queue_sleep_timeout(&wake, polling ? POLL_MS_NO_IRQ : POLL_MS_IDLE);
        }
        wake_pending = false;
        irq_restore(irq);
    }
}

static volatile bool tcpip_started;

static void tcpip_ready(void *arg) {
    (void)arg;
    tcpip_started = true;
}

void net_init(void) {
    tcpip_init(tcpip_ready, NULL);
    while (!tcpip_started) {
        sched_sleep_ms(1);
    }
    stack_ready = true;
    thread_create("net", net_thread, NULL);
}

/* --- what netctl sees and changes ------------------------------------------------------------- */

static struct adapter *find_adapter(const char *name) {
    for (int i = 0; i < adapter_count; i++) {
        if (strcmp(adapters[i].dev->name, name) == 0) {
            return &adapters[i];
        }
    }
    return NULL;
}

int net_netif_info(int index, struct os_netif_info *out) {
    memset(out, 0, sizeof(*out));
    if (index < 0 || index > adapter_count) {
        return -ENOENT;
    }
    if (index == adapter_count) {   /* the loopback, last */
        strlcpy(out->name, "lo", sizeof(out->name));
        strlcpy(out->driver, "loopback", sizeof(out->driver));
        strlcpy(out->model, "Loopback (this computer)", sizeof(out->model));
        out->mtu = 65535;
        out->state = OS_NETIF_CONNECTED;
        out->method = OS_NETIF_MANUAL;
        out->flags = OS_NETIF_ENABLED | OS_NETIF_LOOPBACK;
        out->address = lwip_htonl(0x7F000001);
        out->netmask = lwip_htonl(0xFF000000);
        return 0;
    }
    struct adapter *a = &adapters[index];
    struct net_device *dev = a->dev;
    struct netif *n = &a->netif;
    strlcpy(out->name, dev->name, sizeof(out->name));
    strlcpy(out->driver, dev->driver, sizeof(out->driver));
    strlcpy(out->model, dev->model, sizeof(out->model));
    memcpy(out->mac, dev->mac, 6);
    out->mtu = NET_MTU;
    out->method = a->method;
    out->speed_mbps = dev->link_up ? dev->speed_mbps : 0;
    out->full_duplex = dev->full_duplex;

    LOCK_TCPIP_CORE();
    out->address = ip4_addr_get_u32(netif_ip4_addr(n));
    out->netmask = ip4_addr_get_u32(netif_ip4_netmask(n));
    out->gateway = ip4_addr_get_u32(netif_ip4_gw(n));
    if (!a->enabled) {
        out->state = OS_NETIF_DISCONNECTED;
    } else if (!dev->link_up) {
        out->state = OS_NETIF_UNAVAILABLE;
    } else if (out->address) {
        out->state = OS_NETIF_CONNECTED;
    } else {
        out->state = OS_NETIF_CONNECTING;
    }
    out->flags = (a->enabled ? OS_NETIF_ENABLED : 0) | (netif_default == n ? OS_NETIF_DEFAULT_ROUTE : 0);
    if (out->state == OS_NETIF_CONNECTED) {
        for (int i = 0; i < 2; i++) {
            const ip_addr_t *d = dns_getserver((u8_t)i);
            out->dns[i] = d ? ip4_addr_get_u32(ip_2_ip4(d)) : 0;
        }
    }
    struct dhcp *dhcp = netif_dhcp_data(n);
    if (a->dhcp_running && dhcp && dhcp_supplied_address(n)) {
        out->dhcp_server = ip4_addr_get_u32(ip_2_ip4(&dhcp->server_ip_addr));
        out->lease_seconds = dhcp->offered_t0_lease;
    }
    UNLOCK_TCPIP_CORE();

    out->rx_packets = dev->rx_packets;
    out->rx_bytes = dev->rx_bytes;
    out->rx_errors = dev->rx_errors;
    out->rx_dropped = dev->rx_dropped;
    out->tx_packets = dev->tx_packets;
    out->tx_bytes = dev->tx_bytes;
    out->tx_errors = dev->tx_errors;
    out->tx_dropped = dev->tx_dropped;
    return 0;
}

int net_netif_set(const struct os_netif_config *c) {
    struct adapter *a = find_adapter(c->name);
    if (!a) {
        return strcmp(c->name, "lo") == 0 ? -EPERM : -ENODEV;
    }
    LOCK_TCPIP_CORE();
    int err = 0;
    switch (c->op) {
    case OS_NETIF_SET_CONFIG:
        if (c->method == OS_NETIF_MANUAL && (!c->address || !c->netmask)) {
            err = -EINVAL;
            break;
        }
        a->method = c->method == OS_NETIF_MANUAL ? OS_NETIF_MANUAL : OS_NETIF_AUTO;
        a->address = c->address;
        a->netmask = c->netmask;
        a->gateway = c->gateway;
        a->dns[0] = c->dns[0];
        a->dns[1] = c->dns[1];
        apply_settings(a);
        break;
    case OS_NETIF_SET_UP:
        a->enabled = true;
        apply_settings(a);
        break;
    case OS_NETIF_SET_DOWN:
        a->enabled = false;
        apply_settings(a);
        break;
    case OS_NETIF_SET_RENEW:
        if (!a->enabled || a->method != OS_NETIF_AUTO) {
            err = -EINVAL;
        } else {
            apply_settings(a);   /* release, then a fresh DISCOVER */
        }
        break;
    default:
        err = -EINVAL;
        break;
    }
    UNLOCK_TCPIP_CORE();
    return err;
}

int net_resolve(const char *name, uint32_t *address) {
    ip_addr_t a;
    if (!ipaddr_aton(name, &a) && ip_addr_isany(dns_getserver(0)) && strcmp(name, "localhost") != 0) {
        return -ENETUNREACH;   /* no DNS server to ask (not connected) */
    }
    err_t err = netconn_gethostbyname(name, &a);
    if (err != ERR_OK) {
        return err == ERR_VAL || err == ERR_ARG ? -ENOENT : -EHOSTUNREACH;
    }
    *address = ip4_addr_get_u32(ip_2_ip4(&a));
    return 0;
}
