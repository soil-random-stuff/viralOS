#include <stdbool.h>

#include "proc/tty.h"
#include "proc/file.h"
#include "proc/process.h"
#include "arch/x86_64/cpu.h"
#include "drivers/keyboard.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define CTRL_C 3
#define CTRL_D 4
#define ESC    0x1b

#define PTY_INPUT  1024    /* typed but not yet read by the programs */
#define PTY_OUTPUT 16384   /* printed but not yet read by the window */

struct ring {
    char *data;
    size_t size, head, len;
};

struct tty {
    bool console;

    /* One line being edited / waiting to be consumed (cooked mode). */
    char line[256];
    size_t line_len;      /* bytes in `line` */
    size_t line_pos;      /* bytes already handed out */
    bool line_eof;        /* Ctrl+D: deliver a 0-byte read */
    struct mutex lock;    /* one reader at a time */

    int mode;
    uint64_t raw_owner;   /* pid that turned raw mode on */
    uint64_t foreground;  /* pid Ctrl+C stops, 0 = none */

    /* Pseudo-terminals only. Touched with interrupts off. */
    struct ring input, output;
    bool master_open, slave_open;
    struct tty *next;
};

static struct tty console = { .console = true, .lock = MUTEX_INIT, .mode = OS_TTY_COOKED };
static struct tty *ptys;

/* --- ring buffers --------------------------------------------------------------- */

static size_t ring_put(struct ring *r, const char *src, size_t n) {
    size_t done = 0;
    while (done < n && r->len < r->size) {
        r->data[(r->head + r->len++) % r->size] = src[done++];
    }
    return done;
}

static size_t ring_get(struct ring *r, char *dst, size_t n) {
    size_t done = 0;
    while (done < n && r->len > 0) {
        dst[done++] = r->data[r->head];
        r->head = (r->head + 1) % r->size;
        r->len--;
    }
    return done;
}

/* --- the two kinds of terminal ------------------------------------------------------- */

/* Show something the terminal itself prints (echo, "^C"); never blocks. */
static void echo(struct tty *t, const char *s, size_t n) {
    if (t->console) {
        kwrite(s, n);
        return;
    }
    uint64_t irq = irq_save();
    if (t->master_open && ring_put(&t->output, s, n)) {
        ipc_notify();
    }
    irq_restore(irq);
}

static void echo_str(struct tty *t, const char *s) {
    echo(t, s, strlen(s));
}

/*
 * Next typed character. Returns 1, 0 if nothing more will ever come (the
 * window closed), -EINTR if the reader was killed, or -EAGAIN if `wait`
 * is false and nothing is there yet.
 */
static int get_char(struct tty *t, char *c, bool wait) {
    if (t->console) {
        if (!wait) {
            return keyboard_try_getchar(c) ? 1 : -EAGAIN;
        }
        return keyboard_getchar_unless(c, process_current_killed) ? 1 : -EINTR;
    }
    uint64_t irq = irq_save();
    int result;
    for (;;) {
        if (ring_get(&t->input, c, 1)) {
            result = 1;
            break;
        }
        if (!t->master_open) {
            result = 0;
            break;
        }
        if (!wait) {
            result = -EAGAIN;
            break;
        }
        if (process_current_killed()) {
            result = -EINTR;
            break;
        }
        ipc_wait(0);
    }
    irq_restore(irq);
    return result;
}

/* --- modes and the foreground process ----------------------------------------------------- */

int tty_set_mode(struct tty *t, int new_mode, uint64_t pid) {
    uint64_t irq = irq_save();
    int old = t->mode;
    t->mode = new_mode == OS_TTY_RAW ? OS_TTY_RAW : OS_TTY_COOKED;
    t->raw_owner = t->mode == OS_TTY_RAW ? pid : 0;
    t->line_len = t->line_pos = 0;   /* a half-typed cooked line doesn't survive a switch */
    t->line_eof = false;
    irq_restore(irq);
    return old;
}

void tty_set_foreground(struct tty *t, uint64_t pid) {
    uint64_t irq = irq_save();
    t->foreground = pid;
    irq_restore(irq);
}

