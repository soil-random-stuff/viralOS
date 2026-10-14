/*
 * The interface between the kernel and user programs. Included by both, so
 * it may only use plain C types (both sides are x86_64, so pointers in
 * structs have the same layout).
 *
 * System call convention (x86_64 `syscall` instruction):
 *   rax = call number, arguments in rdi, rsi, rdx, r10, r8, r9
 *   result in rax: >= 0 on success, -errno on failure
 *   rcx and r11 are clobbered
 */
#pragma once

#include <stdint.h>

#define OS_NAME_MAX 256   /* a name is at most 255 bytes of UTF-8, plus the NUL */
#define OS_PATH_MAX 1024

/* viralOS: the kernel is viralKernel. */
#define VIRALKERNEL_VERSION "0.6"

#define SYS_EXIT     0   /* exit(code)                         never returns */
#define SYS_WRITE    1   /* write(fd, buf, len)              -> bytes written */
#define SYS_READ     2   /* read(fd, buf, len)               -> bytes read, 0 = end of file */
#define SYS_OPEN     3   /* open(path, flags)                -> fd */
#define SYS_CLOSE    4   /* close(fd) */
#define SYS_STAT     5   /* stat(path, struct os_file_info *) */
#define SYS_READDIR  6   /* readdir(fd, index, struct os_file_info *); -ENOENT past the end */
#define SYS_SLEEP    7   /* sleep_ms(ms) */
#define SYS_GETPID   8   /* getpid()                         -> process id */
#define SYS_UPTIME   9   /* uptime_ms()                      -> milliseconds since boot */
#define SYS_GETCWD  10   /* getcwd(buf, size)                -> length of the path */
#define SYS_SPAWN   11   /* spawn(const struct os_spawn *)   -> pid of the new child */
#define SYS_WAIT    12   /* wait(pid or -1 for any, int *status, flags) -> pid, 0 (WNOHANG) */
#define SYS_CHDIR   13   /* chdir(path) */
#define SYS_MKDIR   14   /* mkdir(path) */
#define SYS_UNLINK  15   /* unlink(path): remove a file or empty directory */
#define SYS_KILL    16   /* kill(pid) */
#define SYS_BRK     17   /* brk(new_end)                     -> the (new) end of the heap; brk(0) asks */
#define SYS_PROCINFO 18  /* procinfo(index, struct os_proc_info *); -ENOENT past the end */
#define SYS_SETFG   19   /* set_foreground(pid): who gets Ctrl+C; 0 = nobody */
#define SYS_SYSINFO 20   /* sysinfo(struct os_sysinfo *) */
#define SYS_TTY_MODE 21  /* tty_mode(OS_TTY_*)               -> the previous mode */

/* Console modes. Cooked: line-at-a-time with echo and editing (the default).
   Raw: every key as it's pressed, no echo; arrows etc. as ANSI escape
   sequences ("\x1b[A" up, "\x1b[B" down, "\x1b[C" right, "\x1b[D" left,
   "\x1b[H" home, "\x1b[F" end, "\x1b[3~" delete). Ctrl+C arrives as 3 when
   no foreground program claims it. Raw mode ends when its process exits. */
#define OS_TTY_COOKED 0
#define OS_TTY_RAW    1

/* --- graphics ---------------------------------------------------------------- */

#define SYS_DISPLAY_CLAIM   22  /* display_claim(struct os_display_info *): take over the screen */
#define SYS_DISPLAY_RELEASE 23  /* display_release(): give it back to the text console */
#define SYS_INPUT_READ      24  /* input_read(events, max, timeout_ms) -> count; 0 on timeout.
                                   timeout -1 waits forever, 0 just checks. Display owner only. */

/*
 * The framebuffer, mapped into the claiming process at `address`. One
 * process owns the screen at a time; while it does, the text console stops
 * drawing and the keyboard and mouse go to it as input events instead.
 * Ctrl+Alt+Backspace kills the owner, as an emergency exit.
 */
struct os_display_info {
    uint64_t address;            /* first pixel */
    uint32_t width, height;
    uint32_t pitch;              /* bytes from one row to the next */
    uint32_t bpp;                /* bits per pixel (32) */
    uint8_t red_shift, green_shift, blue_shift;   /* bit position of each 8-bit channel */
    uint8_t reserved[5];
};

