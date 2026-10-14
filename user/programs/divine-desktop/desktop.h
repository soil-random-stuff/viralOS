#pragma once

/*
 * DDE, the Divine Desktop Environment: the desktop owns the screen, puts the programs' windows on it (with
 * title bars, shadows and rounded corners), and draws the panel at the
 * bottom with the apps menu, the running apps and the clock.
 *
 * Programs connect through the "desktop" service (see <gui_protocol.h>);
 * each window's content is a buffer shared with its program.
 *
 *   main.c       start-up, the main loop, talking to programs
 *   wm.c         windows: title bars, stacking, focus, moving, maximizing
 *   input.c      what the mouse and keyboard do
 *   render.c     composing the screen from all of it, damaged parts only
 *   panel.c      the panel, the apps menu and tooltips
 *   icons.c      the icons on the desktop (~/Desktop): selecting, opening, their menus
 *   dialog.c     the desktop's own small dialogs (rename, delete)
 *   apps.c       the apps (launcher files in /usr/share/applications), icons, starting them
 *   menu.c       context menus
 *   volume.c     the volume control in the tray
 *   network.c    the network in the tray (Windows 10 style flyout)
 *   lock.c       locking (the lock screen) and logging out
 *   shutdown.c   shutting down and restarting: apps closed, drives unmounted first
 *   cursor.c, wallpaper.c
 */

#include <fileops.h>
#include <gfx.h>
#include <gui.h>
#include <gui_protocol.h>
#include <stdio.h>
#include <ttf.h>
#include <unistd.h>

/* Looks */
#define TITLE_HEIGHT    34
#define CORNER_RADIUS   15
#define SHADOW_BLUR     22
#define SHADOW_OFFSET   7      /* shadows sit a little lower: light from above */
#define BUTTON_RADIUS   6.5f
#define BUTTON_SPACING  20
#define BUTTON_RIGHT    20     /* centre of the rightmost button, from the right edge */
#define TITLE_FONT_SIZE 13.5f
#define MENU_FONT_SIZE  13.5f
#define ACCENT          GFX_RGB(0x2F, 0x6F, 0xEB)
#define CONTENT_BLANK   GFX_RGB(255, 255, 255)   /* content the program hasn't drawn yet */

#define PANEL_MARGIN    15     /* around the floating panel */
#define PANEL_HEIGHT    54
#define PANEL_RADIUS    15
#define PANEL_SHADOW    18

#define MAX_DAMAGE      24
#define MENU_MAX_ITEMS  GUI_MENU_ITEMS_MAX
#define MAX_APPS        32
#define MAX_LAUNCHED    32
#define PANEL_MAX       24
#define NET_MAX         8

struct lockscreen;

/* The power menu (the apps menu's power button). */
enum { POWER_MENU_OFF = 1, POWER_MENU_RESTART, POWER_MENU_EXIT, POWER_MENU_LOCK, POWER_MENU_LOGOUT };
#define EXIT_TO_SHELL 3   /* exit code: the login screen ends too, and the kernel's text shell takes over */

/* Title bar buttons, left to right. */
enum title_button { BUTTON_NONE = -1, BUTTON_MINIMIZE, BUTTON_MAXIMIZE, BUTTON_CLOSE, BUTTON_COUNT };

/* An app from its launcher file in /usr/share/applications. */
struct app {
    char name[40];
    char desc[128];
    char exec[160];               /* command line */
    char path[96];                /* the program: Exec's first word */
    char launcher[160];           /* its file in /usr/share/applications */
    bool pinned;                  /* always on the panel */
    struct gfx_surface *icon;     /* panel size */
    struct gfx_surface *icon_large;   /* apps menu size */
};

/* A connected program. */
struct client {
    int fd;
    int64_t pid;
    int app;                      /* index into apps, or -1 */
    char name[32];                /* its process name */
    struct client *next;
};

