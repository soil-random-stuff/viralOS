/*
 * Network: every adapter (not the loopback), with
 *   - its state and a switch to connect / disconnect it;
 *   - IPv4: DHCP or a fixed address, subnet mask, router, DNS servers (Apply / Renew DHCP lease);
 *   - its hardware and traffic.
 * Changes go through libnetconf (the running adapter, and /etc/network.conf for next time).
 * While the user is editing an adapter's fields, the twice-a-second refresh leaves them alone.
 */

#include <image.h>
#include <netconf.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include <paths.h>

#define MAX_ADAPTERS 8

static const char *const methods[] = { "Using DHCP", "Manually" };

struct adapter {
    struct os_netif_info info;
    int method;                  /* 0 DHCP, 1 manual */
    struct field address, mask, router, dns;
    bool touched;                /* edited, not applied yet */
};

static struct adapter adapters[MAX_ADAPTERS];
static int count;
static struct gfx_surface *icon_online, *icon_offline;

static struct gfx_surface *load_icon(const char *path) {
    struct gfx_surface *img = image_load(path, NULL);
    if (!img) {
        return NULL;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, 36, 36);
    gfx_surface_destroy(img);
    return s;
}

/* The fields from what's saved and what the adapter has now. */
static void fill(struct adapter *a) {
    struct netconf c;
    netconf_load(a->info.mac, &c);
    char ip[16];
    a->method = c.manual ? 1 : 0;
    uint32_t address = c.manual ? c.address : a->info.address;
    uint32_t mask = c.manual ? c.netmask : a->info.netmask;
    uint32_t router = c.manual ? c.gateway : a->info.gateway;
    field_set(&a->address, address ? netconf_ip_text(address, ip) : "");
    field_set(&a->mask, mask ? netconf_ip_text(mask, ip) : "");
    field_set(&a->router, router ? netconf_ip_text(router, ip) : "");
    char dns[40] = "";
    if (c.dns[0]) {
        netconf_ip_text(c.dns[0], dns);
        if (c.dns[1]) {
            strlcat(dns, ", ", sizeof(dns));
            strlcat(dns, netconf_ip_text(c.dns[1], ip), sizeof(dns));
        }
    }
    field_set(&a->dns, dns);
    a->touched = false;
}

void page_network_show(void) {
    if (!icon_online) {
        icon_online = load_icon(_PATH_DDE_RES "/Icons/Status/Ethernet.png");
        icon_offline = load_icon(_PATH_DDE_RES "/Icons/Status/NoInternet.png");
    }
    int n = 0;
    for (int i = 0; n < MAX_ADAPTERS; i++) {
        struct os_netif_info info;
        if (netif_info(i, &info) != 0) {
            break;
        }
        if (info.flags & OS_NETIF_LOOPBACK) {
            continue;
        }
        struct adapter *a = &adapters[n];
        bool same = n < count && memcmp(a->info.mac, info.mac, 6) == 0;
        a->info = info;
        if (!same || !a->touched) {
            fill(a);
        }
        n++;
    }
    count = n;
}

/* "255.255.255.0", "/24" or "24". */
static bool parse_mask(const char *s, uint32_t *out) {
    if (*s == '/') {
        s++;
    }
    if (!strchr(s, '.')) {
        uint32_t ip, mask;
        char cidr[24];
        snprintf(cidr, sizeof(cidr), "1.1.1.1/%s", s);
        return netconf_parse_cidr(cidr, &ip, &mask) && (*out = mask, true);
    }
    return netconf_parse_address(s, out) && netconf_prefix(*out) > 0;
}

static void apply(struct adapter *a) {
    struct netconf c;
    netconf_load(a->info.mac, &c);
    c.manual = a->method == 1;
    if (c.manual) {
        if (!netconf_parse_address(a->address.text, &c.address)) {
            settings_message(GUI_ICON_ERROR, "Not an IP address", "Type the address like 192.168.1.20.");
            return;
        }
        if (!parse_mask(a->mask.text, &c.netmask)) {
            settings_message(GUI_ICON_ERROR, "Not a subnet mask", "Type the mask like 255.255.255.0 (or 24).");
            return;
        }
        c.gateway = 0;
        if (a->router.len && !netconf_parse_address(a->router.text, &c.gateway)) {
            settings_message(GUI_ICON_ERROR, "Not a router address", "Type the router like 192.168.1.1, or leave it empty.");
            return;
        }
    }
    c.dns[0] = c.dns[1] = 0;
    char copy[sizeof(a->dns.text)];
    strlcpy(copy, a->dns.text, sizeof(copy));
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
        if (*t && !netconf_parse_address(t, &c.dns[d++])) {
            settings_message(GUI_ICON_ERROR, "Not a DNS server", "Type DNS servers like 1.1.1.1, 8.8.8.8 (or leave it empty to use DHCP's).");
            return;
        }
        if (!more) {
            break;
        }
        t = end + 1;
    }
    int r = netconf_apply(&a->info, &c);
    if (r < 0) {
        settings_message(GUI_ICON_ERROR, "Couldn't change the adapter", strerror(r));
        return;
    }
    sleep_ms(c.manual ? 50 : 600);   /* (a moment for DHCP) */
    a->touched = false;
    page_network_show();
}

