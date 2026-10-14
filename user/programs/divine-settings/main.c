/*
 * divine-settings [page]: the window. The sidebar (search box, then the categories in groups), the
 * header (back / forward and the page's name), and the page itself, scrolled with the wheel. See
 * settings.h for the rest. `page` opens on that page ("network", "sound"...).
 */

#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include <paths.h>

#define SEARCH_H    32
#define ROW_H       34
#define ROW_TOP     72
#define GROUP_GAP   14
#define SIDE_ICON   24
#define HISTORY_MAX 32
#define SCROLL_STEP 48

static struct page pages[] = {
    { "Network", "Network", "ethernet internet ip address dhcp dns router gateway connection lan adapter mac",
      GFX_RGB(0x2F, 0x7B, 0xF6), 0, page_network, page_network_show, 2000, NULL },
    { "Users", "Users", "accounts account password login lock picture photo avatar name add remove",
      GFX_RGB(0x8E, 0x8E, 0x93), 0, page_users, page_users_show, 0, page_users_work },
    { "General", "General", "about computer name hostname version kernel processor cpu memory ram system viralos dde",
      GFX_RGB(0x8E, 0x8E, 0x93), 1, page_general, page_general_show, 0, NULL },
    { "Sound", "Sound", "volume mute output speakers headphones audio effects startup shutdown alert",
      GFX_RGB(0xF2, 0x3B, 0x5F), 1, page_sound, page_sound_show, 0, NULL },
    { "Displays", "Displays", "screen resolution monitor brightness",
      GFX_RGB(0x2F, 0x7B, 0xF6), 1, page_displays, NULL, 0, NULL },
    { "Wallpaper", "Wallpaper", "background picture image desktop photo",
      GFX_RGB(0x2A, 0xB5, 0xE8), 1, page_wallpaper, page_wallpaper_show, 0, page_wallpaper_work },
    { "Desktop & Panel", "DesktopPanel", "dock taskbar panel icons pinned apps clock arrange size",
      GFX_RGB(0x2C, 0x2C, 0x30), 1, page_desktop, page_desktop_show, 0, NULL },
    { "Mouse", "Mouse", "pointer scrolling natural wheel double-click click speed trackpad",
      GFX_RGB(0x8E, 0x8E, 0x93), 2, page_mouse, page_mouse_show, 0, NULL },
    { "Date & Time", "DateTime", "clock 24-hour 12-hour time date calendar",
      GFX_RGB(0x2F, 0x7B, 0xF6), 2, page_datetime, NULL, 1000, NULL },
    { "Storage", "Storage", "disks drives space usb eject free used",
      GFX_RGB(0x8E, 0x8E, 0x93), 2, page_storage, page_storage_show, 3000, NULL },
};
#define PAGE_COUNT ((int)(sizeof(pages) / sizeof(pages[0])))

static struct gui_window *win;
static struct gfx_surface *icons[PAGE_COUNT];
static int current;
static int scroll[PAGE_COUNT], content_h;
static int back[HISTORY_MAX], back_count, forward[HISTORY_MAX], forward_count;
static struct field search;
static bool visible_page[PAGE_COUNT];

/* --- helpers for the pages ---------------------------------------------------------------------- */

void settings_message(enum gui_dialog_icon icon, const char *title, const char *message) {
    gui_message(ui.g, win, icon, title, message);
}

bool settings_confirm(const char *title, const char *message, const char *yes) {
    return gui_confirm(ui.g, win, GUI_ICON_WARNING, title, message, yes, true);
}

long settings_launch(const char *path, const char *arg) {
    const char *name = strrchr(path, '/');
    const char *argv[] = { name ? name + 1 : path, arg, NULL };
    struct os_spawn sp = {
        .path = path,
        .argv = argv,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
        .flags = OS_SPAWN_DETACHED,
    };
    return spawn(&sp);
}

void human_size(uint64_t bytes, char *out, size_t size) {
    const char *units[] = { "bytes", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000 && u < 4) {
        v /= 1000;
        u++;
    }
    if (u == 0) {
        snprintf(out, size, "%lu bytes", (unsigned long)bytes);
    } else {
        snprintf(out, size, v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
    }
}

/* --- pages and history -------------------------------------------------------------------------- */

static void show_page(int index, bool remember) {
    if (index == current && remember) {
        return;
    }
    if (remember) {
        if (back_count == HISTORY_MAX) {
            memmove(back, back + 1, sizeof(back[0]) * (HISTORY_MAX - 1));
            back_count--;
        }
        back[back_count++] = current;
        forward_count = 0;
    }
    current = index;
    scroll[current] = 0;
    ui.focus = ui.focus == &search ? &search : NULL;
    ui.menu_for = NULL;
    if (pages[current].show) {
        pages[current].show();
    }
    gui_window_set_title(win, pages[current].name);
}

static void go_back(void) {
    if (back_count) {
        forward[forward_count++] = current;
        show_page(back[--back_count], false);
    }
}

static void go_forward(void) {
    if (forward_count) {
        back[back_count++] = current;
        show_page(forward[--forward_count], false);
    }
}

/* --- search ------------------------------------------------------------------------------------- */

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && lower(hay[i]) == lower(needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0;
}

static void filter(void) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        visible_page[i] = contains(pages[i].name, search.text) || contains(pages[i].keywords, search.text);
    }
}

