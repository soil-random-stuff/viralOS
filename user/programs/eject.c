/* eject <drive>: write everything out and unmount it, so it's safe to unplug.
   The drive is its path ("/media/MYSTICK") or just its name ("MYSTICK"). */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: eject <drive>   (see 'drives')\n");
        return 1;
    }
    char path[OS_PATH_MAX];
    if (argv[1][0] == '/') {
        strlcpy(path, argv[1], sizeof(path));
    } else {
        snprintf(path, sizeof(path), _PATH_MEDIA "/%s", argv[1]);
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    int err = eject(path);
    if (err < 0) {
        printf("eject: %s: %s\n", path, err == -ENOENT ? "no such drive (see 'drives')" : strerror(err));
        return 1;
    }
    printf("%s can be unplugged now.\n", path);
    return 0;
}
