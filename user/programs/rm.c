/* rm <path>...: remove files and empty directories. */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: rm <path>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int err = unlink(argv[i]);
        if (err < 0) {
            dprintf(STDERR_FILENO, "rm: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
