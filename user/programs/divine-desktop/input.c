/*
 * Turning raw keyboard/mouse input into window manager actions (moving,
 * focusing, title bar buttons, menus, the panel) and into events for the
 * programs.
 */

#include <string.h>

#include "desktop.h"

/* A mouse event for `w` at the current pointer position. */
static void mouse_event(struct desktop *d, enum gui_event_type type, struct window *w,
                        uint32_t button, int wheel) {
    struct gui_msg_event ev = {
        .header = { GUI_MSG_EVENT, w->id },
        .type = type,
        .x = d->mouse_x - w->frame.x,
        .y = d->mouse_y - w->frame.y - title_height(w),
        .screen_x = d->mouse_x,
        .screen_y = d->mouse_y,
        .button = button,
        .buttons = d->buttons,
        .wheel = wheel,
    };
    send_event(w, &ev);
}

static bool in_content(const struct window *w, int y) {
    return y >= w->frame.y + title_height(w);   /* the title bar spans the full width */
}

/* --- the window manager's reactions ------------------------------------------------------------ */

static void set_hover(struct desktop *d, struct window *w, enum title_button b) {
    /* Only one window's buttons can be hovered: clear the others. */
    for (struct window *o = d->windows; o; o = o->next) {
        enum title_button want = o == w ? b : BUTTON_NONE;
        if (o->hover_button != want) {
            o->hover_button = want;
            draw_decorations(d, o);
        }
    }
}

/* The topmost thing under the pointer, in drawing order from the top. */
enum target { ON_MENU, ON_LAUNCHER, ON_VOLUME, ON_NETWORK, ON_PANEL, ON_WINDOW, ON_DESKTOP };

static enum target target_at(struct desktop *d, int x, int y, struct window **w) {
    *w = NULL;
    if (menu_contains(d, x, y)) {
        return ON_MENU;
    }
    if (launcher_contains(d, x, y)) {
        return ON_LAUNCHER;
    }
    if (volume_contains(d, x, y)) {
        return ON_VOLUME;
    }
    if (network_contains(d, x, y)) {
        return ON_NETWORK;
    }
    if (panel_contains(d, x, y)) {
        return ON_PANEL;
    }
    *w = window_at(d, x, y);
    return *w ? ON_WINDOW : ON_DESKTOP;
}

static void on_move(struct desktop *d) {
    if (d->drag.active) {
        drag_move(d);   /* files following the pointer */
        return;
    }
    if (d->resizing) {
        window_resize_update(d);
        return;
    }
    if (d->dragging) {
        window_move(d, d->dragging, d->mouse_x - d->drag_dx, d->mouse_y - d->drag_dy);
        return;
    }
    if (d->menu.open) {
        menu_hover(d, d->mouse_x, d->mouse_y);
        return;
    }
    if (volume_dragging(d)) {
        volume_mouse_move(d);   /* the volume slider */
        return;
    }
    if (d->captured) {
        mouse_event(d, GUI_EVENT_MOUSE_MOVE, d->captured, 0, 0);
        return;
    }
    if (d->icon_pressing) {
        icons_mouse_move(d);   /* selecting icons with a rectangle */
        return;
    }
    struct window *w;
    enum target t = target_at(d, d->mouse_x, d->mouse_y, &w);
    if (t == ON_DESKTOP) {
        icons_mouse_move(d);
    } else if (d->icon_hover >= 0) {
        damage(d, d->icons[d->icon_hover].rect);
        d->icon_hover = -1;
    }
    panel_mouse_move(d);
    launcher_mouse_move(d);
    volume_mouse_move(d);
    network_mouse_move(d);
    set_hover(d, w, w ? title_button_at(w, d->mouse_x, d->mouse_y) : BUTTON_NONE);
    if (t == ON_WINDOW && in_content(w, d->mouse_y)) {
        mouse_event(d, GUI_EVENT_MOUSE_MOVE, w, 0, 0);
    }
}