enum os_event_type { OS_EVENT_KEY = 1, OS_EVENT_MOUSE = 2 };

#define OS_MOD_SHIFT 1
#define OS_MOD_CTRL  2
#define OS_MOD_ALT   4

#define OS_MOUSE_LEFT   1
#define OS_MOUSE_RIGHT  2
#define OS_MOUSE_MIDDLE 4

/* Key codes: PS/2 set 1 make codes; keys with the E0 prefix get 0x100 added. */
#define OS_KEY_ESCAPE    0x01
#define OS_KEY_BACKSPACE 0x0E
#define OS_KEY_TAB       0x0F
#define OS_KEY_ENTER     0x1C
#define OS_KEY_SPACE     0x39
#define OS_KEY_F1        0x3B
#define OS_KEY_UP        0x148
#define OS_KEY_DOWN      0x150
#define OS_KEY_LEFT      0x14B
#define OS_KEY_RIGHT     0x14D
#define OS_KEY_HOME      0x147
#define OS_KEY_END       0x14F
#define OS_KEY_DELETE    0x153
#define OS_KEY_PRINT     0x137   /* Print Screen */

/* --- talking between processes ---------------------------------------------------
 *
 * Shared memory: shm_create makes a block of zeroed memory and returns a
 * handle (a file descriptor); shm_map maps it into the caller. Any process
 * holding the handle maps the same physical pages, so changes are visible
 * to all of them at once. The memory goes away when the last handle is
 * closed and the last mapping removed.
 *
 * Channels carry messages (up to OS_MSG_MAX bytes) between two ends, in
 * order, each message whole. A message can carry one handle along: the
 * receiver gets its own descriptor for the same object (this is how shared
 * memory is handed over). Receiving 0 bytes means the other end closed.
 *
 * Services: a server registers a name and accepts connections; a client
 * connects by name and gets one end of a fresh channel; the server's
 * accept returns the other end.
 *
 * poll waits until any of several descriptors has something to read (or
 * its peer hung up), or a timeout passes. OS_FD_INPUT stands for the
 * display owner's keyboard/mouse queue.
 */

#define SYS_SHM_CREATE       25  /* shm_create(size)                   -> fd */
#define SYS_SHM_MAP          26  /* shm_map(fd, uint64_t *size)        -> address */
#define SYS_SHM_UNMAP        27  /* shm_unmap(address) */
#define SYS_CHANNEL_PAIR     28  /* channel_pair(int32_t fds[2]): two connected ends */
#define SYS_SERVICE_REGISTER 29  /* service_register(name)             -> listening fd */
#define SYS_SERVICE_CONNECT  30  /* service_connect(name)              -> channel fd */
#define SYS_SERVICE_ACCEPT   31  /* service_accept(listening fd)       -> channel fd (waits) */
#define SYS_MSG_SEND         32  /* msg_send(fd, buf, len, handle or -1, flags); waits if the queue is full
                                    unless flags has OS_MSG_NOWAIT (then -EAGAIN) */
#define SYS_MSG_RECV         33  /* msg_recv(fd, buf, size, int32_t *handle) -> length, 0 = closed (waits) */
#define SYS_POLL             34  /* poll(struct os_pollfd *, count, timeout_ms) -> how many are ready */

#define OS_MSG_MAX          4096
#define OS_SERVICE_NAME_MAX 32
#define OS_SHM_MAX          (64u * 1024 * 1024)
#define OS_POLL_MAX         32

#define OS_MSG_NOWAIT 1          /* msg_send: fail with -EAGAIN instead of waiting for room */

#define OS_FD_INPUT (-2)         /* poll: the display owner's input events */

#define OS_POLL_IN  1            /* something to receive / accept / read */
#define OS_POLL_HUP 2            /* the other end of the channel is gone */

/* --- time and terminals ----------------------------------------------------------- */

#define SYS_TIME     35  /* time()                             -> seconds since 1970 (wall clock) */
#define SYS_PTY_OPEN 36  /* pty_open(int32_t fds[2]): [0] the terminal window's end,
                            [1] the programs' end (give it to them as stdin/out/err) */

#define SYS_RENAME   37  /* rename(from, to): move/rename a file or folder; `to` must not exist */
#define SYS_POWER    38  /* power(OS_POWER_*): turn the machine off or restart it */

#define OS_POWER_OFF     0
#define OS_POWER_RESTART 1