struct window {
    struct client *client;
    uint32_t id;                  /* the client's number for it */
    char title[96];
    struct gfx_rect frame;        /* on screen: title bar + content */
    struct gfx_rect saved_frame;  /* where it was before maximizing */
    struct gfx_rect full_saved;   /* where it was before going full screen */
    struct gfx_surface *title_bar;   /* frame.w x TITLE_HEIGHT */
    struct gfx_alpha *shadow;     /* for the current size */
    uint32_t *buffer;             /* the content, shared with the client */
    int buffer_w, buffer_h;
    bool visible, maximized;
    bool fullscreen;              /* the whole screen: no title bar, corners or panel */
    int hover_button, pressed_button;
    int cursor;                   /* GUI_CURSOR_* the program wants over its content */
    int sent_w, sent_h;           /* content size the program was last told about */
    bool dialog;                  /* GUI_WINDOW_DIALOG: only a close button, fixed size */
    struct window *owner;         /* a dialog's window: it takes no input while the dialog is open */
    struct window *next;          /* stacking order, bottom first */
};

/* Height of the window's title bar (none in full screen): where its content starts. */
static inline int title_height(const struct window *w) {
    return w->fullscreen ? 0 : TITLE_HEIGHT;
}

enum menu_kind {
    MENU_CLIENT,                  /* a program's menu: the choice goes back to it */
    MENU_WINDOW,                  /* a title bar's menu */
    MENU_DESKTOP,                 /* right click on the wallpaper */
    MENU_PANEL,                   /* right click on an app on the panel */
    MENU_POWER,                   /* the apps menu's power button */
    MENU_ICON,                    /* right click on a desktop icon */
};

struct menu {
    bool open;
    enum menu_kind kind;
    struct client *client;        /* MENU_CLIENT */
    uint32_t window_id;           /* MENU_CLIENT: the window it belongs to (0: none) */
    struct window *window;        /* MENU_WINDOW */
    int data;                     /* MENU_PANEL: which panel entry */
    struct gui_menu_item items[MENU_MAX_ITEMS];
    char labels[MENU_MAX_ITEMS][64];
    int count;
    int hover;
    struct gfx_rect rect;
    struct gfx_surface *surface;
    struct gfx_alpha *shadow;
};

/* Something on the panel: an app (pinned or running), or a program not in the list. */
struct panel_entry {
    int app;                      /* -1: `client` instead */
    struct client *client;
    struct gfx_rect slot;         /* on screen */
};

/* A file on the desktop. */
#define MAX_ICONS 128

struct desk_icon {
    struct os_file_info info;
    char label[OS_NAME_MAX];      /* the name (an app launcher's Name) */
    struct gfx_surface *image;    /* not owned: from the icon caches */
    struct gfx_rect rect;         /* its cell on screen */
    int col, row;                 /* its cell in the grid */
    bool selected, cut;
};

/* Files being dragged with the mouse: desktop icons, or from a program's window. */
struct drag {
    bool active;
    bool from_icons;              /* the selected desktop icons (else GUI_DRAG_FILE's paths) */
    int count;
    char (*paths)[FILE_PATH_MAX];
    char source_dir[FILE_PATH_MAX];
    int grab_x, grab_y;           /* from_icons: pointer offset inside the grabbed icon's cell */
    int grabbed;                  /* from_icons: that icon */
    struct gfx_surface *ghost;    /* from a program: the picture under the pointer */
    struct gfx_rect drawn;        /* where the ghost was last drawn */
    int target;                   /* folder icon it would drop into, or -1 */
};

enum desk_dialog { DESK_DIALOG_NONE, DESK_DIALOG_RENAME, DESK_DIALOG_DELETE, DESK_DIALOG_ERROR,
                   DESK_DIALOG_SHUTDOWN /* "... anyway?" while shutting down (shutdown.c) */ };

