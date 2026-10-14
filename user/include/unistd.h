#pragma once

/* System calls. See <viralos.h> for flags, error numbers and structs. */

#include <stddef.h>
#include <stdint.h>
#include <viralos.h>

/* All return >= 0 on success or a negative error number (-ENOENT, ...). */
__attribute__((noreturn)) void exit(int code);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);
int open(const char *path, int flags);
int close(int fd);
int stat(const char *path, struct os_file_info *info);
int readdir(int fd, uint64_t index, struct os_file_info *info);
int sleep_ms(uint64_t ms);
long getpid(void);
uint64_t uptime_ms(void);
long getcwd(char *buf, size_t size);
int chdir(const char *path);
int mkdir(const char *path);
int unlink(const char *path);
int rename(const char *from, const char *to);   /* move or rename; `to` must not exist */

/* Processes */
long spawn(const struct os_spawn *args);
/* Start `path` with `argv` (NULL-terminated), sharing our stdin/stdout/stderr. */
long spawn_simple(const char *path, const char *const *argv);
/* Wait for child `pid` (-1 = any); returns its pid, 0 with WNOHANG if none finished. */
long wait(long pid, int *status, int flags);
int kill(long pid);
int set_foreground(long pid);
int procinfo(uint64_t index, struct os_proc_info *info);
int sysinfo(struct os_sysinfo *info);

/* Console mode: OS_TTY_COOKED (lines, the default) or OS_TTY_RAW (keys). Returns the old one. */
int tty_mode(int mode);

/* Turn the computer off (OS_POWER_OFF) or restart it (OS_POWER_RESTART). Returns only on failure. */
int power(int how);
/* Sound output: 16-bit samples (see SYS_AUDIO_* in <viralos.h>). */
int audio_open(uint32_t rate, uint32_t channels);
long audio_write(const void *buf, size_t bytes, int flags);
long audio_ctl(int op, long arg);
/* The system volume/mute/output (OS_MIXER_*); fills *out (if given) with the new state. */
int audio_mixer(int op, long value, struct os_audio_mixer *out);

/* Mounted drives (/media/...): the index-th one's details; -ENOENT past the last. */
int drives(int index, struct os_drive_info *info);
/* Write everything out and unmount the drive at `path` (and the rest of that disk): safe to unplug. */
int eject(const char *path);

/* Move a file's position (OS_SEEK_SET / CUR / END); returns the new one. */
long seek(int fd, long offset, int whence);
/* Everything written to fd is on the disk. */
int fsync(int fd);
/* Disks and partitions (their contents: /dev/<name>): the index-th one; -ENOENT past the last. */
int blockdev(int index, struct os_block_info *info);
/* Mount /dev/<device> at `path` (NULL: where it would go by itself); unmount one filesystem. */
int mount_device(const char *device, const char *path);
/* The same, read-write even where a disk would come up read-only (another system's ext4). */
int mount_device_writable(const char *device, const char *path);
/* A file's or folder's permission bits (07777). */
int chmod(const char *path, uint32_t mode);
int unmount(const char *path);
/* Unmount every mounted drive (before shutting down). Returns how many couldn't be; `failed` (may be
   NULL) gets them as "/media/STICK (in use), ...". */
int unmount_drives_all(char *failed, size_t size);
/* Read the disk's partition table again (after changing it). */
int rescan(const char *disk);

/* Pseudo-terminal: fds[0] for the terminal window, fds[1] for the programs in it. */
int pty_open(int32_t fds[2]);

/* Random bytes from the kernel, good enough for keys. Returns len, or a negative error. */
int getrandom(void *buf, size_t len);

/* Wall clock, seconds since 1970 (in local time: see <time.h>). */
uint64_t wall_time(void);

/* Graphics: take over the screen, read keyboard/mouse events (timeout -1 = wait forever). */
int display_claim(struct os_display_info *info);
int display_release(void);
int input_read(struct os_input_event *events, int max, long timeout_ms);

/*
 * Talking between processes (details in <viralos.h>).
 *   shm_create / shm_map     shared memory: a handle, then a pointer to the same pages anywhere
 *   service_register/accept  a server waiting for clients by name
 *   service_connect          a client: returns its end of a new channel
 *   msg_send / msg_recv      whole messages; one handle may ride along (-1 = none)
 *   poll                     wait for any of several descriptors (or OS_FD_INPUT)
 */
int shm_create(uint64_t size);
void *shm_map(int fd, uint64_t *size);          /* NULL on failure */
int shm_unmap(void *address);
int channel_pair(int32_t fds[2]);
int service_register(const char *name);
int service_connect(const char *name);
int service_accept(int listener);
int msg_send(int fd, const void *data, size_t len, int handle);
int msg_send_flags(int fd, const void *data, size_t len, int handle, int flags);   /* OS_MSG_NOWAIT */
long msg_recv(int fd, void *buf, size_t size, int *handle);   /* 0 = the other end closed */
int poll(struct os_pollfd *fds, int count, long timeout_ms);

/* Memory: move the end of the heap (brk) or grow it by `increment` bytes (sbrk). */
uintptr_t brk(uintptr_t new_end);
void *sbrk(intptr_t increment);   /* returns the old end, or (void *)-1 */

/* Network adapters ("eth0", ..., "lo" last): the index-th one's details (-ENOENT past the last);
   change one (OS_NETIF_SET_*: settings, up, down, renew DHCP). Sockets: <sys/socket.h>. */
int netif_info(int index, struct os_netif_info *info);
int netif_set(const struct os_netif_config *config);

/* What Ctrl+C does to this program: OS_INTERRUPT_KILL (the default) or OS_INTERRUPT_CATCH (blocking
   calls return -EINTR instead); OS_INTERRUPT_CHECK returns 1 once after each Ctrl+C. */
int interrupt_mode(int op);

/* Microseconds since boot (finer than uptime_ms). */
uint64_t uptime_us(void);

/* The user this program runs for (inherited from whoever started it; "user" before anyone logged in),
   their home folder (/home/<name>) and its Desktop folder. set_user: for the login screen. */
const char *user_name(void);
const char *home_dir(void);
const char *desktop_dir(void);
int set_user(const char *name);
/* Any user's home folder: from their /etc/passwd line, else /home/<name>. */
void user_home(const char *name, char *out, size_t size);
/* Show `prompt` and read a line from the terminal without echoing it (a password). Returns it (in a
   buffer of its own, overwritten by the next call), or NULL if cancelled with Ctrl+C or Ctrl+D. */
char *getpass(const char *prompt);
/* Make `name`'s home folder and the usual folders in it (Desktop, Documents, Downloads, Music,
   Pictures, .config/dde) where they're missing. */
void make_home_dirs(const char *name);

/* Text for an error number (either sign). */
const char *strerror(int err);
