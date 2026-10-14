/*
 * inputtest: raw check of the display and input system calls. Takes over
 * the screen, paints a gradient, follows the mouse with a square and logs
 * every event (the log goes to the serial port while the screen is taken).
 * Escape quits.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static struct os_display_info d;

static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return (r << d.red_shift) | (g << d.green_shift) | (b << d.blue_shift);
}

static void fill(int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= (int)d.height) {
            continue;
        }
        uint32_t *row = (uint32_t *)(d.address + (uint64_t)y * d.pitch);
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && x < (int)d.width) {
                row[x] = color;
            }
        }
    }
}

static uint32_t background(int x, int y) {
    return rgb(30 + x * 40 / d.width, 40 + y * 60 / d.height, 90 + y * 80 / d.height);
}

int main(void) {
    int err = display_claim(&d);
    if (err < 0) {
        printf("inputtest: display_claim: %s\n", strerror(err));
        return 1;
    }
    printf("inputtest: display %ux%u, pitch %u, mapped at %p\n", d.width, d.height, d.pitch,
           (void *)d.address);
    for (uint32_t y = 0; y < d.height; y++) {
        for (uint32_t x = 0; x < d.width; x++) {
            ((uint32_t *)(d.address + (uint64_t)y * d.pitch))[x] = background((int)x, (int)y);
        }
    }

    int mx = -1, my = -1;
    for (;;) {
        struct os_input_event ev[16];
        int n = input_read(ev, 16, 3000);
        if (n == 0) {
            printf("inputtest: (no input for 3 s)\n");
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (ev[i].type == OS_EVENT_KEY) {
                printf("key %#x %s ch=%u mods=%u\n", ev[i].key, ev[i].pressed ? "down" : "up",
                       ev[i].ch, ev[i].modifiers);
                if (ev[i].key == OS_KEY_ESCAPE && ev[i].pressed) {
                    printf("inputtest: bye\n");
                    return 0;
                }
            } else {
                printf("mouse %d,%d d=%d,%d buttons=%u wheel=%d\n", ev[i].x, ev[i].y,
                       ev[i].dx, ev[i].dy, ev[i].buttons, ev[i].wheel);
                if (mx >= 0) {
                    for (int y = my; y < my + 12; y++) {
                        for (int x = mx; x < mx + 12; x++) {
                            fill(x, y, 1, 1, background(x, y));
                        }
                    }
                }
                mx = ev[i].x;
                my = ev[i].y;
                fill(mx, my, 12, 12, ev[i].buttons ? rgb(255, 200, 60) : rgb(255, 255, 255));
            }
        }
    }
}
