/*
 * sha256sum file...
 *
 * Prints each file's SHA-256 (as "hash  name", like coreutils), to check a download against the
 * hash its site publishes. Uses libtls's SHA-256.
 */

#include <stdio.h>
#include <string.h>
#include <tls.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sha256sum file...\n");
        return 2;
    }
    static char buf[65536];
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("sha256sum: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        struct tls_sha256 *h = tls_sha256_start();
        if (!h) {
            printf("sha256sum: out of memory\n");
            close(fd);
            return 1;
        }
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            tls_sha256_add(h, buf, (size_t)n);
        }
        close(fd);
        char hex[65];
        tls_sha256_finish(h, hex);
        if (n < 0) {
            printf("sha256sum: %s: %s\n", argv[i], strerror((int)n));
            status = 1;
            continue;
        }
        printf("%s  %s\n", hex, argv[i]);
    }
    return status;
}
