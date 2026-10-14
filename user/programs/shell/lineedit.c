/*
 * A small line editor (like a tiny readline). The console is switched to
 * raw mode so every key arrives as it's pressed; we do the echoing and
 * cursor movement ourselves with backspaces and reprints.
 *
 *   Left/Right, Home/End, Ctrl+A/Ctrl+E   move the cursor
 *   Backspace, Delete                     delete
 *   Up/Down                               history
 *   Tab                                   complete (twice-ambiguous: list choices)
 *   Ctrl+C                                abandon the line
 *   Ctrl+D                                end of input (on an empty line)
 *   Ctrl+L                                clear the screen
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

#define HISTORY_MAX     50
#define MAX_COMPLETIONS 128
#define LIST_WIDTH      80   /* columns to lay completion lists out in */

#define CTRL(c) ((c) & 0x1F)

static char *history[HISTORY_MAX];
static int history_len;

struct editor {
    char *buf;
    size_t size;
    size_t len;   /* characters in buf */
    size_t pos;   /* cursor, 0..len */
};

/* --- terminal output ---------------------------------------------------------- */

static void out(const char *s, size_t n) {
    if (n) {
        write(STDOUT_FILENO, s, n);
    }
}

static void out_str(const char *s) {
    out(s, strlen(s));
}

static void repeat(char c, size_t n) {
    char chunk[64];
    memset(chunk, c, sizeof(chunk));
    while (n) {
        size_t k = n < sizeof(chunk) ? n : sizeof(chunk);
        out(chunk, k);
        n -= k;
    }
}

/* Move the cursor left n columns. */
static void back(size_t n) {
    repeat('\b', n);
}

/* --- editing -------------------------------------------------------------------- */

static void insert(struct editor *e, const char *text, size_t n) {
    if (e->len + n > e->size - 1) {
        n = e->size - 1 - e->len;
    }
    if (n == 0) {
        return;
    }
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, text, n);
    e->len += n;
    out(e->buf + e->pos, e->len - e->pos);   /* the new text and everything after it */
    e->pos += n;
    back(e->len - e->pos);
}

static void delete_before(struct editor *e) {
    if (e->pos == 0) {
        return;
    }
    memmove(e->buf + e->pos - 1, e->buf + e->pos, e->len - e->pos);
    e->pos--;
    e->len--;
    out("\b", 1);
    out(e->buf + e->pos, e->len - e->pos);
    out(" ", 1);                                /* wipe the old last character */
    back(e->len - e->pos + 1);
}

static void delete_at(struct editor *e) {
    if (e->pos == e->len) {
        return;
    }
    memmove(e->buf + e->pos, e->buf + e->pos + 1, e->len - e->pos - 1);
    e->len--;
    out(e->buf + e->pos, e->len - e->pos);
    out(" ", 1);
    back(e->len - e->pos + 1);
}

static void move_home(struct editor *e) {
    back(e->pos);
    e->pos = 0;
}

static void move_end(struct editor *e) {
    out(e->buf + e->pos, e->len - e->pos);
    e->pos = e->len;
}

/* Swap the whole line for `text` (history). */
static void replace_line(struct editor *e, const char *text) {
    move_home(e);
    repeat(' ', e->len);
    back(e->len);
    size_t n = strlen(text);
    if (n > e->size - 1) {
        n = e->size - 1;
    }
    memcpy(e->buf, text, n);
    e->len = e->pos = n;
    out(e->buf, n);
}

static void redraw(struct editor *e, void (*print_prompt)(void)) {
    print_prompt();
    out(e->buf, e->len);
    back(e->len - e->pos);
}

/* --- history -------------------------------------------------------------------- */

static void history_add(const char *line) {
    if (!line[0] || (history_len && strcmp(history[history_len - 1], line) == 0)) {
        return;
    }
    if (history_len == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(history[0]) * (HISTORY_MAX - 1));
        history_len--;
    }
    char *copy = strdup(line);
    if (copy) {
        history[history_len++] = copy;
    }
}

void history_print(void) {
    for (int i = 0; i < history_len; i++) {
        printf("%4d  %s\n", i + 1, history[i]);
    }
}

/* --- Tab completion ------------------------------------------------------------- */

static size_t common_prefix(const char *a, const char *b) {
    size_t n = 0;
    while (a[n] && a[n] == b[n]) {
        n++;
    }
    return n;
}

