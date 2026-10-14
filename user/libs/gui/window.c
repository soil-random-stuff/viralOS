/* Windows on the program side: a shared buffer for the content, and requests to the desktop. */

#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define WHITE GFX_RGB(255, 255, 255)

/* A shared buffer of width x height pixels: its handle (to send) and our mapping. */
static int make_buffer(int width, int height, void **mapping) {
    uint64_t bytes = (uint64_t)width * (uint64_t)height * sizeof(uint32_t);
    int fd = shm_create(bytes ? bytes : 1);
    if (fd < 0) {
        return fd;
    }
    *mapping = shm_map(fd, NULL);
    if (!*mapping) {
        close(fd);
        return -ENOMEM;
    }
    return fd;
}

static struct gfx_surface surface_over(void *pixels, int width, int height) {
    return (struct gfx_surface){
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = width,
        .clip = { 0, 0, width, height },
        .owned = false,
    };
}

struct gui_window *gui_find_window(struct gui *g, uint32_t id) {
    for (struct gui_window *w = g->windows; w; w = w->next) {
        if (w->id == id) {
            return w;
        }
    }
    return NULL;
}

static struct gui_window *create(struct gui *g, const char *title, int x, int y, int width, int height,
                                 struct gui_window *owner, uint32_t flags) {
    if (width < 1 || height < 1) {
        return NULL;
    }
    struct gui_window *w = calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    int handle = make_buffer(width, height, &w->buffer);
    if (handle < 0) {
        free(w);
        return NULL;
    }
    w->gui = g;
    w->id = g->next_id++;
    w->content = surface_over(w->buffer, width, height);
    w->visible = true;
    gfx_clear(&w->content, WHITE);

    struct gui_msg_create m = { { GUI_MSG_CREATE, w->id }, x, y, width, height, "", owner ? owner->id : 0, flags };
    strlcpy(m.title, title, sizeof(m.title));
    bool sent = gui_send(g, &m, sizeof(m), handle);
    close(handle);   /* the message (and then the desktop) holds it now */
    if (!sent) {
        shm_unmap(w->buffer);
        free(w);
        return NULL;
    }
    w->next = g->windows;
    g->windows = w;
    return w;
}

struct gui_window *gui_window_create(struct gui *g, const char *title, int x, int y,
                                     int width, int height) {
    return create(g, title, x, y, width, height, NULL, 0);
}

struct gui_window *gui_dialog_window_create(struct gui *g, struct gui_window *owner, const char *title,
                                            int width, int height) {
    return create(g, title, -1, -1, width, height, owner, GUI_WINDOW_DIALOG);
}

void gui_window_destroy(struct gui_window *w) {
    if (!w) {
        return;
    }
    struct gui *g = w->gui;
    struct gui_msg m = { GUI_MSG_DESTROY, w->id };
    gui_send(g, &m, sizeof(m), -1);
    for (struct gui_window **link = &g->windows; *link; link = &(*link)->next) {
        if (*link == w) {
            *link = w->next;
            break;
        }
    }
    /* Events already queued for it would point at freed memory. */
    for (int i = 0; i < g->queue_len; i++) {
        struct gui_event *e = &g->queue[(g->queue_head + i) % EVENT_QUEUE];
        if (e->window == w) {
            e->type = GUI_EVENT_NONE;
        }
    }
    shm_unmap(w->buffer);
    free(w);
}

/* After a RESIZE event: a new buffer, keeping what fits of the old picture. */
bool gui_window_resize_buffer(struct gui_window *w, int width, int height) {
    if (width < 1 || height < 1) {
        return false;
    }
    if (width == w->content.width && height == w->content.height) {
        return true;
    }
    void *mapping;
    int handle = make_buffer(width, height, &mapping);
    if (handle < 0) {
        return false;   /* out of memory: keep the old one; the desktop pads it */
    }
    struct gfx_surface s = surface_over(mapping, width, height);
    gfx_clear(&s, WHITE);
    gfx_copy(&s, 0, 0, &w->content, NULL);

    struct gui_msg_buffer m = { { GUI_MSG_BUFFER, w->id }, width, height };
    gui_send(w->gui, &m, sizeof(m), handle);
    close(handle);
    shm_unmap(w->buffer);
    w->buffer = mapping;
    w->content = s;
    w->damage = (struct gfx_rect){ 0, 0, 0, 0 };   /* the desktop redraws it all anyway */
    return true;
}