/* The adapter's name, state and the connect switch. */
static void adapter_header(struct adapter *a) {
    struct gfx_rect r = ui_row(70);
    struct gfx_surface *icon = netconf_online(&a->info) ? icon_online : icon_offline;
    if (icon && !ui.measuring) {
        gfx_blit(ui.s, r.x + 16, r.y + 17, icon, NULL);
    }
    char title[64];
    snprintf(title, sizeof(title), "Ethernet (%s)", a->info.name);
    ui_text(FONT_BODY + 1, true, r.x + 66, r.y + 31, INK, title);
    char sub[96];
    if (a->info.state == OS_NETIF_CONNECTED) {
        char ip[16];
        snprintf(sub, sizeof(sub), "%s  -  %s", netconf_state_text(&a->info), netconf_ip_text(a->info.address, ip));
    } else {
        strlcpy(sub, netconf_state_text(&a->info), sizeof(sub));
    }
    ui_text_clipped(FONT_SMALL, false, r.x + 66, r.y + 50, r.w - 150, INK_SOFT, sub);
    bool on = a->info.state != OS_NETIF_DISCONNECTED;
    if (ui_toggle(gfx_rect(r.x + r.w - 56, r.y + 23, 40, 24), &on)) {
        int e = netconf_set_power(&a->info, on);
        if (e < 0) {
            settings_message(GUI_ICON_ERROR, "Couldn't change the adapter", strerror(e));
        }
        sleep_ms(on ? 600 : 50);
        page_network_show();
    }
}

static void field_row(struct adapter *a, const char *label, struct field *f, const char *placeholder, bool enabled) {
    char before[sizeof(f->text)];
    strlcpy(before, f->text, sizeof(before));
    if (ui_field_row(label, f, placeholder, enabled) && a->touched) {
        apply(a);
    }
    if (strcmp(before, f->text) != 0) {
        a->touched = true;
    }
}

static void adapter_ipv4(struct adapter *a) {
    ui_heading("IPv4");
    ui_card_begin();
    if (ui_choice_row("Configure IPv4", methods, 2, &a->method)) {
        a->touched = true;
    }
    bool manual = a->method == 1;
    field_row(a, "IP address", &a->address, manual ? "192.168.1.20" : "", manual);
    field_row(a, "Subnet mask", &a->mask, manual ? "255.255.255.0" : "", manual);
    field_row(a, "Router", &a->router, manual ? "192.168.1.1" : "", manual);
    field_row(a, "DNS servers", &a->dns, manual ? "1.1.1.1, 8.8.8.8" : "From DHCP", true);

    /* Buttons: Renew (DHCP), Revert and Apply (when there's something to apply). */
    struct gfx_rect r = ui_row(52);
    int x = r.x + r.w - 16;
    struct gfx_rect apply_r = gfx_rect(x - 90, r.y + 12, 90, 28);
    if (ui_button(apply_r, "Apply", true, a->touched)) {
        apply(a);
    }
    x = apply_r.x - 10;
    if (a->touched) {
        struct gfx_rect revert = gfx_rect(x - 90, r.y + 12, 90, 28);
        if (ui_button(revert, "Revert", false, true)) {
            fill(a);
        }
        x = revert.x - 10;
    }
    if (!manual && a->info.state != OS_NETIF_DISCONNECTED) {
        struct gfx_rect renew = gfx_rect(r.x + 16, r.y + 12, 160, 28);
        if (ui_button(renew, "Renew DHCP Lease", false, true)) {
            netconf_renew(&a->info);
            sleep_ms(600);
            page_network_show();
        }
    }
    ui_card_end();
    if (a->info.dhcp_server && !manual) {
        char ip[16], note[128];
        unsigned h = a->info.lease_seconds / 3600;
        snprintf(note, sizeof(note), "Address from the DHCP server at %s, leased for %u hour%s.",
                 netconf_ip_text(a->info.dhcp_server, ip), h, h == 1 ? "" : "s");
        ui_note(note);
    }
}

static void adapter_details(struct adapter *a) {
    char buf[96], size1[24], size2[24];
    ui_heading("Hardware");
    ui_card_begin();
    netconf_mac_text(a->info.mac, buf);
    ui_info_row("MAC address", buf);
    ui_info_row("Model", a->info.model);
    ui_info_row("Driver", a->info.driver);
    if (a->info.speed_mbps) {
        snprintf(buf, sizeof(buf), "%u Mb/s, %s duplex", a->info.speed_mbps, a->info.full_duplex ? "full" : "half");
    } else {
        strlcpy(buf, a->info.state == OS_NETIF_UNAVAILABLE ? "No cable" : "Unknown", sizeof(buf));
    }
    ui_info_row("Link speed", buf);
    snprintf(buf, sizeof(buf), "%u", a->info.mtu);
    ui_info_row("MTU", buf);
    ui_card_end();

    ui_heading("Traffic");
    ui_card_begin();
    human_size(a->info.rx_bytes, size1, sizeof(size1));
    snprintf(buf, sizeof(buf), "%s (%lu packets)", size1, (unsigned long)a->info.rx_packets);
    ui_info_row("Received", buf);
    human_size(a->info.tx_bytes, size2, sizeof(size2));
    snprintf(buf, sizeof(buf), "%s (%lu packets)", size2, (unsigned long)a->info.tx_packets);
    ui_info_row("Sent", buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(a->info.rx_errors + a->info.tx_errors + a->info.rx_dropped +
                                                        a->info.tx_dropped));
    ui_info_row("Errors and dropped", buf);
    ui_card_end();
}

void page_network(void) {
    if (count == 0) {
        ui_card_begin();
        ui_info_row("No network adapters were found", "");
        ui_card_end();
        ui_note("Supported: virtio-net, Intel e1000 / e1000e (including I217-I219), Realtek RTL8111/8168 and RTL8125.");
        return;
    }
    for (int i = 0; i < count; i++) {
        struct adapter *a = &adapters[i];
        ui_card_begin();
        adapter_header(a);
        ui_card_end();
        adapter_ipv4(a);
        adapter_details(a);
        ui_space(10);
    }
}