static void list_choices(const struct completion *c, int n) {
    size_t width = 0;
    for (int i = 0; i < n; i++) {
        size_t w = strlen(c[i].display) + (c[i].is_dir ? 1 : 0);
        if (w > width) {
            width = w;
        }
    }
    width += 2;
    int per_row = (int)(LIST_WIDTH / width);
    if (per_row < 1) {
        per_row = 1;
    }
    out("\n", 1);
    for (int i = 0; i < n; i++) {
        printf(c[i].is_dir ? "\x1b[94m%s/\x1b[0m" : "%s", c[i].display);
        size_t w = strlen(c[i].display) + (c[i].is_dir ? 1 : 0);
        if ((i + 1) % per_row == 0 || i == n - 1) {
            out("\n", 1);
        } else {
            repeat(' ', width - w);
        }
    }
}

static void complete(struct editor *e, completer_fn completer, void (*print_prompt)(void)) {
    struct completion *c = malloc(sizeof(*c) * MAX_COMPLETIONS);
    if (!c) {
        return;
    }
    e->buf[e->len] = '\0';
    size_t word_start = e->pos;
    int n = completer(e->buf, e->pos, &word_start, c, MAX_COMPLETIONS);
    size_t typed = e->pos - word_start;

    if (n == 1) {
        /* Unique: finish the word, and add "/" (keep going) or " " (next argument). */
        if (strlen(c[0].text) > typed) {
            insert(e, c[0].text + typed, strlen(c[0].text) - typed);
        }
        insert(e, c[0].is_dir ? "/" : " ", 1);
    } else if (n > 1) {
        /* Several: fill in what they share; if that adds nothing, show them. */
        size_t shared = strlen(c[0].text);
        for (int i = 1; i < n; i++) {
            size_t p = common_prefix(c[0].text, c[i].text);
            if (p < shared) {
                shared = p;
            }
        }
        if (shared > typed) {
            insert(e, c[0].text + typed, shared - typed);
        } else {
            list_choices(c, n);
            redraw(e, print_prompt);
        }
    }
    free(c);
}

/* --- the main loop ------------------------------------------------------------------ */

long edit_line(char *buf, size_t size, void (*print_prompt)(void), completer_fn completer) {
    struct editor e = { .buf = buf, .size = size };
    int browsing = history_len;     /* history entry shown; history_len = the new line */
    char *draft = NULL;             /* the new line, kept while browsing history */
    long result = -1;

    print_prompt();
    int old_mode = tty_mode(OS_TTY_RAW);

    char in[64];
    int escape = 0;                 /* 1: after ESC, 2: inside "ESC [ ..." */
    int param = 0;
    for (;;) {
        long n = read(STDIN_FILENO, in, sizeof(in));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            char c = in[i];

            if (escape == 1) {
                escape = c == '[' ? 2 : 0;
                param = 0;
                continue;
            }
            if (escape == 2) {
                if (c >= '0' && c <= '9') {
                    param = param * 10 + (c - '0');
                    continue;
                }
                escape = 0;
                if (c == 'D' && e.pos > 0) {                      /* left */
                    out("\b", 1);
                    e.pos--;
                } else if (c == 'C' && e.pos < e.len) {           /* right */
                    out(e.buf + e.pos, 1);
                    e.pos++;
                } else if (c == 'H') {
                    move_home(&e);
                } else if (c == 'F') {
                    move_end(&e);
                } else if (c == '~' && param == 3) {              /* delete */
                    delete_at(&e);
                } else if (c == 'A' && browsing > 0) {            /* up: older */
                    if (browsing == history_len) {
                        e.buf[e.len] = '\0';
                        free(draft);
                        draft = strdup(e.buf);
                    }
                    replace_line(&e, history[--browsing]);
                } else if (c == 'B' && browsing < history_len) {  /* down: newer */
                    browsing++;
                    replace_line(&e, browsing == history_len ? (draft ? draft : "")
                                                             : history[browsing]);
                }
                continue;
            }

            switch (c) {
            case '\x1b':
                escape = 1;
                break;
            case '\n':
            case '\r':
                out("\n", 1);
                buf[e.len] = '\0';
                history_add(buf);
                result = (long)e.len;
                goto done;
            case '\b':
            case 127:
                delete_before(&e);
                break;
            case '\t':
                complete(&e, completer, print_prompt);
                break;
            case CTRL('c'):
                out_str("^C\n");
                buf[0] = '\0';
                result = 0;
                goto done;
            case CTRL('d'):
                if (e.len == 0) {
                    out("\n", 1);
                    goto done;   /* result stays -1: end of input */
                }
                delete_at(&e);
                break;
            case CTRL('a'):
                move_home(&e);
                break;
            case CTRL('e'):
                move_end(&e);
                break;
            case CTRL('l'):
                out_str("\x1b[2J\x1b[H");
                redraw(&e, print_prompt);
                break;
            default:
                if (c >= ' ' && c <= '~') {
                    insert(&e, &c, 1);
                }
                break;
            }
        }
    }
done:
    tty_mode(old_mode);
    free(draft);
    return result;
}
