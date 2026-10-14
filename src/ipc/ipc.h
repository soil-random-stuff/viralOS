#pragma once

/*
 * Talking between processes: message channels, named services and shared
 * memory (see <viralos.h> for the user-facing description).
 *
 * Locking: like the scheduler, these structures are only touched with
 * interrupts off (one CPU). Anything that blocks waits on one shared
 * "activity" queue that is woken whenever any channel, service or the
 * input queue changes; waiters then re-check their own condition. Simple,
 * and with a handful of processes the extra wake-ups cost nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <viralos.h>

struct file;
struct process;
struct channel;
struct listener;
struct shm;

/* ipc.c */
void ipc_notify(void);
/* Sleep until ipc_notify (or timeout_ms, 0 = no limit). Interrupts must be off. */
bool ipc_wait(uint64_t timeout_ms);
int ipc_poll(struct process *p, struct os_pollfd *fds, int count, int64_t timeout_ms);

/* channel.c */
int channel_pair(struct file **a, struct file **b);
/* `attach` (may be NULL) travels with the message; on success the message owns that reference.
   If the queue is full, waits for room, or (wait = false) fails with -EAGAIN. */
int channel_send(struct file *f, const void *data, size_t len, struct file *attach, bool wait);
/* Waits for a message. Returns its length, 0 if the other end closed, or -errno. */
int64_t channel_recv(struct file *f, void *buf, size_t size, struct file **attached);
void channel_close(struct channel *c, int end);
uint16_t channel_poll(struct file *f);

/* service.c */
int service_register(const char *name, struct file **out);
int service_connect(const char *name, struct file **out);
int service_accept(struct file *listener, struct file **out);
void service_close(struct listener *l);
uint16_t service_poll(struct file *f);

/* shm.c */
int shm_create(uint64_t size, struct file **out);
int64_t shm_map(struct process *p, struct file *f, uint64_t *size);   /* address or -errno */
int shm_unmap(struct process *p, uint64_t address);
void shm_put(struct shm *s);
void shm_process_exit(struct process *p);
/* The kernel's own access: copy `len` bytes at `offset` of the object into it (into = true) or out. */
int shm_copy(struct file *f, uint64_t offset, void *buf, size_t len, bool into);
