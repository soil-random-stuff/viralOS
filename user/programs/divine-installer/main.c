/*
 * The installer's window: a list of the steps on the left, the current one on the right, Back and
 * Next at the bottom (see installer.h for what installing does).
 *
 *   Language   the languages viralOS speaks (one so far; the choice goes into /etc/locale.conf)
 *   License    "as is", the credits, and typing "yes" to go on
 *   Disk       which disk (everything on it goes); how the PC started (UEFI or BIOS) decides how
 *              the disk is set up to start
 *   Account    your name, the account name, a password, administrator or not, the computer's name
 *   Summary    the choices, and Install
 *   Install    the progress, then Restart
 */

#include <image.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

#include "installer.h"

#define WIDTH       800
#define HEIGHT      560
#define SIDE_W      220
#define PAD         32
#define FOOT_H      68
#define FIELD_H     36
#define ROW_H       56
#define MAX_DISKS   16
#define MAX_TARGETS 64

#define WHITE       GFX_RGB(255, 255, 255)
#define SIDE        GFX_RGB(0xF3, 0xF4, 0xF8)
#define INK         GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT    GFX_RGB(0x6A, 0x6A, 0x74)
#define INK_FAINT   GFX_RGB(0xB4, 0xB4, 0xBC)
#define LINE        GFX_RGB(0xE4, 0xE5, 0xEB)
#define FIELD       GFX_RGB(0xF4, 0xF5, 0xF8)
#define HOVER       GFX_RGB(0xEE, 0xF0, 0xF6)
#define SELECTED    GFX_RGB(0xDD, 0xE8, 0xFC)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define ACCENT_HOT  GFX_RGB(0x25, 0x5E, 0xD6)
#define DANGER      GFX_RGB(0xE0, 0x3E, 0x3E)
#define GOOD        GFX_RGB(0x1A, 0x9A, 0x55)

/* The languages the installer and the system can be in. Only English so far: another one is a line
   here (and its translations, once there's a way to switch the text). */
static const struct language {
    const char *code;            /* for /etc/locale.conf: LANG=<code>.UTF-8 */
    const char *name;            /* in English */
    const char *native;          /* in itself */
} languages[] = {
    { "en_US", "English (United States)", "English" },
};
#define LANGUAGE_COUNT ((int)(sizeof(languages) / sizeof(languages[0])))

static const char *const license_text =
    "viralOS is provided \"as is\", without any warranty of any kind, express or implied.\n"
    "\n"
    "The developers of this project are NOT responsible in ANY way for hardware damage, data loss or "
    "system instability arising from installing or using it. Installing erases everything on the disk "
    "you choose. Trying viralOS in a virtual machine first is recommended.\n"
    "\n"
    "USE AT YOUR OWN RISK.\n"
    "\n"
    "viralOS is free software under the GNU General Public License, version 2. It stands on a lot of "
    "great free and open-source work:\n"
    "\n"
    "- Limine: the bootloader\n"
    "- lwIP: the TCP/IP stack\n"
    "- lwext4: the ext4 filesystem\n"
    "- Mbed TLS: secure connections; Mozilla's trusted root certificates\n"
    "- The Linux kernel: the xHCI, USB, AHCI, NVMe, HD Audio, Intel e1000/e1000e, Realtek r8169 and "
    "filesystem drivers were ported from or written after its sources\n"
    "- minimp3 (lieff), stb_vorbis (Sean Barrett), dr_flac and dr_wav (David Reid): audio decoding\n"
    "- stb_image_write (Sean Barrett): PNG saving\n"
    "- e2fsprogs, ntfsprogs-plus, dosfstools, exfatprogs, GNU parted and fatresize: the disk tools, "
    "built against musl libc\n"
    "- DejaVu fonts and the Terminus console font\n"
    "- Icons8: the icons (icons8.com)\n"
    "- Syn Cole - Feel Good [NCS Release]: the sample song\n";

enum step { STEP_LANGUAGE, STEP_LICENSE, STEP_DISK, STEP_ACCOUNT, STEP_SUMMARY, STEP_INSTALL, STEP_COUNT };
static const char *const step_names[STEP_COUNT] = { "Language", "License", "Disk", "Account", "Summary", "Install" };

enum target_id {
    T_NONE = -1, T_BACK, T_NEXT, T_ADMIN, T_RESTART, T_QUIT,
    T_LANGUAGE = 100, T_DISK = 200, T_FIELD = 300,
};

struct field {
    char text[128];
    bool secret;
};