/* --- drawing ------------------------------------------------------------------------------------ */

static void load_icons(void) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        char path[128];
        snprintf(path, sizeof(path), _PATH_DDE_RES "/Icons/Settings/%s.png", pages[i].icon);
        struct gfx_surface *img = image_load(path, NULL);
        if (img) {
            icons[i] = gfx_surface_scaled(img, SIDE_ICON, SIDE_ICON);
            gfx_surface_destroy(img);
        }
    }
}

/* The page's icon, or a coloured tile with its initial until the picture is installed. */
static void draw_icon(int index, int x, int y) {
    if (icons[index]) {
        gfx_blit(ui.s, x, y, icons[index], NULL);
        return;
    }
    gfx_fill_rounded_rect(ui.s, gfx_rect(x, y, SIDE_ICON, SIDE_ICON), 6, pages[index].tile);
    char letter[2] = { pages[index].name[0], 0 };
    float tw = ui_text_width(13, true, letter);
    ui_text(13, true, x + (SIDE_ICON - tw) / 2, y + 17, GFX_RGB(255, 255, 255), letter);
}

static void magnifier(int x, int y) {
    gfx_fill_circle(ui.s, x + 6, y + 6, 5.5f, INK_SOFT);
    gfx_fill_circle(ui.s, x + 6, y + 6, 4, GFX_RGB(255, 255, 255));
    gfx_draw_line(ui.s, x + 9.5f, y + 9.5f, x + 13.5f, y + 13.5f, 1.8f, INK_SOFT);
}

static void draw_sidebar(int h) {
    struct gfx_rect panel = gfx_rect(8, 8, SIDEBAR_W - 16, h - 16);
    gfx_fill_rounded_rect(ui.s, panel, 14, SIDEBAR_BG);
    ui.area = panel;

    struct gfx_rect box = gfx_rect(20, 22, SIDEBAR_W - 40, SEARCH_H);
    char before[sizeof(search.text)];
    strlcpy(before, search.text, sizeof(before));
    struct gfx_rect field_box = gfx_rect(box.x, box.y, box.w, box.h);
    bool enter = ui_field(field_box, &search, "      Search", true);
    if (!search.len) {
        magnifier(box.x + 10, box.y + 9);
    }
    if (strcmp(before, search.text) != 0) {
        filter();
    }
    if (enter) {
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (visible_page[i]) {
                show_page(i, true);
                ui.dirty = true;
                break;
            }
        }
    }

    int y = ROW_TOP, group = pages[0].group;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (!visible_page[i]) {
            continue;
        }
        if (pages[i].group != group) {
            y += GROUP_GAP;
            group = pages[i].group;
        }
        struct gfx_rect row = gfx_rect(16, y, SIDEBAR_W - 32, ROW_H);
        if (ui_clicked(row, &pages[i])) {
            show_page(i, true);
            ui.dirty = true;
        }
        if (i == current) {
            gfx_fill_rounded_rect(ui.s, row, 8, SIDEBAR_SEL);
        } else if (ui_hover(row)) {
            gfx_fill_rounded_rect(ui.s, row, 8, GFX_RGBA(0, 0, 0, 10));
        }
        draw_icon(i, row.x + 8, row.y + (ROW_H - SIDE_ICON) / 2);
        ui_text_clipped(FONT_BODY, false, row.x + 42, row.y + ROW_H / 2 + 5, row.w - 50, INK, pages[i].name);
        y += ROW_H + 2;
    }
    if (search.len) {
        bool any = false;
        for (int i = 0; i < PAGE_COUNT; i++) {
            any |= visible_page[i];
        }
        if (!any) {
            ui_text(FONT_SMALL, false, 26, ROW_TOP + 20, INK_SOFT, "No results");
        }
    }
    ui.area = gfx_rect(0, 0, 0, 0);
    gfx_reset_clip(ui.s);
}

static void chevron(float cx, float cy, bool left, gfx_color color) {
    float d = left ? 1 : -1;
    gfx_draw_line(ui.s, cx + 3 * d, cy - 6, cx - 3 * d, cy, 2, color);
    gfx_draw_line(ui.s, cx - 3 * d, cy, cx + 3 * d, cy + 6, 2, color);
}

