/* tree [dir]: show a directory and everything below it. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned long dirs, files;

static void walk(const char *path, int depth) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "tree: %s: %s\n", path, strerror(fd));
        return;
    }
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) == 0; i++) {
        for (int d = 0; d < depth; d++) {
            printf("    ");
        }
        if (info.type == OS_DIRECTORY) {
            printf("\x1b[94m%s/\x1b[0m\n", info.name);
            dirs++;
            char child[256];
            size_t len = strlcpy(child, path, sizeof(child));
            if (depth < 16 && len + 1 + strlen(info.name) < sizeof(child)) {
                if (len > 0 && child[len - 1] != '/') {
                    strcat(child, "/");
                }
                strcat(child, info.name);
                walk(child, depth + 1);
            }
        } else {
            printf("%s  \x1b[90m(%lu bytes)\x1b[0m\n", info.name, info.size);
            files++;
        }
    }
    close(fd);
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    printf("%s\n", root);
    walk(root, 1);
    printf("%lu directories, %lu files\n", dirs, files);
    return 0;
}
