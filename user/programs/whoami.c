/* whoami: the user this program runs for (the one logged in) and their home folder with -l. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        printf("%s (home: %s)\n", user_name(), home_dir());
    } else {
        printf("%s\n", user_name());
    }
    return 0;
}
