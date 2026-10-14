/*
 * Terminal: a window that runs the shell.
 *
 * The shell (and whatever it starts) gets the programs' end of a
 * pseudo-terminal as its stdin, stdout and stderr; we hold the other end:
 * keys typed into the window are written to it, and what the programs
 * print is read from it and drawn as a grid of character cells.
 *
 * Understands the same escape sequences as the text console (colours,
 * clearing, cursor movement), keeps 1000 lines of scrollback (mouse
 * wheel), and grows with the window when it's maximized.
 */

#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define SHELL       _PATH_SHELL
#define FONT_SIZE   14.0f
#define PADDING     8
#define START_COLS  80
#define START_ROWS  25
#define MAX_COLS    300
#define SCROLLBACK  1000
#define WHEEL_LINES 3

/* Colours: the console's palette, then the default text and background. */
static const gfx_color palette[18] = {
    GFX_RGB(0x15, 0x16, 0x1E), GFX_RGB(0xE0, 0x6C, 0x75), GFX_RGB(0x98, 0xC3, 0x79),
    GFX_RGB(0xE5, 0xC0, 0x7B), GFX_RGB(0x61, 0xAF, 0xEF), GFX_RGB(0xC6, 0x78, 0xDD),
    GFX_RGB(0x56, 0xB6, 0xC2), GFX_RGB(0xD0, 0xD0, 0xD8), GFX_RGB(0x5C, 0x63, 0x70),
    GFX_RGB(0xFF, 0x7B, 0x86), GFX_RGB(0xB5, 0xE8, 0x90), GFX_RGB(0xFF, 0xD7, 0x87),
    GFX_RGB(0x7F, 0xD4, 0xFF), GFX_RGB(0xE0, 0xA0, 0xFF), GFX_RGB(0x7F, 0xE0, 0xE8),
    GFX_RGB(0xFF, 0xFF, 0xFF), GFX_RGB(0xDC, 0xDC, 0xE4), GFX_RGB(0x15, 0x16, 0x1E),
};
#define FG_DEFAULT 16
#define BG_DEFAULT 17
#define CURSOR_COLOR GFX_RGB(0xA8, 0xC8, 0xFF)

struct cell {
    uint32_t ch;        /* Unicode code point; 0 = blank */
    uint8_t fg, bg;     /* palette indexes */
};

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *font;
    float cell_w, cell_h, ascent;

    int cols, rows;
    struct cell (*screen)[MAX_COLS];     /* rows lines */
    bool *dirty;                         /* per screen row */
    struct cell (*history)[MAX_COLS];    /* scrollback ring */
    int history_start, history_len;
    int view;                            /* lines scrolled back; 0 = following the output */

    int cx, cy;                          /* cursor */
    uint8_t fg, bg;
    bool bright;
    int drawn_cx, drawn_cy;              /* where the cursor was drawn */

    /* Escape sequences and UTF-8 being decoded. */
    enum { TEXT, ESCAPE, CSI } state;
    int params[8], param_count;
    bool private_mode;                   /* "ESC [ ?": ignored */
    uint32_t utf8;
    int utf8_left;

    int pty;                             /* our end */
    long shell;
    bool focused;
} t;

/* --- the cell grid ---------------------------------------------------------------------- */

static struct cell blank(void) {
    return (struct cell){ 0, t.fg, t.bg };
}

static void mark_all(void) {
    for (int y = 0; y < t.rows; y++) {
        t.dirty[y] = true;
    }
}

static void clear_cells(int row, int from, int to) {
    for (int x = from; x < to && x < MAX_COLS; x++) {
        t.screen[row][x] = blank();
    }
    t.dirty[row] = true;
}

/* The top line goes into the scrollback, everything moves up one. */
static void scroll_up(void) {
    int slot = (t.history_start + t.history_len) % SCROLLBACK;
    if (t.history_len == SCROLLBACK) {
        t.history_start = (t.history_start + 1) % SCROLLBACK;
    } else {
        t.history_len++;
    }
    memcpy(t.history[slot], t.screen[0], sizeof(t.screen[0]));
    memmove(t.screen[0], t.screen[1], sizeof(t.screen[0]) * (size_t)(t.rows - 1));
    clear_cells(t.rows - 1, 0, MAX_COLS);
    if (t.view > 0 && t.view < t.history_len) {
        t.view++;   /* someone reading the scrollback keeps their place */
    }
    mark_all();
}