/* Shutting down (shutdown.c). */
enum shutdown_stage {
    SHUTDOWN_NONE,
    SHUTDOWN_CLOSING,             /* the apps were asked to close: waiting for them */
    SHUTDOWN_ASK_APPS,            /* some didn't: "shut down anyway?" */
    SHUTDOWN_ASK_DRIVES,          /* a drive couldn't be unmounted: "shut down anyway?" */
};

/* What the mouse is over on the panel. */
enum { PANEL_HOVER_NONE = -1, PANEL_HOVER_MENU = -2, PANEL_HOVER_CLOCK = -3, PANEL_HOVER_VOLUME = -4,
       PANEL_HOVER_NETWORK = -5 };

struct desktop {
    struct os_display_info display;
    struct gfx_surface screen;    /* the off-screen picture */
    struct gfx_surface *wallpaper;
    struct ttf_font *regular, *bold;
    int listener;

    struct client *clients;
    struct window *windows;       /* bottom first */
    struct window *focused;
    struct window *dragging;      /* being moved by its title bar */
    int drag_dx, drag_dy;
    struct window *captured;      /* got a mouse-down in its content: gets moves until release */

    int mouse_x, mouse_y;
    uint32_t buttons;
    struct gfx_surface *cursor;   /* the one showing (one of cursors[]) */
    int cursor_hot_x, cursor_hot_y;
    struct gfx_surface *cursors[GUI_CURSOR_COUNT];
    int cursor_hot[GUI_CURSOR_COUNT][2];
    int cursor_kind;

    /* A window being resized by an edge or corner. */
    struct window *resizing;
    int resize_edges;             /* EDGE_* */
    struct gfx_rect resize_from;  /* its frame when it started */
    int resize_x, resize_y;       /* where the mouse was then */
    uint64_t resize_sent_ms;      /* when the program was last told its new size */

    struct menu menu;
    uint8_t corner[CORNER_RADIUS][CORNER_RADIUS];   /* rounded-corner coverage */

    struct gfx_rect damage[MAX_DAMAGE];
    int damage_count;

    struct app apps[MAX_APPS];
    int app_count;
    struct { int64_t pid; int app; } launched[MAX_LAUNCHED];

    /* The panel. */
    struct gfx_rect panel_rect;
    bool panel_hidden;            /* a full-screen window is on top */
    struct gfx_surface *panel;
    struct gfx_alpha *panel_shadow;
    struct gfx_surface *menu_icon;
    struct panel_entry entries[PANEL_MAX];
    int entry_count;
    struct gfx_rect menu_slot, clock_rect;
    int panel_hover, panel_pressed;
    int clock_minute;             /* the minute shown, to know when to redraw */

    /* The apps menu. */
    bool launcher_open;
    struct gfx_rect launcher_rect;
    struct gfx_surface *launcher;
    struct gfx_alpha *launcher_shadow;
    int launcher_hover;           /* app index, LAUNCHER_EXIT, or -1 */
    int launcher_scroll;          /* how far the apps are scrolled (when they don't all fit) */
    int launcher_view_h;          /* the apps' visible height */
    int launcher_content_h;       /* ...and all of theirs */
    bool launcher_bar_drag;       /* dragging the scroll bar's thumb */
    int launcher_bar_grab;        /* where on the thumb it was grabbed */

    /* The volume control in the tray (volume.c). */
    struct os_audio_mixer mixer;
    bool mixer_ok;                /* there's a sound card */
    struct gfx_surface *volume_tray_icons[4], *volume_card_icons[4];   /* mute, low, medium, high */
    struct gfx_rect volume_slot;  /* the tray icon, on screen */
    bool volume_open, volume_list_open, volume_dragging;
    struct gfx_rect volume_rect;
    struct gfx_surface *volume_card;
    struct gfx_alpha *volume_shadow;
    int volume_hover;

    /* The network in the tray (network.c). */
    struct os_netif_info net_ifs[NET_MAX];   /* the adapters (not the loopback) */
    int net_count;
    bool net_online;
    uint64_t net_checked_ms;
    struct gfx_surface *net_tray_icons[2], *net_card_icons[2];   /* no internet, ethernet */
    struct gfx_rect net_slot;
    bool net_open;
    struct gfx_rect net_rect;
    struct gfx_surface *net_card;
    struct gfx_alpha *net_shadow;
    int net_hover, net_selected;

