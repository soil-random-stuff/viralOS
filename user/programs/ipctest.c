/*
 * ipctest: shared memory, channels, services and poll between two processes.
 *
 * Run without arguments: this process registers the service "ipctest" and
 * starts a second copy of itself ("ipctest client") which connects. They
 * then hand a shared memory block across in a message, both write to it,
 * bounce messages back and forth for speed, and check that closing one
 * end is noticed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define SERVICE   "ipctest"
#define SHM_SIZE  (1024 * 1024)
#define ROUNDS    2000

static int failures;

static void check(bool ok, const char *what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    failures += !ok;
}

static long recv_text(int fd, char *buf, size_t size, int *handle) {
    long n = msg_recv(fd, buf, size - 1, handle);
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

static int client(void) {
    int ch = service_connect(SERVICE);
    if (ch < 0) {
        printf("client: connect: %s\n", strerror(ch));
        return 1;
    }
    char hello[64];
    snprintf(hello, sizeof(hello), "hello from pid %ld", getpid());
    msg_send(ch, hello, strlen(hello), -1);

    /* A megabyte of shared memory, filled with a pattern, handed over by message. */
    int shm = shm_create(SHM_SIZE);
    uint64_t size;
    uint32_t *mem = shm_map(shm, &size);
    for (uint32_t i = 0; i < SHM_SIZE / 4; i++) {
        mem[i] = i * 2654435761u;
    }
    msg_send(ch, "shm", 3, shm);
    close(shm);   /* our mapping keeps it alive */

    /* The server writes a reply into the same memory, then tells us. */
    char buf[256];
    recv_text(ch, buf, sizeof(buf), NULL);
    bool seen = strcmp(buf, "look") == 0 && strcmp((char *)mem, "written by the server") == 0;
    msg_send(ch, seen ? "seen" : "not seen", seen ? 4 : 8, -1);

    /* Ping-pong. */
    for (;;) {
        long n = msg_recv(ch, buf, sizeof(buf), NULL);
        if (n <= 0 || (n == 4 && memcmp(buf, "done", 4) == 0)) {
            break;
        }
        msg_send(ch, buf, (size_t)n, -1);
    }
    shm_unmap(mem);
    return 0;   /* exiting closes our end: the server should notice */
}

static int server(void) {
    printf("ipctest: two processes talking\n");
    int listener = service_register(SERVICE);
    check(listener >= 0, "register service \"" SERVICE "\"");
    check(service_register(SERVICE) == -EEXIST, "a second register of the same name is refused");
    check(service_connect("no-such-service") == -ECONNREFUSED, "connecting to a missing service fails");

    struct os_pollfd pfd = { listener, OS_POLL_IN, 0 };
    uint64_t t0 = uptime_ms();
    int r = poll(&pfd, 1, 200);
    uint64_t waited = uptime_ms() - t0;
    check(r == 0 && waited >= 180, "poll with nobody connecting times out after 200 ms");

    const char *argv[] = { "ipctest", "client", NULL };
    long child = spawn_simple(_PATH_BIN "/ipctest", argv);
    check(child > 0, "start the client process");

    r = poll(&pfd, 1, 5000);
    check(r == 1 && (pfd.revents & OS_POLL_IN), "poll wakes up when the client connects");
    int ch = service_accept(listener);
    check(ch >= 0, "accept the connection");

    char buf[256];
    recv_text(ch, buf, sizeof(buf), NULL);
    printf("  client says: \"%s\"\n", buf);
    check(strncmp(buf, "hello from pid", 14) == 0, "receive a message");

    int handle;
    recv_text(ch, buf, sizeof(buf), &handle);
    check(strcmp(buf, "shm") == 0 && handle >= 0, "receive a shared memory handle inside a message");
    uint64_t size = 0;
    uint32_t *mem = handle >= 0 ? shm_map(handle, &size) : NULL;
    check(mem && size == SHM_SIZE, "map the client's shared memory (1 MiB)");
    bool pattern = mem != NULL;
    for (uint32_t i = 0; mem && i < SHM_SIZE / 4; i++) {
        pattern &= mem[i] == i * 2654435761u;
    }
    check(pattern, "see the pattern the client wrote, all 1 MiB of it");

    if (mem) {
        strcpy((char *)mem, "written by the server");
    }
    msg_send(ch, "look", 4, -1);
    recv_text(ch, buf, sizeof(buf), NULL);
    check(strcmp(buf, "seen") == 0, "the client sees what the server wrote");

    t0 = uptime_ms();
    bool echoed = true;
    for (int i = 0; i < ROUNDS; i++) {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "ping %d", i);
        msg_send(ch, msg, (size_t)len, -1);
        long n = msg_recv(ch, buf, sizeof(buf), NULL);
        echoed &= n == len && memcmp(buf, msg, (size_t)len) == 0;
    }
    uint64_t ms = uptime_ms() - t0;
    check(echoed, "2000 round trips, every message echoed intact");
    printf("  %d round trips in %lu ms (%lu us each)\n", ROUNDS, ms, ms * 1000 / ROUNDS);

    msg_send(ch, "done", 4, -1);
    pfd = (struct os_pollfd){ ch, OS_POLL_IN, 0 };
    r = poll(&pfd, 1, 5000);
    check(r == 1 && (pfd.revents & OS_POLL_HUP), "poll reports the client hanging up");
    check(msg_recv(ch, buf, sizeof(buf), NULL) == 0, "receiving after that returns 0 (closed)");
    check(msg_send(ch, "x", 1, -1) == -EPIPE, "sending to a closed channel fails");

    int status;
    wait(child, &status, 0);
    check(status == 0, "the client exited cleanly");

    shm_unmap(mem);
    close(handle);
    close(ch);
    close(listener);
    check(service_connect(SERVICE) == -ECONNREFUSED, "closing the service unregisters it");

    printf(failures ? "ipctest: %d FAILED\n" : "ipctest: all passed\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "client") == 0) {
        return client();
    }
    return server();
}