/*
 * Sound output (one program at a time). Samples are signed 16-bit,
 * interleaved when stereo. The device plays from a ring buffer of
 * OS_AUDIO_BUFFER bytes; what isn't refilled in time plays as silence.
 */
#define SYS_AUDIO_OPEN  39  /* audio_open(rate, channels): play sound (several programs at once: mixed).
                               -ENODEV: no sound card, -EBUSY: too many playing, -EINVAL: bad format */
#define SYS_AUDIO_WRITE 40  /* audio_write(buf, bytes) -> bytes queued; waits for room
                               (OS_AUDIO_NOWAIT in the third argument: queue what fits now) */
#define SYS_AUDIO_CTL   41  /* audio_ctl(OS_AUDIO_*, arg) */

#define OS_AUDIO_BUFFER  (64 * 1024)
#define OS_AUDIO_NOWAIT  1

#define OS_AUDIO_CLOSE   0   /* stop and give the output back (also happens on exit) */
#define OS_AUDIO_PAUSE   1
#define OS_AUDIO_RESUME  2
#define OS_AUDIO_FLUSH   3   /* drop everything queued (e.g. when seeking) */
#define OS_AUDIO_DRAIN   4   /* wait until everything queued has played */
#define OS_AUDIO_QUEUED  5   /* -> bytes queued but not yet played */

/* The system-wide volume, mute and output: the panel's volume control. */
#define SYS_AUDIO_MIXER 42  /* audio_mixer(OS_MIXER_*, value, struct os_audio_mixer *out or NULL) */

#define OS_MIXER_GET         0
#define OS_MIXER_SET_VOLUME  1   /* 0-100 */
#define OS_MIXER_SET_MUTE    2   /* 0 or 1 */
#define OS_MIXER_SET_OUTPUT  3   /* index into outputs[] */

#define OS_AUDIO_OUTPUTS_MAX 4

struct os_audio_mixer {
    uint32_t volume;
    uint32_t muted;
    int32_t output;                              /* which one plays */
    int32_t output_count;
    char outputs[OS_AUDIO_OUTPUTS_MAX][48];      /* "Speakers (HD Audio)", ... */
};

/*
 * Drives: USB sticks and disks, mounted by the kernel at /media/<label>
 * when they're plugged in (FAT and exFAT read-write; ext4 and NTFS read-only, or read-write
 * through userfs when their mount options say "rw").
 */
#define SYS_DRIVES 43  /* drives(index, struct os_drive_info *); -ENOENT past the end */
#define SYS_EJECT  44  /* eject(mount path): write everything out and unmount every drive on that disk */

#define OS_DRIVE_READ_ONLY 1
#define OS_DRIVE_REMOVABLE 2   /* the medium comes out (card readers, some sticks) */
#define OS_DRIVE_EXTERNAL  4   /* plugged in from outside (USB), not built in */

/* Disks and partitions (the Disks app). Their contents are the files /dev/<name>. */
#define SYS_SEEK     45  /* seek(fd, offset, whence: OS_SEEK_*) -> the new offset */
#define SYS_FSYNC    46  /* fsync(fd): everything written to it is on the disk */
#define SYS_BLOCKDEV 47  /* blockdev(index, struct os_block_info *); -ENOENT past the end */
#define SYS_MOUNT    48  /* mount(device name, path or NULL, OS_MOUNT_*): mount it (NULL: where it would go by itself) */
#define OS_MOUNT_WRITABLE 1   /* read-write even where it'd be read-only at first (ext4) */
#define SYS_UNMOUNT  49  /* unmount(path): unmount that one filesystem */
#define SYS_RESCAN   50  /* rescan(disk name): read its partition table again (-EBUSY if mounted) */

#define OS_SEEK_SET 0
#define OS_SEEK_CUR 1
#define OS_SEEK_END 2

#define OS_BLOCK_READ_ONLY 1
#define OS_BLOCK_REMOVABLE 2
#define OS_BLOCK_DISK      4     /* a whole disk (else a partition of `disk`) */
#define OS_BLOCK_EXTERNAL  8     /* plugged in from outside (USB), not built in */

