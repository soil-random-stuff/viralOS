/*
 * The desktop's start-up and main loop: wait for input, programs
 * connecting or sending requests, or the clock; react; redraw what changed.
 */

#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define MAX_CLIENTS (OS_POLL_MAX - 2)   /* the poll set also has the listener and input */
#define LOG(...)    dprintf(STDERR_FILENO, "desktop: " __VA_ARGS__)

static struct desktop desk;

/* --- sending ----------------------------------------------------------------------------- */

/* Never waits: a program that stops reading loses events rather than freezing the screen. */
void client_send(struct client *c, const void *msg, size_t len) {
    msg_send_flags(c->fd, msg, len, -1, OS_MSG_NOWAIT);
}

void send_event(struct window *w, const struct gui_msg_event *ev) {
    client_send(w->client, ev, sizeof(*ev));
}

void send_state(struct desktop *d, struct window *w) {
    struct gui_msg_state m = {
        .header = { GUI_MSG_STATE, w->id },
        .visible = w->visible,
        .focused = w == d->focused,
        .maximized = w->maximized,
    };
    client_send(w->client, &m, sizeof(m));
}

/* --- programs ------------------------------------------------------------------------------ */

static int count_clients(struct desktop *d) {
    int n = 0;
    for (struct client *c = d->clients; c; c = c->next) {
        n++;
    }
    return n;
}

static void accept_client(struct desktop *d) {
    int fd = service_accept(d->listener);
    if (fd < 0) {
        return;
    }
    struct client *c = count_clients(d) < MAX_CLIENTS ? calloc(1, sizeof(*c)) : NULL;
    if (!c) {
        close(fd);   /* full: the program sees the connection close */
        return;
    }
    c->fd = fd;
    c->app = -1;
    strlcpy(c->name, "?", sizeof(c->name));
    /* At the end, so the panel lists programs in the order they came. */
    struct client **link = &d->clients;
    while (*link) {
        link = &(*link)->next;
    }
    *link = c;
}

static void drop_client(struct desktop *d, struct client *c) {
    for (struct window *w = d->windows, *next; w; w = next) {
        next = w->next;
        if (w->client == c) {
            window_destroy(d, w);
        }
    }
    if (d->menu.open && d->menu.kind == MENU_CLIENT && d->menu.client == c) {
        menu_close(d);
    }
    for (struct client **link = &d->clients; *link; link = &(*link)->next) {
        if (*link == c) {
            *link = c->next;
            break;
        }
    }
    close(c->fd);
    free(c);
    panel_update(d);
}

static struct window *find_window(struct desktop *d, struct client *c, uint32_t id) {
    for (struct window *w = d->windows; w; w = w->next) {
        if (w->client == c && w->id == id) {
            return w;
        }
    }
    return NULL;
}

/* Map a buffer sent with a message; NULL unless it's big enough for width x height. */
static uint32_t *map_buffer(int handle, int width, int height) {
    if (handle < 0) {
        return NULL;
    }
    uint64_t size = 0;
    void *p = shm_map(handle, &size);
    close(handle);   /* the mapping keeps it alive */
    if (p && size < (uint64_t)width * (uint64_t)height * sizeof(uint32_t)) {
        shm_unmap(p);
        p = NULL;
    }
    return p;
}

static void process_name(int64_t pid, char *out, size_t size) {
    struct os_proc_info info;
    for (uint64_t i = 0; procinfo(i, &info) == 0; i++) {
        if (info.pid == pid) {
            strlcpy(out, info.name, size);
            return;
        }
    }
}

