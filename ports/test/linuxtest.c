/* linuxtest: checks the kernel's Linux system calls (proc/linux.c) from a musl program. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static __thread int tls_value = 42;

int main(int argc, char **argv) {
    printf("hello from musl: argc=%d argv[0]=%s tls=%d\n", argc, argv[0], tls_value);
    printf("env PATH=%s\n", getenv("PATH") ? getenv("PATH") : "(none)");

    /* malloc: small (brk) and big (mmap) */
    char *small = malloc(100), *big = malloc(8 * 1024 * 1024);
    if (!small || !big) {
        printf("malloc FAILED\n");
        return 1;
    }
    memset(big, 0xAB, 8 * 1024 * 1024);
    free(big);
    big = calloc(1, 3 * 1024 * 1024);
    printf("malloc ok (%p, %p)\n", (void *)small, (void *)big);

    struct utsname u;
    uname(&u);
    printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("time: %lld\n", (long long)ts.tv_sec);

    unsigned char rnd[8];
    getrandom(rnd, sizeof(rnd), 0);
    printf("random: %02x%02x%02x%02x\n", rnd[0], rnd[1], rnd[2], rnd[3]);

    /* a file: create, write, seek, read back, stat */
    FILE *f = fopen("/Temporary/linuxtest.txt", "w+");
    if (!f) {
        printf("fopen: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "line one\nline two\n");
    fseek(f, 5, SEEK_SET);
    char line[64];
    fgets(line, sizeof(line), f);
    printf("read back: %s", line);
    fclose(f);
    struct stat st;
    stat("/Temporary/linuxtest.txt", &st);
    printf("stat: size %lld, regular %d\n", (long long)st.st_size, S_ISREG(st.st_mode));

    /* directories */
    DIR *d = opendir("/Devices");
    if (d) {
        struct dirent *e;
        printf("/Devices:");
        while ((e = readdir(d))) {
            printf(" %s", e->d_name);
        }
        printf("\n");
        closedir(d);
    }

    /* a block device, if there's one */
    const char *dev = argc > 1 ? argv[1] : "/Devices/usb0";
    int fd = open(dev, O_RDONLY);
    if (fd >= 0) {
        unsigned long long bytes = 0;
        int ss = 0;
        ioctl(fd, BLKGETSIZE64, &bytes);
        ioctl(fd, BLKSSZGET, &ss);
        fstat(fd, &st);
        unsigned char sector[512];
        ssize_t n = pread(fd, sector, sizeof(sector), 0);
        printf("%s: block %d, %llu bytes, sector %d, read %zd, signature %02x%02x\n", dev, S_ISBLK(st.st_mode),
               bytes, ss, n, sector[510], sector[511]);
        close(fd);
    } else {
        printf("%s: %s\n", dev, strerror(errno));
    }
    printf("linuxtest done\n");
    return 0;
}