static void forget(struct tty *t, uint64_t pid) {
    if (t->mode == OS_TTY_RAW && t->raw_owner == pid) {
        tty_set_mode(t, OS_TTY_COOKED, 0);
    }
    if (t->foreground == pid) {
        t->foreground = 0;
    }
}

void tty_process_exited(uint64_t pid) {
    uint64_t irq = irq_save();
    forget(&console, pid);
    for (struct tty *t = ptys; t; t = t->next) {
        forget(t, pid);
    }
    irq_restore(irq);
}

/* Ctrl+C: stop the foreground program. Returns false if there is none. Interrupts off. */
static bool interrupt_foreground(struct tty *t) {
    if (!t->foreground || !process_interrupt(t->foreground)) {
        t->foreground = 0;
        return false;
    }
    echo_str(t, "^C\n");
    return true;
}

static bool console_ctrl_c(void) {
    return interrupt_foreground(&console);   /* runs in the keyboard interrupt */
}

void tty_init(void) {
    keyboard_set_ctrl_c_handler(console_ctrl_c);
}

struct tty *tty_console(void) {
    return &console;
}

struct tty *tty_of(struct file *f) {
    if (!f) {
        return NULL;
    }
    switch (f->kind) {
    case FILE_CONSOLE:    return &console;
    case FILE_PTY:
    case FILE_PTY_MASTER: return f->object;
    default:              return NULL;
    }
}

/* --- reading --------------------------------------------------------------------------- */

/* Collect one line. Returns 0, or -EINTR if the reader is killed. */
static int fill_line(struct tty *t) {
    t->line_len = t->line_pos = 0;
    int escape = 0;   /* 1 after ESC, 2 inside "ESC [ ...": skip the sequence */
    for (;;) {
        char c;
        int got = get_char(t, &c, true);
        if (got < 0) {
            return got;
        }
        if (got == 0) {
            t->line_eof = true;   /* hung up: end of file */
            return 0;
        }
        if (escape) {
            if (escape == 1) {
                escape = c == '[' ? 2 : 0;
            } else if (c >= '@' && c <= '~') {
                escape = 0;   /* final byte: arrow keys etc. do nothing here */
            }
            continue;
        }
        if (c == ESC) {
            escape = 1;
        } else if (c == CTRL_D && t->line_len == 0) {
            t->line_eof = true;
            return 0;
        } else if (c == CTRL_C) {
            t->line_len = 0;    /* throw the line away */
            echo_str(t, "^C\n");
        } else if (c == '\b' || c == 127) {
            if (t->line_len > 0) {
                t->line_len--;
                echo_str(t, "\b \b");
            }
        } else if (c == '\n' || c == '\r') {
            t->line[t->line_len++] = '\n';
            echo_str(t, "\n");
            return 0;
        } else if (c >= ' ' && c <= '~' && t->line_len < sizeof(t->line) - 1) {
            t->line[t->line_len++] = c;
            echo(t, &c, 1);
        }
    }
}

/* Raw mode: wait for one key, then take whatever else is already there. */
static int64_t read_raw(struct tty *t, char *buf, size_t len) {
    int got = get_char(t, &buf[0], true);
    if (got <= 0) {
        return got;
    }
    size_t n = 1;
    while (n < len && get_char(t, &buf[n], false) == 1) {
        n++;
    }
    return (int64_t)n;
}

int64_t tty_read(struct tty *t, char *buf, size_t len) {
    if (len == 0) {
        return 0;
    }
    mutex_lock(&t->lock);
    int64_t result = 0;
    if (t->mode == OS_TTY_RAW) {
        result = read_raw(t, buf, len);
    } else {
        if (t->line_pos == t->line_len && !t->line_eof) {
            result = fill_line(t);
        }
        if (result == 0) {
            if (t->line_eof) {
                t->line_eof = false;   /* this read returns 0; the next one reads normally */
            } else {
                size_t n = t->line_len - t->line_pos;
                if (n > len) {
                    n = len;
                }
                memcpy(buf, t->line + t->line_pos, n);
                t->line_pos += n;
                result = (int64_t)n;
            }
        }
    }
    mutex_unlock(&t->lock);
    return result;
}

