/* Runs ntfsd's operations directly on an NTFS image, on the build machine (no kernel in between):
     ntfsd_test <image>   (build: scripts/build_ports.sh servers_test) */
#define main ntfsd_main
#include "../src/ntfsd.c"
#undef main

static uint64_t find(uint64_t dir, const char *name) {
    static uint8_t buf[OS_USERFS_BUFFER];
    int64_t count;
    int more;
    if (n_list(dir, 0, buf, sizeof(buf), &count, &more)) {
        return 0;
    }
    size_t at = 0;
    for (int64_t i = 0; i < count; i++) {
        struct os_userfs_entry *e = (struct os_userfs_entry *)(buf + at);
        if (strcmp(e->name, name) == 0) {
            return e->node;
        }
        at += OS_USERFS_ENTRY_SIZE(e->name_len);
    }
    return 0;
}

#define CHECK(what) do { int r_ = (int)(what); printf("%-60s %d\n", #what, r_); } while (0)

int main(int argc, char **argv) {
    ntfs_log_set_handler(ntfs_log_handler_stderr);
    ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_CRITICAL);
    vol = ntfs_mount(argv[1], NTFS_MNT_RECOVER);
    if (!vol) {
        perror("mount");
        return 1;
    }
    uint64_t dir, file, f2;
    CHECK(n_create(FILE_root, "newdir", 1, &dir));
    CHECK(n_create(dir, "hello.txt", 0, &file));
    CHECK(n_write(file, 0, "hello\n", 6));
    CHECK(n_rename(dir, file, "hello.txt", dir, "Hello.txt", 0));
    CHECK(find(dir, "Hello.txt") == file);
    CHECK(n_rename(dir, file, "Hello.txt", FILE_root, "moved.txt", 0));
    CHECK(find(FILE_root, "moved.txt") == file);
    CHECK(n_create(dir, "two.txt", 0, &f2));
    CHECK(n_truncate(f2, 100000));
    CHECK(n_unlink(dir, f2, "two.txt", 0));
    uint64_t readme = find(FILE_root, "README.TXT");
    CHECK(readme != 0);
    CHECK(n_unlink(FILE_root, readme, "README.TXT", 0));
    CHECK(n_unlink(FILE_root, dir, "newdir", 1));
    CHECK(n_destroy());
    return 0;
}