    /* A tooltip over the panel. */
    int tooltip_for;              /* panel_hover value it belongs to (NONE: hidden) */
    uint64_t hover_since;
    struct gfx_rect tooltip_rect;
    struct gfx_surface *tooltip;

    /* Icons on the desktop. */
    struct desk_icon icons[MAX_ICONS];
    int icon_count;
    int icon_size;                /* 0 small, 1 medium, 2 large */
    bool auto_arrange;            /* keep icons packed in name order (else where they're put) */
    struct drag drag;
    struct gfx_surface *file_icons[FILE_ICON_COUNT];   /* at the current size */
    uint32_t icons_signature;     /* of the folder's contents, to notice changes */
    uint64_t icons_checked_ms;
    int icon_hover, icon_focus;
    bool icon_pressing, icon_marquee;
    int icon_press_x, icon_press_y;
    bool icon_marquee_base[MAX_ICONS];
    uint64_t icon_click_ms;
    int icon_click_index;

    /* The desktop's dialog (rename / delete / errors). */
    enum desk_dialog dialog;
    char dialog_title[64], dialog_message[320];
    char dialog_field[OS_NAME_MAX];
    int dialog_cursor;
    char dialog_target[FILE_PATH_MAX];
    int dialog_hover, dialog_pressed;

    /* ~/.config/dde/settings.conf (settings.c) */
    struct {
        char wallpaper[OS_PATH_MAX];
        bool clock24;
        bool natural_scroll;
        int double_click;         /* ms */
    } cfg;

    /* Locked (lock.c). */
    bool locked;
    struct lockscreen *lock;
    struct gfx_surface *lock_surface;

    bool quit;
    int exit_code;
    bool wallpaper_hidden;        /* shutting down: render() just shows the screen as it is */
    bool shooting;                /* composing a screenshot: no pointer, no tooltip */

    struct {
        enum shutdown_stage stage;
        bool restart;
        uint64_t deadline;        /* SHUTDOWN_CLOSING: when to stop waiting for the apps */
    } shutdown;
};

#define LAUNCHER_EXIT (-10)

/* main.c */
void client_send(struct client *c, const void *msg, size_t len);
void send_event(struct window *w, const struct gui_msg_event *ev);
void send_state(struct desktop *d, struct window *w);
struct gfx_rect work_area(const struct desktop *d);

/* render.c */
void damage(struct desktop *d, struct gfx_rect r);
void render(struct desktop *d);
void init_corners(struct desktop *d);
struct gfx_rect window_bounds(const struct window *w);   /* frame + shadow */
/* The whole screen into `out` (its size, no padding) without `exclude`'s windows or the pointer;
   the visible windows (but those) into `list`, the top one first. False if it can't be done. */
bool render_screenshot(struct desktop *d, uint32_t *out, const struct client *exclude,
                       struct gui_shot_windows *list);

/* sounds.c */
void sounds_init(struct desktop *d);           /* plays Startup */
void sounds_tick(struct desktop *d);           /* disks plugged in / taken out */
void sounds_volume_changed(struct desktop *d); /* VolumeChange, not too often */

/* wm.c */
struct window *window_create(struct desktop *d, struct client *c, uint32_t id, const char *title,
                             int x, int y, int width, int height, struct window *owner, bool dialog);
