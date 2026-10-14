#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "acpi/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/syscall.h"
#include "drivers/hda.h"
#include "drivers/usb/usb.h"
#include "block/block.h"
#include "drivers/console.h"
#include "drivers/display.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/rtc.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "fs/tar.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "shell/shell.h"
#include "net/net.h"
#include <paths.h>

#define TIMER_HZ          100
#define KERNEL_STACK_SIZE (64 * 1024)

static uint64_t kernel_stack_top;

/* Where Limine loaded initrd.tar (copied out before bootloader memory is reclaimed). */
static const uint8_t *initrd_data;
static uint64_t initrd_size;

/*
 * Limine requests. The bootloader finds these between the start and end
 * markers (see linker.ld) and fills in their `response` pointers before
 * jumping to kmain.
 */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

/* The other processors: Limine wakes them and parks them until we give them somewhere to go. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0,   /* xAPIC mode, which apic.c speaks */
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static const char *firmware_name(uint64_t type) {
    switch (type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "BIOS";
    case LIMINE_FIRMWARE_TYPE_EFI32:   return "UEFI (32-bit)";
    case LIMINE_FIRMWARE_TYPE_EFI64:   return "UEFI (64-bit)";
    default:                           return "unknown";
    }
}

static void status_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status_ok(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t irq = irq_save();   /* one line, even with the disks thread printing too */
    kprintf("[ OK ] ");
    kvprintf(fmt, ap);
    kprintf("\n");
    irq_restore(irq);
    va_end(ap);
}

static void print_boot_info(struct limine_framebuffer *fb) {
    console_set_color(0x7FD4FF, 0x101018);
    kprintf("viralKernel " VIRALKERNEL_VERSION "\n");
    console_set_color(0xD0D0D0, 0x101018);
    kprintf("=================\n\n");

    if (bootloader_info_request.response) {
        kprintf("Bootloader:  %s %s\n",
                bootloader_info_request.response->name,
                bootloader_info_request.response->version);
    }
    if (firmware_type_request.response) {
        kprintf("Firmware:    %s\n",
                firmware_name(firmware_type_request.response->firmware_type));
    }
    if (fb) {
        kprintf("Framebuffer: %lux%lu, %u bpp at %p\n",
                fb->width, fb->height, fb->bpp, fb->address);
    }
    kprintf("\n");
}

static void find_initrd(void) {
    struct limine_module_response *resp = module_request.response;
    if (!resp) {
        return;
    }
    for (uint64_t i = 0; i < resp->module_count; i++) {
        struct limine_file *f = resp->modules[i];
        if (f->string && strcmp(f->string, "initrd") == 0) {
            initrd_data = f->address;
            initrd_size = f->size;
            return;
        }
    }
}

/* Build the root filesystem in RAM from initrd.tar, then free the archive. */
static void mount_root(void) {
    vfs_init();
    if (!initrd_data) {
        kprintf("[WARN] No initrd module: root filesystem is empty\n");
        return;
    }

    struct tar_result r;
    int err = tar_unpack(initrd_data, initrd_size, &r);
    if (err) {
        panic("initrd.tar is invalid: %s", strerror(err));
    }

    /* Every byte now lives in ramfs, so the module's pages can be reused. */
    uint64_t phys = (uint64_t)initrd_data - hhdm_offset;
    uint64_t freed = pmm_release_range(phys, initrd_size);
    initrd_data = NULL;

    status_ok("Root filesystem: %lu files, %lu dirs, %lu bytes unpacked into RAM "
              "(%lu KiB archive freed)", r.files, r.directories, r.bytes, freed / 1024);
    if (r.skipped) {
        kprintf("[WARN] initrd: skipped %lu unsupported entries (links, devices)\n", r.skipped);
    }
}

/* Print a small file from the filesystem, if it exists. */
static void print_file(const char *path) {
    struct vnode *f;
    if (vfs_resolve(NULL, path, &f) != 0) {
        return;
    }
    char buf[512];
    int64_t n = vfs_read(f, 0, buf, sizeof(buf));
    if (n > 0) {
        kwrite(buf, (size_t)n);
    }
    vfs_put(f);
}

#define DEFAULT_INIT _PATH_SHELL

/* Start a program on the console and wait for it to finish. */
static void run_program(const char *path) {
    char copy[VFS_PATH_MAX];
    strlcpy(copy, path, sizeof(copy));
    char *argv[] = { copy };
    struct spawn_options opts = { 0 };
    int err;
    struct process *p = process_spawn(copy, 1, argv, &opts, &err);
    if (!p) {
        kprintf("[WARN] cannot start %s: %s\n", copy, strerror(err));
        return;
    }
    int code = process_wait(p);
    kprintf("\n[%s exited with status %d]\n", copy, code);
}

/*
 * Start the first user program: the path in /etc/init (first
 * line), or the shell. Returns when it (and the fallback shell) exit.
 */
static void run_init(void) {
    char path[VFS_PATH_MAX];
    strlcpy(path, DEFAULT_INIT, sizeof(path));

    struct vnode *cfg;
    if (vfs_resolve(NULL, _PATH_INIT_CONF, &cfg) == 0) {
        int64_t n = vfs_read(cfg, 0, path, sizeof(path) - 1);
        vfs_put(cfg);
        path[n > 0 ? n : 0] = '\0';
        for (int64_t i = 0; i < n; i++) {
            if (path[i] == '\n' || path[i] == '\r' || path[i] == ' ') {
                path[i] = '\0';
                break;
            }
        }
        if (!path[0]) {
            strlcpy(path, DEFAULT_INIT, sizeof(path));
        }
    }

    run_program(path);
    /* The desktop is gone (exited, or Ctrl+Alt+Backspace): carry on in the text shell. */
    if (strcmp(path, DEFAULT_INIT) != 0) {
        kprintf("Starting the text shell. Type 'desktop' to go back to the desktop.\n");
        run_program(DEFAULT_INIT);
    }
}