static void handle_message(struct desktop *d, struct client *c, const void *data, long len, int handle) {
    const struct gui_msg *h = data;
    struct window *w = h->type != GUI_MSG_HELLO && h->type != GUI_MSG_CREATE && h->window
                           ? find_window(d, c, h->window) : NULL;
#define NEED(type) if (len < (long)sizeof(type)) break
    switch (h->type) {
    case GUI_MSG_HELLO: {
        NEED(struct gui_msg_hello);
        const struct gui_msg_hello *m = data;
        c->pid = m->pid;
        process_name(c->pid, c->name, sizeof(c->name));
        c->app = app_for_pid(d, c->pid, c->name);
        struct gui_msg_welcome reply = { { GUI_MSG_WELCOME, 0 }, d->screen.width, d->screen.height };
        client_send(c, &reply, sizeof(reply));
        break;
    }
    case GUI_MSG_CREATE: {
        NEED(struct gui_msg_create);
        const struct gui_msg_create *m = data;
        if (find_window(d, c, h->window)) {
            break;   /* that number is taken */
        }
        uint32_t *buffer = map_buffer(handle, m->width, m->height);
        handle = -1;
        if (!buffer) {
            break;
        }
        char title[sizeof(m->title)];
        strlcpy(title, m->title, sizeof(title));
        bool dialog = m->flags & GUI_WINDOW_DIALOG;
        struct window *owner = dialog && m->owner ? find_window(d, c, m->owner) : NULL;
        w = window_create(d, c, h->window, title, m->x, m->y, m->width, m->height, owner, dialog);
        if (!w) {
            shm_unmap(buffer);
            break;
        }
        window_set_buffer(d, w, buffer, m->width, m->height);
        launcher_close(d);   /* the app the user asked for is here */
        break;
    }
    case GUI_MSG_BUFFER: {
        NEED(struct gui_msg_buffer);
        const struct gui_msg_buffer *m = data;
        if (!w) {
            break;
        }
        uint32_t *buffer = map_buffer(handle, m->width, m->height);
        handle = -1;
        if (buffer) {
            window_set_buffer(d, w, buffer, m->width, m->height);
        }
        break;
    }
    case GUI_MSG_DAMAGE: {
        NEED(struct gui_msg_damage);
        const struct gui_msg_damage *m = data;
        if (w && w->visible) {
            struct gfx_rect r = gfx_rect_intersect(gfx_rect(m->x, m->y, m->width, m->height),
                                                   gfx_rect(0, 0, content_width(w), content_height(w)));
            r.x += w->frame.x;
            r.y += w->frame.y + title_height(w);
            damage(d, r);
        }
        break;
    }
    case GUI_MSG_TITLE: {
        NEED(struct gui_msg_title);
        const struct gui_msg_title *m = data;
        if (w) {
            char title[sizeof(m->title)];
            strlcpy(title, m->title, sizeof(title));
            window_set_title(d, w, title);
        }
        break;
    }
    case GUI_MSG_SHOW: {
        NEED(struct gui_msg_show);
        const struct gui_msg_show *m = data;
        if (w && m->visible) {
            window_show(d, w);
        } else if (w) {
            window_minimize(d, w);
        }
        break;
    }
    case GUI_MSG_RAISE:
        if (w && w->visible) {
            window_raise(d, w);
            focus(d, w);
        }
        break;
    case GUI_MSG_DESTROY:
        if (w) {
            window_destroy(d, w);
        }
        break;
    case GUI_MSG_CURSOR: {
        NEED(struct gui_msg_cursor);
        const struct gui_msg_cursor *m = data;
        if (w && m->cursor < GUI_CURSOR_COUNT) {
            w->cursor = (int)m->cursor;
            update_cursor(d);
        }
        break;
    }
    case GUI_MSG_SCREENSHOT: {
        NEED(struct gui_msg_screenshot);
        const struct gui_msg_screenshot *m = data;
        struct gui_msg_event reply = { .header = { GUI_MSG_EVENT, 0 }, .type = GUI_EVENT_SCREENSHOT };
        uint64_t size = 0;
        uint8_t *shared = handle >= 0 ? shm_map(handle, &size) : NULL;
        if (handle >= 0) {
            close(handle);
            handle = -1;
        }
        if (shared && m->width == d->screen.width && m->height == d->screen.height &&
            size >= GUI_SCREENSHOT_SIZE(m->width, m->height)) {
            struct gui_shot_windows *list = (void *)(shared + (uint64_t)m->width * m->height * 4);
            reply.x = render_screenshot(d, (uint32_t *)shared, c, list);
        }
        if (shared) {
            shm_unmap(shared);
        }
        client_send(c, &reply, sizeof(reply));
        break;
    }
    case GUI_MSG_FULLSCREEN: {
        NEED(struct gui_msg_fullscreen);
        const struct gui_msg_fullscreen *m = data;
        if (w) {
            window_set_fullscreen(d, w, m->on != 0);
            if (m->on && w->visible) {
                window_raise(d, w);
                focus(d, w);
            }
        }
        break;
    }
    case GUI_MSG_SETTINGS: {
        NEED(struct gui_msg_settings);
        dde_settings_changed(d, data);
        break;
    }
    case GUI_MSG_DRAG: {
        NEED(struct gui_msg_drag);
        const struct gui_msg_drag *m = data;
        drag_from_client(d, c, w, m->count);
        break;
    }
    case GUI_MSG_MENU: {
        NEED(struct gui_msg_menu);
        const struct gui_msg_menu *m = data;
        if (h->window && !w) {
            break;
        }
        struct gui_menu_item items[MENU_MAX_ITEMS];
        char labels[MENU_MAX_ITEMS][sizeof(m->items[0].label)];
        int count = m->count < 0 ? 0 : m->count > MENU_MAX_ITEMS ? MENU_MAX_ITEMS : m->count;
        for (int i = 0; i < count; i++) {
            strlcpy(labels[i], m->items[i].label, sizeof(labels[i]));
            items[i] = (struct gui_menu_item){ labels[i][0] ? labels[i] : NULL, m->items[i].id,
                                               m->items[i].disabled, m->items[i].checked };
        }
        launcher_close(d);
        menu_show(d, m->x, m->y, items, count, MENU_CLIENT);
        d->menu.client = c;
        d->menu.window_id = h->window;
        break;
    }
    }
#undef NEED
    if (handle >= 0) {
        close(handle);   /* came with a message that doesn't take one */
    }
}

