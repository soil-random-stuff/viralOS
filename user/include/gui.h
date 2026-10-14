#pragma once

/*
 * libgui: windows for programs on the desktop.
 *
 * The desktop, DDE (/usr/bin/divine-desktop) owns the screen and draws every window's
 * title bar, shadow and rounded corners; a program just draws the content
 * of its windows and reacts to events. Moving, focusing, raising,
 * minimizing, maximizing and resizing (by dragging the edges) are handled by
 * the desktop; a resized window gets GUI_EVENT_RESIZE and should redraw.
 *
 *     struct gui *g = gui_open();
 *     struct gui_window *w = gui_window_create(g, "Hello", -1, -1, 400, 300);
 *     gfx_clear(gui_window_surface(w), GFX_RGB(255, 255, 255));  // draw
 *     gui_window_invalidate(w, NULL);                           // show it
 *     struct gui_event ev;
 *     while (gui_wait_event(g, &ev, -1)) { ... }
 *
 * Window contents live in memory shared with the desktop, so drawing is
 * just writing pixels; gui_window_invalidate tells the desktop what to
 * put on the screen (sent before the next wait).
 */

#include <stdbool.h>
#include <viralos.h>

#include <gfx.h>
#include <ttf.h>

struct gui;
struct gui_window;

enum gui_font { GUI_FONT_REGULAR, GUI_FONT_BOLD, GUI_FONT_MONO, GUI_FONT_COUNT };

/* Mouse pointers (pictures in /usr/share/dde-res/Cursors). */
enum gui_cursor {
    GUI_CURSOR_ARROW,            /* the normal pointer */
    GUI_CURSOR_HAND,             /* over something clickable like a link */
    GUI_CURSOR_RESIZE_EW,        /* resize left / right */
    GUI_CURSOR_RESIZE_NS,        /* resize up / down */
    GUI_CURSOR_RESIZE_NWSE,      /* resize along the top-left / bottom-right diagonal */
    GUI_CURSOR_RESIZE_NESW,      /* resize along the top-right / bottom-left diagonal */
    GUI_CURSOR_COUNT,
};

enum gui_event_type {
    GUI_EVENT_NONE,         /* gui_wait_event timed out */
    GUI_EVENT_MOUSE_DOWN,   /* in a window's content */
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_SCROLL,       /* wheel: negative = up */
    GUI_EVENT_KEY,          /* key pressed; goes to the focused window */
    GUI_EVENT_MENU,         /* a menu item was chosen: menu_id */
    GUI_EVENT_CLOSE,        /* close button: the program decides what to do */
    GUI_EVENT_RESIZE,       /* the content changed size (maximize): redraw it all */
    GUI_EVENT_FOCUS,        /* the window got or lost the keyboard: focused */
    GUI_EVENT_QUIT,         /* the desktop is gone */
    GUI_EVENT_DROP,         /* files were dropped on the window at x, y: gui_drop_paths() */
    GUI_EVENT_SCREENSHOT,   /* (libgui's own: gui_screenshot waits for it) */
};

struct gui_event {
    enum gui_event_type type;
    struct gui_window *window;   /* NULL: not about a window */
    int x, y;                    /* mouse: in content coordinates */
    int screen_x, screen_y;
    uint32_t button;             /* the button that changed (OS_MOUSE_*) */
    uint32_t buttons;            /* all buttons held */
    int wheel;
    uint32_t key, ch, modifiers; /* GUI_EVENT_KEY: OS_KEY_*, typed character, OS_MOD_* */
    int menu_id;
    bool focused;                /* GUI_EVENT_FOCUS */
    int count;                   /* GUI_EVENT_DROP: how many files */
};

struct gui_menu_item {
    const char *label;           /* NULL: a separator line */
    int id;
    bool disabled;
    bool checked;
};

/* --- the connection ------------------------------------------------------------------ */

struct gui *gui_open(void);      /* NULL (with a message) if the desktop isn't running */
void gui_close(struct gui *g);   /* closes all its windows */

int gui_screen_width(struct gui *g);
int gui_screen_height(struct gui *g);
struct ttf_font *gui_font(struct gui *g, enum gui_font which);

/*
 * Send pending updates, then wait for the next event, at most timeout_ms
 * (-1: forever, 0: just check). Returns false (type GUI_EVENT_NONE) if
 * none came in time.
 */
bool gui_wait_event(struct gui *g, struct gui_event *ev, int timeout_ms);

/* The connection's descriptor, for programs that poll() other things too:
   when it's readable, call gui_wait_event(g, &ev, 0) until it returns false. */
int gui_fd(struct gui *g);
void gui_flush(struct gui *g);   /* send pending updates now */

/* --- windows ----------------------------------------------------------------------- */

/* width/height: the content area. x, y < 0: let the desktop place it. */
struct gui_window *gui_window_create(struct gui *g, const char *title, int x, int y,
                                     int width, int height);
void gui_window_destroy(struct gui_window *w);

/* Draw into this, then call gui_window_invalidate for the part that changed. */
struct gfx_surface *gui_window_surface(struct gui_window *w);
void gui_window_invalidate(struct gui_window *w, const struct gfx_rect *area);   /* NULL: all */

