#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/shell.h"
#include "shell/commands.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include <paths.h>

#define LINE_MAX (VFS_PATH_MAX + 64)   /* room for a command and a full path */

#define COLOR_TEXT   0xD0D0D0
#define COLOR_HOST   0x9CE89C
#define COLOR_KERNEL 0xE5C07B
#define COLOR_PROMPT 0x7FD4FF
#define COLOR_BG     0x101018

struct vnode *shell_cwd;

static const struct {
    const char *name;
    void (*fn)(char *args);
} commands[] = {
    { "help",    cmd_help },
    { "clear",   cmd_clear },
    { "cd",      cmd_cd },
    { "pwd",     cmd_pwd },
    { "tree",    cmd_tree },
    { "mkdir",   cmd_mkdir },
    { "touch",   cmd_touch },
    { "rm",      cmd_rm },
    { "df",      cmd_df },
    { "uptime",  cmd_uptime },
    { "ps",      cmd_ps },
    { "kill",    cmd_kill },
    { "sleep",   cmd_sleep },
    { "spin",    cmd_spin },
    { "demo",    cmd_demo },
    { "meminfo", cmd_meminfo },
    { "memmap",  cmd_memmap },
    { "crash",   cmd_crash },
};

char *next_word(char *s, char **word) {
    while (*s == ' ') {
        s++;
    }
    *word = s;
    while (*s && *s != ' ') {
        s++;
    }
    if (*s) {
        *s++ = '\0';
    }
    while (*s == ' ') {
        s++;
    }
    return s;
}

uint64_t parse_u64(const char *s, uint64_t fallback) {
    if (*s < '0' || *s > '9') {
        return fallback;
    }
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s++ - '0');
    }
    return v;
}

static size_t read_line(char *buf, size_t size) {
    size_t len = 0;
    int escape = 0;   /* skipping an arrow-key escape sequence */
    for (;;) {
        char c = keyboard_getchar();
        if (escape) {
            if (escape == 1) {
                escape = c == '[' ? 2 : 0;
            } else if (c >= '@' && c <= '~') {
                escape = 0;
            }
            continue;
        }
        if (c == '\x1b') {
            escape = 1;
            continue;
        }
        if (c == 3) {   /* Ctrl+C: drop the line */
            kprintf("^C\n");
            buf[0] = '\0';
            return 0;
        }
        if (c == '\n') {
            kprintf("\n");
            buf[len] = '\0';
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                kprintf("\b \b");
            }
            continue;
        }
        if (c >= ' ' && c <= '~' && len < size - 1) {
            buf[len++] = c;
            kprintf("%c", c);
        }
    }
}

/* Read fresh each time, so `echo name > /etc/hostname` renames the machine. */
static void read_hostname(char *buf, size_t size) {
    strlcpy(buf, "viralos", size);
    struct vnode *f;
    if (vfs_resolve(NULL, _PATH_HOSTNAME, &f) != 0) {
        return;
    }
    int64_t n = vfs_read(f, 0, buf, size - 1);
    vfs_put(f);
    if (n <= 0) {
        strlcpy(buf, "viralos", size);
        return;
    }
    buf[n] = '\0';
    for (int64_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ') {
            buf[i] = '\0';
            break;
        }
    }
}

static void print_prompt(void) {
    char host[32], path[VFS_PATH_MAX];
    read_hostname(host, sizeof(host));
    vfs_path(shell_cwd, path, sizeof(path));

    console_set_color(COLOR_KERNEL, COLOR_BG);
    kprintf("[kernel] ");
    console_set_color(COLOR_HOST, COLOR_BG);
    kprintf("%s", host);
    console_set_color(COLOR_TEXT, COLOR_BG);
    kprintf(":");
    console_set_color(COLOR_PROMPT, COLOR_BG);
    kprintf("%s> ", path);
    console_set_color(COLOR_TEXT, COLOR_BG);
}

void shell_run(void) {
    char line[LINE_MAX];

    shell_cwd = vfs_root();
    vfs_get(shell_cwd);

    for (;;) {
        print_prompt();
        read_line(line, sizeof(line));
        char original[LINE_MAX];
        strlcpy(original, line, sizeof(original));

        char *name;
        char *args = next_word(line, &name);
        if (!*name) {
            continue;
        }

        bool found = false;
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (strcmp(name, commands[i].name) == 0) {
                commands[i].fn(args);
                found = true;
                break;
            }
        }
        if (!found) {
            run_program(original);   /* not built in: look for a program */
        }
    }
}
