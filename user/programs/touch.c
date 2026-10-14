/* touch <file>...: create empty files (existing ones are left alone). */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: touch <file>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "touch: %s: %s\n", argv[i], strerror(fd));
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
