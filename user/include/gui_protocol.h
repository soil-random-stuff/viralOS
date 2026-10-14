#pragma once

/*
 * How programs talk to the desktop (/usr/bin/divine-desktop), which owns the
 * screen. Programs don't use this directly: libgui (<gui.h>) does.
 *
 * A program connects to the "desktop" service and says hello. Each window
 * it creates comes with a shared memory buffer holding the window's
 * content (premultiplied ARGB, width * height pixels, no padding): the
 * program draws there, then reports the changed part (DAMAGE) and the
 * desktop puts it on the screen. The title bar, shadow, rounded corners,
 * moving and stacking are all the desktop's job.
 *
 * When a window changes size (maximize/restore), the desktop sends a
 * RESIZE event with the new content size; the program answers with a
 * BUFFER message carrying a new buffer of that size. Until then the
 * desktop shows what it has (the rest stays white).
 *
 * Every message starts with struct gui_msg (type, window id). Window ids
 * are chosen by the program, unique within its connection.
 *
 * Dragging files: while a mouse button is held in its window, a program
 * writes the paths (one per line) to GUI_DRAG_FILE and sends DRAG; the
 * desktop then moves them along with the pointer. Dropped on a window,
 * that window's program gets a GUI_EVENT_DROP (the paths are in the same
 * file); dropped on the desktop, the desktop moves the files itself.
 */

#define GUI_DRAG_FILE _PATH_TMP "/.dragged"

#include <stdint.h>
#include <paths.h>

#define GUI_SERVICE "desktop"

enum gui_msg_type {
    /* program -> desktop */
    GUI_MSG_HELLO = 1,      /* struct gui_msg_hello */
    GUI_MSG_CREATE,         /* struct gui_msg_create, with the buffer's shm handle */
    GUI_MSG_BUFFER,         /* struct gui_msg_buffer, with the new buffer's shm handle */
    GUI_MSG_DAMAGE,         /* struct gui_msg_damage */
    GUI_MSG_TITLE,          /* struct gui_msg_title */
    GUI_MSG_SHOW,           /* struct gui_msg_show */
    GUI_MSG_RAISE,          /* struct gui_msg (raise and focus) */
    GUI_MSG_DESTROY,        /* struct gui_msg */
    GUI_MSG_MENU,           /* struct gui_msg_menu (window 0: not tied to a window) */
    GUI_MSG_DRAG,           /* struct gui_msg_drag: start dragging files (paths in GUI_DRAG_FILE) */
    GUI_MSG_CURSOR,         /* struct gui_msg_cursor: the pointer's look over the window's content */
    GUI_MSG_FULLSCREEN,     /* struct gui_msg_fullscreen: the whole screen, no title bar or panel */
    GUI_MSG_SETTINGS,       /* struct gui_msg_settings: the desktop's settings changed (Settings) */
    GUI_MSG_SCREENSHOT,     /* struct gui_msg_screenshot, with a buffer's shm handle: see below */

    /* desktop -> program */
    GUI_MSG_WELCOME = 100,  /* struct gui_msg_welcome */
    GUI_MSG_EVENT,          /* struct gui_msg_event */
    GUI_MSG_STATE,          /* struct gui_msg_state: shown/hidden/focused changed */
};

struct gui_msg {
    uint32_t type;
    uint32_t window;
};

struct gui_msg_hello {
    struct gui_msg header;
    int64_t pid;
};

struct gui_msg_welcome {
    struct gui_msg header;
    int32_t screen_width, screen_height;
};

struct gui_msg_create {
    struct gui_msg header;
    int32_t x, y;                /* < 0: let the desktop place it */
    int32_t width, height;       /* content size = the buffer's size */
    char title[96];
    uint32_t owner;              /* GUI_WINDOW_DIALOG: the window it belongs to (0: none) */
    uint32_t flags;              /* GUI_WINDOW_* */
};

/* A dialog: centred over its owner, which takes no input while it's open; only a close button,
   and it can't be resized. */
#define GUI_WINDOW_DIALOG 1

struct gui_msg_buffer {
    struct gui_msg header;
    int32_t width, height;
};

struct gui_msg_damage {
    struct gui_msg header;
    int32_t x, y, width, height; /* in content coordinates */
};

struct gui_msg_title {
    struct gui_msg header;
    char title[96];
};

struct gui_msg_show {
    struct gui_msg header;
    uint32_t visible;
};

#define GUI_MENU_ITEMS_MAX 24

struct gui_msg_menu_item {
    char label[56];              /* empty: a separator */
    int32_t id;
    uint8_t disabled, checked, reserved[2];
};

struct gui_msg_menu {
    struct gui_msg header;
    int32_t x, y;                /* screen position */
    int32_t count;
    struct gui_msg_menu_item items[GUI_MENU_ITEMS_MAX];
};

struct gui_msg_event {
    struct gui_msg header;       /* window: 0 for none */
    uint32_t type;               /* enum gui_event_type */
    int32_t x, y, screen_x, screen_y;
    uint32_t button, buttons;
    int32_t wheel;
    uint32_t key, ch, modifiers;
    int32_t menu_id;
    int32_t width, height;       /* GUI_EVENT_RESIZE: the new content size */
    int32_t count;               /* GUI_EVENT_DROP: how many paths */
};

struct gui_msg_cursor {
    struct gui_msg header;
    uint32_t cursor;             /* enum gui_cursor */
};

struct gui_msg_fullscreen {
    struct gui_msg header;
    uint32_t on;
};

/* Re-read ~/.config/dde/settings.conf and /usr/share/applications (pinned apps); and the desktop icons' size (0-2) and auto
   arrangement (0/1), which the desktop keeps itself (-1: unchanged). */
struct gui_msg_settings {
    struct gui_msg header;
    int32_t icon_size;
    int32_t auto_arrange;
};

/*
 * A picture of the screen (the Snipping Tool): the program sends a buffer of
 * GUI_SCREENSHOT_SIZE(width, height) bytes, width x height being the screen's. The desktop draws
 * the screen into it as it is, but without the asking program's windows and the pointer, then
 * puts a struct gui_shot_windows after the pixels: the other windows, the top one first. It
 * answers with a GUI_EVENT_SCREENSHOT event (window 0): x = 1 if it worked, 0 if not (the
 * screen is locked, or the buffer is the wrong size).
 */
#define GUI_SHOT_WINDOWS_MAX 32

struct gui_msg_screenshot {
    struct gui_msg header;
    int32_t width, height;
};

struct gui_shot_window {
    int32_t x, y, width, height; /* on screen, title bar included */
    char title[96];
};

struct gui_shot_windows {
    int32_t count;
    struct gui_shot_window windows[GUI_SHOT_WINDOWS_MAX];
};

#define GUI_SCREENSHOT_SIZE(w, h) ((uint64_t)(w) * (uint64_t)(h) * 4 + sizeof(struct gui_shot_windows))

struct gui_msg_drag {
    struct gui_msg header;
    int32_t count;
};

struct gui_msg_state {
    struct gui_msg header;
    uint32_t visible, focused, maximized;
};