/* The open dialog that `w` is waiting for (the innermost one), or NULL. */
struct window *window_modal(const struct desktop *d, struct window *w);
void window_destroy(struct desktop *d, struct window *w);
void window_set_buffer(struct desktop *d, struct window *w, uint32_t *buffer, int width, int height);
void window_set_title(struct desktop *d, struct window *w, const char *title);
void draw_decorations(struct desktop *d, struct window *w);
struct window *window_at(struct desktop *d, int x, int y);
enum title_button title_button_at(const struct window *w, int x, int y);
void window_move(struct desktop *d, struct window *w, int x, int y);
void window_toggle_maximize(struct desktop *d, struct window *w);
void window_set_fullscreen(struct desktop *d, struct window *w, bool on);
bool fullscreen_on_top(const struct desktop *d);
void window_minimize(struct desktop *d, struct window *w);
void window_show(struct desktop *d, struct window *w);   /* un-minimize, raise, focus */
void window_raise(struct desktop *d, struct window *w);
void focus(struct desktop *d, struct window *w);
void focus_top(struct desktop *d);
void window_close_request(struct window *w);

/* Resizing by the edges: which edges are under a point (and of which window). */
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };
int resize_edges_at(struct desktop *d, int x, int y, struct window **w);
int resize_cursor(int edges);
void window_resize_begin(struct desktop *d, struct window *w, int edges);
void window_resize_update(struct desktop *d);
void window_resize_end(struct desktop *d);
int content_width(const struct window *w);
int content_height(const struct window *w);

/* input.c */
void handle_input(struct desktop *d, const struct os_input_event *in);

/* menu.c */
void menu_show(struct desktop *d, int x, int y, const struct gui_menu_item *items, int count,
               enum menu_kind kind);
void menu_close(struct desktop *d);
void menu_hover(struct desktop *d, int x, int y);
bool menu_contains(const struct desktop *d, int x, int y);
void menu_choose(struct desktop *d);
void menu_key(struct desktop *d, uint32_t key);
struct gfx_rect menu_bounds(const struct menu *m);
void show_window_menu(struct desktop *d, struct window *w, int x, int y);

/* panel.c */
bool panel_init(struct desktop *d);
void panel_update(struct desktop *d);        /* entries changed (apps started, windows...) */
void panel_tick(struct desktop *d);          /* the clock and tooltips */
int panel_next_timeout(struct desktop *d);   /* ms until panel_tick has something to do */
bool panel_contains(const struct desktop *d, int x, int y);
void panel_mouse_move(struct desktop *d);
void panel_mouse_down(struct desktop *d, uint32_t button);
void panel_mouse_up(struct desktop *d, uint32_t button);
void panel_menu_chosen(struct desktop *d, int entry, int id);
void panel_compose(struct desktop *d, struct gfx_rect area);
bool launcher_contains(const struct desktop *d, int x, int y);
void launcher_toggle(struct desktop *d);
void launcher_close(struct desktop *d);
void launcher_mouse_move(struct desktop *d);
void launcher_click(struct desktop *d);
/* When the apps don't all fit: the wheel scrolls them (true if it did), and the scroll bar's thumb
   can be dragged (mouse down on it; true from launcher_mouse_up if a drag ended). */
bool launcher_wheel(struct desktop *d, int wheel);
void launcher_mouse_down(struct desktop *d);
bool launcher_mouse_up(struct desktop *d);
void launcher_compose(struct desktop *d, struct gfx_rect area);
void panel_redraw(struct desktop *d);

/* volume.c */
void volume_init(struct desktop *d);
void volume_draw_tray(struct desktop *d, struct gfx_surface *panel, bool hovered, bool pressed);
void volume_tooltip(const struct desktop *d, char *text, size_t size);
void volume_toggle(struct desktop *d);
void volume_close(struct desktop *d);
bool volume_contains(const struct desktop *d, int x, int y);
void volume_mouse_down(struct desktop *d);
void volume_mouse_move(struct desktop *d);
void volume_mouse_up(struct desktop *d);
bool volume_dragging(const struct desktop *d);
bool volume_hover_clickable(const struct desktop *d);
bool volume_wheel(struct desktop *d, int wheel);
void volume_compose(struct desktop *d, struct gfx_rect area);
void power_menu_chosen(struct desktop *d, int id);

