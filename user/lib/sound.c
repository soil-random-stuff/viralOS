/* The system's sounds: see <sound.h>. */

#include <config.h>
#include <sound.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define SOUNDS_DIR _PATH_DDE_RES "/Sounds/"
#define PLAYER     _PATH_BIN "/playsound"

/* Settings > Sound: sound effects, and the start-up / shut-down sounds, can be turned off. */
static bool wanted(const char *name) {
    if (strcmp(name, "Startup") == 0 || strcmp(name, "Shutdown") == 0 || strcmp(name, "Login") == 0 ||
        strcmp(name, "Logout") == 0) {
        return config_get_int(DDE_CONFIG, "StartupSound", 1) != 0;
    }
    return config_get_int(DDE_CONFIG, "SoundEffects", 1) != 0;
}

static long start(const char *name, bool detached) {
    if (!wanted(name)) {
        return 0;
    }
    char path[OS_PATH_MAX];
    snprintf(path, sizeof(path), SOUNDS_DIR "%s.ogg", name);
    const char *argv[] = { "playsound", path, NULL };
    struct os_spawn sp = {
        .path = PLAYER,
        .argv = argv,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
        .flags = detached ? OS_SPAWN_DETACHED : 0,
    };
    return spawn(&sp);
}

long system_sound(const char *name) {
    return start(name, true);
}

int system_sound_wait(const char *name) {
    long pid = start(name, false);
    if (pid <= 0) {
        return (int)pid;
    }
    int status = 0;
    long r = wait(pid, &status, 0);
    return r < 0 ? (int)r : status;
}
