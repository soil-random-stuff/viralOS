/*
 * The connection to the desktop: saying hello, fonts, and turning the
 * desktop's messages into events for the program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include <paths.h>

#define HELLO_TIMEOUT_MS 5000

static const char *const font_paths[GUI_FONT_COUNT] = {
    _PATH_DDE_RES "/Fonts/DejaVuSans.ttf",
    _PATH_DDE_RES "/Fonts/DejaVuSans-Bold.ttf",
    _PATH_DDE_RES "/Fonts/DejaVuSansMono.ttf",
};

bool gui_send(struct gui *g, const void *msg, size_t len, int handle) {
    if (g->disconnected) {
        return false;
    }
    if (msg_send(g->fd, msg, len, handle) < 0) {
        g->disconnected = true;
        return false;
    }
    return true;
}

/* --- opening and closing ------------------------------------------------------------------- */

struct gui *gui_open(void) {
    struct gui *g = calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    g->next_id = 1;
    g->fd = service_connect(GUI_SERVICE);
    if (g->fd < 0) {
        dprintf(STDERR_FILENO, "gui: the desktop isn't running (%s)\n", strerror(g->fd));
        free(g);
        return NULL;
    }
    struct gui_msg_hello hello = { { GUI_MSG_HELLO, 0 }, getpid() };
    gui_send(g, &hello, sizeof(hello), -1);

    struct os_pollfd pfd = { g->fd, OS_POLL_IN, 0 };
    struct gui_msg_welcome welcome;
    if (poll(&pfd, 1, HELLO_TIMEOUT_MS) <= 0 ||
        msg_recv(g->fd, &welcome, sizeof(welcome), NULL) != (long)sizeof(welcome) ||
        welcome.header.type != GUI_MSG_WELCOME) {
        dprintf(STDERR_FILENO, "gui: the desktop didn't answer\n");
        close(g->fd);
        free(g);
        return NULL;
    }
    g->screen_width = welcome.screen_width;
    g->screen_height = welcome.screen_height;
    return g;
}

void gui_close(struct gui *g) {
    if (!g) {
        return;
    }
    while (g->windows) {
        gui_window_destroy(g->windows);
    }
    close(g->fd);
    for (int i = 0; i < GUI_FONT_COUNT; i++) {
        ttf_free(g->fonts[i]);
    }
    free(g);
}

int gui_screen_width(struct gui *g) {
    return g->screen_width;
}

int gui_screen_height(struct gui *g) {
    return g->screen_height;
}

int gui_fd(struct gui *g) {
    return g->fd;
}

/* Loaded the first time they're asked for: not every program needs all three. */
struct ttf_font *gui_font(struct gui *g, enum gui_font which) {
    if (which >= GUI_FONT_COUNT) {
        return NULL;
    }
    if (!g->fonts[which]) {
        g->fonts[which] = ttf_load(font_paths[which]);
        if (!g->fonts[which]) {
            dprintf(STDERR_FILENO, "gui: cannot load font %s\n", font_paths[which]);
        }
    }
    return g->fonts[which];
}

/* --- menus ----------------------------------------------------------------------------------- */

void gui_show_menu(struct gui *g, int x, int y, const struct gui_menu_item *items, int count,
                   struct gui_window *owner) {
    struct gui_msg_menu m;
    memset(&m, 0, sizeof(m));
    m.header = (struct gui_msg){ GUI_MSG_MENU, owner ? owner->id : 0 };
    m.x = x;
    m.y = y;
    if (count > GUI_MENU_ITEMS_MAX) {
        count = GUI_MENU_ITEMS_MAX;
    }
    m.count = count;
    for (int i = 0; i < count; i++) {
        if (items[i].label) {
            strlcpy(m.items[i].label, items[i].label, sizeof(m.items[i].label));
        }
        m.items[i].id = items[i].id;
        m.items[i].disabled = items[i].disabled;
        m.items[i].checked = items[i].checked;
    }
    gui_flush(g);   /* what the window shows should be up to date under the menu */
    gui_send(g, &m, sizeof(m), -1);
}

