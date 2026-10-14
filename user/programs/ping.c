/*
 * ping [-c count] [-i seconds] [-W seconds] [-s size] [-t ttl] [-q] host
 *
 * Sends ICMP echo requests and reports the replies, like iputils' ping: one line per reply, and
 * statistics at the end (after -c replies, or Ctrl+C). Uses a raw ICMP socket.
 */

#include <arpa/inet.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ICMP_ECHOREPLY   0
#define ICMP_UNREACH     3
#define ICMP_ECHO        8
#define ICMP_TIMXCEED    11
#define MAX_PAYLOAD      65000

struct icmp_header {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
};

static uint16_t checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2, p += 2) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
    }
    if (len) {
        sum += (uint32_t)(p[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return htons((uint16_t)~sum);
}

static const char *unreachable_text(int code) {
    switch (code) {
    case 0:  return "Destination Net Unreachable";
    case 1:  return "Destination Host Unreachable";
    case 2:  return "Destination Protocol Unreachable";
    case 3:  return "Destination Port Unreachable";
    case 4:  return "Frag needed and DF set";
    case 13: return "Packet filtered";
    default: return "Destination Unreachable";
    }
}

static void print_ms(double ms) {
    if (ms < 1) {
        printf("%.3f", ms);
    } else if (ms < 10) {
        printf("%.2f", ms);
    } else if (ms < 100) {
        printf("%.1f", ms);
    } else {
        printf("%.0f", ms);
    }
}

static void usage(void) {
    printf("Usage: ping [-c count] [-i interval] [-W timeout] [-s size] [-t ttl] [-q] host\n"
           "  -c count     stop after this many replies (default: until Ctrl+C)\n"
           "  -i interval  seconds between requests (default 1, at least 0.2)\n"
           "  -W timeout   seconds to wait for the last reply (default 2)\n"
           "  -s size      bytes of data in each request (default 56)\n"
           "  -t ttl       time to live (hops) of the requests\n"
           "  -q           quiet: only the summary\n");
}

/* "0.5" -> 500 (milliseconds). */
static long seconds_to_ms(const char *s) {
    long whole = 0, frac = 0, scale = 1000;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale > 1) {
            scale /= 10;
            frac += (*s++ - '0') * scale;
        }
    }
    return *s ? -1 : whole * 1000 + frac;
}