/* network.c */
void network_init(struct desktop *d);
void network_tick(struct desktop *d);        /* re-reads the adapters every couple of seconds */
void network_draw_tray(struct desktop *d, struct gfx_surface *panel, bool hovered, bool pressed);
void network_tooltip(const struct desktop *d, char *text, size_t size);
void network_toggle(struct desktop *d);
void network_close(struct desktop *d);
bool network_contains(const struct desktop *d, int x, int y);
void network_mouse_move(struct desktop *d);
void network_mouse_up(struct desktop *d);
bool network_hover_clickable(const struct desktop *d);
void network_compose(struct desktop *d, struct gfx_rect area);

/* apps.c */
void apps_load(struct desktop *d);
int app_launch(struct desktop *d, int app);   /* pid or -errno */
/* The app whose program is `path`, with one more argument (NULL: none). pid or -errno. */
int app_launch_path(struct desktop *d, const char *path, const char *arg);
int app_for_pid(struct desktop *d, int64_t pid, const char *name);
void apps_reap(struct desktop *d);
struct gfx_surface *app_icon(struct desktop *d, int app, const char *name, bool large);
void apps_reload_pinned(struct desktop *d);   /* Pinned= in DDE's settings changed (Settings) */

/* settings.c */
void dde_load_settings(struct desktop *d);
void dde_settings_changed(struct desktop *d, const struct gui_msg_settings *m);

/* icons.c */
void icons_init(struct desktop *d);
void icons_tick(struct desktop *d);          /* notices changes in the folder */
void icons_compose(struct desktop *d, struct gfx_rect area);
bool icons_mouse_down(struct desktop *d, uint32_t button, uint32_t modifiers);
void icons_mouse_move(struct desktop *d);
void icons_mouse_up(struct desktop *d, uint32_t button);
void icons_key(struct desktop *d, const struct os_input_event *in);
void icons_menu_chosen(struct desktop *d, int id);
void icons_reload(struct desktop *d);
void icons_set_size(struct desktop *d, int size);        /* 0 small, 1 medium, 2 large (saved) */
void icons_set_auto_arrange(struct desktop *d, bool on); /* (saved) */
void icons_renamed(struct desktop *d, const char *old_name, const char *new_name);
void drag_from_client(struct desktop *d, struct client *c, struct window *w, int count);
void drag_move(struct desktop *d);
void drag_drop(struct desktop *d);
void drag_cancel(struct desktop *d);
void drag_compose(struct desktop *d, struct gfx_rect area);

/* dialog.c */
void desk_dialog_open(struct desktop *d, enum desk_dialog kind, const char *title, const char *message,
                      const char *field, const char *target);
void desk_dialog_compose(struct desktop *d, struct gfx_rect area);
void desk_dialog_mouse(struct desktop *d, bool down, bool up);
void desk_dialog_key(struct desktop *d, const struct os_input_event *in);
struct gfx_rect desk_dialog_rect(const struct desktop *d);

/* shutdown.c */
void shutdown_begin(struct desktop *d, bool restart);
void shutdown_tick(struct desktop *d);
int shutdown_timeout(const struct desktop *d);   /* ms until shutdown_tick has something to do; -1: never */
void shutdown_answer(struct desktop *d, bool anyway);   /* the dialog's answer */

/* lock.c */
void lock_screen(struct desktop *d);
void lock_input(struct desktop *d, const struct os_input_event *in);
void lock_tick(struct desktop *d);
int lock_timeout(const struct desktop *d);
void lock_compose(struct desktop *d, struct gfx_rect area);
void log_out(struct desktop *d);

/* input.c */
void move_pointer(struct desktop *d, int x, int y);

/* cursor.c, wallpaper.c */
bool load_cursors(struct desktop *d);
void set_cursor(struct desktop *d, int kind);   /* GUI_CURSOR_* */
void update_cursor(struct desktop *d);          /* input.c: the right one for where the pointer is */
struct gfx_surface *make_wallpaper(struct desktop *d);