static void on_button_down(struct desktop *d, uint32_t button, uint32_t modifiers) {
    if (d->drag.active) {
        return;   /* another button while dragging: ignored */
    }
    if (d->menu.open) {
        if (!menu_contains(d, d->mouse_x, d->mouse_y)) {
            menu_close(d);   /* clicking elsewhere just closes it */
        }
        return;
    }
    struct window *w;
    /* An edge or corner of a window: resize it. */
    if (button == OS_MOUSE_LEFT) {
        int edges = resize_edges_at(d, d->mouse_x, d->mouse_y, &w);
        if (edges) {
            launcher_close(d);
            window_raise(d, w);
            focus(d, w);
            window_resize_begin(d, w, edges);
            return;
        }
    }
    enum target t = target_at(d, d->mouse_x, d->mouse_y, &w);
    if (t == ON_PANEL) {
        panel_mouse_down(d, button);
        return;
    }
    if (t == ON_LAUNCHER) {
        if (button == OS_MOUSE_LEFT) {
            launcher_mouse_down(d);   /* (the scroll bar) */
        }
        return;   /* apps act on release */
    }
    if (t == ON_VOLUME) {
        if (button == OS_MOUSE_LEFT) {
            volume_mouse_down(d);
        }
        return;
    }
    if (t == ON_NETWORK) {
        return;   /* acts on release */
    }
    launcher_close(d);   /* a click anywhere else closes the apps menu */
    volume_close(d);     /* ...and the volume card */
    network_close(d);    /* ...and the network card */
    if (t == ON_DESKTOP) {
        focus(d, NULL);   /* the keyboard now goes to the icons */
        icons_mouse_down(d, button, modifiers);
        return;
    }
    struct window *modal = window_modal(d, w);
    if (modal) {
        /* Waiting for a dialog: bring that forward instead. */
        window_raise(d, w);
        focus(d, modal);
        return;
    }
    window_raise(d, w);
    focus(d, w);

    if (in_content(w, d->mouse_y)) {
        d->captured = w;
        mouse_event(d, GUI_EVENT_MOUSE_DOWN, w, button, 0);
        return;
    }
    /* The title bar. */
    if (button == OS_MOUSE_RIGHT) {
        if (!w->dialog) {
            show_window_menu(d, w, d->mouse_x, d->mouse_y);
        }
        return;
    }
    if (button != OS_MOUSE_LEFT) {
        return;
    }
    enum title_button b = title_button_at(w, d->mouse_x, d->mouse_y);
    if (b != BUTTON_NONE) {
        w->pressed_button = b;
        draw_decorations(d, w);
    } else if (!w->maximized) {
        d->dragging = w;
        d->drag_dx = d->mouse_x - w->frame.x;
        d->drag_dy = d->mouse_y - w->frame.y;
    }
}

static void on_button_up(struct desktop *d, uint32_t button) {
    if (button == OS_MOUSE_LEFT && launcher_mouse_up(d)) {
        return;   /* the end of dragging the apps menu's scroll bar */
    }
    if (d->resizing) {
        if (button == OS_MOUSE_LEFT) {
            window_resize_end(d);
        }
        return;
    }
    if (d->drag.active) {
        if (button == OS_MOUSE_LEFT) {
            drag_drop(d);
        }
        return;
    }
    if (d->menu.open) {
        if (menu_contains(d, d->mouse_x, d->mouse_y)) {
            menu_choose(d);
        }
        return;
    }
    if (button == OS_MOUSE_LEFT && d->dragging) {
        d->dragging = NULL;
        return;
    }
    if (button == OS_MOUSE_LEFT && (volume_dragging(d) || volume_contains(d, d->mouse_x, d->mouse_y))) {
        volume_mouse_up(d);
        return;
    }
    if (button == OS_MOUSE_LEFT && network_contains(d, d->mouse_x, d->mouse_y)) {
        network_mouse_up(d);
        return;
    }
    if (d->panel_pressed != PANEL_HOVER_NONE) {
        panel_mouse_up(d, button);
        return;
    }
    if (d->icon_pressing) {
        icons_mouse_up(d, button);
        return;
    }
    /* A title bar button counts if the release is still over it. */
    for (struct window *w = d->windows; w; w = w->next) {
        if (w->pressed_button == BUTTON_NONE) {
            continue;
        }
        enum title_button pressed = w->pressed_button;
        w->pressed_button = BUTTON_NONE;
        draw_decorations(d, w);
        if (title_button_at(w, d->mouse_x, d->mouse_y) != pressed) {
            continue;
        }
        if (pressed == BUTTON_CLOSE) {
            window_close_request(w);
        } else if (pressed == BUTTON_MINIMIZE) {
            window_minimize(d, w);
        } else {
            window_toggle_maximize(d, w);
        }
        return;
    }
    if (d->captured) {
        struct window *w = d->captured;
        if (!d->buttons) {
            d->captured = NULL;
        }
        mouse_event(d, GUI_EVENT_MOUSE_UP, w, button, 0);
        return;
    }
    struct window *w;
    enum target t = target_at(d, d->mouse_x, d->mouse_y, &w);
    if (t == ON_LAUNCHER && button == OS_MOUSE_LEFT) {
        launcher_click(d);
    } else if (t == ON_WINDOW && in_content(w, d->mouse_y)) {
        mouse_event(d, GUI_EVENT_MOUSE_UP, w, button, 0);
    }
}

void move_pointer(struct desktop *d, int x, int y) {
    if (x == d->mouse_x && y == d->mouse_y) {
        return;
    }
    struct gfx_rect old = gfx_rect(d->mouse_x - d->cursor_hot_x, d->mouse_y - d->cursor_hot_y,
                                   d->cursor->width, d->cursor->height);
    d->mouse_x = x;
    d->mouse_y = y;
    damage(d, old);
    damage(d, gfx_rect(x - d->cursor_hot_x, y - d->cursor_hot_y, d->cursor->width, d->cursor->height));
}