struct os_block_info {
    char name[16];               /* "usb0p1": /dev/usb0p1 */
    char disk[16];               /* the disk it's on ("usb0"; itself for a disk) */
    char model[48];              /* "WDC WD10SPZX-24Z10" */
    char serial[40];
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t flags;              /* OS_BLOCK_* */
    char table[8];               /* disks: "gpt", "mbr", "" */
    int32_t partition;           /* its number on the disk (1..), 0 for a disk */
    uint32_t mbr_type;           /* MBR partitions: the type byte */
    uint64_t start;              /* first sector on the disk */
    uint8_t type_guid[16];       /* GPT partitions */
    uint8_t part_guid[16];
    char part_name[72];
    char mounted_at[OS_PATH_MAX];   /* "" if not mounted */
};

struct os_drive_info {
    char path[OS_PATH_MAX];      /* "/media/MYSTICK" */
    char label[OS_NAME_MAX];     /* the volume label, may be empty */
    char filesystem[16];         /* "FAT32", "exFAT", "ext4", "NTFS" */
    char device[16];             /* "usb0p1" */
    char model[48];              /* "SanDisk Cruzer Blade" */
    uint64_t total;              /* bytes */
    uint64_t free;
    uint32_t flags;              /* OS_DRIVE_* */
    uint32_t reserved;
    char uuid[40];               /* the filesystem's serial/UUID as blkid shows it ("1A2B-3C4D"), may be empty */
};

/*
 * Filesystems served by a program (userfs): NTFS mounted read-write is run by a port of the Linux
 * tools' library (/usr/bin/ntfsd), which the kernel starts when a
 * filesystem's mount options say "rw". The program opens the device, then calls userfs(): the
 * filesystem appears in the tree, and the kernel sends it requests on the returned channel
 * (struct os_userfs_request; it answers each with a struct os_userfs_reply), with file data and
 * folder listings in a shared buffer. Every file and folder is a node number the program chose
 * (never 0). When the program exits, the filesystem goes away; on unmount it gets OS_UFS_DESTROY,
 * answers once everything is on the disk, and exits.
 *
 * Ported Linux programs reach the system calls here by adding OS_LINUX_NATIVE_BASE to the number.
 */
#define SYS_USERFS 51  /* userfs(struct os_userfs *) -> the channel fd */

#define OS_LINUX_NATIVE_BASE 0x10000
#define OS_USERFS_BUFFER     (256 * 1024)
#define OS_USERFS_CASE_INSENSITIVE 1   /* "Readme.TXT" finds "README.txt" */

struct os_userfs {
    char device[16];             /* in: the disk or partition ("nvme0p2") */
    char path[OS_PATH_MAX];      /* in: where; "" for the configured place or /media/<label> */
    char fs_name[16];            /* in: shown to the user ("NTFS") */
    char label[OS_NAME_MAX];     /* in */
    char uuid[40];               /* in: as blkid shows it */
    uint64_t root;               /* in: the root folder's node */
    uint32_t flags;              /* in: OS_USERFS_* */
    int32_t buffer_fd;           /* out: the shared buffer (shm_map it) */
};

enum os_userfs_op {
    OS_UFS_LIST = 1,   /* node: a folder; offset: the entry to start at -> entries in the buffer */
    OS_UFS_READ,       /* node, offset, length -> bytes in the buffer */
    OS_UFS_WRITE,      /* node, offset, length bytes from the buffer -> bytes written */
    OS_UFS_TRUNCATE,   /* node, length: the new size */
    OS_UFS_CREATE,     /* dir, name, type -> reply.node */
    OS_UFS_UNLINK,     /* dir, node (an empty folder or a file), name */
    OS_UFS_RENAME,     /* dir, node, name -> new_dir, new_name (free there) */
    OS_UFS_STATFS,     /* -> reply.total, reply.free */
    OS_UFS_SYNC,       /* write everything out */
    OS_UFS_DESTROY,    /* unmounting: write everything out, answer, then exit */
};

struct os_userfs_request {
    uint32_t op;                 /* enum os_userfs_op */
    uint32_t type;               /* CREATE: OS_FILE or OS_DIRECTORY */
    uint32_t seq;                /* copied into the reply */
    uint32_t reserved;
    uint64_t node;
    uint64_t dir;
    uint64_t new_dir;
    uint64_t offset;
    uint64_t length;
    char name[OS_NAME_MAX];
    char new_name[OS_NAME_MAX];
};

