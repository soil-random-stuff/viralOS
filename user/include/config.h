#pragma once

/*
 * Settings files of "Key=value" lines (blank lines and # comments kept as they are), like DDE's
 * settings. Each user has their own, DDE_CONFIG (~/.config/dde/settings.conf); a key it doesn't have
 * comes from the system's defaults, /etc/dde/settings.conf:
 *
 *   Wallpaper=/usr/share/dde-res/Backgrounds/default.png
 *   Clock24=1              the panel's clock: 24-hour (1) or 12-hour with AM/PM (0)
 *   NaturalScroll=0        1: the wheel moves the content, like a touch screen
 *   DoubleClick=450        ms between the clicks of a double click
 *   SoundEffects=1         system sounds (dialogs, devices, volume)
 *   StartupSound=1         the sounds when starting up and shutting down
 *   Pinned=divine-files,divine-terminal   the apps always on the panel (their .desktop files' names)
 *   Picture=/home/alex/Pictures/me.png    the account's picture (empty: the default)
 *
 * DDE_DESKTOP_LAYOUT (~/.config/dde/desktop) has the desktop icons' size, arrangement and places.
 */

#include <paths.h>
#include <stdbool.h>
#include <stddef.h>

#define DDE_CONFIG          dde_config_path(NULL)
#define DDE_DESKTOP_LAYOUT  dde_desktop_layout_path()

/* ~/.config/dde/settings.conf of `user` (NULL: the user this program runs for). The text is kept
   until the next call. */
const char *dde_config_path(const char *user);
const char *dde_desktop_layout_path(void);

/* The value of `key` (true), or false if the file or the key isn't there. */
bool config_get(const char *path, const char *key, char *out, size_t size);
/* As a number, or `fallback`. */
long config_get_int(const char *path, const char *key, long fallback);
/* Set `key` (replacing its line, or adding one at the end; the file and its folder are made if
   they aren't there). 0 or -errno. */
int config_set(const char *path, const char *key, const char *value);
int config_set_int(const char *path, const char *key, long value);

/* Is `item` one of the comma-separated words in `list` (e.g. Pinned=)? */
bool config_list_has(const char *list, const char *item);
