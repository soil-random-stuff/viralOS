/* Starting the network card drivers: in a thread of their own, since bringing a card up (resets,
   PHY auto-negotiation) can take a while and shouldn't hold up booting. */

#include "drivers/net/nic.h"
#include "drivers/pci.h"
#include "net/net.h"
#include "sched/sched.h"

static void netcards_thread(void *arg) {
    (void)arg;
    pci_init();
    virtio_net_probe();
    e1000_probe();
    e1000e_probe();
    r8169_probe();
    thread_exit();
}

void net_drivers_init(void) {
    thread_create("netcards", netcards_thread, NULL);
}