struct os_userfs_reply {
    int64_t result;              /* bytes, entries, 0, or -errno */
    uint64_t node;               /* CREATE */
    uint64_t total, free;        /* STATFS */
    uint32_t more;               /* LIST: more entries after these (ask from offset + result) */
    uint32_t seq;                /* the request's */
};

/* LIST: entries one after another in the buffer, each padded to 8 bytes. */
struct os_userfs_entry {
    uint64_t node;
    uint64_t size;
    int64_t mtime;               /* seconds since 1970 */
    uint32_t type;               /* OS_FILE or OS_DIRECTORY */
    uint16_t name_len;           /* without the NUL that follows the name */
    uint16_t reserved;
    char name[];
};

#define OS_USERFS_ENTRY_SIZE(name_len) ((sizeof(struct os_userfs_entry) + (name_len) + 1 + 7) & ~(size_t)7)

/* --- networking ---------------------------------------------------------------------- */

/*
 * Sockets work like BSD/Linux ones (the same constants and address layouts), on the kernel's
 * TCP/IP stack (lwIP). A socket is a file descriptor: read/write/close/poll work on it too.
 * sendto/recvfrom take their extra arguments in a struct os_sockmsg (six don't fit in registers).
 * Addresses are struct sockaddr_in (IPv4): family, port and address in network byte order.
 *
 * Network adapters ("eth0", ...): netif(index, info) describes the index-th one; netif_set()
 * changes one (OS_NETIF_SET_*). Their settings are saved in /etc/network.conf by netctl.
 */
#define SYS_SOCKET      52  /* socket(OS_AF_INET, OS_SOCK_* [| OS_SOCK_NONBLOCK], protocol) -> fd */
#define SYS_BIND        53  /* bind(fd, const struct sockaddr *, len) */
#define SYS_CONNECT     54  /* connect(fd, const struct sockaddr *, len) */
#define SYS_LISTEN      55  /* listen(fd, backlog) */
#define SYS_ACCEPT      56  /* accept(fd, struct sockaddr * or NULL, uint32_t *len) -> fd */
#define SYS_SENDTO      57  /* sendto(fd, struct os_sockmsg *)  -> bytes sent */
#define SYS_RECVFROM    58  /* recvfrom(fd, struct os_sockmsg *) -> bytes received (msg.addrlen updated) */
#define SYS_SETSOCKOPT  59  /* setsockopt(fd, level, name, const void *value, len) */
#define SYS_GETSOCKOPT  60  /* getsockopt(fd, level, name, void *value, uint32_t *len) */
#define SYS_SOCKNAME    61  /* sockname(fd, 0 = ours / 1 = the peer's, struct sockaddr *, uint32_t *len) */
#define SYS_SHUTDOWN    62  /* shutdown(fd, OS_SHUT_*) */
#define SYS_RESOLVE     63  /* resolve(name, uint32_t *ipv4 out): a host name (DNS) or "1.2.3.4" */
#define SYS_NETIF       64  /* netif(index, struct os_netif_info *); -ENOENT past the end */
#define SYS_NETIF_SET   65  /* netif_set(const struct os_netif_config *) */
#define SYS_INTERRUPT   66  /* interrupt(OS_INTERRUPT_*): what Ctrl+C does to this program */
#define SYS_UPTIME_US   67  /* uptime_us() -> microseconds since boot */

/*
 * The user a program runs for (the login screen sets it before starting the desktop; programs
 * started from then on inherit it). Their home is /home/<name>. No permissions are enforced yet.
 */
#define SYS_USER        68  /* user(OS_USER_GET, char buf[OS_USER_MAX]) / user(OS_USER_SET, name) */
#define SYS_CHMOD       69  /* chmod(path, mode): a file's or folder's permission bits (07777) */
#define SYS_GETRANDOM   70  /* getrandom(buf, len) -> len: random bytes good enough for keys */
#define OS_USER_GET     0
#define OS_USER_SET     1
#define OS_USER_MAX     32

#define OS_AF_INET        2
#define OS_SOCK_STREAM    1
#define OS_SOCK_DGRAM     2
#define OS_SOCK_RAW       3
#define OS_SOCK_NONBLOCK  0x800      /* (or'ed into the type) */
#define OS_IPPROTO_IP     0
#define OS_IPPROTO_ICMP   1
#define OS_IPPROTO_TCP    6
#define OS_IPPROTO_UDP    17

#define OS_MSG_PEEK       0x02
#define OS_MSG_DONTWAIT   0x40

