/* cp: copy files and folders. "cp a b", "cp a b c dir/" (into a folder); folders are copied whole. */

#include <fileops.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-R") == 0)) {
        argv++;   /* folders are always copied whole; -r is accepted out of habit */
        argc--;
    }
    if (argc < 3) {
        printf("usage: cp <from>... <to>\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct os_file_info info;
    bool into = stat(dest, &info) == 0 && info.type == OS_DIRECTORY;
    if (argc > 3 && !into) {
        printf("cp: %s is not a folder\n", dest);
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        char to[FILE_PATH_MAX];
        if (into) {
            path_join(to, sizeof(to), dest, path_base(argv[i]));
        } else {
            strlcpy(to, dest, sizeof(to));
        }
        int err = copy_tree(argv[i], to);
        if (err < 0) {
            printf("cp: %s -> %s: %s\n", argv[i], to, strerror(err));
            status = 1;
        }
    }
    return status;
}