enum field_id { F_CONFIRM, F_DISPLAY, F_USER, F_PASSWORD, F_PASSWORD2, F_HOSTNAME, F_COUNT };

struct disk {
    char name[16], model[48];
    uint64_t bytes;
    uint32_t sector_size;
    bool external, usable;
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *ui, *bold;
    struct gfx_surface *logo;
    enum step step;
    int hover, pressed;
    struct { struct gfx_rect r; int id; } targets[MAX_TARGETS];
    int target_count;

    int language;
    int license_scroll, license_height;
    struct gfx_rect license_box;
    struct disk disks[MAX_DISKS];
    int disk_count, disk;
    bool uefi;
    struct field fields[F_COUNT];
    int focus;                   /* a field, or -1 */
    bool user_edited;            /* the account name was typed, not made from the name */
    bool admin;
    enum install_state state;
} ui;

/* --- small drawing helpers ------------------------------------------------------------------------ */

static void target(struct gfx_rect r, int id) {
    if (ui.target_count < MAX_TARGETS) {
        ui.targets[ui.target_count].r = r;
        ui.targets[ui.target_count++].id = id;
    }
}

static int target_at(int x, int y) {
    for (int i = ui.target_count - 1; i >= 0; i--) {
        if (gfx_rect_contains(ui.targets[i].r, x, y)) {
            return ui.targets[i].id;
        }
    }
    return T_NONE;
}

static float text(struct gfx_surface *s, bool bold, float size, float x, float y, gfx_color c, const char *t) {
    return ttf_draw_text(s, bold ? ui.bold : ui.ui, size, x, y, c, t);
}

static float width(bool bold, float size, const char *t) {
    return ttf_text_width(bold ? ui.bold : ui.ui, size, t);
}

/* Text wrapped to `w`, from (x, y), lines `line` apart; returns the height used. Draws only if s. */
static int wrapped(struct gfx_surface *s, bool bold, float size, int x, int y, int w, int line, gfx_color c,
                   const char *t) {
    int lines = 0;
    const char *p = t;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end_para = nl ? nl : p + strlen(p);
        if (end_para == p) {   /* an empty line */
            lines++;
            p = nl + 1;
            continue;
        }
        while (p < end_para) {
            const char *fit = p, *q = p;
            char buf[256];
            while (q < end_para) {
                const char *word = q;
                while (word < end_para && *word == ' ') {
                    word++;
                }
                const char *stop = word;
                while (stop < end_para && *stop != ' ') {
                    stop++;
                }
                size_t n = (size_t)(stop - p) < sizeof(buf) - 1 ? (size_t)(stop - p) : sizeof(buf) - 1;
                memcpy(buf, p, n);
                buf[n] = '\0';
                if (fit != p && width(bold, size, buf) > w) {
                    break;
                }
                fit = stop;
                q = stop;
            }
            if (fit == p) {
                fit = end_para;
            }
            size_t n = (size_t)(fit - p) < sizeof(buf) - 1 ? (size_t)(fit - p) : sizeof(buf) - 1;
            memcpy(buf, p, n);
            buf[n] = '\0';
            if (s) {
                text(s, bold, size, x, y + lines * line, c, buf);
            }
            lines++;
            p = fit;
            while (p < end_para && *p == ' ') {
                p++;
            }
        }
        p = nl ? nl + 1 : end_para;
    }
    return lines * line;
}

static void button(struct gfx_surface *s, struct gfx_rect r, const char *label, int id, bool primary, bool danger,
                   bool enabled) {
    bool hot = enabled && ui.hover == id;
    gfx_color fill = !enabled  ? GFX_RGB(0xEE, 0xEE, 0xF1)
                   : primary   ? (danger ? (hot ? GFX_RGB(0xC9, 0x30, 0x30) : DANGER) : (hot ? ACCENT_HOT : ACCENT))
                   : hot       ? GFX_RGB(0xE4, 0xE5, 0xEA) : GFX_RGB(0xEC, 0xED, 0xF1);
    gfx_fill_rounded_rect(s, r, 8, fill);
    float w = width(false, 13.5f, label);
    text(s, false, 13.5f, r.x + (r.w - w) / 2, r.y + r.h / 2.0f + 5,
         !enabled ? INK_FAINT : primary ? WHITE : INK, label);
    if (enabled) {
        target(r, id);
    }
}