/* --- events ------------------------------------------------------------------------------------ */

void gui_push_event(struct gui *g, const struct gui_event *ev) {
    /* Consecutive mouse moves for the same window: only the latest matters. */
    if (ev->type == GUI_EVENT_MOUSE_MOVE && g->queue_len > 0) {
        struct gui_event *last = &g->queue[(g->queue_head + g->queue_len - 1) % EVENT_QUEUE];
        if (last->type == GUI_EVENT_MOUSE_MOVE && last->window == ev->window) {
            *last = *ev;
            return;
        }
    }
    if (g->queue_len == EVENT_QUEUE) {
        return;   /* the program isn't keeping up: drop it */
    }
    g->queue[(g->queue_head + g->queue_len++) % EVENT_QUEUE] = *ev;
}

static bool pop_event(struct gui *g, struct gui_event *ev) {
    while (g->queue_len > 0) {
        *ev = g->queue[g->queue_head];
        g->queue_head = (g->queue_head + 1) % EVENT_QUEUE;
        g->queue_len--;
        if (ev->type != GUI_EVENT_NONE) {   /* NONE: cancelled (window destroyed) */
            return true;
        }
    }
    return false;
}

static void handle_event(struct gui *g, const struct gui_msg_event *m) {
    if (m->type == GUI_EVENT_SCREENSHOT) {
        g->shot_result = m->x ? 1 : -1;   /* (gui_screenshot is waiting for it) */
        return;
    }
    struct gui_window *w = m->header.window ? gui_find_window(g, m->header.window) : NULL;
    if (m->header.window && !w) {
        return;   /* for a window we've destroyed meanwhile */
    }
    if (m->type == GUI_EVENT_RESIZE && w) {
        /* Swap in a buffer of the new size before the program redraws. */
        if (!gui_window_resize_buffer(w, m->width, m->height)) {
            return;
        }
    }
    struct gui_event ev = {
        .type = (enum gui_event_type)m->type,
        .window = w,
        .x = m->x,
        .y = m->y,
        .screen_x = m->screen_x,
        .screen_y = m->screen_y,
        .button = m->button,
        .buttons = m->buttons,
        .wheel = m->wheel,
        .key = m->key,
        .ch = m->ch,
        .modifiers = m->modifiers,
        .menu_id = m->menu_id,
        .count = m->count,
    };
    gui_push_event(g, &ev);
}

static void handle_state(struct gui *g, const struct gui_msg_state *m) {
    struct gui_window *w = gui_find_window(g, m->header.window);
    if (!w) {
        return;
    }
    w->visible = m->visible;
    if (w->focused != (bool)m->focused) {
        w->focused = m->focused;
        struct gui_event ev = { .type = GUI_EVENT_FOCUS, .window = w, .focused = w->focused };
        gui_push_event(g, &ev);
    }
}

/* Take in everything the desktop has sent so far. */
static void receive(struct gui *g) {
    union {
        struct gui_msg header;
        struct gui_msg_event event;
        struct gui_msg_state state;
        uint8_t raw[OS_MSG_MAX];
    } m;
    for (;;) {
        struct os_pollfd pfd = { g->fd, OS_POLL_IN, 0 };
        if (g->disconnected || poll(&pfd, 1, 0) <= 0) {
            return;
        }
        int handle;
        long n = msg_recv(g->fd, &m, sizeof(m), &handle);
        if (handle >= 0) {
            close(handle);   /* we never expect one */
        }
        if (n <= 0) {
            g->disconnected = true;
            struct gui_event ev = { .type = GUI_EVENT_QUIT };
            gui_push_event(g, &ev);
            return;
        }
        if (m.header.type == GUI_MSG_EVENT && n >= (long)sizeof(m.event)) {
            handle_event(g, &m.event);
        } else if (m.header.type == GUI_MSG_STATE && n >= (long)sizeof(m.state)) {
            handle_state(g, &m.state);
        }
    }
}

