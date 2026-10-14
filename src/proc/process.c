#include <stddef.h>

#include "proc/process.h"
#include "proc/elf.h"
#include "proc/file.h"
#include "proc/linux.h"
#include "proc/tty.h"
#include "proc/usercopy.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/syscall.h"
#include "drivers/hda.h"
#include "drivers/display.h"
#include "drivers/input.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "fs/vfs.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

#define MAX_EXECUTABLE_SIZE (16 * 1024 * 1024)
#define USER_HEAP_MAX       (1ULL << 30)   /* brk() can grow the heap to 1 GiB */

/*
 * Locking: the process list, parent/child links and refs are
 * only touched with interrupts off (irq_save), like the scheduler's lists.
 */
static struct process *all_processes;
static uint64_t next_pid = 1;

/* --- reference counting --------------------------------------------------------- */

static void process_unlink(struct process *p) {
    for (struct process **link = &all_processes; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            return;
        }
    }
}

static void unlink_child(struct process *parent, struct process *child) {
    for (struct process **link = &parent->children; *link; link = &(*link)->sibling) {
        if (*link == child) {
            *link = child->sibling;
            return;
        }
    }
}

/* Drop a reference; the last one frees whatever is left. Interrupts must be off. */
static void process_release(struct process *p) {
    if (--p->refs > 0) {
        return;
    }
    process_unlink(p);
    if (p->space) {
        vmm_destroy_space(p->space);
    }
    linux_free_maps(p);
    kfree(p);
}

void process_thread_reaped(struct process *p) {
    /* The thread was the last thing running on these page tables. */
    vmm_destroy_space(p->space);
    p->space = 0;
    p->thread = NULL;
    process_release(p);
}

/* --- Ctrl+C ------------------------------------------------------------------------ */

void process_init(void) {
    tty_init();   /* Ctrl+C on the console stops its foreground process */
}

void process_set_foreground(struct process *p) {
    tty_set_foreground(tty_console(), p ? p->pid : 0);
}

/* --- starting a program ------------------------------------------------------------ */

/* Read a whole file into a kernel buffer. */
static int read_file(struct vnode *n, uint8_t **out, size_t *size) {
    if (n->type != VNODE_FILE) {
        return -EACCES;
    }
    if (n->size > MAX_EXECUTABLE_SIZE) {
        return -ENOEXEC;
    }
    uint8_t *buf = kmalloc(n->size ? n->size : 1);
    if (!buf) {
        return -ENOMEM;
    }
    int64_t got = vfs_read(n, 0, buf, n->size);
    if (got != (int64_t)n->size) {
        kfree(buf);
        return got < 0 ? (int)got : -EIO;
    }
    *out = buf;
    *size = n->size;
    return 0;
}

/* Allocate zeroed pages for [start, end) in a user address space. */
int process_map_zeroed(uint64_t space, uint64_t start, uint64_t end, uint32_t flags) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            return -ENOMEM;
        }
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (!vmm_map_user(space, va, phys, flags)) {
            pmm_free_page(phys);
            return -ENOMEM;
        }
    }
    return 0;
}

/*
 * Map the user stack and lay out the arguments the way the System V ABI
 * expects at _start:  [rsp] = argc, then argv[0..argc-1], NULL,
 * an empty environment (NULL) and an empty auxiliary vector (0, 0).
 * The strings themselves sit above that, at the top of the stack.
 */
static int setup_stack(struct process *p, int argc, char **argv) {
    int err = process_map_zeroed(p->space, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_WRITE);
    if (err) {
        return err;
    }

    uint64_t sp = USER_STACK_TOP;
    uint64_t arg_addrs[PROCESS_MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        if (!copy_to_user(p->space, sp, argv[i], len)) {
            return -EFAULT;
        }
        arg_addrs[i] = sp;
    }

    size_t words = 1 + (size_t)argc + 1 + 1 + 2;   /* argc, argv..., NULL, envp NULL, auxv */
    sp = ALIGN_DOWN(sp - words * 8, 16);
    uint64_t table[1 + PROCESS_MAX_ARGS + 4] = { 0 };
    table[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        table[1 + i] = arg_addrs[i];
    }
    if (!copy_to_user(p->space, sp, table, words * 8)) {
        return -EFAULT;
    }
    p->user_rsp = sp;
    return 0;
}

