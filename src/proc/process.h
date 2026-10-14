#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <viralos.h>

#include "sched/sync.h"

struct file;
struct thread;
struct vfs_sb;
struct vnode;
struct interrupt_frame;
struct os_proc_info;

#define PROCESS_MAX_FILES 32
#define PROCESS_MAX_ARGS  32
#define PROCESS_MAX_SHM   128

struct shm;
struct linux_map;

/* A shared memory object mapped into this process. */
struct shm_mapping {
    uint64_t address;
    uint64_t pages;
    struct shm *shm;      /* NULL: slot free */
};

/*
 * A running program: an address space, open files, a working directory and
 * one thread.
 *
 * Every process has an owner who collects its exit code: its parent process
 * (via wait()), or for programs started by the kernel, the kernel code that
 * called process_spawn (via process_wait()). Until then an exited process
 * stays around as a zombie. Orphans and detached processes clean up after
 * themselves.
 */
struct process {
    uint64_t pid;
    char name[32];
    char user[OS_USER_MAX];                  /* who it runs for (SYS_USER): inherited by what it starts */
    uint64_t space;                          /* page tables (CR3 value) */
    struct thread *thread;
    struct file *files[PROCESS_MAX_FILES];   /* indexed by file descriptor */
    struct vnode *cwd;

    uint64_t entry;                          /* where user code starts */
    uint64_t user_rsp;                       /* initial user stack pointer */
    uint64_t heap_start, heap_end;           /* brk() region, right after the program image */
    struct shm_mapping shm_maps[PROCESS_MAX_SHM];
    uint64_t shm_next;                       /* where the next shared memory mapping goes */

    /* A Linux program (proc/linux.c): Linux system call numbers, mmap, TLS. */
    bool linux_abi;
    uint64_t mmap_next;                      /* where the next anonymous mapping goes */
    struct linux_map *maps;                  /* its anonymous mappings */

    struct process *parent;                  /* NULL: owned by the kernel, or orphaned */
    struct process *children;                /* first child */
    struct process *sibling;                 /* next child of the same parent */
    struct wait_queue child_waiters;         /* this process waiting for a child */
    struct wait_queue exit_waiters;          /* the kernel waiting for this process */

    int exit_code;
    bool exited;
    bool killed;                             /* exit at the next return to user mode */
    bool catch_interrupt;                    /* Ctrl+C only sets `interrupted` (OS_INTERRUPT_CATCH) */
    bool interrupted;                        /* Ctrl+C pressed, not checked yet: blocking calls give up */

    /* One for the thread, one for the owner (if any). Freed at zero. */
    uint32_t refs;
    struct process *next;                    /* list of all processes */
};

struct spawn_options {
    struct vnode *cwd;            /* NULL = root */
    struct file *files[3];        /* stdin, stdout, stderr; NULL = console */
    struct process *parent;       /* NULL = owned by the calling kernel code */
    bool detached;                /* (kernel-owned only) nobody will wait */
    uint64_t pid;                 /* out: the new process's id */
};

void process_init(void);

/*
 * Load the ELF executable at `path` into a new process and start it with
 * the given arguments (argv[0] should be the program name).
 * Returns the process, or NULL with *err set. A kernel-owned, non-detached
 * process must eventually be passed to process_wait().
 */
struct process *process_spawn(const char *path, int argc, char **argv,
                              struct spawn_options *opts, int *err);

/* Kernel-owned processes: block until `p` exits, return its exit code, release it. */
int process_wait(struct process *p);

/* wait() for children of the current process. Returns a pid, 0 (WNOHANG) or -errno. */
int64_t process_wait_child(int64_t pid, int *status, int flags);

struct process *process_current(void);
struct process *process_find(uint64_t pid);   /* caller must hold irq_save */
__attribute__((noreturn)) void process_exit(int code);
bool process_kill(uint64_t pid);
/* Ctrl+C: kill it, unless it asked to only be told (then its blocking calls return -EINTR). */
bool process_interrupt(uint64_t pid);
/* SYS_INTERRUPT */
int64_t process_interrupt_mode(struct process *p, int op);
bool process_current_killed(void);

/* Map fresh zeroed pages over [start, end) of a user address space. */
int process_map_zeroed(uint64_t space, uint64_t start, uint64_t end, uint32_t flags);

/* Move the program break; returns the new end, or the old one if it can't. */
uint64_t process_brk(struct process *p, uint64_t new_end);

/* The process Ctrl+C on the console goes to (NULL = none). */
void process_set_foreground(struct process *p);

/* For ps / sysinfo: the index-th process, or -ENOENT. */
int process_info(uint64_t index, struct os_proc_info *out);
uint32_t process_count(void);
/* Does a running program have a file or folder open on this filesystem (unmounting it: "busy")? */
bool process_files_on(const struct vfs_sb *sb);

/* Called by the interrupt/syscall paths. */
__attribute__((noreturn)) void process_fault(struct interrupt_frame *frame, const char *what);
void process_exit_if_killed(void);

/* Called by the scheduler once the process's thread is gone for good. */
void process_thread_reaped(struct process *p);
