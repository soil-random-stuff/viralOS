/* mv: move or rename a file or folder. "mv a b/" or "mv a dir" moves a into the folder.
   Between drives it copies, then deletes the original. */

#include <fileops.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: mv <from> <to>\n");
        return 1;
    }
    char to[FILE_PATH_MAX];
    struct os_file_info info;
    if (stat(argv[2], &info) == 0 && info.type == OS_DIRECTORY) {
        path_join(to, sizeof(to), argv[2], path_base(argv[1]));
    } else {
        strlcpy(to, argv[2], sizeof(to));
    }
    int err = move_tree(argv[1], to);
    if (err < 0) {
        printf("mv: %s -> %s: %s\n", argv[1], to, strerror(err));
        return 1;
    }
    return 0;
}