#define OS_SHUT_RD   0
#define OS_SHUT_WR   1
#define OS_SHUT_RDWR 2

/* setsockopt/getsockopt (Linux's numbers) */
#define OS_SOL_SOCKET     1
#define OS_SO_REUSEADDR   2
#define OS_SO_ERROR       4
#define OS_SO_BROADCAST   6
#define OS_SO_SNDBUF      7
#define OS_SO_RCVBUF      8
#define OS_SO_KEEPALIVE   9
#define OS_SO_RCVTIMEO    20         /* value: struct os_timeval */
#define OS_SO_SNDTIMEO    21
#define OS_IP_TOS         1          /* level OS_IPPROTO_IP */
#define OS_IP_TTL         2
#define OS_TCP_NODELAY    1          /* level OS_IPPROTO_TCP */

struct os_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct os_sockmsg {
    void *buf;
    uint64_t len;
    void *addr;                  /* struct sockaddr *: sendto's destination / recvfrom's sender, or NULL */
    uint32_t addrlen;            /* in: the size of *addr; out (recvfrom): the address's size */
    int32_t flags;               /* OS_MSG_* */
};

/* poll: also OS_POLL_OUT (room to send; a connect() finished) and OS_POLL_ERR. */
#define OS_POLL_OUT 4
#define OS_POLL_ERR 8

/* Network adapters */
enum os_netif_state {
    OS_NETIF_UNAVAILABLE = 0,    /* no cable / no link */
    OS_NETIF_DISCONNECTED,       /* turned off (netctl device down) */
    OS_NETIF_CONNECTING,         /* asking for an address (DHCP) */
    OS_NETIF_CONNECTED,          /* has an address */
};

enum os_netif_method {
    OS_NETIF_AUTO = 0,           /* DHCP */
    OS_NETIF_MANUAL = 1,         /* a fixed address */
};

#define OS_NETIF_DEFAULT_ROUTE 1 /* flags: the internet goes out through this one */
#define OS_NETIF_ENABLED       2 /* flags: on (connects when there's a link) */
#define OS_NETIF_LOOPBACK      4

struct os_netif_info {
    char name[16];               /* "eth0", "lo" */
    char driver[16];             /* "e1000e" */
    char model[64];              /* "Intel 82574L Gigabit Ethernet" */
    uint8_t mac[6];
    uint16_t mtu;
    uint32_t state;              /* enum os_netif_state */
    uint32_t method;             /* enum os_netif_method */
    uint32_t flags;              /* OS_NETIF_* */
    uint32_t speed_mbps;         /* link speed; 0 = unknown */
    uint32_t full_duplex;
    /* IPv4, network byte order (0 = none) */
    uint32_t address, netmask, gateway;
    uint32_t dns[2];             /* the servers in use (from DHCP, or set) */
    uint32_t dhcp_server;
    uint32_t lease_seconds;      /* DHCP lease length, 0 = none */
    uint32_t reserved;
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_errors, tx_dropped;
};

enum os_netif_op {
    OS_NETIF_SET_CONFIG = 1,     /* method, address/netmask/gateway (manual), dns (0 = from DHCP) */
    OS_NETIF_SET_UP,             /* turn on and connect */
    OS_NETIF_SET_DOWN,           /* disconnect and stay off */
    OS_NETIF_SET_RENEW,          /* ask DHCP again */
};

struct os_netif_config {
    char name[16];
    uint32_t op;                 /* enum os_netif_op */
    uint32_t method;             /* SET_CONFIG: enum os_netif_method */
    uint32_t address, netmask, gateway;
    uint32_t dns[2];
};

/* What Ctrl+C does (SYS_INTERRUPT): */
#define OS_INTERRUPT_KILL  0     /* end the program (the default) */
#define OS_INTERRUPT_CATCH 1     /* only note it: blocking calls return -EINTR, then OS_INTERRUPT_CHECK */
#define OS_INTERRUPT_CHECK 2     /* -> 1 if Ctrl+C was pressed since the last check (and clears it) */

/* SYS_SETFG and SYS_TTY_MODE act on the caller's terminal: the console, or
   the pseudo-terminal its stdin/stdout/stderr is. */

struct os_pollfd {
    int32_t fd;
    uint16_t events;             /* what to wait for (OS_POLL_IN) */
    uint16_t revents;            /* filled in: what is ready */
};

