#pragma once

/* Shared between the parts of libgui (the program side of the desktop protocol). */

#include <gui.h>
#include <gui_protocol.h>
#include <unistd.h>

#define EVENT_QUEUE 64

struct gui_window {
    struct gui *gui;
    uint32_t id;
    struct gfx_surface content;   /* over the shared buffer */
    void *buffer;                 /* the shared memory mapping */
    struct gfx_rect damage;       /* not yet reported; empty if none */
    bool visible, focused;
    int cursor;                   /* enum gui_cursor last asked for */
    void *user_data;
    struct gui_window *next;
};

struct gui {
    int fd;                       /* channel to the desktop */
    int screen_width, screen_height;
    struct ttf_font *fonts[GUI_FONT_COUNT];
    struct gui_window *windows;
    uint32_t next_id;
    bool disconnected;

    struct gui_event queue[EVENT_QUEUE];
    int queue_head, queue_len;

    int shot_result;              /* gui_screenshot's answer: 0 none yet, 1 taken, -1 refused */
};

/* gui.c */
bool gui_send(struct gui *g, const void *msg, size_t len, int handle);
void gui_push_event(struct gui *g, const struct gui_event *ev);

/* window.c */
struct gui_window *gui_find_window(struct gui *g, uint32_t id);
bool gui_window_resize_buffer(struct gui_window *w, int width, int height);
void gui_window_flush(struct gui_window *w);
