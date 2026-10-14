#pragma once

/*
 * divine-settings: Settings for viralOS and DDE, laid out like macOS's System Settings: a sidebar of
 * categories (with a search box) on the left, the chosen page on the right, its settings grouped in
 * rounded cards.
 *
 *   main.c           the window, the sidebar, the header (back / forward), scrolling, the event loop
 *   ui.c             the controls: an immediate-mode toolkit (every event redraws the page, and the
 *                    page's code both draws its controls and learns what was clicked)
 *   page_network.c   Network
 *   page_system.c    General (about, computer name), Displays, Date & Time, Storage
 *   page_look.c      Wallpaper, Desktop & Panel
 *   page_input.c     Sound, Mouse
 *
 * Settings take effect at once: the desktop's in ~/.config/dde/settings.conf (then gui_settings_changed tells
 * it), the network's through libnetconf, the volume through audio_mixer().
 */

#include <stdbool.h>
#include <stdint.h>

#include <gfx.h>
#include <gui.h>
#include <ttf.h>
#include <unistd.h>

#define WIN_W       900
#define WIN_H       640
#define SIDEBAR_W   240
#define HEADER_H    60
#define CONTENT_MAX 700        /* the page column's widest */

#define BG          GFX_RGB(0xFF, 0xFF, 0xFF)
#define SIDEBAR_BG  GFX_RGB(0xF2, 0xF2, 0xF5)
#define SIDEBAR_SEL GFX_RGB(0xDC, 0xDC, 0xE1)
#define CARD        GFX_RGB(0xF5, 0xF5, 0xF7)
#define CARD_LINE   GFX_RGB(0xE3, 0xE3, 0xE8)
#define CONTROL     GFX_RGB(0xE6, 0xE6, 0xEB)
#define CONTROL_HOT GFX_RGB(0xDA, 0xDA, 0xE0)
#define INK         GFX_RGB(0x1D, 0x1D, 0x1F)
#define INK_SOFT    GFX_RGB(0x86, 0x86, 0x8B)
#define INK_FAINT   GFX_RGB(0xB0, 0xB0, 0xB6)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define DANGER      GFX_RGB(0xE0, 0x3A, 0x3A)

#define FONT_BODY   14.0f
#define FONT_SMALL  12.5f
#define FONT_TITLE  18.0f
#define FONT_HEAD   15.0f

/* A one-line text box. */
struct field {
    char text[128];
    int len, cursor;
    float scroll;
    bool secret;                 /* a password: shown as stars */
};

void field_set(struct field *f, const char *text);

/* The toolkit's state: what this pass of drawing knows about input, and what lasts between passes. */
struct ui {
    struct gfx_surface *s;
    struct ttf_font *font, *bold;
    struct gui *g;
    struct gui_window *win;

    /* This pass's input. */
    int mx, my;
    bool press, release;          /* the left button went down / up in this event */
    bool held;
    const struct gui_event *key;  /* a key for the focused text box (consumed by it) */
    int menu_choice;              /* a menu item was chosen (GUI_EVENT_MENU): its id, else -1 */
    bool measuring;               /* the layout pass: nothing drawn, no input */

    /* Lasting state. */
    const void *active;           /* the control the button went down on (a slider being dragged...) */
    const void *focus;            /* the text box with the keyboard */
    const void *menu_for;         /* the control whose pop-up menu is open */
    bool focus_claimed;           /* (a text box took the click) */

    /* Layout: the page column, and where the next thing goes (screen coordinates, scrolled). */
    struct gfx_rect area;         /* the page's visible area */
    int left, width;
    int y;
    int screen_dx, screen_dy;     /* window coordinates + these = the screen's (for menus) */
    bool hand;                    /* the pointer is over something clickable */
    bool dirty;                   /* something changed: draw again after this pass */

    /* Cards: their heights from the layout pass. */
    int card, card_top, card_rows;
    bool in_card;
    int card_heights[32];
};

extern struct ui ui;

/* --- ui.c: layout and controls (all in the page column) ------------------------------------------ */