struct os_input_event {
    uint32_t type;               /* enum os_event_type */
    uint32_t modifiers;          /* OS_MOD_* held at the time */
    uint64_t time_ms;            /* uptime when it happened */
    /* OS_EVENT_KEY */
    uint32_t key;                /* OS_KEY_* / key code */
    uint32_t pressed;            /* 1 = down (or auto-repeat), 0 = released */
    uint32_t ch;                 /* the character it types (with Shift etc.), 0 if none */
    /* OS_EVENT_MOUSE */
    uint32_t buttons;            /* OS_MOUSE_* currently held */
    int32_t x, y;                /* pointer position on the screen */
    int32_t dx, dy;              /* movement since the previous event */
    int32_t wheel;               /* scroll: negative = up / away from you */
    uint32_t reserved;
};

/* wait() flags */
#define WNOHANG 1        /* don't block; return 0 if no child has exited yet */

/* For spawn(): which of the caller's descriptors become the child's 0, 1, 2. */
#define OS_SPAWN_INHERIT (-1)   /* the caller's own descriptor with the same number */

struct os_spawn {
    const char *path;            /* executable */
    const char *const *argv;     /* NULL-terminated; argv[0] is the program name */
    int32_t fds[3];              /* caller fds for the child's stdin, stdout, stderr */
    uint32_t flags;              /* OS_SPAWN_* */
};

#define OS_SPAWN_DETACHED 1      /* not our child: nobody waits for it, it cleans up after itself */

enum os_proc_state { OS_PROC_RUNNING = 0, OS_PROC_SLEEPING = 1, OS_PROC_BLOCKED = 2,
                     OS_PROC_ZOMBIE = 3 };

struct os_proc_info {
    int64_t pid;
    int64_t parent;              /* 0 = started by the kernel */
    char name[32];
    uint32_t state;              /* enum os_proc_state */
    uint32_t reserved;
    uint64_t cpu_ms;
    uint64_t memory;             /* bytes of user memory mapped */
    char user[OS_USER_MAX];      /* who it runs for ("" = nobody: started before anyone logged in) */
};

#define OS_FIRMWARE_BIOS 1
#define OS_FIRMWARE_UEFI 2

struct os_sysinfo {
    uint64_t memory_total;       /* bytes of RAM the kernel manages */
    uint64_t memory_free;
    uint64_t kernel_heap_used;
    uint64_t uptime_ms;
    uint32_t processes;
    uint32_t cpus;
    uint32_t firmware;           /* how the machine started: OS_FIRMWARE_BIOS, OS_FIRMWARE_UEFI (0: unknown) */
    uint32_t reserved;
    uint64_t idle_ms;            /* time the CPUs spent with nothing to run, added up: CPU use is
                                    1 - (change in idle_ms) / (change in uptime_ms * cpus) */
};

/* open() flags (same values as Linux, so a ported program's own <fcntl.h> agrees) */
#ifndef O_RDONLY
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_ACCMODE 0x003
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

enum os_file_type { OS_FILE = 0, OS_DIRECTORY = 1 };

struct os_file_info {
    char name[OS_NAME_MAX];
    uint32_t type;      /* enum os_file_type */
    uint32_t mode;      /* permission bits, e.g. 0755 */
    uint64_t size;
    uint64_t mtime;     /* last changed: seconds since 1970, 0 if unknown */
    uint64_t ctime;     /* created, same units */
};

/* Error numbers returned (negated) by system calls. */
#define EPERM         1
#define ENOENT        2
#define EINTR         4
#define EIO           5
#define ENXIO         6
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18   /* rename across drives: copy and delete instead */
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOSPC       28
#define EROFS        30   /* the drive is read-only */
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define EAGAIN       11
#define EPIPE        32
#define EMSGSIZE     90
#define ECONNREFUSED 111
/* networking (Linux's numbers) */
#define ENFILE           23
#define ERANGE           34
#define EWOULDBLOCK      EAGAIN
#define ENOTSOCK         88
#define EDESTADDRREQ     89
#define EPROTOTYPE       91
#define ENOPROTOOPT      92
#define EPROTONOSUPPORT  93
#define EOPNOTSUPP       95
#define EAFNOSUPPORT     97
#define EADDRINUSE       98
#define EADDRNOTAVAIL    99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ESHUTDOWN       108
#define ETIMEDOUT       110
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