static void newline(void) {
    t.cx = 0;
    if (t.cy + 1 < t.rows) {
        t.cy++;
    } else {
        scroll_up();
    }
}

static void put(uint32_t ch) {
    if (t.cx >= t.cols) {
        newline();   /* the line was full: wrap */
    }
    t.screen[t.cy][t.cx++] = (struct cell){ ch, t.fg, t.bg };
    t.dirty[t.cy] = true;
}

/* --- escape sequences ------------------------------------------------------------------ */

static void sgr(int code) {
    if (code == 0) {
        t.fg = FG_DEFAULT;
        t.bg = BG_DEFAULT;
        t.bright = false;
    } else if (code == 1) {
        t.bright = true;
    } else if (code == 22) {
        t.bright = false;
    } else if (code >= 30 && code <= 37) {
        t.fg = (uint8_t)(code - 30 + (t.bright ? 8 : 0));
    } else if (code >= 90 && code <= 97) {
        t.fg = (uint8_t)(code - 90 + 8);
    } else if (code == 39) {
        t.fg = FG_DEFAULT;
    } else if (code >= 40 && code <= 47) {
        t.bg = (uint8_t)(code - 40);
    } else if (code >= 100 && code <= 107) {
        t.bg = (uint8_t)(code - 100 + 8);
    } else if (code == 49) {
        t.bg = BG_DEFAULT;
    }
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void run_csi(char final) {
    int p0 = t.param_count > 0 ? t.params[0] : 0;
    int n = p0 > 0 ? p0 : 1;
    t.dirty[clampi(t.cy, 0, t.rows - 1)] = true;
    switch (final) {
    case 'm':
        if (t.param_count == 0) {
            sgr(0);
        }
        for (int i = 0; i < t.param_count; i++) {
            sgr(t.params[i]);
        }
        break;
    case 'J':
        if (p0 == 2 || p0 == 3) {
            for (int y = 0; y < t.rows; y++) {
                clear_cells(y, 0, MAX_COLS);
            }
            t.cx = t.cy = 0;
        } else if (p0 == 0) {
            clear_cells(t.cy, t.cx, MAX_COLS);
            for (int y = t.cy + 1; y < t.rows; y++) {
                clear_cells(y, 0, MAX_COLS);
            }
        }
        break;
    case 'K':
        if (p0 == 0) {
            clear_cells(t.cy, t.cx, MAX_COLS);
        } else if (p0 == 1) {
            clear_cells(t.cy, 0, t.cx + 1);
        } else {
            clear_cells(t.cy, 0, MAX_COLS);
        }
        break;
    case 'H':
    case 'f': {
        int row = p0 > 0 ? p0 - 1 : 0;
        int col = t.param_count > 1 && t.params[1] > 0 ? t.params[1] - 1 : 0;
        t.cy = clampi(row, 0, t.rows - 1);
        t.cx = clampi(col, 0, t.cols - 1);
        break;
    }
    case 'A': t.cy = clampi(t.cy - n, 0, t.rows - 1); break;
    case 'B': t.cy = clampi(t.cy + n, 0, t.rows - 1); break;
    case 'C': t.cx = clampi(t.cx + n, 0, t.cols - 1); break;
    case 'D': t.cx = clampi(t.cx - n, 0, t.cols - 1); break;
    }
}

static void feed(uint8_t c) {
    switch (t.state) {
    case ESCAPE:
        if (c == '[') {
            t.state = CSI;
            t.param_count = 0;
            t.params[0] = 0;
            t.private_mode = false;
        } else {
            t.state = TEXT;   /* other escapes: not supported, dropped */
        }
        return;
    case CSI:
        if (c >= '0' && c <= '9') {
            if (t.param_count == 0) {
                t.param_count = 1;
            }
            int *p = &t.params[t.param_count - 1];
            *p = *p < 10000 ? *p * 10 + (c - '0') : *p;
        } else if (c == ';') {
            if (t.param_count == 0) {
                t.param_count = 1;
            }
            if (t.param_count < 8) {
                t.params[t.param_count++] = 0;
            }
        } else if (c == '?') {
            t.private_mode = true;
        } else {
            if (c >= '@' && c <= '~' && !t.private_mode) {
                run_csi((char)c);
            }
            t.state = TEXT;
        }
        return;
    case TEXT:
        break;
    }

    /* UTF-8: collect the bytes of one character. */
    if (t.utf8_left > 0) {
        if ((c & 0xC0) == 0x80) {
            t.utf8 = t.utf8 << 6 | (c & 0x3F);
            if (--t.utf8_left == 0) {
                put(t.utf8);
            }
            return;
        }
        t.utf8_left = 0;   /* broken sequence: start over with this byte */
    }
    if (c >= 0xC0 && c < 0xF8) {
        t.utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        t.utf8 = c & (0x3F >> t.utf8_left);
        return;
    }
    switch (c) {
    case 0x1b:
        t.state = ESCAPE;
        break;
    case '\n':
        newline();
        break;
    case '\r':
        t.cx = 0;
        break;
    case '\b':
        if (t.cx > 0) {
            t.cx--;
        }
        break;
    case '\t':
        do {
            put(' ');
        } while (t.cx % 8 != 0 && t.cx < t.cols);
        break;
    default:
        if (c >= ' ' && c != 0x7F && c < 0x80) {
            put(c);
        }
        break;
    }
}

/* --- drawing ------------------------------------------------------------------------------- */

/* The line shown at screen row y, taking scrollback into account. */
static const struct cell *shown_line(int y) {
    int from_history = t.view - y;   /* > 0: that many lines up into the history */
    if (from_history > 0) {
        int index = t.history_len - from_history;
        return t.history[(t.history_start + index) % SCROLLBACK];
    }
    return t.screen[y - t.view];
}

static void encode_utf8(uint32_t c, char *out) {
    if (c < 0x80) {
        out[0] = (char)c;
        out[1] = 0;
    } else if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        out[2] = 0;
    } else if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | c >> 18);
        out[1] = (char)(0x80 | (c >> 12 & 0x3F));
        out[2] = (char)(0x80 | (c >> 6 & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        out[4] = 0;
    }
}