static void field(struct gfx_surface *s, int x, int y, int w, const char *label, int id, const char *placeholder) {
    struct field *f = &ui.fields[id];
    if (label) {
        text(s, false, 12.5f, x, y - 8, INK_SOFT, label);
    }
    struct gfx_rect box = gfx_rect(x, y, w, FIELD_H);
    bool focused = ui.focus == id;
    gfx_fill_rounded_rect(s, box, 8, FIELD);
    gfx_stroke_rounded_rect(s, box, 8, focused ? 1.5f : 1, focused ? ACCENT : LINE);
    char shown[160];
    if (f->secret) {
        size_t n = strlen(f->text);
        n = n < sizeof(shown) / 3 - 1 ? n : sizeof(shown) / 3 - 1;
        shown[0] = '\0';
        for (size_t i = 0; i < n; i++) {
            strlcat(shown, "\xE2\x80\xA2", sizeof(shown));   /* a dot */
        }
    } else {
        strlcpy(shown, f->text, sizeof(shown));
    }
    struct gfx_rect old = s->clip;
    gfx_set_clip(s, gfx_rect_inset(box, 4));
    float end = x + 12;
    if (shown[0]) {
        end = text(s, false, 14, x + 12, y + 23, INK, shown);
    } else if (placeholder && !focused) {
        text(s, false, 14, x + 12, y + 23, INK_FAINT, placeholder);
    }
    if (focused) {
        gfx_fill_rect(s, gfx_rect((int)end + 1, y + 9, 1, 18), INK);
    }
    gfx_set_clip(s, old);
    target(box, T_FIELD + id);
}

static void size_text(uint64_t bytes, char *out, size_t size) {
    if (bytes >= 1000ull * 1000 * 1000) {
        snprintf(out, size, "%.1f GB", (double)bytes / 1e9);
    } else {
        snprintf(out, size, "%lu MB", (unsigned long)(bytes / 1000000));
    }
}

/* --- the steps' state ----------------------------------------------------------------------------- */

static void load_disks(void) {
    static struct os_block_info b;
    ui.disk_count = 0;
    for (int i = 0; blockdev(i, &b) == 0 && ui.disk_count < MAX_DISKS; i++) {
        if (!(b.flags & OS_BLOCK_DISK)) {
            continue;
        }
        struct disk *d = &ui.disks[ui.disk_count++];
        strlcpy(d->name, b.name, sizeof(d->name));
        strlcpy(d->model, b.model[0] ? b.model : b.name, sizeof(d->model));
        d->sector_size = b.sector_size ? b.sector_size : 512;
        d->bytes = b.sectors * d->sector_size;
        d->external = b.flags & OS_BLOCK_EXTERNAL;
        d->usable = !(b.flags & OS_BLOCK_READ_ONLY) && d->bytes >= MIN_DISK_BYTES;
    }
    if (ui.disk >= ui.disk_count || (ui.disk >= 0 && !ui.disks[ui.disk].usable)) {
        ui.disk = -1;
    }
    struct os_sysinfo si;
    ui.uefi = sysinfo(&si) == 0 && si.firmware == OS_FIRMWARE_UEFI;
}

static bool valid_hostname(const char *h) {
    if (!h[0] || strlen(h) > 63 || h[0] == '-') {
        return false;
    }
    for (const char *p = h; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            return false;
        }
    }
    return true;
}

/* What's wrong with the account (NULL: nothing). */
static const char *account_problem(void) {
    const char *user = ui.fields[F_USER].text;
    if (!ui.fields[F_DISPLAY].text[0]) {
        return "Type your name.";
    }
    if (strchr(ui.fields[F_DISPLAY].text, ':')) {
        return "Your name can't have a colon (:) in it.";
    }
    if (!users_valid_name(user)) {
        return "The account name can only have lowercase letters, digits, - and _.";
    }
    if (strcmp(user, "root") == 0) {
        return "\"root\" is the system's own account: choose another name.";
    }
    if (strcmp(ui.fields[F_PASSWORD].text, ui.fields[F_PASSWORD2].text) != 0) {
        return "The passwords don't match.";
    }
    if (!valid_hostname(ui.fields[F_HOSTNAME].text)) {
        return "The computer's name can only have letters, digits and -.";
    }
    return NULL;
}

static bool yes_typed(void) {
    const char *t = ui.fields[F_CONFIRM].text;
    return (t[0] == 'y' || t[0] == 'Y') && (t[1] == 'e' || t[1] == 'E') && (t[2] == 's' || t[2] == 'S') && !t[3];
}

static bool can_go_on(void) {
    switch (ui.step) {
    case STEP_LANGUAGE: return ui.language >= 0;
    case STEP_LICENSE: return yes_typed();
    case STEP_DISK: return ui.disk >= 0;
    case STEP_ACCOUNT: return account_problem() == NULL;
    default: return true;
    }
}