void gui_flush(struct gui *g) {
    for (struct gui_window *w = g->windows; w; w = w->next) {
        gui_window_flush(w);
    }
}

bool gui_wait_event(struct gui *g, struct gui_event *ev, int timeout_ms) {
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        if (pop_event(g, ev)) {
            return true;
        }
        gui_flush(g);
        if (g->disconnected) {
            *ev = (struct gui_event){ .type = GUI_EVENT_QUIT };
            return true;   /* again and again: the program should give up */
        }

        long wait = timeout_ms < 0 ? -1 : 0;
        if (timeout_ms > 0) {
            uint64_t now = uptime_ms();
            wait = now >= deadline ? 0 : (long)(deadline - now);
        }
        struct os_pollfd pfd = { g->fd, OS_POLL_IN, 0 };
        int ready = poll(&pfd, 1, wait);
        if (ready > 0) {
            receive(g);
            continue;
        }
        if (ready < 0 || timeout_ms >= 0) {
            *ev = (struct gui_event){ .type = GUI_EVENT_NONE };
            return false;
        }
    }
}

/* --- screenshots --------------------------------------------------------------------------------- */

struct gui_screenshot *gui_screenshot(struct gui *g) {
    int w = g->screen_width, h = g->screen_height;
    uint64_t size = GUI_SCREENSHOT_SIZE(w, h);
    int handle = shm_create(size);
    if (handle < 0) {
        return NULL;
    }
    uint64_t mapped = 0;
    uint8_t *shared = shm_map(handle, &mapped);
    struct gui_screenshot *s = shared ? calloc(1, sizeof(*s)) : NULL;
    if (!s) {
        if (shared) {
            shm_unmap(shared);
        }
        close(handle);
        return NULL;
    }
    gui_flush(g);
    g->shot_result = 0;
    struct gui_msg_screenshot m = { { GUI_MSG_SCREENSHOT, 0 }, w, h };
    bool sent = gui_send(g, &m, sizeof(m), handle);
    close(handle);   /* (the mapping keeps it alive; the desktop has its own) */

    /* Wait for the answer; other events meanwhile go to the queue as usual. */
    uint64_t deadline = uptime_ms() + 5000;
    while (sent && g->shot_result == 0 && !g->disconnected && uptime_ms() < deadline) {
        struct os_pollfd pfd = { g->fd, OS_POLL_IN, 0 };
        if (poll(&pfd, 1, 200) > 0) {
            receive(g);
        }
    }
    if (g->shot_result == 1) {
        s->image = gfx_surface_create(w, h);
    }
    if (s->image) {
        for (int y = 0; y < h; y++) {
            memcpy(gfx_row(s->image, y), shared + (size_t)y * (size_t)w * 4, (size_t)w * 4);
        }
        const struct gui_shot_windows *list = (const void *)(shared + (uint64_t)w * h * 4);
        int n = list->count < 0 ? 0 : list->count > GUI_SHOT_WINDOWS_MAX ? GUI_SHOT_WINDOWS_MAX : list->count;
        n = n > (int)(sizeof(s->windows) / sizeof(s->windows[0])) ? (int)(sizeof(s->windows) / sizeof(s->windows[0])) : n;
        for (int i = 0; i < n; i++) {
            const struct gui_shot_window *sw = &list->windows[i];
            s->windows[i].frame = gfx_rect(sw->x, sw->y, sw->width, sw->height);
            strlcpy(s->windows[i].title, sw->title, sizeof(s->windows[i].title));
        }
        s->window_count = n;
    }
    shm_unmap(shared);
    if (!s->image) {
        free(s);
        return NULL;
    }
    return s;
}

void gui_screenshot_free(struct gui_screenshot *s) {
    if (s) {
        gfx_surface_destroy(s->image);
        free(s);
    }
}