/* --- writing ----------------------------------------------------------------------------- */

int64_t tty_write(struct tty *t, const char *buf, size_t len) {
    if (t->console) {
        kwrite(buf, len);
        return (int64_t)len;
    }
    /* Into the window's queue, waiting while it's full. Once the window is
       gone, output just disappears (like a terminal that was closed). */
    uint64_t irq = irq_save();
    size_t done = 0;
    int64_t result = 0;
    while (done < len && t->master_open) {
        size_t n = ring_put(&t->output, buf + done, len - done);
        if (n) {
            done += n;
            ipc_notify();
            continue;
        }
        if (process_current_killed()) {
            result = -EINTR;
            break;
        }
        ipc_wait(0);
    }
    irq_restore(irq);
    return done || !result ? (int64_t)len : result;
}

/* --- pseudo-terminals ------------------------------------------------------------------------ */

int pty_open(struct file **master, struct file **slave) {
    struct tty *t = kzalloc(sizeof(*t));
    char *in = kmalloc(PTY_INPUT), *out = kmalloc(PTY_OUTPUT);
    struct file *m = file_new(FILE_PTY_MASTER), *s = file_new(FILE_PTY);
    if (!t || !in || !out || !m || !s) {
        kfree(t);
        kfree(in);
        kfree(out);
        kfree(m);
        kfree(s);
        return -ENOMEM;
    }
    t->lock = (struct mutex)MUTEX_INIT;
    t->mode = OS_TTY_COOKED;
    t->input = (struct ring){ .data = in, .size = PTY_INPUT };
    t->output = (struct ring){ .data = out, .size = PTY_OUTPUT };
    t->master_open = t->slave_open = true;
    m->object = s->object = t;

    uint64_t irq = irq_save();
    t->next = ptys;
    ptys = t;
    irq_restore(irq);
    *master = m;
    *slave = s;
    return 0;
}

int64_t pty_master_read(struct tty *t, char *buf, size_t len) {
    uint64_t irq = irq_save();
    int64_t result;
    for (;;) {
        size_t n = ring_get(&t->output, buf, len);
        if (n) {
            ipc_notify();   /* room for writers again */
            result = (int64_t)n;
            break;
        }
        if (!t->slave_open || len == 0) {
            result = 0;
            break;
        }
        if (process_current_killed()) {
            result = -EINTR;
            break;
        }
        ipc_wait(0);
    }
    irq_restore(irq);
    return result;
}

int64_t pty_master_write(struct tty *t, const char *buf, size_t len) {
    uint64_t irq = irq_save();
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == CTRL_C && interrupt_foreground(t)) {
            continue;
        }
        ring_put(&t->input, &buf[i], 1);   /* typing too fast: the rest is lost */
    }
    ipc_notify();
    irq_restore(irq);
    return (int64_t)len;
}

uint16_t pty_master_poll(struct tty *t) {
    if (t->output.len) {
        return OS_POLL_IN;
    }
    return t->slave_open ? 0 : OS_POLL_HUP;
}

uint16_t pty_slave_poll(struct tty *t) {
    if (t->input.len) {
        return OS_POLL_IN;
    }
    return t->master_open ? 0 : OS_POLL_HUP;
}

void pty_close(struct tty *t, bool master) {
    uint64_t irq = irq_save();
    if (master) {
        t->master_open = false;
    } else {
        t->slave_open = false;
    }
    bool gone = !t->master_open && !t->slave_open;
    if (gone) {
        for (struct tty **link = &ptys; *link; link = &(*link)->next) {
            if (*link == t) {
                *link = t->next;
                break;
            }
        }
    }
    ipc_notify();   /* readers and pollers on the other end */
    irq_restore(irq);
    if (gone) {
        kfree(t->input.data);
        kfree(t->output.data);
        kfree(t);
    }
}
