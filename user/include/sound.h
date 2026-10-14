#pragma once

/*
 * The system's sounds: /usr/share/dde-res/Sounds/<name>.ogg, played by /usr/bin/playsound in the
 * background (mixed with anything else playing, at the system volume).
 *
 *   Startup, Shutdown           the desktop starting; powering off or restarting
 *   Login, Logout               the login screen (future)
 *   DialogInfo, DialogWarning,  a dialog appearing, by its icon
 *   DialogError, DialogQuestion
 *   DeviceAdd, DeviceRemove     a disk plugged in / taken out
 *   VolumeChange                the volume was changed
 *   Notification                (future)
 */

#include <stdbool.h>

/* Start playing it and return at once. Returns the player's pid (> 0), or -errno. */
long system_sound(const char *name);
/* Play it and wait until it has finished (for Shutdown: before the power goes). */
int system_sound_wait(const char *name);
