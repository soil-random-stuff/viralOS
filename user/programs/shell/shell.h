#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <paths.h>

#define HOME        home_dir()   /* the user's (<unistd.h>) */
#define PROGRAM_DIR _PATH_BIN "/"

/* --- lineedit.c: reading a line with editing, history and Tab completion --- */

struct completion {
    char text[256];      /* what the word becomes, e.g. "/etc/motd" */
    char display[64];    /* what to show in a list of choices, e.g. "motd" */
    bool is_dir;
};

/*
 * Fills `out` with up to `max` completions for the word that ends at
 * `cursor` in `line`, and sets *word_start to where that word begins.
 * Returns how many there are.
 */
typedef int (*completer_fn)(const char *line, size_t cursor, size_t *word_start,
                            struct completion *out, int max);

/*
 * Print the prompt and read a line into buf (NUL-terminated, no newline).
 * Returns its length, or -1 at end of input (Ctrl+D on an empty line).
 */
long edit_line(char *buf, size_t size, void (*print_prompt)(void), completer_fn complete);
void history_print(void);

/* --- complete.c --- */
int shell_complete(const char *line, size_t cursor, size_t *word_start,
                   struct completion *out, int max);

/* --- main.c --- */
extern const char *const shell_builtins[];
/* "~" or "~/x" -> HOME or HOME "/x"; anything else is copied. */
void expand_home(const char *in, char *out, size_t size);