/* First thing the process's thread runs (in the kernel, already on its page tables). */
static void process_start(void *arg) {
    struct process *p = arg;
    enter_user_mode(p->entry, p->user_rsp);
}

static void close_all_files(struct process *p) {
    for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        if (p->files[fd]) {
            file_put(p->files[fd]);
            p->files[fd] = NULL;
        }
    }
}

struct process *process_spawn(const char *path, int argc, char **argv,
                              struct spawn_options *opts, int *err) {
    if (argc < 1 || argc > PROCESS_MAX_ARGS) {
        *err = -EINVAL;
        return NULL;
    }

    struct vnode *exe;
    *err = vfs_resolve(opts->cwd, path, &exe);
    if (*err) {
        return NULL;
    }
    if (exe->type == VNODE_FILE && !(exe->mode & 0111)) {
        vfs_put(exe);
        *err = -EACCES;   /* not marked executable */
        return NULL;
    }
    uint8_t *image = NULL;
    size_t size = 0;
    *err = read_file(exe, &image, &size);
    vfs_put(exe);
    if (*err) {
        return NULL;
    }

    struct process *p = kzalloc(sizeof(*p));
    if (!p) {
        kfree(image);
        *err = -ENOMEM;
        return NULL;
    }
    const char *base = strrchr(argv[0], '/');
    strlcpy(p->name, base ? base + 1 : argv[0], sizeof(p->name));
    p->space = vmm_create_space();
    *err = p->space ? 0 : -ENOMEM;
    struct elf_info info = { 0 };
    if (!*err) {
        *err = elf_load(p->space, image, size, &info);
    }
    kfree(image);
    p->entry = info.entry;
    p->linux_abi = info.linux_abi;
    if (!*err) {
        *err = p->linux_abi ? linux_setup_stack(p, argc, argv, &info) : setup_stack(p, argc, argv);
    }
    p->heap_start = p->heap_end = ALIGN_UP(info.image_end, PAGE_SIZE);

    for (int fd = 0; fd < 3 && !*err; fd++) {
        if (opts->files[fd]) {
            file_get(opts->files[fd]);
            p->files[fd] = opts->files[fd];
        } else if (!(p->files[fd] = file_open_console())) {
            *err = -ENOMEM;
        }
    }
    if (*err) {
        close_all_files(p);
        if (p->space) {
            vmm_destroy_space(p->space);
        }
        kfree(p);
        return NULL;
    }

    struct process *creator = process_current();
    if (creator) {
        strlcpy(p->user, creator->user, sizeof(p->user));
    }
    p->cwd = opts->cwd ? opts->cwd : vfs_root();
    vfs_get(p->cwd);
    bool owned = opts->parent || !opts->detached;
    p->refs = owned ? 2 : 1;   /* the thread, plus whoever collects the exit code */

    uint64_t irq = irq_save();
    p->pid = next_pid++;
    opts->pid = p->pid;
    p->next = all_processes;
    all_processes = p;
    if (opts->parent) {
        p->parent = opts->parent;
        p->sibling = opts->parent->children;
        opts->parent->children = p;
    }
    p->thread = thread_create_in(p->name, process_start, p, p, p->space);
    if (!p->thread) {
        /* Never ran: undo by hand (no thread means no reaper will). */
        if (p->parent) {
            unlink_child(p->parent, p);
        }
        process_unlink(p);
    }
    irq_restore(irq);

    if (!p->thread) {
        close_all_files(p);
        vfs_put(p->cwd);
        vmm_destroy_space(p->space);
        kfree(p);
        *err = -ENOMEM;
        return NULL;
    }
    return p;
}

/* --- ending ------------------------------------------------------------------------- */

struct process *process_current(void) {
    struct thread *t = sched_current();
    return t ? t->process : NULL;
}

