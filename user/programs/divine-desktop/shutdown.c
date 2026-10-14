/*
 * Shutting down and restarting, carefully:
 *
 *   1. Every app is asked to close (as if its close button was clicked), so it can save its work or
 *      ask about unsaved changes. We wait for them to go.
 *   2. Any still open after a few seconds (one asking "save changes?", or not answering): a warning
 *      names them, and the user either goes back to them or shuts down anyway.
 *   3. Every drive is unmounted (all its writes on the disk). One that can't be (a program still has
 *      a file open on it, ...): a warning names it, and again back or anyway.
 *   4. Goodbye: the Shutdown sound on a dark screen, then the power (the kernel writes out whatever
 *      is left in its disk caches either way).
 */

#include <sound.h>
#include <string.h>

#include "desktop.h"

#define CLOSE_WAIT_MS 4000

/* The programs that still have a window. Their names into `names` ("Text Editor, Terminal"). */
static int open_apps(const struct desktop *d, char *names, size_t size) {
    const struct client *seen[64];
    int count = 0;
    if (names) {
        names[0] = '\0';
    }
    for (const struct window *w = d->windows; w; w = w->next) {
        bool known = false;
        for (int i = 0; i < count && !known; i++) {
            known = seen[i] == w->client;
        }
        if (known || count == 64) {
            continue;
        }
        seen[count++] = w->client;
        if (names) {
            const char *name = w->client->app >= 0 ? d->apps[w->client->app].name
                             : w->title[0]         ? w->title
                                                   : w->client->name;
            if (names[0]) {
                strlcat(names, ", ", size);
            }
            strlcat(names, name, size);
        }
    }
    return count;
}

static void finish(struct desktop *d) {
    bool restart = d->shutdown.restart;
    d->shutdown.stage = SHUTDOWN_NONE;
    /* Say goodbye on a dark screen, then go. */
    gfx_clear(&d->screen, GFX_RGB(0x10, 0x10, 0x16));
    if (d->regular) {
        const char *text = restart ? "Restarting..." : "Shutting down...";
        float w = ttf_text_width(d->regular, 18, text);
        ttf_draw_text(&d->screen, d->regular, 18, (d->screen.width - w) / 2, d->screen.height / 2.0f,
                      GFX_RGB(0xE8, 0xE8, 0xEE), text);
    }
    d->wallpaper_hidden = true;
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
    render(d);
    system_sound_wait("Shutdown");   /* to the end, before the power goes */
    power(restart ? OS_POWER_RESTART : OS_POWER_OFF);
    d->wallpaper_hidden = false;   /* still here: it didn't work */
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
}

static const char *anyway(const struct desktop *d) {
    return d->shutdown.restart ? "Restart anyway" : "Shut down anyway";
}

static void unmount_drives(struct desktop *d) {
    char failed[200];
    int bad = unmount_drives_all(failed, sizeof(failed));
    if (bad == 0) {
        finish(d);
        return;
    }
    char message[320];
    snprintf(message, sizeof(message),
             "%s. A program may still have files open on %s, so they may not be saved completely.", failed,
             bad == 1 ? "it" : "them");
    d->shutdown.stage = SHUTDOWN_ASK_DRIVES;
    desk_dialog_open(d, DESK_DIALOG_SHUTDOWN, bad == 1 ? "A drive couldn't be unmounted" : "Drives couldn't be unmounted",
                     message, anyway(d), NULL);
}

void shutdown_begin(struct desktop *d, bool restart) {
    if (d->shutdown.stage != SHUTDOWN_NONE) {
        return;
    }
    launcher_close(d);
    menu_close(d);
    d->shutdown.restart = restart;
    if (open_apps(d, NULL, 0) == 0) {
        unmount_drives(d);
        return;
    }
    for (struct window *w = d->windows; w; w = w->next) {
        if (!w->dialog) {
            window_close_request(w);
        }
    }
    d->shutdown.stage = SHUTDOWN_CLOSING;
    d->shutdown.deadline = uptime_ms() + CLOSE_WAIT_MS;
}

void shutdown_tick(struct desktop *d) {
    if (d->shutdown.stage != SHUTDOWN_CLOSING) {
        return;
    }
    char names[200];
    int left = open_apps(d, names, sizeof(names));
    if (left == 0) {
        unmount_drives(d);
        return;
    }
    if (uptime_ms() < d->shutdown.deadline) {
        return;
    }
    char message[320];
    snprintf(message, sizeof(message),
             "%s %s still open and may have unsaved work. Cancel to go back and save it, or %s now.", names,
             left == 1 ? "is" : "are", d->shutdown.restart ? "restart" : "shut down");
    d->shutdown.stage = SHUTDOWN_ASK_APPS;
    desk_dialog_open(d, DESK_DIALOG_SHUTDOWN, left == 1 ? "An app didn't close" : "Some apps didn't close", message,
                     anyway(d), NULL);
}

int shutdown_timeout(const struct desktop *d) {
    if (d->shutdown.stage != SHUTDOWN_CLOSING) {
        return -1;
    }
    uint64_t now = uptime_ms();
    return now >= d->shutdown.deadline ? 0 : (int)(d->shutdown.deadline - now < 250 ? d->shutdown.deadline - now : 250);
}

void shutdown_answer(struct desktop *d, bool go) {
    enum shutdown_stage stage = d->shutdown.stage;
    d->shutdown.stage = SHUTDOWN_NONE;
    if (!go) {
        return;   /* back to the desktop (and to the apps' own questions) */
    }
    if (stage == SHUTDOWN_ASK_APPS) {
        unmount_drives(d);   /* (the apps go with the power) */
    } else if (stage == SHUTDOWN_ASK_DRIVES) {
        finish(d);
    }
}