/* --- small API --------------------------------------------------------------------------- */

struct gfx_surface *gui_window_surface(struct gui_window *w) {
    return &w->content;
}

void gui_window_invalidate(struct gui_window *w, const struct gfx_rect *area) {
    struct gfx_rect all = gfx_rect(0, 0, w->content.width, w->content.height);
    struct gfx_rect r = area ? gfx_rect_intersect(*area, all) : all;
    if (gfx_rect_empty(r)) {
        return;
    }
    w->damage = gfx_rect_empty(w->damage) ? r : gfx_rect_union(w->damage, r);
}

void gui_window_flush(struct gui_window *w) {
    if (gfx_rect_empty(w->damage)) {
        return;
    }
    struct gfx_rect r = w->damage;
    struct gui_msg_damage m = { { GUI_MSG_DAMAGE, w->id }, r.x, r.y, r.w, r.h };
    gui_send(w->gui, &m, sizeof(m), -1);
    w->damage = (struct gfx_rect){ 0, 0, 0, 0 };
}

void gui_window_set_title(struct gui_window *w, const char *title) {
    struct gui_msg_title m = { { GUI_MSG_TITLE, w->id }, "" };
    strlcpy(m.title, title, sizeof(m.title));
    gui_send(w->gui, &m, sizeof(m), -1);
}

void gui_window_set_cursor(struct gui_window *w, enum gui_cursor cursor) {
    if ((int)cursor == w->cursor || cursor >= GUI_CURSOR_COUNT) {
        return;   /* only tell the desktop about changes */
    }
    w->cursor = cursor;
    struct gui_msg_cursor m = { { GUI_MSG_CURSOR, w->id }, cursor };
    gui_send(w->gui, &m, sizeof(m), -1);
}

void gui_window_set_fullscreen(struct gui_window *w, bool on) {
    struct gui_msg_fullscreen m = { { GUI_MSG_FULLSCREEN, w->id }, on };
    gui_send(w->gui, &m, sizeof(m), -1);
}

void gui_settings_changed(struct gui *g, int icon_size, int auto_arrange) {
    struct gui_msg_settings m = { { GUI_MSG_SETTINGS, 0 }, icon_size, auto_arrange };
    gui_send(g, &m, sizeof(m), -1);
    gui_flush(g);
}

void gui_window_set_user_data(struct gui_window *w, void *data) {
    w->user_data = data;
}

void *gui_window_user_data(struct gui_window *w) {
    return w->user_data;
}

bool gui_window_visible(struct gui_window *w) {
    return w->visible;
}

bool gui_window_focused(struct gui_window *w) {
    return w->focused;
}

void gui_window_show(struct gui_window *w, bool visible) {
    gui_window_flush(w);
    w->visible = visible;
    struct gui_msg_show m = { { GUI_MSG_SHOW, w->id }, visible };
    gui_send(w->gui, &m, sizeof(m), -1);
}

void gui_window_raise(struct gui_window *w) {
    struct gui_msg m = { GUI_MSG_RAISE, w->id };
    gui_send(w->gui, &m, sizeof(m), -1);
}

/* --- dragging files -------------------------------------------------------------------- */

bool gui_start_drag(struct gui_window *w, const char *const *paths, int count) {
    int fd = open(GUI_DRAG_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || count <= 0) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    for (int i = 0; i < count; i++) {
        write(fd, paths[i], strlen(paths[i]));
        write(fd, "\n", 1);
    }
    close(fd);
    gui_window_flush(w);
    struct gui_msg_drag m = { { GUI_MSG_DRAG, w->id }, count };
    return gui_send(w->gui, &m, sizeof(m), -1);
}

int gui_drop_paths(char (*paths)[OS_PATH_MAX], int max) {
    int fd = open(GUI_DRAG_FILE, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    static char buf[64 * 1024];
    long len = 0, n;
    while (len < (long)sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += n;
    }
    close(fd);
    buf[len > 0 ? len : 0] = '\0';
    int count = 0;
    for (char *line = buf, *next; line && *line && count < max; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0]) {
            strlcpy(paths[count++], line, OS_PATH_MAX);
        }
    }
    return count;
}