static void draw_header(int w) {
    struct gfx_rect nav = gfx_rect(SIDEBAR_W + 12, 13, 80, 34);
    gfx_fill_rounded_rect(ui.s, gfx_rect(nav.x, nav.y + 1, nav.w, nav.h), 17, GFX_RGBA(0, 0, 0, 14));
    gfx_fill_rounded_rect(ui.s, nav, 17, GFX_RGB(255, 255, 255));
    gfx_stroke_rounded_rect(ui.s, nav, 17, 1, CARD_LINE);
    struct gfx_rect b = gfx_rect(nav.x, nav.y, nav.w / 2, nav.h), f = gfx_rect(nav.x + nav.w / 2, nav.y, nav.w / 2, nav.h);
    ui.area = gfx_rect(SIDEBAR_W, 0, w - SIDEBAR_W, HEADER_H);
    if (back_count && ui_clicked(b, &back)) {
        go_back();
        ui.dirty = true;
    }
    if (forward_count && ui_clicked(f, &forward)) {
        go_forward();
        ui.dirty = true;
    }
    gfx_fill_rect(ui.s, gfx_rect(nav.x + nav.w / 2, nav.y + 8, 1, nav.h - 16), CARD_LINE);
    chevron(b.x + b.w / 2.0f + 1, b.y + b.h / 2.0f, true, back_count ? INK : INK_FAINT);
    chevron(f.x + f.w / 2.0f - 1, f.y + f.h / 2.0f, false, forward_count ? INK : INK_FAINT);
    ui_text(FONT_TITLE, true, nav.x + nav.w + 16, 37, INK, pages[current].name);
    ui.area = gfx_rect(0, 0, 0, 0);
}

static struct gfx_rect page_area(void) {
    struct gfx_surface *s = gui_window_surface(win);
    return gfx_rect(SIDEBAR_W, HEADER_H, s->width - SIDEBAR_W, s->height - HEADER_H);
}

static void clamp_scroll(void) {
    int max = content_h - page_area().h;
    max = max < 0 ? 0 : max;
    scroll[current] = scroll[current] > max ? max : scroll[current] < 0 ? 0 : scroll[current];
}

static void draw_scrollbar(void) {
    struct gfx_rect a = page_area();
    if (content_h <= a.h) {
        return;
    }
    int track = a.h - 16;
    int thumb = track * a.h / content_h;
    thumb = thumb < 30 ? 30 : thumb;
    int y = a.y + 8 + (track - thumb) * scroll[current] / (content_h - a.h);
    gfx_fill_rounded_rect(ui.s, gfx_rect(a.x + a.w - 9, y, 5, thumb), 2.5f, GFX_RGBA(0, 0, 0, 60));
}

/*
 * Drawing happens off screen (the desktop shows the window's own buffer whenever it likes: drawing
 * straight into it showed half-drawn frames). Then only the rows that changed are copied over, and
 * only that part is redrawn on the screen: a mouse move that changes nothing costs nothing.
 */
static struct gfx_surface *offscreen;

static struct gfx_surface *back_buffer(void) {
    struct gfx_surface *front = gui_window_surface(win);
    if (!offscreen || offscreen->width != front->width || offscreen->height != front->height) {
        gfx_surface_destroy(offscreen);
        offscreen = gfx_surface_create(front->width, front->height);
    }
    return offscreen;
}

static void present(void) {
    struct gfx_surface *front = gui_window_surface(win);
    int w = front->width, top = -1, bottom = -1, left = w, right = -1;
    for (int y = 0; y < front->height; y++) {
        uint32_t *b = gfx_row(offscreen, y), *f = gfx_row(front, y);
        if (memcmp(b, f, (size_t)w * 4) == 0) {
            continue;
        }
        int x0 = 0, x1 = w - 1;
        while (b[x0] == f[x0]) {
            x0++;
        }
        while (b[x1] == f[x1]) {
            x1--;
        }
        left = x0 < left ? x0 : left;
        right = x1 > right ? x1 : right;
        top = top < 0 ? y : top;
        bottom = y;
        memcpy(f + x0, b + x0, (size_t)(x1 - x0 + 1) * 4);
    }
    if (top >= 0) {
        struct gfx_rect changed = gfx_rect(left, top, right - left + 1, bottom - top + 1);
        gui_window_invalidate(win, &changed);
    }
}

