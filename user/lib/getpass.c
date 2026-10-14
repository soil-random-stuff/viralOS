/* getpass: read a password from the terminal without showing it (see <unistd.h>). */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CTRL(c) ((c) & 0x1F)

char *getpass(const char *prompt) {
    static char buf[128];
    size_t len = 0;
    bool done = false, cancelled = false;
    if (prompt) {
        write(STDOUT_FILENO, prompt, strlen(prompt));
    }
    int old_mode = tty_mode(OS_TTY_RAW);
    while (!done) {
        char in[32];
        long n = read(STDIN_FILENO, in, sizeof(in));
        if (n <= 0) {
            cancelled = len == 0;
            break;
        }
        for (long i = 0; i < n && !done; i++) {
            char c = in[i];
            if (c == '\r' || c == '\n') {
                done = true;
            } else if (c == CTRL('c') || (c == CTRL('d') && len == 0)) {
                done = cancelled = true;
            } else if (c == '\b' || c == 127) {
                len -= len > 0;
            } else if (c == CTRL('u')) {
                len = 0;
            } else if ((unsigned char)c >= ' ' && c != 127 && len < sizeof(buf) - 1) {
                buf[len++] = c;
            }
        }
    }
    tty_mode(old_mode);
    write(STDOUT_FILENO, "\n", 1);
    buf[len] = '\0';
    if (cancelled) {
        memset(buf, 0, sizeof(buf));
        return NULL;
    }
    return buf;
}
