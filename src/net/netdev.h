#pragma once

/*
 * What a network card driver (drivers/net/) gives the network stack, and what it gets back.
 *
 * A driver fills in a struct net_device and calls net_register(). From then on:
 *   - transmit() is called to send one Ethernet frame (the driver copies it; a full ring = drop);
 *   - poll() is called from the "net" thread whenever the card may have something: received frames
 *     go to net_receive(), finished transmits are reclaimed, the link state is refreshed. The thread
 *     runs it after the driver's interrupt calls net_wake(), or every tick for cards without one.
 *   - the driver reports link changes with net_link_changed().
 * poll() and transmit() can run at the same time on different threads: the transmit ring is the
 * driver's to protect (irq_save is enough, under the big kernel lock).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MTU        1500
#define NET_FRAME_MAX  1518   /* 14-byte header + MTU + 4-byte VLAN tag */

struct net_device {
    /* filled in by the driver */
    const char *driver;         /* "virtio-net", "e1000", "e1000e", "r8169" */
    char model[64];             /* "Intel 82574L Gigabit Ethernet" */
    uint8_t mac[6];
    bool has_interrupt;         /* false: poll every tick */
    void *priv;
    int (*transmit)(struct net_device *dev, const void *frame, size_t len);
    void (*poll)(struct net_device *dev);

    /* kept up to date by the driver */
    bool link_up;
    uint32_t speed_mbps;        /* 0 = unknown */
    bool full_duplex;
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_errors, tx_dropped;

    /* the network stack's (net.c) */
    char name[16];              /* "eth0" */
    int index;
    void *stack;
};

/* A new card: gets its name ("eth0", ...) and joins the network as its saved settings say. */
void net_register(struct net_device *dev);
/* A frame the card received (copied; any size up to NET_FRAME_MAX). Only from poll(). */
void net_receive(struct net_device *dev, const void *frame, size_t len);
/* The cable was plugged in or out (dev->link_up, speed and duplex already updated). */
void net_link_changed(struct net_device *dev);
/* Something happened on a card: run the poll()s soon. Safe in interrupt handlers. */
void net_wake(void);