/*
 * Second half of boot, running on our own guarded kernel stack and page
 * tables. Nothing here may touch Limine's responses: their memory is
 * handed to the page allocator first thing.
 */
static void kmain_stage2(void) {
    uint64_t reclaimed = pmm_reclaim_bootloader_memory();
    status_ok("Switched to guarded kernel stack; reclaimed %lu KiB of bootloader memory",
              reclaimed / 1024);

    if (!heap_self_test()) {
        panic("kernel heap self-test failed");
    }
    status_ok("Kernel heap ready (self-test passed)");

    /* From here on, this code is thread 1 ("kmain"). */
    sched_init(kernel_stack_top, KERNEL_STACK_SIZE);
    timer_init(TIMER_HZ);
    keyboard_init();
    interrupts_enable();
    status_ok("Scheduler running: round-robin, %u Hz tick", TIMER_HZ);

    /* Proves the timer and the scheduler work: this only returns if ticks advance. */
    uint64_t start = timer_ticks();
    sched_sleep_ms(100);
    status_ok("APIC timer at %lu MHz input clock (%lu ticks in 100 ms)",
              lapic_timer_frequency() / 1000000, timer_ticks() - start);
    status_ok("PS/2 keyboard on I/O APIC");
    mouse_init();
    if (mouse_present()) {
        status_ok("PS/2 mouse%s", mouse_absolute() ? " with absolute positioning (vmmouse)" : "");
    } else {
        kprintf("[WARN] No PS/2 mouse found\n");
    }

    rtc_init();
    if (audio_init()) {
        uint32_t id;
        int outputs;
        audio_info(&id, &outputs);
        status_ok("Sound: Intel HD Audio, codec %04x:%04x, %d output%s", id >> 16, id & 0xFFFF, outputs,
                  outputs == 1 ? "" : "s");
    } else {
        kprintf("[WARN] No sound card found\n");
    }
    mount_root();
    devfs_init();
    block_init();
    usb_init();
    net_init();
    status_ok("TCP/IP stack ready (lwIP)");
    net_drivers_init();

    fpu_init();
    status_ok("FPU for programs: %s (%lu-byte save area per thread)",
              fpu_features(), fpu_state_size());
    syscall_init();
    process_init();
    status_ok("User mode ready: programs load from /usr/bin");
    smp_release_aps();
    status_ok("%u CPU%s running programs (one at a time in the kernel)", smp_cpu_count(),
              smp_cpu_count() == 1 ? "" : "s");
    kprintf("\n");
    print_file(_PATH_MOTD);
    kprintf("\n");

    run_init();

    /* The first program is gone (e.g. `exit` in the shell): fall back to the
       kernel's own console, from which it can be started again. */
    kprintf("\nStarting the kernel console. Type 'help', or 'shell' to go back.\n");
    shell_run();
}

void kmain(void) {
    serial_init();

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        serial_write("panic: bootloader does not support Limine base revision 6\n");
        halt_forever();
    }

    struct limine_framebuffer_response *fb_resp = framebuffer_request.response;
    struct limine_framebuffer *fb = NULL;
    if (fb_resp && fb_resp->framebuffer_count > 0) {
        fb = fb_resp->framebuffers[0];
        if (!console_init(fb)) {
            serial_write("warning: framebuffer format not supported, serial only\n");
        }
    }
    print_boot_info(fb);

    /* Exception handlers come first so any mistake below gets a proper report. */
    smp_init_bsp();
    status_ok("GDT and TSS loaded");

    idt_init();
    status_ok("IDT loaded: 256 vectors");

    if (!memmap_request.response || !hhdm_request.response ||
        !executable_address_request.response) {
        panic("bootloader did not provide the memory map, HHDM or kernel address");
    }
    hhdm_offset = hhdm_request.response->offset;
    /* Needs hhdm_offset (to find the framebuffer's physical address); copies
       what it needs, since Limine's structures go away later. */
    display_init(fb);

    pmm_init(memmap_request.response);
    status_ok("Physical memory: %lu MiB usable, %lu MiB free",
              pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);

    vmm_init(executable_address_request.response->physical_base,
             executable_address_request.response->virtual_base);
    status_ok("Kernel page tables active (NX %s, write-combining framebuffer)",
              vmm_nx_enabled() ? "on" : "unavailable");

    find_initrd();

    /* ACPI tables are read now: the RSDP response is in bootloader memory. */
    if (!rsdp_request.response || !acpi_init(rsdp_request.response->address)) {
        panic("no valid ACPI tables (RSDP missing or bad checksum)");
    }
    pic_disable();
    apic_init();
    status_ok("ACPI %s: %u CPU(s), %u I/O APIC(s); 8259 PIC masked, local APIC %u enabled",
              acpi_revision() >= 2 ? "2.0+" : "1.0", apic_cpu_count(), apic_ioapic_count(),
              lapic_id());

    /* The other processors leave the bootloader's memory now (it's reclaimed next), and wait. */
    smp_start_aps(mp_request.response);

    kernel_stack_top = vmm_alloc_kernel_stack(KERNEL_STACK_SIZE);
    if (!kernel_stack_top) {
        panic("could not allocate the kernel stack");
    }
    gdt_set_kernel_stack(kernel_stack_top);
    switch_stack_and_call(kernel_stack_top, kmain_stage2);
}
