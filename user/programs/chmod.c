/* chmod: set permission bits, in octal.   chmod 755 file...
   (Kept on ext4; FAT, exFAT and NTFS have none, so there it lasts until the next boot.) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: chmod MODE file...   (MODE in octal, e.g. 755 or 0644)\n");
        return 2;
    }
    uint32_t mode = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '7' || mode > 07777) {
            printf("chmod: invalid mode '%s' (octal digits, like 755)\n", argv[1]);
            return 2;
        }
        mode = mode * 8 + (uint32_t)(*p - '0');
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        int r = chmod(argv[i], mode);
        if (r < 0) {
            printf("chmod: %s: %s\n", argv[i], strerror(r));
            status = 1;
        }
    }
    return status;
}
