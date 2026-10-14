/*
 * The login / lock screen (see <users.h>), after the user's design: over the wallpaper, a big clock
 * and the date; under them a white card with the account's picture, its name, the password box and
 * the button. Logging in also lists the other accounts and has a power button in the corner.
 *
 * Checking a password takes a moment (PBKDF2), so the check waits for the next tick: the screen
 * shows "Signing in..." first.
 */

#include <config.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <users.h>

#define CARD_W        332
#define CARD_RADIUS   16
#define PICTURE       96
#define FIELD_H       46
#define BUTTON_W      112
#define BUTTON_H      46
#define POWER_SIZE    44
#define SHAKE_MS      420
#define WHITE         GFX_RGB(255, 255, 255)
#define INK           GFX_RGB(0x0A, 0x0A, 0x0C)
#define FIELD         GFX_RGB(0xEE, 0xEE, 0xF0)
#define PLACEHOLDER   GFX_RGB(0x8E, 0x8E, 0x93)
#define BLUE          GFX_RGB(0x3A, 0x8E, 0xF6)
#define BLUE_HOT      GFX_RGB(0x2B, 0x7D, 0xE6)
#define RED           GFX_RGB(0xE5, 0x3E, 0x3E)

enum hot { HOT_NONE, HOT_FIELD, HOT_BUTTON, HOT_POWER, HOT_MENU_OFF, HOT_MENU_RESTART, HOT_USER };

struct lockscreen {
    int w, h;
    struct ttf_font *regular, *bold;
    bool login;
    const struct gfx_surface *background;

    struct user_account users[USERS_MAX];
    int user_count, current;
    struct gfx_surface *picture;          /* the current account's, round */
    struct gfx_surface *small[USERS_MAX]; /* the list's */

    char password[72];
    int password_len;
    bool checking;                        /* the next tick checks it */
    bool wrong;                           /* "Incorrect password" */
    uint64_t shake_start;
    bool menu_open;

    int mx, my;
    uint32_t buttons;
    enum hot hot;
    int hot_user;
    enum hot pressed;
    int pressed_user;
    int minute;
    bool clock24;
};

static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December" };

/* --- layout --------------------------------------------------------------------------------------- */

static bool needs_password(const struct lockscreen *l) {
    return l->user_count && users_has_password(&l->users[l->current]);
}

static int card_h(const struct lockscreen *l) {
    return needs_password(l) ? 346 : 280;
}

static int shake(const struct lockscreen *l) {
    if (!l->shake_start) {
        return 0;
    }
    uint64_t t = uptime_ms() - l->shake_start;
    if (t >= SHAKE_MS) {
        return 0;
    }
    float k = 1.0f - (float)t / SHAKE_MS;
    return (int)(sin(t / 1000.0 * 2 * 3.14159 * 9) * 14 * k);
}

static struct gfx_rect card_rect(const struct lockscreen *l) {
    return gfx_rect((l->w - CARD_W) / 2 + shake(l), l->h * 238 / 800 + 0, CARD_W, card_h(l));
}

static struct gfx_rect field_rect(const struct lockscreen *l) {
    struct gfx_rect c = card_rect(l);
    return gfx_rect(c.x + 18, c.y + 215, c.w - 36, FIELD_H);
}

static struct gfx_rect button_rect(const struct lockscreen *l) {
    struct gfx_rect c = card_rect(l);
    int y = needs_password(l) ? c.y + 279 : c.y + 212;
    return gfx_rect(c.x + (c.w - BUTTON_W) / 2, y, BUTTON_W, BUTTON_H);
}

static struct gfx_rect power_rect(const struct lockscreen *l) {
    return gfx_rect(18, l->h - 18 - POWER_SIZE, POWER_SIZE, POWER_SIZE);
}

static struct gfx_rect menu_rect(const struct lockscreen *l) {
    struct gfx_rect p = power_rect(l);
    return gfx_rect(p.x, p.y - 10 - 92, 170, 92);
}

