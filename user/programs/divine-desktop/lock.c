/*
 * Locking: Lock in the power menu (or Ctrl+Alt+L) covers everything with the lock screen (libusers'
 * lockscreen, the same as the login screen, but only for the user logged in) until their password
 * is typed. Programs keep running underneath; while locked, every key and click goes to the lock
 * screen. Logging out: the desktop ends (the login screen, which started it, comes back).
 */

#include <sound.h>
#include <string.h>
#include <users.h>

#include "desktop.h"

static void redraw(struct desktop *d) {
    lockscreen_draw(d->lock, d->lock_surface);
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
}

void lock_screen(struct desktop *d) {
    if (d->locked) {
        return;
    }
    launcher_close(d);
    volume_close(d);
    network_close(d);
    menu_close(d);
    d->lock_surface = gfx_surface_create(d->screen.width, d->screen.height);
    d->lock = lockscreen_create(d->screen.width, d->screen.height, d->regular, d->bold, false);
    if (!d->lock || !d->lock_surface) {
        lockscreen_destroy(d->lock);
        gfx_surface_destroy(d->lock_surface);
        d->lock = NULL;
        d->lock_surface = NULL;
        return;
    }
    lockscreen_select(d->lock, user_name());
    lockscreen_set_background(d->lock, d->wallpaper);
    d->locked = true;
    d->captured = NULL;
    d->dragging = NULL;
    redraw(d);
}

static void unlock(struct desktop *d) {
    d->locked = false;
    lockscreen_destroy(d->lock);
    gfx_surface_destroy(d->lock_surface);
    d->lock = NULL;
    d->lock_surface = NULL;
    system_sound("Login");
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
}

static void act(struct desktop *d, enum lockscreen_action a) {
    switch (a) {
    case LOCK_REDRAW:   redraw(d); break;
    case LOCK_UNLOCKED: unlock(d); break;
    case LOCK_SHUTDOWN: power_menu_chosen(d, POWER_MENU_OFF); break;
    case LOCK_RESTART:  power_menu_chosen(d, POWER_MENU_RESTART); break;
    default:            break;
    }
}

void lock_input(struct desktop *d, const struct os_input_event *in) {
    if (in->type == OS_EVENT_MOUSE) {
        move_pointer(d, in->x, in->y);
        d->buttons = in->buttons;
    }
    act(d, lockscreen_input(d->lock, in));
    if (d->locked) {
        set_cursor(d, lockscreen_hand(d->lock) ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
    }
}

void lock_tick(struct desktop *d) {
    if (d->locked) {
        act(d, lockscreen_tick(d->lock));
    }
}

int lock_timeout(const struct desktop *d) {
    return d->locked ? lockscreen_next_tick_ms(d->lock) : 1000;
}

void lock_compose(struct desktop *d, struct gfx_rect area) {
    gfx_copy(&d->screen, area.x, area.y, d->lock_surface, &area);
}

/* Log out: say goodbye, then end (the login screen takes over). */
void log_out(struct desktop *d) {
    launcher_close(d);
    gfx_clear(&d->screen, GFX_RGB(0x10, 0x10, 0x16));
    if (d->regular) {
        const char *text = "Logging out...";
        float w = ttf_text_width(d->regular, 18, text);
        ttf_draw_text(&d->screen, d->regular, 18, (d->screen.width - w) / 2, d->screen.height / 2.0f,
                      GFX_RGB(0xE8, 0xE8, 0xEE), text);
    }
    d->wallpaper_hidden = true;
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
    render(d);
    system_sound_wait("Logout");
    d->quit = true;
}
