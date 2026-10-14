/*
 * divine-login: DDE's login screen, the first program (/etc/init). It owns the screen
 * while nobody is logged in: the login screen (libusers' lockscreen) over the wallpaper. When an
 * account logs in it plays Login, becomes that user (set_user: everything started from now on runs
 * for them, at home in /home/<name>), gives the screen to the desktop and waits for it. When the
 * desktop ends (Log out) the login screen comes back; if it ends with EXIT_TO_SHELL, so does this
 * program, and the kernel's text shell takes over.
 */

#include <config.h>
#include <fileops.h>
#include <gfx.h>
#include <image.h>
#include <sound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ttf.h>
#include <unistd.h>
#include <users.h>
#include <paths.h>

#define DESKTOP        _PATH_DESKTOP
#define EXIT_TO_SHELL  3      /* the desktop's exit code for "Exit to the text shell" */
#define CURSOR_DIR     _PATH_DDE_RES "/Cursors/"

static struct os_display_info display;
static struct gfx_surface *scene;      /* the login screen, without the pointer */
static struct gfx_surface *screen;     /* what's shown: scene + pointer */
static struct gfx_surface *wallpaper;
static struct gfx_surface *cursors[2];
static int hot[2][2];
static int cursor_kind;
static int mouse_x, mouse_y;
static struct ttf_font *regular, *bold;

/* --- the screen ------------------------------------------------------------------------------------- */

static void present(struct gfx_rect area) {
    area = gfx_rect_intersect(area, gfx_rect(0, 0, screen->width, screen->height));
    bool native = display.red_shift == 16 && display.green_shift == 8 && display.blue_shift == 0;
    for (int y = area.y; y < area.y + area.h; y++) {
        const uint32_t *src = gfx_row(screen, y) + area.x;
        uint32_t *dst = (uint32_t *)(display.address + (uint64_t)y * display.pitch) + area.x;
        if (native) {
            memcpy(dst, src, (size_t)area.w * 4);
            continue;
        }
        for (int x = 0; x < area.w; x++) {
            uint32_t p = src[x];
            dst[x] = ((p >> 16) & 0xFF) << display.red_shift | ((p >> 8) & 0xFF) << display.green_shift |
                     (p & 0xFF) << display.blue_shift;
        }
    }
}

static struct gfx_rect cursor_rect(void) {
    struct gfx_surface *c = cursors[cursor_kind];
    return c ? gfx_rect(mouse_x - hot[cursor_kind][0], mouse_y - hot[cursor_kind][1], c->width, c->height)
             : gfx_rect(0, 0, 0, 0);
}

/* Show `area` of the scene again, with the pointer on top. */
static void show(struct gfx_rect area) {
    struct gfx_rect a = gfx_rect_intersect(area, gfx_rect(0, 0, screen->width, screen->height));
    if (gfx_rect_empty(a)) {
        return;
    }
    gfx_copy(screen, a.x, a.y, scene, &a);
    struct gfx_rect c = cursor_rect();
    if (!gfx_rect_empty(gfx_rect_intersect(c, a)) && cursors[cursor_kind]) {
        gfx_set_clip(screen, a);
        gfx_blit(screen, c.x, c.y, cursors[cursor_kind], NULL);
        gfx_reset_clip(screen);
    }
    present(a);
}

static void move_pointer(int x, int y, int kind) {
    struct gfx_rect old = cursor_rect();
    mouse_x = x;
    mouse_y = y;
    cursor_kind = kind;
    show(old);
    show(cursor_rect());
}

static void load_cursors(void) {
    const char *files[2] = { "arrow.png", "hand.png" };
    for (int i = 0; i < 2; i++) {
        char path[96];
        snprintf(path, sizeof(path), CURSOR_DIR "%s", files[i]);
        cursors[i] = image_load(path, NULL);
    }
    hot[0][0] = 6, hot[0][1] = 3, hot[1][0] = 12, hot[1][1] = 2;   /* (hotspots.txt's, as the desktop reads) */
    char line[256];
    int fd = open(CURSOR_DIR "hotspots.txt", O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, line, sizeof(line) - 1);
        close(fd);
        line[n > 0 ? n : 0] = '\0';
        for (int i = 0; i < 2; i++) {
            char *at = strstr(line, files[i]);
            if (at) {
                char *end;
                long x = strtol(at + strlen(files[i]), &end, 10);
                long y = strtol(end, NULL, 10);
                hot[i][0] = (int)x;
                hot[i][1] = (int)y;
            }
        }
    }
}

