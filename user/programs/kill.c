/* kill <pid>...: stop processes. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: kill <pid>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int err = kill(atoi(argv[i]));
        if (err < 0) {
            dprintf(STDERR_FILENO, "kill: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