/* The account name from the name: "Alex Smith" -> "alex". */
static void suggest_user(void) {
    if (ui.user_edited) {
        return;
    }
    char out[OS_USER_MAX] = "";
    size_t n = 0;
    for (const char *p = ui.fields[F_DISPLAY].text; *p && *p != ' ' && n < sizeof(out) - 1; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    strlcpy(ui.fields[F_USER].text, out, sizeof(ui.fields[F_USER].text));
}

/* --- drawing -------------------------------------------------------------------------------------- */

static void heading(struct gfx_surface *s, const char *title, const char *sub) {
    int x = SIDE_W + PAD;
    text(s, true, 24, x, 58, INK, title);
    if (sub) {
        wrapped(s, false, 13.5f, x, 86, WIDTH - SIDE_W - 2 * PAD, 19, INK_SOFT, sub);
    }
}

static void draw_side(struct gfx_surface *s) {
    gfx_fill_rect(s, gfx_rect(0, 0, SIDE_W, HEIGHT), SIDE);
    gfx_fill_rect(s, gfx_rect(SIDE_W - 1, 0, 1, HEIGHT), LINE);
    if (ui.logo) {
        gfx_blit(s, 24, 26, ui.logo, NULL);
    }
    text(s, true, 15, 24 + 44 + 10, 44, INK, "viralOS");
    text(s, false, 12, 24 + 44 + 10, 61, INK_SOFT, "Installer");
    for (int i = 0; i < STEP_COUNT; i++) {
        int y = 110 + i * 40;
        bool now = (int)ui.step == i, done = (int)ui.step > i;
        if (now) {
            gfx_fill_rounded_rect(s, gfx_rect(12, y - 2, SIDE_W - 24, 34), 8, WHITE);
        }
        float cx = 34, cy = y + 15;
        gfx_fill_circle(s, cx, cy, 11, now ? ACCENT : done ? GOOD : GFX_RGB(0xD9, 0xDB, 0xE3));
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        if (done) {
            gfx_draw_line(s, cx - 4.5f, cy, cx - 1, cy + 3.5f, 2, WHITE);
            gfx_draw_line(s, cx - 1, cy + 3.5f, cx + 5, cy - 3.5f, 2, WHITE);
        } else {
            float w = width(true, 11.5f, n);
            text(s, true, 11.5f, cx - w / 2, cy + 4, now ? WHITE : INK_SOFT, n);
        }
        text(s, now, 14, 56, y + 20, now ? INK : done ? INK : INK_SOFT, step_names[i]);
    }
}

static void draw_language(struct gfx_surface *s) {
    heading(s, "Welcome to viralOS", "Let's put viralOS on this computer. First, choose your language.");
    int x = SIDE_W + PAD, w = WIDTH - SIDE_W - 2 * PAD;
    for (int i = 0; i < LANGUAGE_COUNT; i++) {
        struct gfx_rect r = gfx_rect(x, 140 + i * ROW_H, w, ROW_H - 6);
        bool on = ui.language == i;
        gfx_fill_rounded_rect(s, r, 10, on ? SELECTED : ui.hover == T_LANGUAGE + i ? HOVER : WHITE);
        gfx_stroke_rounded_rect(s, r, 10, 1, on ? ACCENT : LINE);
        text(s, true, 14.5f, r.x + 18, r.y + 22, INK, languages[i].native);
        text(s, false, 12.5f, r.x + 18, r.y + 40, INK_SOFT, languages[i].name);
        target(r, T_LANGUAGE + i);
    }
    text(s, false, 12.5f, x, 140 + LANGUAGE_COUNT * ROW_H + 20, INK_FAINT,
         "More languages will come with a way to switch the system's text.");
}

static void draw_license(struct gfx_surface *s) {
    heading(s, "Before you install", NULL);
    int x = SIDE_W + PAD, w = WIDTH - SIDE_W - 2 * PAD;
    ui.license_box = gfx_rect(x, 80, w, 290);
    gfx_fill_rounded_rect(s, ui.license_box, 10, FIELD);
    gfx_stroke_rounded_rect(s, ui.license_box, 10, 1, LINE);
    struct gfx_rect inside = gfx_rect_inset(ui.license_box, 16);
    ui.license_height = wrapped(NULL, false, 13, 0, 0, inside.w - 10, 19, INK, license_text);
    int max = ui.license_height - inside.h + 10;
    ui.license_scroll = ui.license_scroll > max ? (max > 0 ? max : 0) : ui.license_scroll;
    gfx_set_clip(s, inside);
    wrapped(s, false, 13, inside.x, inside.y + 14 - ui.license_scroll, inside.w - 10, 19, INK, license_text);
    gfx_reset_clip(s);
    if (max > 0) {
        int track = inside.h, thumb = track * inside.h / ui.license_height;
        int ty = inside.y + (track - thumb) * ui.license_scroll / max;
        gfx_fill_rounded_rect(s, gfx_rect(ui.license_box.x + ui.license_box.w - 9, ty, 4, thumb), 2,
                              GFX_RGBA(0, 0, 0, 70));
    }
    text(s, true, 14, x, 402, INK, "Do you want to install?");
    text(s, false, 13, x, 422, INK_SOFT, "Type yes to agree and continue.");
    field(s, x, 436, 160, NULL, F_CONFIRM, "yes");
}

static void draw_disk(struct gfx_surface *s) {
    heading(s, "Where should viralOS go?", NULL);
    int x = SIDE_W + PAD, w = WIDTH - SIDE_W - 2 * PAD;
    char mode[160];
    snprintf(mode, sizeof(mode), "This computer started in %s mode, so the disk will be set up to start with %s.",
             ui.uefi ? "UEFI" : "BIOS (legacy)", ui.uefi ? "UEFI" : "BIOS");
    wrapped(s, false, 13, x, 88, w, 18, INK_SOFT, mode);
    if (!ui.disk_count) {
        text(s, false, 14, x, 140, INK, "No disks found.");
    }
    for (int i = 0; i < ui.disk_count; i++) {
        const struct disk *d = &ui.disks[i];
        struct gfx_rect r = gfx_rect(x, 124 + i * (ROW_H + 4), w, ROW_H);
        bool on = ui.disk == i;
        gfx_fill_rounded_rect(s, r, 10, on ? SELECTED : d->usable && ui.hover == T_DISK + i ? HOVER : WHITE);
        gfx_stroke_rounded_rect(s, r, 10, 1, on ? ACCENT : LINE);
        /* A drive: a box with a light. */
        struct gfx_rect icon = gfx_rect(r.x + 16, r.y + 15, 34, 26);
        gfx_fill_rounded_rect(s, icon, 5, d->usable ? GFX_RGB(0x8E, 0x95, 0xA6) : INK_FAINT);
        gfx_fill_rect(s, gfx_rect(icon.x + 5, icon.y + icon.h - 8, icon.w - 10, 2), GFX_RGBA(255, 255, 255, 150));
        gfx_fill_circle(s, icon.x + icon.w - 7, icon.y + 7, 2.5f, d->usable ? GFX_RGB(0x7D, 0xE0, 0x9F) : WHITE);
        char line[128], size[24];
        size_text(d->bytes, size, sizeof(size));
        text(s, true, 14, r.x + 66, r.y + 24, d->usable ? INK : INK_FAINT, d->model);
        snprintf(line, sizeof(line), "%s  -  %s%s%s", size, d->name, d->external ? "  -  USB" : "",
                 d->usable ? "" : d->bytes < MIN_DISK_BYTES ? "  -  too small (1 GB at least)" : "  -  read-only");
        text(s, false, 12.5f, r.x + 66, r.y + 42, d->usable ? INK_SOFT : INK_FAINT, line);
        if (d->usable) {
            target(r, T_DISK + i);
        }
    }
    int wy = 124 + (ui.disk_count ? ui.disk_count : 1) * (ROW_H + 4) + 26;
    if (ui.disk >= 0) {
        char warn[160];
        snprintf(warn, sizeof(warn), "Everything on %s will be erased.", ui.disks[ui.disk].model);
        text(s, true, 13.5f, x, wy, DANGER, warn);
    }
}

static void draw_account(struct gfx_surface *s) {
    heading(s, "Create your account", NULL);
    int x = SIDE_W + PAD, w = WIDTH - SIDE_W - 2 * PAD, half = (w - 16) / 2;
    field(s, x, 110, half, "Your name", F_DISPLAY, "Alex Smith");
    field(s, x + half + 16, 110, half, "Account name", F_USER, "alex");
    field(s, x, 178, half, "Password", F_PASSWORD, "Optional");
    field(s, x + half + 16, 178, half, "Confirm the password", F_PASSWORD2, NULL);
    field(s, x, 246, half, "Computer name", F_HOSTNAME, "viralos");
    /* Administrator: a checkbox. */
    struct gfx_rect box = gfx_rect(x, 312, 22, 22);
    gfx_fill_rounded_rect(s, box, 6, ui.admin ? ACCENT : WHITE);
    gfx_stroke_rounded_rect(s, box, 6, 1.5f, ui.admin ? ACCENT : INK_FAINT);
    if (ui.admin) {
        gfx_draw_line(s, box.x + 5.5f, box.y + 11.5f, box.x + 9.5f, box.y + 15.5f, 2.2f, WHITE);
        gfx_draw_line(s, box.x + 9.5f, box.y + 15.5f, box.x + 16.5f, box.y + 7, 2.2f, WHITE);
    }
    text(s, true, 14, x + 34, 322, INK, "Administrator");
    text(s, false, 12.5f, x + 34, 340, INK_SOFT, "Can use sudo, and add and change other users");
    target(gfx_rect(x, 308, w, 40), T_ADMIN);
    const char *problem = account_problem();
    bool started = ui.fields[F_DISPLAY].text[0] || ui.fields[F_PASSWORD].text[0];
    if (problem && started) {
        text(s, false, 13, x, 384, DANGER, problem);
    } else if (!ui.fields[F_PASSWORD].text[0]) {
        text(s, false, 13, x, 384, INK_SOFT, "With no password, logging in won't ask for one.");
    }
}

static void summary_row(struct gfx_surface *s, int *y, const char *label, const char *value) {
    int x = SIDE_W + PAD;
    text(s, false, 13, x, *y, INK_SOFT, label);
    text(s, false, 14, x + 150, *y, INK, value);
    *y += 30;
}

static void draw_summary(struct gfx_surface *s) {
    heading(s, "Ready to install", "Check your choices. Back changes them; Install starts.");
    int y = 140;
    char line[160], size[24];
    const struct disk *d = &ui.disks[ui.disk];
    size_text(d->bytes, size, sizeof(size));
    summary_row(s, &y, "Language", languages[ui.language].name);
    snprintf(line, sizeof(line), "%s (%s, %s)", d->model, d->name, size);
    summary_row(s, &y, "Disk", line);
    summary_row(s, &y, "Starts with", ui.uefi ? "UEFI (GPT, EFI System Partition)" : "BIOS (MBR)");
    summary_row(s, &y, "Partitions", "Boot (FAT32, 256 MB) and viralOS (ext4)");
    snprintf(line, sizeof(line), "%s (%s)", ui.fields[F_DISPLAY].text, ui.fields[F_USER].text);
    summary_row(s, &y, "Account", line);
    summary_row(s, &y, "Administrator", ui.admin ? "Yes" : "No");
    summary_row(s, &y, "Computer name", ui.fields[F_HOSTNAME].text);
    snprintf(line, sizeof(line), "Everything on %s will be erased.", d->model);
    text(s, true, 13.5f, SIDE_W + PAD, y + 16, DANGER, line);
}

static void draw_install(struct gfx_surface *s) {
    int x = SIDE_W + PAD, w = WIDTH - SIDE_W - 2 * PAD;
    if (ui.state == INSTALL_DONE) {
        heading(s, "viralOS is installed", NULL);
        gfx_fill_circle(s, x + 28, 150, 28, GOOD);
        gfx_draw_line(s, x + 16, 151, x + 25, 160, 4, WHITE);
        gfx_draw_line(s, x + 25, 160, x + 41, 141, 4, WHITE);
        wrapped(s, false, 14, x, 214, w, 21, INK,
                "Take out the installation media (the USB stick or disc), then restart. The computer will "
                "start viralOS from the disk, and you can log in with your new account.");
        return;
    }
    if (ui.state == INSTALL_FAILED) {
        heading(s, "The installation didn't finish", NULL);
        wrapped(s, true, 14, x, 110, w, 21, DANGER, install_status());
        wrapped(s, false, 13, x, 170, w, 19, INK_SOFT,
                "The disk may be left half set up. Back goes to the summary to try again; the details "
                "are in /tmp (installer-*.log).");
        return;
    }
    heading(s, "Installing viralOS", "This takes a minute or two. Don't turn the computer off.");
    struct gfx_rect bar = gfx_rect(x, 160, w, 10);
    gfx_fill_rounded_rect(s, bar, 5, GFX_RGB(0xE6, 0xE8, 0xEE));
    int filled = (int)(bar.w * install_progress());
    if (filled > 0) {
        gfx_fill_rounded_rect(s, gfx_rect(bar.x, bar.y, filled < 10 ? 10 : filled, bar.h), 5, ACCENT);
    }
    text(s, false, 13.5f, x, 196, INK_SOFT, install_status());
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(ui.w);
    ui.target_count = 0;
    gfx_clear(s, WHITE);
    draw_side(s);
    switch (ui.step) {
    case STEP_LANGUAGE: draw_language(s); break;
    case STEP_LICENSE: draw_license(s); break;
    case STEP_DISK: draw_disk(s); break;
    case STEP_ACCOUNT: draw_account(s); break;
    case STEP_SUMMARY: draw_summary(s); break;
    default: draw_install(s); break;
    }
    /* The bottom: Back and Next (or what fits the moment). */
    int fy = HEIGHT - FOOT_H;
    gfx_fill_rect(s, gfx_rect(SIDE_W, fy, WIDTH - SIDE_W, 1), LINE);
    struct gfx_rect next = gfx_rect(WIDTH - PAD - 120, fy + (FOOT_H - 38) / 2, 120, 38);
    struct gfx_rect back = gfx_rect(next.x - 12 - 100, next.y, 100, 38);
    if (ui.step == STEP_INSTALL) {
        if (ui.state == INSTALL_DONE) {
            button(s, next, "Restart", T_RESTART, true, false, true);
        } else if (ui.state == INSTALL_FAILED) {
            button(s, next, "Quit", T_QUIT, false, false, true);
            button(s, back, "Back", T_BACK, false, false, true);
        }
    } else {
        bool last = ui.step == STEP_SUMMARY;
        button(s, next, last ? "Install" : "Next", T_NEXT, true, last, can_go_on());
        if (ui.step > STEP_LANGUAGE) {
            button(s, back, "Back", T_BACK, false, false, true);
        }
    }
    gui_window_invalidate(ui.w, NULL);
}

/* --- input ---------------------------------------------------------------------------------------- */

static int first_field(enum step step) {
    return step == STEP_LICENSE ? F_CONFIRM : step == STEP_ACCOUNT ? F_DISPLAY : -1;
}

static void go(enum step step) {
    ui.step = step;
    ui.focus = first_field(step);
    if (step == STEP_DISK) {
        load_disks();
    }
}

static void next(void) {
    if (!can_go_on()) {
        return;
    }
    if (ui.step == STEP_SUMMARY) {
        struct choices c;
        memset(&c, 0, sizeof(c));
        const struct disk *d = &ui.disks[ui.disk];
        strlcpy(c.language, languages[ui.language].code, sizeof(c.language));
        strlcpy(c.disk, d->name, sizeof(c.disk));
        strlcpy(c.disk_model, d->model, sizeof(c.disk_model));
        c.disk_bytes = d->bytes;
        c.sector_size = d->sector_size;
        c.uefi = ui.uefi;
        strlcpy(c.display, ui.fields[F_DISPLAY].text, sizeof(c.display));
        strlcpy(c.user, ui.fields[F_USER].text, sizeof(c.user));
        strlcpy(c.password, ui.fields[F_PASSWORD].text, sizeof(c.password));
        c.admin = ui.admin;
        strlcpy(c.hostname, ui.fields[F_HOSTNAME].text, sizeof(c.hostname));
        install_start(&c);
        ui.state = INSTALL_RUNNING;
        go(STEP_INSTALL);
        return;
    }
    go(ui.step + 1);
}

static void click(int id) {
    if (id == T_NEXT) {
        next();
    } else if (id == T_BACK) {
        go(ui.step == STEP_INSTALL ? STEP_SUMMARY : ui.step - 1);
    } else if (id == T_ADMIN) {
        ui.admin = !ui.admin;
    } else if (id == T_RESTART) {
        power(OS_POWER_RESTART);
    } else if (id == T_QUIT) {
        gui_close(ui.g);
        exit(0);
    } else if (id >= T_FIELD && id < T_FIELD + F_COUNT) {
        ui.focus = id - T_FIELD;
    } else if (id >= T_DISK && id < T_DISK + MAX_DISKS) {
        ui.disk = id - T_DISK;
    } else if (id >= T_LANGUAGE && id < T_LANGUAGE + LANGUAGE_COUNT) {
        ui.language = id - T_LANGUAGE;
    }
}

/* The fields on the current step, in Tab order. */
static int next_field(int from) {
    if (ui.step == STEP_ACCOUNT) {
        static const int order[] = { F_DISPLAY, F_USER, F_PASSWORD, F_PASSWORD2, F_HOSTNAME };
        for (int i = 0; i < 5; i++) {
            if (order[i] == from) {
                return order[(i + 1) % 5];
            }
        }
        return F_DISPLAY;
    }
    return first_field(ui.step);
}

static bool key(const struct gui_event *ev) {
    if (ev->key == OS_KEY_ENTER) {
        next();
        return true;
    }
    if (ev->key == OS_KEY_TAB) {
        ui.focus = next_field(ui.focus);
        return true;
    }
    if (ui.focus < 0) {
        return false;
    }
    struct field *f = &ui.fields[ui.focus];
    size_t n = strlen(f->text);
    if (ev->key == OS_KEY_BACKSPACE) {
        if (n) {
            f->text[n - 1] = '\0';
        }
    } else if (ev->ch >= ' ' && ev->ch < 0x7F && !(ev->modifiers & OS_MOD_CTRL) && n < sizeof(f->text) - 1 &&
               !(ui.focus == F_USER && n >= OS_USER_MAX - 1)) {
        f->text[n] = (char)ev->ch;
        f->text[n + 1] = '\0';
    } else {
        return false;
    }
    if (ui.focus == F_DISPLAY) {
        suggest_user();
    } else if (ui.focus == F_USER) {
        ui.user_edited = true;
    }
    return true;
}

int main(void) {
    ui.g = gui_open();
    if (!ui.g) {
        return 1;
    }
    ui.ui = gui_font(ui.g, GUI_FONT_REGULAR);
    ui.bold = gui_font(ui.g, GUI_FONT_BOLD);
    if (!ui.ui || !ui.bold) {
        return 1;
    }
    struct gfx_surface *logo = image_load(_PATH_DDE_RES "/Icons/Apps/OsInstall.png", NULL);
    if (logo) {
        ui.logo = gfx_surface_scaled(logo, 44, 44);
        gfx_surface_destroy(logo);
    }
    ui.w = gui_window_create(ui.g, "Install viralOS", -1, -1, WIDTH, HEIGHT);
    if (!ui.w) {
        return 1;
    }
    ui.hover = ui.pressed = T_NONE;
    ui.focus = -1;
    ui.disk = -1;
    ui.admin = true;
    ui.fields[F_PASSWORD].secret = ui.fields[F_PASSWORD2].secret = true;
    strlcpy(ui.fields[F_HOSTNAME].text, "viralos", sizeof(ui.fields[F_HOSTNAME].text));
    draw();

    struct gui_event ev;
    for (;;) {
        bool working = ui.step == STEP_INSTALL && ui.state == INSTALL_RUNNING;
        if (working) {
            enum install_state before = ui.state;
            ui.state = install_step();
            static uint64_t last_draw;
            if (ui.state != before || uptime_ms() - last_draw > 200) {
                last_draw = uptime_ms();
                draw();
            }
        }
        if (!gui_wait_event(ui.g, &ev, working ? 0 : -1)) {
            continue;
        }
        bool redraw = true;
        switch (ev.type) {
        case GUI_EVENT_QUIT:
            return 0;
        case GUI_EVENT_CLOSE:
            if (!working) {   /* (not halfway through writing the disk) */
                gui_close(ui.g);
                return 0;
            }
            redraw = false;
            break;
        case GUI_EVENT_MOUSE_MOVE: {
            int id = target_at(ev.x, ev.y);
            redraw = id != ui.hover;
            ui.hover = id;
            gui_window_set_cursor(ui.w, id != T_NONE && !(id >= T_FIELD && id < T_FIELD + F_COUNT)
                                            ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
            break;
        }
        case GUI_EVENT_MOUSE_DOWN:
            ui.pressed = ev.button == OS_MOUSE_LEFT ? target_at(ev.x, ev.y) : T_NONE;
            if (ui.pressed >= T_FIELD && ui.pressed < T_FIELD + F_COUNT) {
                click(ui.pressed);   /* (a field takes the keyboard at once) */
            }
            break;
        case GUI_EVENT_MOUSE_UP:
            if (ui.pressed != T_NONE && target_at(ev.x, ev.y) == ui.pressed &&
                !(ui.pressed >= T_FIELD && ui.pressed < T_FIELD + F_COUNT)) {
                click(ui.pressed);
            }
            ui.pressed = T_NONE;
            break;
        case GUI_EVENT_SCROLL:
            if (ui.step == STEP_LICENSE && gfx_rect_contains(ui.license_box, ev.x, ev.y)) {
                ui.license_scroll += ev.wheel * 38;
                ui.license_scroll = ui.license_scroll < 0 ? 0 : ui.license_scroll;
            }
            break;
        case GUI_EVENT_KEY:
            redraw = !working && key(&ev);
            break;
        default:
            redraw = false;
            break;
        }
        if (redraw) {
            draw();
        }
    }
}
