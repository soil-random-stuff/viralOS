#pragma once

/*
 * libusers: user accounts, and the login / lock screen that DDE's login (divine-login) and the
 * desktop's lock share.
 *
 * Accounts are kept the Unix way:
 *   /etc/passwd   name:x:uid:gid:Display Name:/home/name:/usr/bin/shell
 *   /etc/shadow   name:password:last changed (days since 1970):0:99999:7:::
 *                 password: empty (none), "!" (locked: can't log in), or
 *                 "pbkdf2-sha256$<iterations>$<salt, hex>$<key, hex>"
 *   /etc/group    name:x:gid:            (each account has a group of its own)
 * The name is lowercase letters, digits, - and _. People's accounts have uids from 1000 (root, 0,
 * is the system's own); only those are listed. An account's picture is in its DDE settings
 * (Picture=, see <config.h>), or the default one.
 * No permissions are enforced yet: the accounts decide whose files and settings a session uses.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gfx.h>
#include <paths.h>
#include <ttf.h>
#include <viralos.h>

#define USERS_MAX       32
#define USERS_FIRST_UID 1000
#define DEFAULT_PICTURE _PATH_DDE_RES "/Icons/Misc/DefaultPfp.png"

struct user_account {
    char name[OS_USER_MAX];
    char display[64];
    char password[160];          /* the stored hash ("" = no password) */
    char picture[OS_PATH_MAX];   /* "" = the default */
    char home[256];
    char shell[128];
    int uid, gid;
    long changed;                /* when the password was last changed (days since 1970) */
};

/* People's accounts (uid 1000 and up), in /etc/passwd's order. Returns how many. */
int users_load(struct user_account *out, int max);
/* Any account, by name. */
bool users_find(const char *name, struct user_account *out);
/* The account's display name (or name), for the user this program runs for. */
const char *session_display_name(void);

/* Does logging in as it need a password? (Locked accounts too: no password opens them.) */
bool users_has_password(const struct user_account *a);
bool users_is_locked(const struct user_account *a);     /* shadow password starting with '!' */
bool users_check_password(const struct user_account *a, const char *password);

/* Changes (each rewrites the files). 0 or -errno (-EEXIST, -ENOENT, -EINVAL). */
/* + /home/<name>. password: "" for none, NULL for a locked account (until `passwd` gives it one). */
int users_add(const char *name, const char *display, const char *password);
int users_remove(const char *name);                  /* the account only: the home folder stays */
/* The picture goes into the account's DDE settings. */
int users_set_password(const char *name, const char *password);   /* "" removes it */
int users_set_locked(const char *name, bool locked); /* passwd -l / -u: '!' before the hash */
int users_set_display(const char *name, const char *display);
int users_set_picture(const char *name, const char *path);       /* "" = the default */
bool users_valid_name(const char *name);

/*
 * Groups, /etc/group: "name:x:gid:member,member". Each account has one of its own (its gid); others
 * list their members, like "sudo", whose members may use sudo.
 */
#define USERS_ADMIN_GROUP "sudo"

struct user_group {
    char name[OS_USER_MAX];
    int gid;
    char members[512];           /* comma-separated */
};

int users_groups_load(struct user_group *out, int max);
/* Is `user` in `group` (listed as a member, or the group is their own)? */
bool users_in_group(const char *user, const char *group);
/* Add or remove a member. 0, or -ENOENT (no such group or user), -EEXIST (already in it). */
int users_group_add(const char *group, const char *user);
int users_group_remove(const char *group, const char *user);

/* The picture as a circle `size` across (anti-aliased edge); NULL if it can't be read. */
struct gfx_surface *users_picture(const struct user_account *a, int size);

/* For tests: lowercase hex of SHA-256(data). out has room for 65. */
void users_sha256_hex(const void *data, size_t len, char *out);

/* --- the login / lock screen -------------------------------------------------------------------- */

/*
 * Drawn over the wallpaper: the time and date, and a card with the user's picture, name, password
 * box and a Log in (or Unlock) button. Logging in (divine-login) also lists the other accounts
 * and has a power button (shut down / restart) in the corner.
 */
enum lockscreen_action {
    LOCK_NONE,                   /* nothing (or just a redraw) */
    LOCK_REDRAW,                 /* draw it again */
    LOCK_UNLOCKED,               /* the password was right: lockscreen_user() is in */
    LOCK_SHUTDOWN,
    LOCK_RESTART,
};

struct lockscreen;

struct lockscreen *lockscreen_create(int width, int height, struct ttf_font *regular, struct ttf_font *bold,
                                     bool login);
void lockscreen_destroy(struct lockscreen *l);
/* The user shown (login: whoever was chosen; lock: the session's). */
void lockscreen_select(struct lockscreen *l, const char *name);
const struct user_account *lockscreen_user(const struct lockscreen *l);
/* The background (the wallpaper, already the screen's size); not owned. */
void lockscreen_set_background(struct lockscreen *l, const struct gfx_surface *background);
void lockscreen_draw(struct lockscreen *l, struct gfx_surface *dst);
/* Mouse (x, y, buttons pressed / released) and keys. */
enum lockscreen_action lockscreen_input(struct lockscreen *l, const struct os_input_event *ev);
/* Call now and then (lockscreen_next_tick_ms): the clock, checking a password (LOCK_UNLOCKED when
   it was right), the "wrong password" shake. */
enum lockscreen_action lockscreen_tick(struct lockscreen *l);
int lockscreen_next_tick_ms(const struct lockscreen *l);
/* Is the pointer over something clickable (for the hand cursor)? */
bool lockscreen_hand(const struct lockscreen *l);
