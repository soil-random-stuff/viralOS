#pragma once

#include <stdint.h>

/* A panic: take the screen back from any program, clear it in these colours. */
void console_panic_screen(uint32_t fg_rgb, uint32_t bg_rgb);

#include <stdbool.h>
#include <stdint.h>

struct limine_framebuffer;

/* Text console drawn onto the bootloader-provided framebuffer. */
bool console_init(struct limine_framebuffer *fb);
void console_putc(char c);
void console_set_color(uint32_t fg_rgb, uint32_t bg_rgb);
void console_clear(void);

/* While a program owns the screen, stop drawing (serial output continues).
   Resuming clears the screen. */
void console_suspend(bool suspended);