static struct gfx_rect user_row(const struct lockscreen *l, int i) {
    /* The other accounts, bottom-left above the power button, the last one lowest. */
    struct gfx_rect p = power_rect(l);
    int rows = l->user_count;
    return gfx_rect(14, p.y - 14 - (rows - i) * 52, 240, 48);
}

static enum hot hit(const struct lockscreen *l, int x, int y, int *user) {
    if (l->menu_open) {
        struct gfx_rect m = menu_rect(l);
        if (gfx_rect_contains(m, x, y)) {
            return y < m.y + m.h / 2 ? HOT_MENU_OFF : HOT_MENU_RESTART;
        }
    }
    if (gfx_rect_contains(power_rect(l), x, y)) {
        return HOT_POWER;
    }
    if (l->login && l->user_count > 1) {
        for (int i = 0; i < l->user_count; i++) {
            if (gfx_rect_contains(user_row(l, i), x, y)) {
                *user = i;
                return HOT_USER;
            }
        }
    }
    if (!l->checking && gfx_rect_contains(button_rect(l), x, y)) {
        return HOT_BUTTON;
    }
    if (needs_password(l) && gfx_rect_contains(field_rect(l), x, y)) {
        return HOT_FIELD;
    }
    return HOT_NONE;
}

/* --- state ---------------------------------------------------------------------------------------- */

static void load_pictures(struct lockscreen *l) {
    gfx_surface_destroy(l->picture);
    l->picture = l->user_count ? users_picture(&l->users[l->current], PICTURE) : users_picture(NULL, PICTURE);
}

struct lockscreen *lockscreen_create(int width, int height, struct ttf_font *regular, struct ttf_font *bold,
                                     bool login) {
    struct lockscreen *l = calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }
    l->w = width;
    l->h = height;
    l->regular = regular;
    l->bold = bold;
    l->login = login;
    l->user_count = users_load(l->users, USERS_MAX);
    for (int i = 0; i < l->user_count; i++) {
        l->small[i] = users_picture(&l->users[i], 34);
    }
    l->minute = -1;
    load_pictures(l);
    return l;
}

void lockscreen_destroy(struct lockscreen *l) {
    if (!l) {
        return;
    }
    gfx_surface_destroy(l->picture);
    for (int i = 0; i < l->user_count; i++) {
        gfx_surface_destroy(l->small[i]);
    }
    memset(l->password, 0, sizeof(l->password));
    free(l);
}

static void clear_password(struct lockscreen *l) {
    memset(l->password, 0, sizeof(l->password));
    l->password_len = 0;
}

void lockscreen_select(struct lockscreen *l, const char *name) {
    for (int i = 0; i < l->user_count; i++) {
        if (strcmp(l->users[i].name, name) == 0 && i != l->current) {
            l->current = i;
            load_pictures(l);
        }
    }
    clear_password(l);
    l->wrong = false;
}

const struct user_account *lockscreen_user(const struct lockscreen *l) {
    return l->user_count ? &l->users[l->current] : NULL;
}

void lockscreen_set_background(struct lockscreen *l, const struct gfx_surface *background) {
    l->background = background;
}

/* --- drawing -------------------------------------------------------------------------------------- */

static void centered(struct gfx_surface *s, struct ttf_font *f, float size, float cx, float y, gfx_color c,
                     const char *text) {
    if (f) {
        float w = ttf_text_width(f, size, text);
        ttf_draw_text(s, f, size, cx - w / 2, y, c, text);
    }
}

static void shadowed(struct gfx_surface *s, struct ttf_font *f, float size, float cx, float y, const char *text) {
    centered(s, f, size, cx, y + 2, GFX_RGBA(0, 0, 0, 70), text);
    centered(s, f, size, cx, y, WHITE, text);
}

