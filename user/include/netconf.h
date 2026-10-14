#pragma once

/*
 * libnetconf: network adapter settings, shared by netctl, the desktop's network tray and Settings.
 *
 * They're kept in /etc/network.conf, one line per adapter by its MAC address:
 *     52:54:00:12:34:56  auto|manual  [address=A.B.C.D/prefix] [gateway=A.B.C.D] [dns=A.B.C.D,A.B.C.D] [off]
 * The kernel reads the file when an adapter appears; these functions change it and the running
 * adapter together. Addresses are IPv4 in network byte order, as in struct os_netif_info.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <viralos.h>
#include <paths.h>

#define NETCONF_FILE _PATH_NETWORK_CONF

struct netconf {
    bool off;                    /* kept disconnected */
    bool manual;                 /* a fixed address (else DHCP) */
    uint32_t address, netmask, gateway;
    uint32_t dns[2];             /* 0: DHCP's */
};

/* "52:54:00:12:34:56" (out has room for 18). */
void netconf_mac_text(const uint8_t mac[6], char *out);
/* "10.0.2.15" (out has room for 16); returns out. */
const char *netconf_ip_text(uint32_t address, char *out);
/* 255.255.255.0 -> 24 */
int netconf_prefix(uint32_t netmask);
/* "A.B.C.D" / "A.B.C.D/N" (N defaults to 24). False if it isn't one. */
bool netconf_parse_address(const char *s, uint32_t *out);
bool netconf_parse_cidr(const char *s, uint32_t *address, uint32_t *netmask);

/* This adapter's saved settings (DHCP, on, if it has no line yet). */
void netconf_load(const uint8_t mac[6], struct netconf *out);
/* Save them (`name`, like "eth0", goes in a comment). 0 or -errno. */
int netconf_save(const uint8_t mac[6], const char *name, const struct netconf *c);
/* Apply the address settings to the running adapter (and save them). 0 or -errno. */
int netconf_apply(const struct os_netif_info *i, const struct netconf *c);
/* Connect (on) or disconnect and keep it off (saved too). 0 or -errno. */
int netconf_set_power(const struct os_netif_info *i, bool on);
/* Ask DHCP for an address again. 0, or -EINVAL if it doesn't use DHCP or is off. */
int netconf_renew(const struct os_netif_info *i);

/* A one-line summary for people: "Connected", "No internet", "Cable unplugged"... */
const char *netconf_state_text(const struct os_netif_info *i);
/* Online: connected, with a gateway to the internet. */
bool netconf_online(const struct os_netif_info *i);
