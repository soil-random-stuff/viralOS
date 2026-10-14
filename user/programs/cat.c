/* cat [files...]: print files, or copy the keyboard to the screen until Ctrl+D. */

#include <stdio.h>
#include <unistd.h>

static int copy(int fd, const char *name) {
    static char buf[64 * 1024];   /* big pieces: much faster on USB drives */
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "cat: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return copy(STDIN_FILENO, "stdin");
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        status |= copy(fd, argv[i]);
        close(fd);
    }
    return status;
}
