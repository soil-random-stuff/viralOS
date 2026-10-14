/* ls [paths...]: list directories (or describe files) with permissions, size and date. */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void print_mode(const struct os_file_info *f) {
    static const char flags[] = "rwxrwxrwx";
    char s[11];
    s[0] = f->type == OS_DIRECTORY ? 'd' : '-';
    for (int i = 0; i < 9; i++) {
        s[i + 1] = (f->mode & (0400 >> i)) ? flags[i] : '-';
    }
    s[10] = '\0';
    printf("%s", s);
}

/* Seconds since 1970 -> "YYYY-MM-DD HH:MM" (UTC), via days-to-civil-date. */
static void print_time(uint64_t t) {
    if (t == 0) {
        printf("        -       ");
        return;
    }
    int64_t z = (int64_t)(t / 86400) + 719468;
    int64_t era = z / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t day = doy - (153 * mp + 2) / 5 + 1;
    uint64_t month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = (int64_t)yoe + era * 400 + (month <= 2);
    uint64_t secs = t % 86400;
    printf("%04ld-%02lu-%02lu %02lu:%02lu", year, month, day, secs / 3600, secs / 60 % 60);
}

static void print_entry(const struct os_file_info *f) {
    print_mode(f);
    if (f->type == OS_DIRECTORY) {
        printf("        -  ");
    } else {
        printf(" %8lu  ", f->size);
    }
    print_time(f->mtime);
    printf("  %s%s\n", f->name, f->type == OS_DIRECTORY ? "/" : "");
}

static int list(const char *path) {
    struct os_file_info info;
    int err = stat(path, &info);
    if (err < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(err));
        return 1;
    }
    if (info.type != OS_DIRECTORY) {
        print_entry(&info);
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }
    for (uint64_t i = 0; readdir(fd, i, &info) == 0; i++) {
        print_entry(&info);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return list(".");
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            printf("%s%s:\n", i > 1 ? "\n" : "", argv[i]);
        }
        status |= list(argv[i]);
    }
    return status;
}
