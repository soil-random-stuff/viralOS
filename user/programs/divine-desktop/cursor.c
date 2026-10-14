/*
 * Mouse pointers: the pictures in /usr/share/dde-res/Cursors (with their hot spots
 * listed in hotspots.txt), and set_cursor to switch between them. If the
 * arrow's picture is missing, one is drawn from a polygon instead; other
 * missing ones fall back to the arrow.
 */

#include <image.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include <paths.h>

#define SIZE 32
#define PAD  4      /* room for outline and shadow left/above the tip */

/* The arrow, tip at (0, 0). */
static const float arrow[] = {
    0, 0,   0, 16.5f,   4.1f, 12.7f,   6.9f, 19.0f,   9.5f, 17.9f,   6.8f, 11.8f,   11.8f, 11.8f,
};
#define ARROW_POINTS 7

/* Coverage of the arrow shifted by (dx, dy), as a SIZE x SIZE mask. */
static void arrow_mask(uint8_t *out, float dx, float dy) {
    struct gfx_raster r;
    if (!gfx_raster_init(&r, SIZE, SIZE)) {
        memset(out, 0, SIZE * SIZE);
        return;
    }
    for (int i = 0; i < ARROW_POINTS; i++) {
        int j = (i + 1) % ARROW_POINTS;
        gfx_raster_line(&r, arrow[2 * i] + PAD + dx, arrow[2 * i + 1] + PAD + dy,
                        arrow[2 * j] + PAD + dx, arrow[2 * j + 1] + PAD + dy);
    }
    gfx_raster_coverage(&r, out);
    gfx_raster_free(&r);
}

static struct gfx_surface *make_cursor(int *hot_x, int *hot_y) {
    struct gfx_surface *s = gfx_surface_create(SIZE, SIZE);
    struct gfx_alpha *body = gfx_alpha_create(SIZE, SIZE);
    struct gfx_alpha *outline = gfx_alpha_create(SIZE, SIZE);
    struct gfx_alpha *shadow = gfx_alpha_create(SIZE, SIZE);
    uint8_t *tmp = malloc(SIZE * SIZE);
    if (!s || !body || !outline || !shadow || !tmp) {
        gfx_alpha_destroy(body);
        gfx_alpha_destroy(outline);
        gfx_alpha_destroy(shadow);
        free(tmp);
        return s;
    }

    arrow_mask(body->data, 0, 0);
    /* Outline: the arrow traced at 8 nearby positions, keeping the strongest
       coverage everywhere (a cheap, smooth "grow by 1.4 px"). */
    for (int i = 0; i < 8; i++) {
        double a = i * M_PI / 4;
        arrow_mask(tmp, 1.4f * (float)cos(a), 1.4f * (float)sin(a));
        for (int p = 0; p < SIZE * SIZE; p++) {
            outline->data[p] = tmp[p] > outline->data[p] ? tmp[p] : outline->data[p];
        }
    }
    /* Shadow: the outline, nudged down-right and blurred. */
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int sx = x - 1, sy = y - 2;
            shadow->data[y * SIZE + x] = sx >= 0 && sy >= 0 ? outline->data[sy * SIZE + sx] : 0;
        }
    }
    gfx_alpha_blur(shadow, 4);

    gfx_fill_mask(s, 0, 0, shadow, GFX_RGBA(0, 0, 0, 110));
    gfx_fill_mask(s, 0, 0, outline, GFX_RGB(255, 255, 255));
    gfx_fill_mask(s, 0, 0, body, GFX_RGB(0x1C, 0x1C, 0x22));

    gfx_alpha_destroy(body);
    gfx_alpha_destroy(outline);
    gfx_alpha_destroy(shadow);
    free(tmp);
    *hot_x = PAD;
    *hot_y = PAD;
    return s;
}

/* --- the cursor set ----------------------------------------------------------------------- */

#define CURSOR_DIR _PATH_DDE_RES "/Cursors/"

static const char *const files[GUI_CURSOR_COUNT] = {
    "arrow.png", "hand.png", "resize_ew.png", "resize_ns.png", "resize_nwse.png", "resize_nesw.png",
};

/* hotspots.txt: lines of "name.png x y". */
static void read_hot_spots(struct desktop *d) {
    int fd = open(CURSOR_DIR "hotspots.txt", O_RDONLY);
    if (fd < 0) {
        return;
    }
    char text[1024];
    long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *space = strchr(line, ' ');
        if (line[0] == '#' || !space) {
            continue;
        }
        *space = '\0';
        for (int i = 0; i < GUI_CURSOR_COUNT; i++) {
            if (strcmp(line, files[i]) == 0) {
                char *p = space + 1;
                d->cursor_hot[i][0] = (int)strtol(p, &p, 10);
                d->cursor_hot[i][1] = (int)strtol(p, &p, 10);
            }
        }
    }
}

bool load_cursors(struct desktop *d) {
    for (int i = 0; i < GUI_CURSOR_COUNT; i++) {
        char path[96];
        snprintf(path, sizeof(path), CURSOR_DIR "%s", files[i]);
        d->cursors[i] = image_load_png(path, NULL);
        /* Without a list, assume the middle (right for the resize arrows). */
        d->cursor_hot[i][0] = d->cursors[i] ? d->cursors[i]->width / 2 : 0;
        d->cursor_hot[i][1] = d->cursors[i] ? d->cursors[i]->height / 2 : 0;
    }
    read_hot_spots(d);
    if (!d->cursors[GUI_CURSOR_ARROW]) {
        d->cursors[GUI_CURSOR_ARROW] = make_cursor(&d->cursor_hot[0][0], &d->cursor_hot[0][1]);
    }
    for (int i = 1; i < GUI_CURSOR_COUNT; i++) {
        if (!d->cursors[i]) {
            d->cursors[i] = d->cursors[GUI_CURSOR_ARROW];
            d->cursor_hot[i][0] = d->cursor_hot[0][0];
            d->cursor_hot[i][1] = d->cursor_hot[0][1];
        }
    }
    d->cursor_kind = -1;
    set_cursor(d, GUI_CURSOR_ARROW);
    return d->cursor != NULL;
}

/* Switch the pointer's picture, redrawing where the old and new ones are. */
void set_cursor(struct desktop *d, int kind) {
    if (kind == d->cursor_kind || kind < 0 || kind >= GUI_CURSOR_COUNT) {
        return;
    }
    if (d->cursor) {
        damage(d, gfx_rect(d->mouse_x - d->cursor_hot_x, d->mouse_y - d->cursor_hot_y, d->cursor->width,
                           d->cursor->height));
    }
    d->cursor_kind = kind;
    d->cursor = d->cursors[kind];
    d->cursor_hot_x = d->cursor_hot[kind][0];
    d->cursor_hot_y = d->cursor_hot[kind][1];
    if (d->cursor) {
        damage(d, gfx_rect(d->mouse_x - d->cursor_hot_x, d->mouse_y - d->cursor_hot_y, d->cursor->width,
                           d->cursor->height));
    }
}