static const char *ordinal(int day) {
    if (day % 100 >= 11 && day % 100 <= 13) {
        return "th";
    }
    switch (day % 10) {
    case 1:  return "st";
    case 2:  return "nd";
    case 3:  return "rd";
    default: return "th";
    }
}

static void power_icon(struct gfx_surface *s, float cx, float cy, float r, gfx_color c) {
    /* An open ring with a bar through its gap. */
    const float gap = 0.62f;   /* radians either side of the top */
    for (int i = 0; i < 40; i++) {
        float a0 = gap + i * (2 * 3.14159f - 2 * gap) / 40, a1 = gap + (i + 1) * (2 * 3.14159f - 2 * gap) / 40;
        gfx_draw_line(s, cx + r * (float)sin(a0), cy - r * (float)cos(a0), cx + r * (float)sin(a1),
                      cy - r * (float)cos(a1), 2.6f, c);
    }
    gfx_draw_line(s, cx, cy - r - 2, cx, cy - r * 0.15f, 2.6f, c);
}

void lockscreen_draw(struct lockscreen *l, struct gfx_surface *s) {
    if (l->background) {
        gfx_copy(s, 0, 0, l->background, NULL);
    } else {
        gfx_fill_gradient(s, gfx_rect(0, 0, s->width, s->height), GFX_RGB(0x0B, 0x1B, 0x3A), GFX_RGB(0x0E, 0x4E, 0x6E));
    }
    float cx = l->w / 2.0f;

    /* The clock and the date. */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    l->clock24 = config_get_int(DDE_CONFIG, "Clock24", 1) != 0;
    char clock[16], date[48];
    if (l->clock24) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        int h = tm.tm_hour % 12;
        snprintf(clock, sizeof(clock), "%d:%02d", h ? h : 12, tm.tm_min);
    }
    snprintf(date, sizeof(date), "%s %d%s %d", months[tm.tm_mon], tm.tm_mday, ordinal(tm.tm_mday), tm.tm_year + 1900);
    float scale = l->h / 800.0f;
    shadowed(s, l->bold, 96 * scale, cx, 158 * scale, clock);
    shadowed(s, l->regular, 24 * scale, cx, 202 * scale, date);

    /* The card. */
    struct gfx_rect c = card_rect(l);
    struct gfx_alpha *shadow = gfx_shadow_create(c.w, c.h, CARD_RADIUS, 22);
    if (shadow) {
        gfx_fill_mask(s, c.x - 22, c.y - 22 + 6, shadow, GFX_RGBA(0, 0, 0, 70));
        gfx_alpha_destroy(shadow);
    }
    gfx_fill_rounded_rect(s, c, CARD_RADIUS, WHITE);
    if (l->picture) {
        gfx_blit(s, c.x + (c.w - PICTURE) / 2, c.y + 57, l->picture, NULL);
    }
    const struct user_account *u = lockscreen_user(l);
    const char *name = u ? (u->display[0] ? u->display : u->name) : "User";
    centered(s, l->bold, 28, cx + (c.x - (l->w - CARD_W) / 2), c.y + 192, INK, name);

    if (needs_password(l)) {
        struct gfx_rect f = field_rect(l);
        gfx_fill_rounded_rect(s, f, 8, FIELD);
        if (l->wrong) {
            gfx_stroke_rounded_rect(s, f, 8, 1.5f, RED);
        }
        float baseline = f.y + f.h / 2.0f + 6.5f;
        if (l->checking) {
            if (l->regular) {
                ttf_draw_text(s, l->regular, 18, f.x + 12, baseline, PLACEHOLDER, "Signing in...");
            }
        } else if (l->password_len == 0) {
            if (l->regular) {
                ttf_draw_text(s, l->regular, 18, f.x + 12, baseline, l->wrong ? RED : PLACEHOLDER,
                              l->wrong ? "Incorrect password" : "Password");
            }
        } else {
            /* Dots, then the caret. */
            int x = f.x + 16;
            for (int i = 0; i < l->password_len && x < f.x + f.w - 20; i++) {
                gfx_fill_circle(s, x, f.y + f.h / 2.0f, 4.5f, INK);
                x += 14;
            }
            gfx_fill_rect(s, gfx_rect(x - 4, f.y + 12, 2, f.h - 24), INK);
        }
        if (l->password_len == 0 && !l->checking && !l->wrong) {
            gfx_fill_rect(s, gfx_rect(f.x + 12, f.y + 12, 2, f.h - 24), GFX_RGBA(0, 0, 0, 120));
        }
    }
    struct gfx_rect b = button_rect(l);
    gfx_fill_rounded_rect(s, b, 8, l->checking ? GFX_RGB(0x9C, 0xC4, 0xF8) : l->hot == HOT_BUTTON ? BLUE_HOT : BLUE);
    centered(s, l->regular, 18, b.x + b.w / 2.0f, b.y + b.h / 2.0f + 6.5f, WHITE, l->login ? "Login" : "Unlock");

    /* The other accounts (logging in). */
    if (l->login && l->user_count > 1) {
        for (int i = 0; i < l->user_count; i++) {
            struct gfx_rect r = user_row(l, i);
            if (i == l->current) {
                gfx_fill_rounded_rect(s, r, 10, GFX_RGBA(255, 255, 255, 60));
            } else if (l->hot == HOT_USER && l->hot_user == i) {
                gfx_fill_rounded_rect(s, r, 10, GFX_RGBA(255, 255, 255, 34));
            }
            if (l->small[i]) {
                gfx_blit(s, r.x + 8, r.y + 7, l->small[i], NULL);
            }
            const char *n = l->users[i].display[0] ? l->users[i].display : l->users[i].name;
            if (l->regular) {
                ttf_draw_text(s, l->regular, 16, r.x + 52, r.y + r.h / 2.0f + 6, WHITE, n);
            }
        }
    }

    /* Power. */
    struct gfx_rect p = power_rect(l);
    if (l->hot == HOT_POWER || l->menu_open) {
        gfx_fill_rounded_rect(s, gfx_rect_inset(p, -4), 12, GFX_RGBA(255, 255, 255, 40));
    }
    power_icon(s, p.x + p.w / 2.0f, p.y + p.h / 2.0f + 2, 15, WHITE);
    if (l->menu_open) {
        struct gfx_rect m = menu_rect(l);
        gfx_fill_rounded_rect(s, m, 12, WHITE);
        struct gfx_rect off = gfx_rect(m.x + 4, m.y + 4, m.w - 8, m.h / 2 - 4);
        struct gfx_rect re = gfx_rect(m.x + 4, m.y + m.h / 2, m.w - 8, m.h / 2 - 4);
        if (l->hot == HOT_MENU_OFF) {
            gfx_fill_rounded_rect(s, off, 8, FIELD);
        } else if (l->hot == HOT_MENU_RESTART) {
            gfx_fill_rounded_rect(s, re, 8, FIELD);
        }
        if (l->regular) {
            ttf_draw_text(s, l->regular, 16, off.x + 14, off.y + off.h / 2.0f + 6, INK, "Shut down");
            ttf_draw_text(s, l->regular, 16, re.x + 14, re.y + re.h / 2.0f + 6, INK, "Restart");
        }
    }
    l->minute = (int)(now / 60);
}