int main(int argc, char **argv) {
    long count = -1, interval_ms = 1000, wait_ms = 2000, size = 56, ttl = 0;
    bool quiet = false;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2] && strchr("ciWst", a[1])) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            const char *v = argv[++i];
            switch (a[1]) {
            case 'c': count = atoi(v); break;
            case 'i': interval_ms = seconds_to_ms(v); break;
            case 'W': wait_ms = seconds_to_ms(v); break;
            case 's': size = atoi(v); break;
            case 't': ttl = atoi(v); break;
            }
        } else if (strcmp(a, "-q") == 0) {
            quiet = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (a[0] == '-') {
            printf("ping: unknown option %s\n", a);
            usage();
            return 2;
        } else {
            host = a;
        }
    }
    if (!host || count == 0 || interval_ms < 0 || wait_ms < 0 || size < 0 || size > MAX_PAYLOAD || ttl < 0 || ttl > 255) {
        usage();
        return 2;
    }
    if (interval_ms < 200) {
        interval_ms = 200;   /* (as iputils does for anyone but root) */
    }

    struct in_addr dest;
    int r = resolve_host(host, &dest);
    if (r < 0) {
        if (r == -ENOENT) {
            printf("ping: %s: Name or service not known\n", host);
        } else if (r == -ENETUNREACH) {
            printf("ping: %s: Temporary failure in name resolution (not connected to a network)\n", host);
        } else {
            printf("ping: %s: Temporary failure in name resolution\n", host);
        }
        return 2;
    }
    char dest_text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dest, dest_text, sizeof(dest_text));

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        printf("ping: socket: %s\n", strerror(fd));
        return 2;
    }
    if (ttl) {
        int t = (int)ttl;
        setsockopt(fd, IPPROTO_IP, IP_TTL, &t, sizeof(t));
    }
    interrupt_mode(OS_INTERRUPT_CATCH);   /* Ctrl+C ends with the statistics */

    uint8_t *packet = malloc(sizeof(struct icmp_header) + (size_t)size);
    uint8_t *reply = malloc(sizeof(struct icmp_header) + (size_t)size + 128);
    uint64_t *sent_at = calloc(65536, sizeof(uint64_t));
    if (!packet || !reply || !sent_at) {
        printf("ping: out of memory\n");
        return 2;
    }
    uint16_t id = (uint16_t)getpid();
    printf("PING %s (%s) %ld(%ld) bytes of data.\n", host, dest_text, size, size + 28);

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = dest };
    long transmitted = 0, received = 0, errors = 0;
    double min = 0, max = 0, sum = 0, sum2 = 0;
    uint64_t start = uptime_us();
    uint64_t next_send = start;
    bool stop = false;
    uint64_t deadline = 0;   /* after the last request: how long to wait for its reply */

    while (!stop) {
        uint64_t now = uptime_us();
        if (deadline == 0 && now >= next_send) {
            /* Send the next request: header, then the data (8 bytes of pattern repeated). */
            struct icmp_header *h = (struct icmp_header *)packet;
            uint16_t seq = (uint16_t)(transmitted + 1);
            h->type = ICMP_ECHO;
            h->code = 0;
            h->checksum = 0;
            h->id = htons(id);
            h->seq = htons(seq);
            for (long i = 0; i < size; i++) {
                packet[sizeof(*h) + i] = (uint8_t)(0x10 + i);
            }
            h->checksum = checksum(packet, sizeof(*h) + (size_t)size);
            sent_at[seq] = uptime_us();
            long s = sendto(fd, packet, sizeof(*h) + (size_t)size, 0, (struct sockaddr *)&to, sizeof(to));
            transmitted++;
            if (s < 0) {
                printf("ping: sendto: %s\n", strerror((int)s));
                errors++;
            }
            next_send += (uint64_t)interval_ms * 1000;
            if (count > 0 && transmitted >= count) {
                deadline = uptime_us() + (uint64_t)wait_ms * 1000;
            }
            continue;
        }

        /* Wait for a reply until the next request is due (or the deadline). */
        uint64_t until = deadline ? deadline : next_send;
        long wait = until > now ? (long)((until - now + 999) / 1000) : 0;
        struct os_pollfd p = { .fd = fd, .events = OS_POLL_IN };
        int ready = poll(&p, 1, wait > 0 ? wait : 1);
        if (interrupt_mode(OS_INTERRUPT_CHECK) || ready == -EINTR) {
            break;
        }
        if (deadline && uptime_us() >= deadline) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        long n = recvfrom(fd, reply, sizeof(struct icmp_header) + (size_t)size + 128, MSG_DONTWAIT,
                          (struct sockaddr *)&from, &from_len);
        uint64_t arrived = uptime_us();
        if (n < 20) {
            continue;
        }
        /* A raw socket gets the IP header too. */
        int ihl = (reply[0] & 15) * 4;
        int reply_ttl = reply[8];
        if (n < ihl + (long)sizeof(struct icmp_header)) {
            continue;
        }
        struct icmp_header *h = (struct icmp_header *)(reply + ihl);
        char from_text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, from_text, sizeof(from_text));

        if (h->type == ICMP_ECHOREPLY && ntohs(h->id) == id) {
            uint16_t seq = ntohs(h->seq);
            if (!sent_at[seq]) {
                continue;
            }
            double ms = (double)(arrived - sent_at[seq]) / 1000.0;
            sent_at[seq] = 0;
            received++;
            if (received == 1 || ms < min) {
                min = ms;
            }
            if (ms > max) {
                max = ms;
            }
            sum += ms;
            sum2 += ms * ms;
            if (!quiet) {
                printf("%ld bytes from %s: icmp_seq=%u ttl=%d time=", n - ihl, from_text, seq, reply_ttl);
                print_ms(ms);
                printf(" ms\n");
            }
            if (count > 0 && received >= count) {
                break;
            }
        } else if (h->type == ICMP_UNREACH || h->type == ICMP_TIMXCEED) {
            /* The original request's headers follow: is it one of ours? */
            uint8_t *orig = (uint8_t *)(h + 1);
            if (n < ihl + 8 + 28) {
                continue;
            }
            int oihl = (orig[0] & 15) * 4;
            if (n < ihl + 8 + oihl + 8) {
                continue;
            }
            struct icmp_header *oh = (struct icmp_header *)(orig + oihl);
            if (oh->type != ICMP_ECHO || ntohs(oh->id) != id) {
                continue;
            }
            errors++;
            if (!quiet) {
                printf("From %s icmp_seq=%u %s\n", from_text, ntohs(oh->seq),
                       h->type == ICMP_TIMXCEED ? "Time to live exceeded" : unreachable_text(h->code));
            }
        }
    }

    long elapsed = (long)((uptime_us() - start) / 1000);
    printf("\n--- %s ping statistics ---\n", host);
    printf("%ld packets transmitted, %ld received, ", transmitted, received);
    if (errors) {
        printf("+%ld errors, ", errors);
    }
    long loss = transmitted ? (transmitted - received) * 100 / transmitted : 0;
    printf("%ld%% packet loss, time %ldms\n", loss, elapsed);
    if (received) {
        double avg = sum / (double)received;
        double var = sum2 / (double)received - avg * avg;
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", min, avg, max, var > 0 ? sqrt(var) : 0.0);
    }
    close(fd);
    return received ? 0 : 1;
}