/* Everything this program has sent. Returns false if it has gone away. */
static bool serve_client(struct desktop *d, struct client *c) {
    static uint8_t buf[OS_MSG_MAX];
    for (int i = 0; i < 64; i++) {   /* then give the others a turn */
        struct os_pollfd pfd = { c->fd, OS_POLL_IN, 0 };
        if (poll(&pfd, 1, 0) <= 0) {
            return true;
        }
        int handle;
        long n = msg_recv(c->fd, buf, sizeof(buf), &handle);
        if (n <= 0) {
            if (handle >= 0) {
                close(handle);
            }
            return false;
        }
        if (n < (long)sizeof(struct gui_msg)) {
            if (handle >= 0) {
                close(handle);
            }
            continue;
        }
        handle_message(d, c, buf, n, handle);
    }
    return true;
}

/* --- start and main loop --------------------------------------------------------------------- */

static bool open_display(struct desktop *d) {
    d->regular = ttf_load(_PATH_DDE_RES "/Fonts/DejaVuSans.ttf");
    d->bold = ttf_load(_PATH_DDE_RES "/Fonts/DejaVuSans-Bold.ttf");
    if (!d->regular || !d->bold) {
        LOG("cannot load the fonts in /usr/share/dde-res/Fonts\n");
    }
    int err = display_claim(&d->display);
    if (err < 0) {
        LOG("cannot take over the screen: %s\n", strerror(err));
        return false;
    }
    int w = (int)d->display.width, h = (int)d->display.height;
    d->screen = (struct gfx_surface){
        .pixels = malloc((size_t)w * h * sizeof(uint32_t)),
        .width = w,
        .height = h,
        .stride = w,
        .clip = { 0, 0, w, h },
        .owned = true,
    };
    if (!d->screen.pixels || !load_cursors(d)) {
        LOG("out of memory\n");
        return false;
    }
    /* Something to look at while the wallpaper loads (compose draws a plain colour). */
    d->mouse_x = w / 2;
    d->mouse_y = h / 2;
    damage(d, gfx_rect(0, 0, w, h));
    render(d);

    dde_load_settings(d);
    d->wallpaper = make_wallpaper(d);
    if (!d->wallpaper) {
        LOG("out of memory\n");
        return false;
    }
    init_corners(d);
    d->mouse_x = w / 2;
    d->mouse_y = h / 2;
    return true;
}

int main(void) {
    struct desktop *d = &desk;
    d->listener = service_register(GUI_SERVICE);
    if (d->listener < 0) {
        LOG("cannot register the \"%s\" service: %s\n", GUI_SERVICE,
            d->listener == -EEXIST ? "is a desktop running already?" : strerror(d->listener));
        return 1;
    }
    if (!open_display(d)) {
        display_release();
        return 1;
    }
    ensure_home_folders();
    apps_load(d);
    if (!panel_init(d)) {
        LOG("out of memory\n");
        display_release();
        return 1;
    }
    icons_init(d);
    damage(d, gfx_rect(0, 0, d->screen.width, d->screen.height));
    render(d);
    sounds_init(d);

    while (!d->quit) {
        render(d);

        struct os_pollfd fds[OS_POLL_MAX];
        struct client *who[OS_POLL_MAX];
        int n = 0;
        fds[n++] = (struct os_pollfd){ OS_FD_INPUT, OS_POLL_IN, 0 };
        fds[n++] = (struct os_pollfd){ d->listener, OS_POLL_IN, 0 };
        for (struct client *c = d->clients; c && n < OS_POLL_MAX; c = c->next) {
            who[n] = c;
            fds[n++] = (struct os_pollfd){ c->fd, OS_POLL_IN, 0 };
        }
        int timeout = panel_next_timeout(d);
        timeout = lock_timeout(d) < timeout ? lock_timeout(d) : timeout;
        int shut = shutdown_timeout(d);
        timeout = shut >= 0 && (timeout < 0 || shut < timeout) ? shut : timeout;
        poll(fds, n, timeout);

        if (fds[0].revents & OS_POLL_IN) {
            struct os_input_event in[32];
            int count = input_read(in, 32, 0);
            for (int i = 0; i < count; i++) {
                handle_input(d, &in[i]);
            }
        }
        if (fds[1].revents & OS_POLL_IN) {
            accept_client(d);
        }
        for (int i = 2; i < n; i++) {
            if (fds[i].revents && !serve_client(d, who[i])) {
                drop_client(d, who[i]);
            }
        }
        apps_reap(d);
        panel_tick(d);
        icons_tick(d);
        sounds_tick(d);
        lock_tick(d);
        shutdown_tick(d);
    }

    /* Leaving: programs see the connection close and exit on their own. */
    while (d->clients) {
        drop_client(d, d->clients);
    }
    close(d->listener);
    display_release();
    return d->exit_code;
}
