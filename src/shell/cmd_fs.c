/*
 * Built-in filesystem commands. `cd` has to be built in (it changes the
 * shell's own state); ls, cat and echo are user programs in /usr/bin.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/commands.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"

static void report(const char *cmd, const char *path, int err) {
    kprintf("%s: %s: %s\n", cmd, path, strerror(err));
}

void cmd_cd(char *args) {
    const char *path = *args ? args : "/";
    struct vnode *n;
    int err = vfs_resolve(shell_cwd, path, &n);
    if (!err && n->type != VNODE_DIR) {
        vfs_put(n);
        err = -ENOTDIR;
    }
    if (err) {
        report("cd", path, err);
        return;
    }
    vfs_put(shell_cwd);
    shell_cwd = n;
}

void cmd_pwd(char *args) {
    (void)args;
    char path[VFS_PATH_MAX];
    vfs_path(shell_cwd, path, sizeof(path));
    kprintf("%s\n", path);
}

static void tree_walk(struct vnode *dir, int depth, uint64_t *dirs, uint64_t *files) {
    struct vnode *child;
    for (size_t i = 0; vfs_readdir(dir, i, &child) == 0; i++) {
        for (int d = 0; d < depth; d++) {
            kprintf("    ");
        }
        if (child->type == VNODE_DIR) {
            kprintf("%s/\n", child->name);
            (*dirs)++;
            if (depth < 16) {
                tree_walk(child, depth + 1, dirs, files);
            }
        } else {
            kprintf("%s  (%lu bytes)\n", child->name, child->size);
            (*files)++;
        }
        vfs_put(child);
    }
}

void cmd_tree(char *args) {
    const char *path = *args ? args : ".";
    struct vnode *n;
    int err = vfs_resolve(shell_cwd, path, &n);
    if (!err && n->type != VNODE_DIR) {
        vfs_put(n);
        err = -ENOTDIR;
    }
    if (err) {
        report("tree", path, err);
        return;
    }
    uint64_t dirs = 0, files = 0;
    kprintf("%s\n", path);
    tree_walk(n, 1, &dirs, &files);
    kprintf("%lu directories, %lu files\n", dirs, files);
    vfs_put(n);
}

void cmd_mkdir(char *args) {
    char *path;
    next_word(args, &path);
    if (!*path) {
        kprintf("usage: mkdir <path>\n");
        return;
    }
    int err = vfs_create(shell_cwd, path, VNODE_DIR, 0755, NULL);
    if (err) {
        report("mkdir", path, err);
    }
}

void cmd_touch(char *args) {
    char *path;
    next_word(args, &path);
    if (!*path) {
        kprintf("usage: touch <path>\n");
        return;
    }
    int err = vfs_create(shell_cwd, path, VNODE_FILE, 0644, NULL);
    if (err && err != -EEXIST) {
        report("touch", path, err);
    }
}

void cmd_rm(char *args) {
    char *path;
    next_word(args, &path);
    if (!*path) {
        kprintf("usage: rm <path>\n");
        return;
    }
    int err = vfs_unlink(shell_cwd, path);
    if (err) {
        report("rm", path, err);
    }
}

void cmd_df(char *args) {
    (void)args;
    struct ramfs_stats s;
    ramfs_get_stats(&s);
    kprintf("Filesystem  Type   Files  Dirs  Bytes  Mounted on\n");
    kprintf("rootfs      ramfs  %5lu  %4lu  %5lu  /\n", s.files, s.directories, s.bytes);
}