/* `user`'s wallpaper (their DDE settings), covering the screen as the desktop shows it. */
static void load_wallpaper(const char *user) {
    static char loaded[OS_PATH_MAX];
    char path[OS_PATH_MAX] = _PATH_DDE_RES "/Backgrounds/default.png";
    config_get(dde_config_path(user), "Wallpaper", path, sizeof(path));
    if (wallpaper && strcmp(path, loaded) == 0) {
        return;   /* (decoding a big picture takes a while: only when it changed) */
    }
    strlcpy(loaded, path, sizeof(loaded));
    gfx_surface_destroy(wallpaper);
    wallpaper = NULL;
    struct gfx_surface *img = image_load(path, NULL);
    if (!img) {
        img = image_load(_PATH_DDE_RES "/Backgrounds/default.png", NULL);
    }
    if (!img) {
        return;
    }
    int w = (int)display.width, h = (int)display.height;
    wallpaper = gfx_surface_create(w, h);
    if (wallpaper) {
        float sx = (float)w / img->width, sy = (float)h / img->height, scale = sx > sy ? sx : sy;
        int cw = (int)(w / scale + 0.5f), ch = (int)(h / scale + 0.5f);
        cw = cw > img->width ? img->width : cw;
        ch = ch > img->height ? img->height : ch;
        struct gfx_rect from = gfx_rect((img->width - cw) / 2, (img->height - ch) / 2, cw, ch);
        gfx_draw_scaled(wallpaper, gfx_rect(0, 0, w, h), img, &from);
    }
    gfx_surface_destroy(img);
}

static void message_screen(const char *text) {
    gfx_clear(scene, GFX_RGB(0x10, 0x10, 0x16));
    if (regular) {
        float w = ttf_text_width(regular, 18, text);
        ttf_draw_text(scene, regular, 18, (scene->width - w) / 2, scene->height / 2.0f, GFX_RGB(0xE8, 0xE8, 0xEE), text);
    }
    cursor_kind = 0;
    mouse_x = mouse_y = -100;
    show(gfx_rect(0, 0, scene->width, scene->height));
}

/* A question on a dark screen: the title, a detail and the keys. True for Enter, false for Escape. */
static bool ask_screen(const char *title, const char *detail, const char *keys) {
    gfx_clear(scene, GFX_RGB(0x10, 0x10, 0x16));
    const char *texts[3] = { title, detail, keys };
    const float sizes[3] = { 20, 15, 14 };
    const gfx_color colors[3] = { GFX_RGB(0xF2, 0xF2, 0xF6), GFX_RGB(0xB8, 0xB8, 0xC4), GFX_RGB(0x8A, 0x8A, 0x96) };
    for (int i = 0; i < 3 && regular; i++) {
        struct ttf_font *f = i == 0 && bold ? bold : regular;
        float w = ttf_text_width(f, sizes[i], texts[i]);
        ttf_draw_text(scene, f, sizes[i], (scene->width - w) / 2, scene->height / 2.0f - 30 + i * 36, colors[i],
                      texts[i]);
    }
    cursor_kind = 0;
    mouse_x = mouse_y = -100;
    show(gfx_rect(0, 0, scene->width, scene->height));
    system_sound("DialogWarning");
    for (;;) {
        struct os_input_event ev;
        if (input_read(&ev, 1, -1) == 1 && ev.type == OS_EVENT_KEY && ev.pressed) {
            if (ev.key == OS_KEY_ENTER) {
                return true;
            }
            if (ev.key == OS_KEY_ESCAPE) {
                return false;
            }
        }
    }
}

/* --- a session ---------------------------------------------------------------------------------------- */