static void on_key(struct desktop *d, const struct os_input_event *in) {
    if (d->drag.active) {
        if (in->key == OS_KEY_ESCAPE) {
            drag_cancel(d);
        }
        return;
    }
    if (d->menu.open) {
        menu_key(d, in->key);
        return;
    }
    if (d->launcher_open) {
        if (in->key == OS_KEY_ESCAPE) {
            launcher_close(d);
        }
        return;
    }
    if (d->volume_open && in->key == OS_KEY_ESCAPE) {
        volume_close(d);
        return;
    }
    if (d->net_open && in->key == OS_KEY_ESCAPE) {
        network_close(d);
        return;
    }
    if (!d->focused) {
        icons_key(d, in);
        return;
    }
    struct gui_msg_event ev = {
        .header = { GUI_MSG_EVENT, d->focused->id },
        .type = GUI_EVENT_KEY,
        .key = in->key,
        .ch = in->ch,
        .modifiers = in->modifiers,
    };
    send_event(d->focused, &ev);
}

/* Which pointer to show for where it is and what's going on. */
void update_cursor(struct desktop *d) {
    int kind = GUI_CURSOR_ARROW;
    struct window *w;
    if (d->resizing) {
        kind = resize_cursor(d->resize_edges);
    } else if (d->drag.active || d->dragging || d->menu.open || d->dialog != DESK_DIALOG_NONE) {
        kind = GUI_CURSOR_ARROW;
    } else if (d->captured) {
        kind = d->captured->cursor;
    } else {
        int edges = resize_edges_at(d, d->mouse_x, d->mouse_y, &w);
        enum target t = target_at(d, d->mouse_x, d->mouse_y, &w);
        if (edges) {
            kind = resize_cursor(edges);
        } else if (t == ON_WINDOW && in_content(w, d->mouse_y)) {
            kind = w->cursor;
        } else if (t == ON_VOLUME && volume_hover_clickable(d)) {
            kind = GUI_CURSOR_HAND;   /* the volume card's controls */
        } else if (t == ON_NETWORK && network_hover_clickable(d)) {
            kind = GUI_CURSOR_HAND;
        } else if (t == ON_PANEL && (d->panel_hover >= 0 || d->panel_hover == PANEL_HOVER_MENU ||
                                     d->panel_hover == PANEL_HOVER_VOLUME ||
                                     d->panel_hover == PANEL_HOVER_NETWORK)) {
            kind = GUI_CURSOR_HAND;   /* the panel's buttons */
        } else if (t == ON_LAUNCHER && (d->launcher_hover >= 0 || d->launcher_hover == LAUNCHER_EXIT)) {
            kind = GUI_CURSOR_HAND;   /* apps in the apps menu */
        }
    }
    set_cursor(d, kind);
}

#define KEY_L 0x26

void handle_input(struct desktop *d, const struct os_input_event *in) {
    if (d->locked) {
        lock_input(d, in);
        return;
    }
    if (in->type == OS_EVENT_KEY && in->pressed && in->key == KEY_L &&
        (in->modifiers & (OS_MOD_CTRL | OS_MOD_ALT)) == (OS_MOD_CTRL | OS_MOD_ALT)) {
        lock_screen(d);   /* Ctrl+Alt+L */
        return;
    }
    if (in->type == OS_EVENT_KEY && in->pressed && in->key == OS_KEY_PRINT && d->dialog == DESK_DIALOG_NONE) {
        app_launch_path(d, _PATH_BIN "/divine-snip", "--new");   /* Print Screen: snip right away */
        return;
    }
    if (d->dialog != DESK_DIALOG_NONE) {
        /* The desktop's dialog takes everything until it's answered. */
        if (in->type == OS_EVENT_KEY) {
            if (in->pressed) {
                desk_dialog_key(d, in);
            }
            return;
        }
        move_pointer(d, in->x, in->y);
        uint32_t changed = in->buttons ^ d->buttons;
        d->buttons = in->buttons;
        desk_dialog_mouse(d, (changed & in->buttons & OS_MOUSE_LEFT) != 0,
                          (changed & ~in->buttons & OS_MOUSE_LEFT) != 0);
        return;
    }
    if (in->type == OS_EVENT_KEY) {
        if (in->pressed) {
            on_key(d, in);
        }
        return;
    }

    bool moved = in->x != d->mouse_x || in->y != d->mouse_y;
    move_pointer(d, in->x, in->y);
    uint32_t changed = in->buttons ^ d->buttons;
    d->buttons = in->buttons;
    if (moved) {
        on_move(d);
    }
    int wheel = d->cfg.natural_scroll ? -in->wheel : in->wheel;   /* (Settings > Mouse) */
    if (wheel && !volume_wheel(d, wheel) && !launcher_wheel(d, wheel)) {
        struct window *w;
        if (target_at(d, d->mouse_x, d->mouse_y, &w) == ON_WINDOW && in_content(w, d->mouse_y)) {
            mouse_event(d, GUI_EVENT_SCROLL, w, 0, wheel);
        }
    }
    for (uint32_t b = 1; b <= OS_MOUSE_MIDDLE; b <<= 1) {
        if (changed & b) {
            if (in->buttons & b) {
                on_button_down(d, b, in->modifiers);
            } else {
                on_button_up(d, b);
            }
        }
    }
    update_cursor(d);
}