static struct gfx_rect row_rect(int y) {
    struct gfx_surface *s = gui_window_surface(t.w);
    return gfx_rect(0, PADDING + (int)(y * t.cell_h), s->width, (int)t.cell_h + 1);
}

static void draw_row(int y) {
    struct gfx_surface *s = gui_window_surface(t.w);
    const struct cell *line = shown_line(y);
    struct gfx_rect r = row_rect(y);
    gfx_set_clip(s, r);
    gfx_fill_rect(s, r, palette[BG_DEFAULT]);
    float top = PADDING + y * t.cell_h;
    for (int x = 0; x < t.cols; x++) {
        float left = PADDING + x * t.cell_w;
        if (line[x].bg != BG_DEFAULT) {
            gfx_fill_rect(s, gfx_rect((int)left, (int)top, (int)(left + t.cell_w) - (int)left,
                                      (int)t.cell_h + 1), palette[line[x].bg]);
        }
    }
    bool cursor_here = t.view == 0 && y == t.cy && t.cx < t.cols + 1;
    if (cursor_here) {
        int cx = t.cx < t.cols ? t.cx : t.cols - 1;
        struct gfx_rect c = gfx_rect((int)(PADDING + cx * t.cell_w), (int)top,
                                     (int)t.cell_w, (int)t.cell_h);
        if (t.focused) {
            gfx_fill_rect(s, c, CURSOR_COLOR);
        } else {
            gfx_fill_rect(s, gfx_rect(c.x, c.y + c.h - 2, c.w, 2), CURSOR_COLOR);
        }
    }
    for (int x = 0; x < t.cols; x++) {
        uint32_t ch = line[x].ch;
        if (ch <= ' ') {
            continue;
        }
        char text[5];
        encode_utf8(ch, text);
        gfx_color ink = palette[line[x].fg];
        if (cursor_here && t.focused && x == t.cx) {
            ink = palette[BG_DEFAULT];   /* readable on the block cursor */
        }
        ttf_draw_text(s, t.font, FONT_SIZE, PADDING + x * t.cell_w, top + t.ascent, ink, text);
    }
    gfx_reset_clip(s);
    gui_window_invalidate(t.w, &r);
}

