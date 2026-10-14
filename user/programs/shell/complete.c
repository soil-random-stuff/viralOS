/*
 * What Tab completes:
 *   the first word            built-in commands and programs in /usr/bin
 *                             (or a path, if it contains '/')
 *   the argument of cd        directories only
 *   any other word            files and directories, e.g. "/us" -> "/usr/"
 * "~/" at the start of a path means the home folder.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int add(struct completion *out, int n, int max, const char *dir_part, const char *name,
               bool is_dir) {
    if (n >= max || strlen(dir_part) + strlen(name) >= sizeof(out[n].text)) {
        return n;
    }
    strcpy(out[n].text, dir_part);
    strcat(out[n].text, name);
    strlcpy(out[n].display, name, sizeof(out[n].display));
    out[n].is_dir = is_dir;
    return n + 1;
}

/* Sort by text and drop duplicates (a program named like a built-in). */
static int sort_unique(struct completion *c, int n) {
    for (int i = 1; i < n; i++) {
        struct completion tmp = c[i];
        int j = i - 1;
        while (j >= 0 && strcmp(c[j].text, tmp.text) > 0) {
            c[j + 1] = c[j];
            j--;
        }
        c[j + 1] = tmp;
    }
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (kept == 0 || strcmp(c[kept - 1].text, c[i].text) != 0) {
            c[kept++] = c[i];
        }
    }
    return kept;
}

/* Entries of `dir` whose names start with `prefix`. */
static int from_directory(const char *dir, const char *dir_part, const char *prefix,
                          bool dirs_only, bool files_only,
                          struct completion *out, int n, int max) {
    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        return n;
    }
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) == 0; i++) {
        bool is_dir = info.type == OS_DIRECTORY;
        if (!starts_with(info.name, prefix) || (dirs_only && !is_dir) || (files_only && is_dir)) {
            continue;
        }
        n = add(out, n, max, dir_part, info.name, is_dir);
    }
    close(fd);
    return n;
}

int shell_complete(const char *line, size_t cursor, size_t *word_start,
                   struct completion *out, int max) {
    size_t start = cursor;
    while (start > 0 && line[start - 1] != ' ') {
        start--;
    }
    *word_start = start;

    char word[256];
    size_t len = cursor - start;
    if (len >= sizeof(word)) {
        return 0;
    }
    memcpy(word, line + start, len);
    word[len] = '\0';

    /* Which word is this, and what command does the line start with? */
    size_t cmd_start = 0;
    while (cmd_start < start && line[cmd_start] == ' ') {
        cmd_start++;
    }
    bool first_word = cmd_start == start;
    bool after_cd = !first_word && strncmp(line + cmd_start, "cd ", 3) == 0;

    if (first_word && !strchr(word, '/')) {
        int n = 0;
        for (int i = 0; shell_builtins[i]; i++) {
            if (starts_with(shell_builtins[i], word)) {
                n = add(out, n, max, "", shell_builtins[i], false);
            }
        }
        n = from_directory(PROGRAM_DIR, "", word, false, true, out, n, max);
        return sort_unique(out, n);
    }

    /* A path: complete the part after the last '/' inside the part before it. */
    char dir_part[256] = "";
    const char *prefix = word;
    const char *slash = strrchr(word, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - word) + 1;
        memcpy(dir_part, word, dir_len);
        dir_part[dir_len] = '\0';
        prefix = slash + 1;
    }
    char dir[256];
    if (dir_part[0]) {
        expand_home(dir_part, dir, sizeof(dir));
    } else {
        strcpy(dir, ".");
    }
    return from_directory(dir, dir_part, prefix, after_cd, false, out, 0, max);
}