/* --- input ---------------------------------------------------------------------------------------- */

static enum lockscreen_action submit(struct lockscreen *l) {
    if (!l->user_count) {
        return LOCK_NONE;
    }
    if (!needs_password(l)) {
        return LOCK_UNLOCKED;
    }
    l->checking = true;   /* shown first; checked on the next tick */
    l->wrong = false;
    return LOCK_REDRAW;
}

enum lockscreen_action lockscreen_input(struct lockscreen *l, const struct os_input_event *ev) {
    if (l->checking) {
        return LOCK_NONE;
    }
    if (ev->type == OS_EVENT_KEY) {
        if (!ev->pressed) {
            return LOCK_NONE;
        }
        if (ev->key == OS_KEY_ESCAPE) {
            if (l->menu_open) {
                l->menu_open = false;
            } else {
                clear_password(l);
            }
            return LOCK_REDRAW;
        }
        if (ev->key == OS_KEY_ENTER) {
            return submit(l);
        }
        if (ev->key == OS_KEY_BACKSPACE) {
            if (l->password_len) {
                l->password[--l->password_len] = '\0';
            }
            return LOCK_REDRAW;
        }
        if (ev->ch >= ' ' && ev->ch < 0x7F && !(ev->modifiers & OS_MOD_CTRL) && needs_password(l) &&
            l->password_len < (int)sizeof(l->password) - 1) {
            l->password[l->password_len++] = (char)ev->ch;
            l->wrong = false;
            return LOCK_REDRAW;
        }
        return LOCK_NONE;
    }
    if (ev->type != OS_EVENT_MOUSE) {
        return LOCK_NONE;
    }
    l->mx = ev->x;
    l->my = ev->y;
    uint32_t down = ev->buttons & ~l->buttons, up = l->buttons & ~ev->buttons;
    l->buttons = ev->buttons;
    int user = -1;
    enum hot hot = hit(l, ev->x, ev->y, &user);
    enum lockscreen_action action = LOCK_NONE;
    if (hot != l->hot || user != l->hot_user) {
        l->hot = hot;
        l->hot_user = user;
        action = LOCK_REDRAW;
    }
    if (down & OS_MOUSE_LEFT) {
        l->pressed = hot;
        l->pressed_user = user;
        if (l->menu_open && hot != HOT_MENU_OFF && hot != HOT_MENU_RESTART && hot != HOT_POWER) {
            l->menu_open = false;
            action = LOCK_REDRAW;
        }
    }
    if ((up & OS_MOUSE_LEFT) && hot == l->pressed && hot != HOT_NONE) {
        switch (hot) {
        case HOT_BUTTON:       return submit(l);
        case HOT_POWER:        l->menu_open = !l->menu_open; return LOCK_REDRAW;
        case HOT_MENU_OFF:     l->menu_open = false; return LOCK_SHUTDOWN;
        case HOT_MENU_RESTART: l->menu_open = false; return LOCK_RESTART;
        case HOT_USER:
            if (user == l->pressed_user && user != l->current) {
                l->current = user;
                load_pictures(l);
                clear_password(l);
                l->wrong = false;
            }
            return LOCK_REDRAW;
        default:               break;
        }
    }
    return action;
}

enum lockscreen_action lockscreen_tick(struct lockscreen *l) {
    if (l->checking) {
        bool ok = users_check_password(&l->users[l->current], l->password);
        l->checking = false;
        clear_password(l);
        if (ok) {
            return LOCK_UNLOCKED;
        }
        l->wrong = true;
        l->shake_start = uptime_ms();
        return LOCK_REDRAW;
    }
    if (l->shake_start) {
        if (uptime_ms() - l->shake_start >= SHAKE_MS) {
            l->shake_start = 0;
        }
        return LOCK_REDRAW;
    }
    return (int)(time(NULL) / 60) != l->minute ? LOCK_REDRAW : LOCK_NONE;
}

int lockscreen_next_tick_ms(const struct lockscreen *l) {
    if (l->checking) {
        return 0;
    }
    if (l->shake_start) {
        return 16;
    }
    return 1000;
}

bool lockscreen_hand(const struct lockscreen *l) {
    return l->hot == HOT_BUTTON || l->hot == HOT_POWER || l->hot == HOT_USER || l->hot == HOT_MENU_OFF ||
           l->hot == HOT_MENU_RESTART;
}
