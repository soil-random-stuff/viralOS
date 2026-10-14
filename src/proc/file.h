#pragma once

#include <stdint.h>
#include <stddef.h>

struct vnode;

enum file_kind {
    FILE_CONSOLE,    /* the terminal */
    FILE_VNODE,      /* a file or directory */
    FILE_CHANNEL,    /* one end of a message channel (ipc/channel.c) */
    FILE_LISTENER,   /* a registered service, waiting for connections */
    FILE_SHM,        /* a shared memory object */
    FILE_PTY,        /* a pseudo-terminal, the programs' end */
    FILE_PTY_MASTER, /* a pseudo-terminal, the terminal window's end */
    FILE_SOCKET,     /* a network socket (net/socket.c); `end` is lwIP's socket number */
};

/* An open file: what a file descriptor points to. Shared by reference. */
struct file {
    enum file_kind kind;
    struct vnode *vnode;   /* FILE_VNODE */
    void *object;          /* FILE_CHANNEL / LISTENER / SHM: the object; FILE_PTY*: struct tty */
    int end;               /* FILE_CHANNEL: which end (0 or 1); FILE_SOCKET: the socket */
    uint64_t offset;
    int flags;             /* O_* from <viralos.h> */
    uint32_t refs;
};

/* Open `path` (relative to `cwd`) with O_* flags. On error returns NULL and sets *err. */
struct file *file_open(struct vnode *cwd, const char *path, int flags, int *err);
struct file *file_open_console(void);
/* A bare file of the given kind (one reference), for the IPC objects to fill in. */
struct file *file_new(enum file_kind kind);

void file_get(struct file *f);
void file_put(struct file *f);

int64_t file_read(struct file *f, void *buf, size_t len);
int64_t file_write(struct file *f, const void *buf, size_t len);