struct process *process_find(uint64_t pid) {
    for (struct process *p = all_processes; p; p = p->next) {
        if (p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

void process_exit(int code) {
    struct process *p = process_current();
    close_all_files(p);
    vfs_put(p->cwd);
    p->cwd = NULL;
    tty_process_exited(p->pid);   /* give terminals back if we had one in raw mode */
    display_release(p->pid);      /* ...and the screen, if we had that */
    audio_process_exited(p->pid); /* ...and the sound output */
    shm_process_exit(p);          /* drop our hold on shared memory */

    uint64_t irq = irq_save();
    /* Nobody will wait for our children now: they become orphans that
       clean up after themselves (zombies among them go right away). */
    for (struct process *c = p->children, *next; c; c = next) {
        next = c->sibling;
        c->parent = NULL;
        c->sibling = NULL;
        process_release(c);
    }
    p->children = NULL;

    p->exit_code = code;
    p->exited = true;
    if (p->parent) {
        wait_queue_wake_all(&p->parent->child_waiters);
    }
    wait_queue_wake_all(&p->exit_waiters);
    irq_restore(irq);

    thread_exit();   /* the reaper frees the address space and drops the thread's ref */
}

int process_wait(struct process *p) {
    uint64_t irq = irq_save();
    while (!p->exited) {
        wait_queue_sleep(&p->exit_waiters);
    }
    int code = p->exit_code;
    process_release(p);
    irq_restore(irq);
    return code;
}

int64_t process_wait_child(int64_t pid, int *status, int flags) {
    struct process *self = process_current();
    uint64_t irq = irq_save();
    int64_t result;
    for (;;) {
        bool any = false;
        struct process *done = NULL;
        for (struct process *c = self->children; c; c = c->sibling) {
            if (pid != -1 && (int64_t)c->pid != pid) {
                continue;
            }
            any = true;
            if (c->exited) {
                done = c;
                break;
            }
        }
        if (done) {
            unlink_child(self, done);
            done->parent = NULL;
            *status = done->exit_code;
            result = (int64_t)done->pid;
            process_release(done);
            break;
        }
        if (!any) {
            result = -ECHILD;
            break;
        }
        if (flags & WNOHANG) {
            result = 0;
            break;
        }
        if (self->killed) {
            result = -EINTR;
            break;
        }
        wait_queue_sleep(&self->child_waiters);
    }
    irq_restore(irq);
    return result;
}

bool process_kill(uint64_t pid) {
    uint64_t irq = irq_save();
    struct process *p = process_find(pid);
    bool found = p && !p->exited;
    if (found) {
        /* It exits when it next returns to user mode; wake it if it's
           waiting for a child or for the keyboard. (A sleep runs out.) */
        p->killed = true;
        wait_queue_wake_all(&p->child_waiters);
        keyboard_wake_readers();
        input_wake_readers();
        ipc_notify();   /* ...or for a message, a connection or poll */
    }
    irq_restore(irq);
    return found;
}

bool process_interrupt(uint64_t pid) {
    uint64_t irq = irq_save();
    struct process *p = process_find(pid);
    bool found = p && !p->exited;
    if (found && p->catch_interrupt) {
        p->interrupted = true;   /* wake it the same way as a kill, but it lives on */
        wait_queue_wake_all(&p->child_waiters);
        keyboard_wake_readers();
        input_wake_readers();
        ipc_notify();
    } else if (found) {
        process_kill(pid);
    }
    irq_restore(irq);
    return found;
}

int64_t process_interrupt_mode(struct process *p, int op) {
    switch (op) {
    case OS_INTERRUPT_KILL:  p->catch_interrupt = false; p->interrupted = false; return 0;
    case OS_INTERRUPT_CATCH: p->catch_interrupt = true; return 0;
    case OS_INTERRUPT_CHECK: {
        bool was = p->interrupted;
        p->interrupted = false;
        return was;
    }
    default: return -EINVAL;
    }
}

/* Killed, or Ctrl+C'd (caught): whatever it's waiting for, it should stop waiting. */
bool process_current_killed(void) {
    struct process *p = process_current();
    return p && (p->killed || p->interrupted);
}

void process_exit_if_killed(void) {
    struct process *p = process_current();
    if (p && p->killed) {
        interrupts_enable();   /* cleanup may need to sleep on locks */
        process_exit(130);     /* 128 + SIGINT, as shells report it */
    }
}

void process_fault(struct interrupt_frame *f, const char *what) {
    struct process *p = process_current();
    kprintf("\n[%s (pid %lu)] killed: %s at rip %p", p->name, p->pid, what, (void *)f->rip);
    if (f->vector == 14) {
        kprintf(", address %p (%s, %s)", (void *)read_cr2(),
                f->error_code & 1 ? "protection violation" : "not mapped",
                f->error_code & 2 ? "write" : f->error_code & 0x10 ? "execute" : "read");
    }
    kprintf("\n");
    interrupts_enable();
    process_exit(128 + 11);   /* like SIGSEGV */
}

/* --- heap ------------------------------------------------------------------------------ */

uint64_t process_brk(struct process *p, uint64_t new_end) {
    if (new_end < p->heap_start || new_end > p->heap_start + USER_HEAP_MAX) {
        return p->heap_end;
    }
    uint64_t old_top = ALIGN_UP(p->heap_end, PAGE_SIZE);
    uint64_t new_top = ALIGN_UP(new_end, PAGE_SIZE);
    if (new_top > old_top) {
        if (process_map_zeroed(p->space, old_top, new_top, VMM_WRITE) != 0) {
            /* Out of memory: give back what we got and leave the break where it was. */
            for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
                uint64_t phys;
                if (vmm_unmap_user(p->space, va, &phys) && phys) {
                    pmm_free_page(phys);
                }
            }
            return p->heap_end;
        }
    } else {
        for (uint64_t va = new_top; va < old_top; va += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_unmap_user(p->space, va, &phys) && phys) {
                pmm_free_page(phys);
            }
        }
    }
    p->heap_end = new_end;
    return new_end;
}

/* --- information ------------------------------------------------------------------------ */

uint32_t process_count(void) {
    uint64_t irq = irq_save();
    uint32_t n = 0;
    for (struct process *p = all_processes; p; p = p->next) {
        n++;
    }
    irq_restore(irq);
    return n;
}

bool process_files_on(const struct vfs_sb *sb) {
    uint64_t irq = irq_save();
    bool busy = false;
    for (struct process *p = all_processes; p && !busy; p = p->next) {
        for (int i = 0; !p->exited && i < PROCESS_MAX_FILES && !busy; i++) {
            const struct file *f = p->files[i];
            busy = f && f->kind == FILE_VNODE && f->vnode && f->vnode->sb == sb;
        }
    }
    irq_restore(irq);
    return busy;
}

int process_info(uint64_t index, struct os_proc_info *out) {
    uint64_t irq = irq_save();
    struct process *p = all_processes;
    while (p && index--) {
        p = p->next;
    }
    if (!p) {
        irq_restore(irq);
        return -ENOENT;
    }
    memset(out, 0, sizeof(*out));
    out->pid = (int64_t)p->pid;
    out->parent = p->parent ? (int64_t)p->parent->pid : 0;
    strlcpy(out->name, p->name, sizeof(out->name));
    strlcpy(out->user, p->user, sizeof(out->user));
    if (p->exited || !p->thread) {
        out->state = OS_PROC_ZOMBIE;
    } else {
        switch (p->thread->state) {
        case THREAD_SLEEPING: out->state = OS_PROC_SLEEPING; break;
        case THREAD_BLOCKED:  out->state = OS_PROC_BLOCKED; break;
        default:              out->state = OS_PROC_RUNNING; break;
        }
        out->cpu_ms = p->thread->cpu_ticks * 1000 / timer_hz();
    }
    out->memory = p->space ? vmm_count_user_pages(p->space) * PAGE_SIZE : 0;
    irq_restore(irq);
    return 0;
}