static void redraw(void) {
    if (!back_buffer()) {
        return;
    }
    for (int pass = 0; pass < 3; pass++) {
        ui.s = offscreen;
        gfx_reset_clip(ui.s);
        ui.hand = false;
        ui.dirty = false;
        if (ui.press) {
            ui.focus_claimed = false;
        }
        gfx_clear(ui.s, BG);
        draw_sidebar(ui.s->height);
        draw_header(ui.s->width);
        int h = 0;
        ui_run_page(pages[current].draw, page_area(), scroll[current], &h);
        content_h = h;
        draw_scrollbar();
        if (ui.press && !ui.focus_claimed) {
            ui.focus = NULL;   /* a click anywhere but a text box */
        }
        /* The input has been seen; another pass only shows the new state. */
        ui.press = false;
        ui.release = false;
        ui.key = NULL;
        ui.menu_choice = -1;
        if (!ui.dirty) {
            break;
        }
        clamp_scroll();
    }
    gui_window_set_cursor(win, ui.hand ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
    present();
}

/* --- events ------------------------------------------------------------------------------------- */

static void key(const struct gui_event *ev) {
    if (ui_wants_key()) {
        ui.key = ev;
        return;
    }
    int step = ev->key == OS_KEY_DOWN ? 1 : ev->key == OS_KEY_UP ? -1 : 0;
    if (step) {
        for (int i = current + step; i >= 0 && i < PAGE_COUNT; i += step) {
            if (visible_page[i]) {
                show_page(i, true);
                break;
            }
        }
    } else if (ev->key == OS_KEY_LEFT && (ev->modifiers & OS_MOD_ALT)) {
        go_back();
    } else if (ev->key == OS_KEY_RIGHT && (ev->modifiers & OS_MOD_ALT)) {
        go_forward();
    } else if (ev->ch >= ' ' && ev->ch < 0x7F) {
        /* Typing anywhere starts a search. */
        ui.focus = &search;
        ui.key = ev;
    }
}

int main(int argc, char **argv) {
    ui.g = gui_open();
    if (!ui.g) {
        return 1;
    }
    ui.font = gui_font(ui.g, GUI_FONT_REGULAR);
    ui.bold = gui_font(ui.g, GUI_FONT_BOLD);
    ui.menu_choice = -1;
    win = gui_window_create(ui.g, "Settings", -1, -1, WIN_W, WIN_H);
    if (!win) {
        return 1;
    }
    load_icons();
    filter();
    current = 0;
    if (argc > 1) {
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (contains(pages[i].name, argv[1]) || contains(pages[i].icon, argv[1])) {
                current = i;
                break;
            }
        }
    }
    show_page(current, false);
    redraw();

    uint64_t last_tick = uptime_ms();
    bool working = true;
    for (;;) {
        int tick = pages[current].tick_ms;
        int timeout = -1;
        if (tick) {
            uint64_t since = uptime_ms() - last_tick;
            timeout = since >= (uint64_t)tick ? 0 : (int)(tick - since);
        }
        if (pages[current].work && working) {
            timeout = 0;   /* events first, then the next piece of work */
        }
        struct gui_event ev;
        gui_wait_event(ui.g, &ev, timeout);   /* (false: the timeout ran out, ev.type is GUI_EVENT_NONE) */
        switch (ev.type) {
        case GUI_EVENT_NONE:
            if (pages[current].work && working) {
                working = pages[current].work();
                break;
            }
            if (pages[current].show && tick >= 2000) {
                pages[current].show();   /* fresh data now and then (network, drives) */
            }
            last_tick = uptime_ms();
            break;
        case GUI_EVENT_CLOSE:
        case GUI_EVENT_QUIT:
            gui_close(ui.g);
            return 0;
        case GUI_EVENT_MOUSE_MOVE:
        case GUI_EVENT_MOUSE_DOWN:
        case GUI_EVENT_MOUSE_UP:
            ui.mx = ev.x;
            ui.my = ev.y;
            ui.screen_dx = ev.screen_x - ev.x;
            ui.screen_dy = ev.screen_y - ev.y;
            ui.held = ev.buttons & OS_MOUSE_LEFT;
            if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.button == OS_MOUSE_LEFT) {
                ui.press = true;
            } else if (ev.type == GUI_EVENT_MOUSE_UP && ev.button == OS_MOUSE_LEFT) {
                ui.release = true;
            }
            break;
        case GUI_EVENT_SCROLL:
            if (gfx_rect_contains(page_area(), ev.x, ev.y)) {
                scroll[current] += ev.wheel * SCROLL_STEP;
                clamp_scroll();
            }
            break;
        case GUI_EVENT_KEY:
            key(&ev);
            break;
        case GUI_EVENT_MENU:
            ui.menu_choice = ev.menu_id;
            break;
        default:
            break;
        }
        if (ev.type != GUI_EVENT_NONE) {
            working = true;   /* (a page change may have brought new work) */
        }
        bool released = ui.release;
        redraw();
        if (released) {
            ui.active = NULL;
        }
    }
    gui_close(ui.g);
    return 0;
}