void gui_window_set_title(struct gui_window *w, const char *title);
void gui_window_show(struct gui_window *w, bool visible);   /* also un-minimizes */
bool gui_window_visible(struct gui_window *w);
bool gui_window_focused(struct gui_window *w);
void gui_window_raise(struct gui_window *w);
/* The pointer's look while it's over the window's content (GUI_CURSOR_ARROW at first). */
void gui_window_set_cursor(struct gui_window *w, enum gui_cursor cursor);
/* Cover the whole screen (no title bar, the panel hidden), or go back. A RESIZE event follows. */
void gui_window_set_fullscreen(struct gui_window *w, bool on);
void gui_window_set_user_data(struct gui_window *w, void *data);
void *gui_window_user_data(struct gui_window *w);

/*
 * Settings changed things the desktop shows (see <config.h>: ~/.config/dde/settings.conf, and /usr/share/applications): it
 * re-reads them. icon_size (0 small - 2 large) and auto_arrange (0/1) set the desktop icons' (-1:
 * leave as they are).
 */
void gui_settings_changed(struct gui *g, int icon_size, int auto_arrange);

/* --- screenshots --------------------------------------------------------------------- */

struct gui_screenshot_window {
    struct gfx_rect frame;       /* on screen, title bar included */
    char title[96];
};

struct gui_screenshot {
    struct gfx_surface *image;   /* the whole screen */
    int window_count;
    struct gui_screenshot_window windows[32];   /* the other programs' windows, the top one first */
};

/* The screen as it looks now, without this program's windows or the pointer (they needn't be
   hidden first), and where the other windows are. NULL if it couldn't be taken (e.g. the screen
   is locked). Free it with gui_screenshot_free. */
struct gui_screenshot *gui_screenshot(struct gui *g);
void gui_screenshot_free(struct gui_screenshot *s);

/* --- dragging files ------------------------------------------------------------------ */

/*
 * Start dragging files (call while the mouse button is held, e.g. on a
 * mouse move after a press on a selected item). From then on the desktop
 * moves them with the pointer; the window gets no more mouse events for
 * this press. Where they're dropped decides what happens.
 */
bool gui_start_drag(struct gui_window *w, const char *const *paths, int count);
/* After a GUI_EVENT_DROP: the dropped paths (up to max of them). */
int gui_drop_paths(char (*paths)[OS_PATH_MAX], int max);

/* --- menus ------------------------------------------------------------------------- */

/*
 * Pop up a menu at a screen position (usually the mouse: ev.screen_x/y).
 * The choice comes back as a GUI_EVENT_MENU for `owner`, with the item's
 * id. Clicking elsewhere or pressing Escape closes it without one.
 */
void gui_show_menu(struct gui *g, int x, int y, const struct gui_menu_item *items, int count,
                   struct gui_window *owner);

/* --- dialogs ------------------------------------------------------------------------------ */

/*
 * A dialog is a small window of its own over `owner` (centred on it). While it's open the owner
 * takes no input: clicking it brings the dialog forward. The pictures are in /usr/share/dde-res/Icons/Dialogs.
 *
 *     struct gui_dialog dlg = { GUI_ICON_WARNING, "Delete the folder?", "Everything in it goes too.",
 *                               { "Delete", "Cancel" }, .danger = true };
 *     if (gui_dialog_run(g, window, &dlg) == 0) { ... }
 *
 * gui_dialog_run waits for the answer: events for the program's other windows meanwhile are kept
 * (resizes, focus) or dropped (input), so the program needn't care.
 */
enum gui_dialog_icon { GUI_ICON_NONE, GUI_ICON_INFO, GUI_ICON_WARNING, GUI_ICON_ERROR, GUI_ICON_QUESTION };

struct gui_dialog {
    enum gui_dialog_icon icon;
    const char *title;           /* the heading, in bold (and the window's title) */
    const char *message;         /* wrapped to fit; "\n" starts a new line; may be NULL */
    const char *buttons[4];      /* [0] is the main one (Enter), on the right; none at all: "OK" */
    bool danger;                 /* the main button is red: it destroys something */
    const char *check;           /* a checkbox under the message (NULL: none) */
    bool checked;                /* its state, in and out */
    bool require_check;          /* the main button works only while the box is ticked */
};

/* Returns the index of the button chosen, or -1 if the dialog was closed (Escape, its close button). */
int gui_dialog_run(struct gui *g, struct gui_window *owner, struct gui_dialog *dlg);
/* Shortcuts: a message with an OK button; a question whose main button is `yes` (true if chosen). */
void gui_message(struct gui *g, struct gui_window *owner, enum gui_dialog_icon icon, const char *title,
                 const char *message);
bool gui_confirm(struct gui *g, struct gui_window *owner, enum gui_dialog_icon icon, const char *title,
                 const char *message, const char *yes, bool danger);
/* For programs that draw a dialog of their own: a fixed-size window over `owner`, modal like the above. */
struct gui_window *gui_dialog_window_create(struct gui *g, struct gui_window *owner, const char *title,
                                            int width, int height);
