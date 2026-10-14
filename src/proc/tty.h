#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Terminals for programs: the console (keyboard + screen), and
 * pseudo-terminals, where a program (a terminal window) plays the keyboard
 * and screen for the programs it runs.
 *
 * Two modes (OS_TTY_* in <viralos.h>):
 *
 *   cooked  the default: reads return whole lines, edited with echo and
 *           backspace, like a Unix terminal. Ctrl+D on an empty line means
 *           end of file; Ctrl+C (if no program claims it) cancels the line.
 *   raw     reads return keys as soon as they're pressed, with no echo.
 *           Arrow keys etc. come as escape sequences. For line editors.
 *
 * Raw mode belongs to the process that turned it on and ends when it exits.
 * Each terminal also has a foreground process: the one Ctrl+C stops.
 *
 * A pseudo-terminal has two ends. The "master" belongs to the terminal
 * window: what it writes is typed into the terminal, and it reads what the
 * programs print. The other end is an ordinary terminal for those programs
 * (their stdin/stdout/stderr). When the window closes the master, the
 * programs read end of file; when every program has closed the other end,
 * the window's read returns 0.
 */

struct tty;
struct file;

void tty_init(void);
struct tty *tty_console(void);
/* The terminal a file is (console or pseudo-terminal end), or NULL. */
struct tty *tty_of(struct file *f);

int64_t tty_read(struct tty *t, char *buf, size_t len);
int64_t tty_write(struct tty *t, const char *buf, size_t len);

/* Returns the previous mode. */
int tty_set_mode(struct tty *t, int mode, uint64_t pid);
void tty_set_foreground(struct tty *t, uint64_t pid);
/* Forget a process that's gone: raw mode it owned, foreground. */
void tty_process_exited(uint64_t pid);

/* Pseudo-terminals. */
int pty_open(struct file **master, struct file **slave);
int64_t pty_master_read(struct tty *t, char *buf, size_t len);
int64_t pty_master_write(struct tty *t, const char *buf, size_t len);
uint16_t pty_master_poll(struct tty *t);
uint16_t pty_slave_poll(struct tty *t);
void pty_close(struct tty *t, bool master);