void ui_space(int h);
void ui_heading(const char *text);                       /* a bold title above a card */
void ui_note(const char *text);                          /* grey text under a card, wrapped */
void ui_card_begin(void);
void ui_card_end(void);
/* A row of the current card: its rectangle (separated from the previous row by a line). */
struct gfx_rect ui_row(int height);

bool ui_hover(struct gfx_rect r);
/* Clicked: pressed and released inside r (true once, on the release). */
bool ui_clicked(struct gfx_rect r, const void *key);

/* Rows with a label (and a grey line under it, if `sub`) on the left and a control on the right.
   Each returns true when the user changed the value. */
bool ui_toggle_row(const char *label, const char *sub, bool *value);
bool ui_toggle_row_enabled(const char *label, const char *sub, bool *value, bool enabled);   /* greyed when not */
bool ui_slider_row(const char *label, int *value, int min, int max, const char *low, const char *high);
bool ui_choice_row(const char *label, const char *const *options, int count, int *selected);
bool ui_segmented_row(const char *label, const char *const *options, int count, int *selected);
void ui_info_row(const char *label, const char *value);
/* A text box; true when Enter is pressed in it. */
bool ui_field_row(const char *label, struct field *f, const char *placeholder, bool enabled);
/* A row with a button on the right; true when it's clicked. */
bool ui_button_row(const char *label, const char *sub, const char *button, bool primary);
bool ui_button_row_enabled(const char *label, const char *sub, const char *button, bool primary, bool enabled);

/* Drawing pieces, for pages' own rows. */
void ui_text(float size, bool bold, float x, float y, gfx_color color, const char *text);
float ui_text_width(float size, bool bold, const char *text);
void ui_text_clipped(float size, bool bold, float x, float y, float max_w, gfx_color color, const char *text);
bool ui_button(struct gfx_rect r, const char *label, bool primary, bool enabled);
bool ui_field(struct gfx_rect box, struct field *f, const char *placeholder, bool enabled);   /* true on Enter */
bool ui_toggle(struct gfx_rect r, bool *value);
bool ui_toggle_enabled(struct gfx_rect r, bool *value, bool enabled);
void ui_bar(struct gfx_rect r, float fraction, gfx_color fill);   /* a progress / usage bar */

/* Run a pass of drawing the given page into the area (layout first, then the real one). */
void ui_run_page(void (*page)(void), struct gfx_rect area, int scroll, int *content_height);
/* Keys for the focused text box (main.c hands every key here first). */
bool ui_wants_key(void);

/* --- the pages ------------------------------------------------------------------------------------ */

struct page {
    const char *name;            /* in the sidebar and the header */
    const char *icon;            /* /usr/share/dde-res/Icons/Settings/<icon>.png */
    const char *keywords;        /* what else finds it in the search */
    gfx_color tile;              /* the stand-in icon's colour, until the picture exists */
    int group;                   /* sidebar groups, separated by a gap */
    void (*draw)(void);
    void (*show)(void);          /* about to be shown: read fresh state (may be NULL) */
    int tick_ms;                 /* redraw this often while shown (0: only on events) */
    bool (*work)(void);          /* slow work done a piece at a time between events (true: more to do) */
};

void page_network(void);
void page_network_show(void);
void page_general(void);
void page_general_show(void);
void page_displays(void);
void page_datetime(void);
void page_storage(void);
void page_storage_show(void);
void page_wallpaper(void);
void page_wallpaper_show(void);
bool page_wallpaper_work(void);
void page_desktop(void);
void page_desktop_show(void);
void page_sound(void);
void page_sound_show(void);
void page_mouse(void);
void page_mouse_show(void);
void page_users(void);
void page_users_show(void);
bool page_users_work(void);

/* main.c: helpers for pages */
void settings_message(enum gui_dialog_icon icon, const char *title, const char *message);
bool settings_confirm(const char *title, const char *message, const char *yes);   /* a red main button */
long settings_launch(const char *path, const char *arg);   /* another program */
void human_size(uint64_t bytes, char *out, size_t size);