/* Run the desktop for `user` until it ends. Returns its exit status. */
static int session(const struct user_account *user) {
    system_sound("Login");
    set_user(user->name);
    make_home_dirs(user->name);
    chdir(home_dir());
    display_release();
    const char *argv[] = { "divine-desktop", NULL };
    long pid = spawn_simple(DESKTOP, argv);
    int status = 1;
    if (pid > 0) {
        wait(pid, &status, 0);
    }
    set_user("");
    return status;
}

/* The account the login screen shows ("" if there are none). */
static const char *chosen(const struct lockscreen *l) {
    const struct user_account *u = lockscreen_user(l);
    return u ? u->name : "";
}

/* --- main ---------------------------------------------------------------------------------------------- */

int main(void) {
    regular = ttf_load(_PATH_DDE_RES "/Fonts/DejaVuSans.ttf");
    bold = ttf_load(_PATH_DDE_RES "/Fonts/DejaVuSans-Bold.ttf");
    load_cursors();
    bool first = true;

    for (;;) {
        if (display_claim(&display) < 0) {
            dprintf(STDERR_FILENO, "divine-login: can't take the screen\n");
            return 1;
        }
        int w = (int)display.width, h = (int)display.height;
        if (!scene) {
            scene = gfx_surface_create(w, h);
            screen = gfx_surface_create(w, h);
            if (!scene || !screen) {
                return 1;
            }
            mouse_x = w / 2;
            mouse_y = h / 2;
        }
        struct lockscreen *l = lockscreen_create(w, h, regular, bold, true);
        if (!l) {
            return 1;
        }
        char shown[OS_USER_MAX];   /* whose wallpaper is behind the card */
        strlcpy(shown, chosen(l), sizeof(shown));
        load_wallpaper(shown);
        lockscreen_set_background(l, wallpaper);
        lockscreen_draw(l, scene);
        cursor_kind = 0;
        show(gfx_rect(0, 0, w, h));
        if (first) {
            system_sound("Startup");
            first = false;
        }

        enum lockscreen_action done = LOCK_NONE;
        while (done == LOCK_NONE) {
            struct os_input_event events[16];
            int n = input_read(events, 16, lockscreen_next_tick_ms(l));
            bool redraw = false;
            for (int i = 0; i < n && done == LOCK_NONE; i++) {
                enum lockscreen_action a = lockscreen_input(l, &events[i]);
                if (a == LOCK_REDRAW) {
                    redraw = true;
                } else if (a != LOCK_NONE) {
                    done = a;
                }
                if (events[i].type == OS_EVENT_MOUSE) {
                    move_pointer(events[i].x, events[i].y, lockscreen_hand(l) ? 1 : 0);
                }
            }
            if (done == LOCK_NONE) {
                enum lockscreen_action a = lockscreen_tick(l);
                if (a == LOCK_REDRAW) {
                    redraw = true;
                } else if (a != LOCK_NONE) {
                    done = a;
                }
            }
            if (done == LOCK_NONE && strcmp(shown, chosen(l)) != 0) {
                /* Another account chosen: its wallpaper. */
                strlcpy(shown, chosen(l), sizeof(shown));
                load_wallpaper(shown);
                lockscreen_set_background(l, wallpaper);
                redraw = true;
            }
            if (redraw || done == LOCK_UNLOCKED) {
                lockscreen_draw(l, scene);
                show(gfx_rect(0, 0, w, h));
            }
        }

        if (done == LOCK_SHUTDOWN || done == LOCK_RESTART) {
            lockscreen_destroy(l);
            /* The drives unmounted first; if one can't be, ask. */
            char failed[200];
            if (unmount_drives_all(failed, sizeof(failed)) > 0 &&
                !ask_screen("A drive couldn't be unmounted", failed,
                            done == LOCK_SHUTDOWN ? "Enter: shut down anyway    Esc: go back"
                                                  : "Enter: restart anyway    Esc: go back")) {
                continue;
            }
            message_screen(done == LOCK_SHUTDOWN ? "Shutting down..." : "Restarting...");
            system_sound_wait("Shutdown");
            power(done == LOCK_SHUTDOWN ? OS_POWER_OFF : OS_POWER_RESTART);
            continue;   /* still here: it didn't work */
        }
        struct user_account user = *lockscreen_user(l);
        lockscreen_destroy(l);
        if (session(&user) == EXIT_TO_SHELL) {
            return 0;
        }
    }
}
