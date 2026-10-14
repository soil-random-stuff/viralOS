#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <viralos.h>

struct limine_framebuffer;
struct process;

/* The screen, as handed over by the bootloader. One process can own it at a time. */
void display_init(struct limine_framebuffer *fb);

uint32_t display_width(void);
uint32_t display_height(void);

/* Map the framebuffer into `p` and route keyboard/mouse to it. */
int display_claim(struct process *p, struct os_display_info *info);
/* Give the screen back to the text console (if `pid` owns it). */
void display_release(uint64_t pid);
bool display_claimed(void);
/* Ctrl+Alt+Backspace: kill whoever owns the screen. */
void display_zap(void);