static void redraw(void) {
    if (t.view == 0 && (t.cx != t.drawn_cx || t.cy != t.drawn_cy)) {
        t.dirty[clampi(t.drawn_cy, 0, t.rows - 1)] = true;
        t.dirty[t.cy] = true;
    }
    for (int y = 0; y < t.rows; y++) {
        if (t.dirty[y]) {
            draw_row(y);
            t.dirty[y] = false;
        }
    }
    t.drawn_cx = t.cx;
    t.drawn_cy = t.cy;
}

/* The whole window, including the margins. */
static void redraw_all(void) {
    struct gfx_surface *s = gui_window_surface(t.w);
    gfx_clear(s, palette[BG_DEFAULT]);
    gui_window_invalidate(t.w, NULL);
    mark_all();
    redraw();
}

/* --- size --------------------------------------------------------------------------------- */

static bool alloc_grid(int cols, int rows) {
    struct cell (*screen)[MAX_COLS] = calloc((size_t)rows, sizeof(*screen));
    bool *dirty = calloc((size_t)rows, sizeof(bool));
    if (!screen || !dirty) {
        free(screen);
        free(dirty);
        return false;
    }
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < MAX_COLS; x++) {
            screen[y][x] = (struct cell){ 0, FG_DEFAULT, BG_DEFAULT };
        }
    }
    /* Keep the bottom of what was there (where the cursor is), like other terminals do. */
    if (t.screen) {
        int keep = t.cy + 1 < rows ? t.cy + 1 : rows;
        int first = t.cy + 1 - keep;
        for (int i = 0; i < first; i++) {
            scroll_up();   /* lines that no longer fit go to the scrollback */
        }
        memcpy(screen, t.screen, sizeof(*screen) * (size_t)keep);
        t.cy -= first;
        free(t.screen);
        free(t.dirty);
    }
    t.screen = screen;
    t.dirty = dirty;
    t.cols = cols;
    t.rows = rows;
    t.cx = clampi(t.cx, 0, cols);
    t.view = 0;
    return true;
}

static void fit_to_window(void) {
    struct gfx_surface *s = gui_window_surface(t.w);
    int cols = (int)((s->width - 2 * PADDING) / t.cell_w);
    int rows = (int)((s->height - 2 * PADDING) / t.cell_h);
    cols = clampi(cols, 10, MAX_COLS);
    rows = clampi(rows, 3, 500);
    if (cols != t.cols || rows != t.rows) {
        alloc_grid(cols, rows);
    }
    redraw_all();
}

/* --- input --------------------------------------------------------------------------------- */

static void send_keys(const char *s, size_t n) {
    write(t.pty, s, n);
}

static void on_key(const struct gui_event *ev) {
    const char *seq = NULL;
    switch (ev->key) {
    case OS_KEY_UP:     seq = "\x1b[A"; break;
    case OS_KEY_DOWN:   seq = "\x1b[B"; break;
    case OS_KEY_RIGHT:  seq = "\x1b[C"; break;
    case OS_KEY_LEFT:   seq = "\x1b[D"; break;
    case OS_KEY_HOME:   seq = "\x1b[H"; break;
    case OS_KEY_END:    seq = "\x1b[F"; break;
    case OS_KEY_DELETE: seq = "\x1b[3~"; break;
    }
    if (seq) {
        send_keys(seq, strlen(seq));
    } else if (ev->ch) {
        char c = (char)ev->ch;
        send_keys(&c, 1);
    } else {
        return;
    }
    if (t.view) {
        t.view = 0;   /* typing jumps back to the bottom */
        mark_all();
    }
}

static void on_scroll(int wheel) {
    int view = clampi(t.view + (wheel < 0 ? WHEEL_LINES : -WHEEL_LINES), 0, t.history_len);
    if (view != t.view) {
        t.view = view;
        mark_all();
    }
}

