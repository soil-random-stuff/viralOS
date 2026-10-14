#include <limine.h>

#include "drivers/display.h"
#include "arch/x86_64/cpu.h"
#include "drivers/console.h"
#include "drivers/input.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "proc/process.h"

static struct {
    uint64_t phys;
    uint64_t size;
    uint32_t width, height, pitch, bpp;
    uint8_t red_shift, green_shift, blue_shift;
} fb;

static uint64_t owner;   /* pid, 0 = the text console has the screen */

void display_init(struct limine_framebuffer *f) {
    if (!f) {
        return;
    }
    fb.phys = (uint64_t)f->address - hhdm_offset;
    fb.width = (uint32_t)f->width;
    fb.height = (uint32_t)f->height;
    fb.pitch = (uint32_t)f->pitch;
    fb.bpp = f->bpp;
    fb.size = ALIGN_UP((uint64_t)fb.pitch * fb.height, PAGE_SIZE);
    fb.red_shift = f->red_mask_shift;
    fb.green_shift = f->green_mask_shift;
    fb.blue_shift = f->blue_mask_shift;
}

uint32_t display_width(void) {
    return fb.width;
}

uint32_t display_height(void) {
    return fb.height;
}

bool display_claimed(void) {
    return owner != 0;
}

int display_claim(struct process *p, struct os_display_info *info) {
    if (!fb.phys || fb.bpp != 32) {
        return -ENODEV;
    }
    uint64_t irq = irq_save();
    if (owner && owner != p->pid) {
        irq_restore(irq);
        return -EBUSY;
    }
    owner = p->pid;
    irq_restore(irq);

    /* Write-combining: the CPU batches the stores, much faster for pixels.
       VMM_DEVICE: these pages aren't the process's to free. */
    for (uint64_t off = 0; off < fb.size; off += PAGE_SIZE) {
        if (!vmm_map_user(p->space, USER_FRAMEBUFFER + off, fb.phys + off,
                          VMM_WRITE | VMM_WC | VMM_DEVICE)) {
            display_release(p->pid);
            return -ENOMEM;
        }
    }

    console_suspend(true);
    input_set_owner(p->pid);

    *info = (struct os_display_info){
        .address = USER_FRAMEBUFFER,
        .width = fb.width,
        .height = fb.height,
        .pitch = fb.pitch,
        .bpp = fb.bpp,
        .red_shift = fb.red_shift,
        .green_shift = fb.green_shift,
        .blue_shift = fb.blue_shift,
    };
    return 0;
}

void display_release(uint64_t pid) {
    uint64_t irq = irq_save();
    bool was_owner = owner && owner == pid;
    if (was_owner) {
        owner = 0;
        input_set_owner(0);
    }
    irq_restore(irq);
    if (was_owner) {
        console_suspend(false);   /* clears the screen and goes back to text */
    }
}

void display_zap(void) {
    if (owner) {
        kprintf("[display] Ctrl+Alt+Backspace: stopping pid %lu\n", owner);
        process_kill(owner);
    }
}