/* --- start and main loop --------------------------------------------------------------------- */

static long start_shell(void) {
    int32_t fds[2];
    int err = pty_open(fds);
    if (err < 0) {
        dprintf(STDERR_FILENO, "terminal: no pseudo-terminal: %s\n", strerror(err));
        return err;
    }
    const char *argv[] = { "shell", NULL };
    struct os_spawn args = { SHELL, argv, { fds[1], fds[1], fds[1] }, 0 };
    long pid = spawn(&args);
    close(fds[1]);   /* the shell has it now; when it (and its children) close it, we hear */
    if (pid < 0) {
        dprintf(STDERR_FILENO, "terminal: cannot start %s: %s\n", SHELL, strerror((int)pid));
        close(fds[0]);
        return pid;
    }
    t.pty = fds[0];
    return pid;
}

int main(void) {
    t.g = gui_open();
    if (!t.g) {
        return 1;
    }
    t.font = gui_font(t.g, GUI_FONT_MONO);
    if (!t.font) {
        return 1;
    }
    struct ttf_metrics m;
    ttf_get_metrics(t.font, FONT_SIZE, &m);
    t.cell_w = ttf_text_width(t.font, FONT_SIZE, "M");
    t.cell_h = (float)(int)(m.line_height + 0.99f);
    t.ascent = (float)(int)(m.ascent + 0.5f) + (t.cell_h - (m.ascent + m.descent)) / 2;
    t.fg = FG_DEFAULT;
    t.bg = BG_DEFAULT;
    t.history = calloc(SCROLLBACK, sizeof(*t.history));
    if (!t.history || !alloc_grid(START_COLS, START_ROWS)) {
        return 1;
    }

    int width = (int)(START_COLS * t.cell_w + 0.99f) + 2 * PADDING;
    int height = (int)(START_ROWS * t.cell_h) + 2 * PADDING;
    t.w = gui_window_create(t.g, "Terminal", -1, -1, width, height);
    if (!t.w) {
        return 1;
    }
    t.shell = start_shell();
    if (t.shell < 0) {
        return 1;
    }
    t.focused = true;
    redraw_all();

    static char buf[4096];
    for (;;) {
        gui_flush(t.g);
        struct os_pollfd fds[2] = {
            { gui_fd(t.g), OS_POLL_IN, 0 },
            { t.pty, OS_POLL_IN, 0 },
        };
        poll(fds, 2, -1);

        if (fds[1].revents & OS_POLL_IN) {
            /* Take what's there (a bounded amount, so the window stays responsive). */
            for (int i = 0; i < 16; i++) {
                long n = read(t.pty, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                for (long k = 0; k < n; k++) {
                    feed((uint8_t)buf[k]);
                }
                struct os_pollfd more = { t.pty, OS_POLL_IN, 0 };
                if (poll(&more, 1, 0) <= 0 || !(more.revents & OS_POLL_IN)) {
                    break;
                }
            }
            if (t.view) {
                mark_all();
            }
            redraw();
        } else if (fds[1].revents & OS_POLL_HUP) {
            break;   /* the shell and everything it started are gone */
        }

        struct gui_event ev;
        bool quit = false;
        while (gui_wait_event(t.g, &ev, 0)) {
            switch (ev.type) {
            case GUI_EVENT_KEY:
                on_key(&ev);
                break;
            case GUI_EVENT_SCROLL:
                on_scroll(ev.wheel);
                break;
            case GUI_EVENT_RESIZE:
                fit_to_window();
                break;
            case GUI_EVENT_FOCUS:
                t.focused = ev.focused;
                t.dirty[t.cy] = true;
                break;
            case GUI_EVENT_CLOSE:
            case GUI_EVENT_QUIT:
                quit = true;
                break;
            default:
                break;
            }
            if (quit) {
                break;
            }
        }
        if (quit) {
            break;
        }
        redraw();
    }

    kill(t.shell);
    close(t.pty);
    gui_close(t.g);
    int status;
    wait(t.shell, &status, 0);
    return 0;
}
