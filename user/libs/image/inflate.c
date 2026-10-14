/*
 * DEFLATE decompression (RFC 1951), in a zlib wrapper (RFC 1950).
 *
 * DEFLATE data is a series of blocks. Each holds either raw bytes, or
 * symbols coded with Huffman codes: literal bytes, and (length, distance)
 * pairs meaning "copy `length` bytes from `distance` back". The codes are
 * either a fixed table or described at the start of the block, themselves
 * Huffman-coded. Decoding walks the canonical code one bit at a time, in
 * the style of zlib's reference decoder "puff": short and easy to check.
 */

#include <image.h>
#include <stdbool.h>
#include <string.h>

#define MAX_BITS  15
#define MAX_LCODES 286
#define MAX_DCODES 30
#define FIX_LCODES 288

struct state {
    const uint8_t *in;
    size_t in_size, in_pos;
    uint32_t bit_buf;
    int bit_count;
    uint8_t *out;
    size_t out_size, out_pos;
    bool error;
};

/* Canonical Huffman code: how many codes of each length, and the symbols in code order. */
struct huffman {
    uint16_t count[MAX_BITS + 1];
    uint16_t symbol[FIX_LCODES];
};

static uint32_t bits(struct state *s, int need) {
    uint32_t val = s->bit_buf;
    while (s->bit_count < need) {
        if (s->in_pos >= s->in_size) {
            s->error = true;
            return 0;
        }
        val |= (uint32_t)s->in[s->in_pos++] << s->bit_count;
        s->bit_count += 8;
    }
    s->bit_buf = val >> need;
    s->bit_count -= need;
    return val & ((1u << need) - 1);
}

static bool build(struct huffman *h, const uint8_t *lengths, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }
    if (h->count[0] == n) {
        return true;   /* no codes at all: fine, as long as none is used */
    }
    int left = 1;
    for (int len = 1; len <= MAX_BITS; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0) {
            return false;   /* more codes than lengths allow */
        }
    }
    uint16_t offs[MAX_BITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAX_BITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    return true;
}

static int decode(struct state *s, const struct huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        code |= (int)bits(s, 1);
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->error = true;   /* ran out of code lengths */
    return -1;
}

static void put(struct state *s, uint8_t byte) {
    if (s->out_pos >= s->out_size) {
        s->error = true;
        return;
    }
    s->out[s->out_pos++] = byte;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint16_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint16_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* The body of a Huffman-coded block. */
static void codes(struct state *s, const struct huffman *lens, const struct huffman *dists) {
    for (;;) {
        int sym = decode(s, lens);
        if (s->error) {
            return;
        }
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return;   /* end of block */
        } else {
            sym -= 257;
            if (sym >= 29) {
                s->error = true;
                return;
            }
            size_t len = length_base[sym] + bits(s, length_extra[sym]);
            int dsym = decode(s, dists);
            if (dsym < 0 || dsym >= 30) {
                s->error = true;
                return;
            }
            size_t dist = dist_base[dsym] + bits(s, dist_extra[dsym]);
            if (dist > s->out_pos || s->error) {
                s->error = true;   /* points before the start */
                return;
            }
            /* Byte by byte: the source may overlap what's being written. */
            while (len--) {
                put(s, s->out[s->out_pos - dist]);
            }
        }
        if (s->error) {
            return;
        }
    }
}

static void stored(struct state *s) {
    s->bit_buf = 0;
    s->bit_count = 0;   /* to the next byte boundary */
    if (s->in_pos + 4 > s->in_size) {
        s->error = true;
        return;
    }
    size_t len = s->in[s->in_pos] | (size_t)s->in[s->in_pos + 1] << 8;
    size_t nlen = s->in[s->in_pos + 2] | (size_t)s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || s->in_pos + len > s->in_size) {
        s->error = true;
        return;
    }
    while (len-- && !s->error) {
        put(s, s->in[s->in_pos++]);
    }
}

static void fixed(struct state *s) {
    static struct huffman lens, dists;
    static bool ready;
    if (!ready) {
        uint8_t l[FIX_LCODES];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < FIX_LCODES; i++) l[i] = 8;
        build(&lens, l, FIX_LCODES);
        for (i = 0; i < MAX_DCODES; i++) l[i] = 5;
        build(&dists, l, MAX_DCODES);
        ready = true;
    }
    codes(s, &lens, &dists);
}

static void dynamic(struct state *s) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (nlen > MAX_LCODES || ndist > MAX_DCODES) {
        s->error = true;
        return;
    }
    uint8_t lengths[MAX_LCODES + MAX_DCODES] = { 0 };
    for (int i = 0; i < ncode; i++) {
        lengths[order[i]] = (uint8_t)bits(s, 3);
    }
    struct huffman lencode;
    if (!build(&lencode, lengths, 19)) {
        s->error = true;
        return;
    }
    /* The literal/length and distance code lengths, run-length coded. */
    int index = 0;
    while (index < nlen + ndist && !s->error) {
        int sym = decode(s, &lencode);
        if (sym < 0) {
            return;
        }
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
            continue;
        }
        uint8_t value = 0;
        int repeat;
        if (sym == 16) {   /* repeat the previous length 3..6 times */
            if (index == 0) {
                s->error = true;
                return;
            }
            value = lengths[index - 1];
            repeat = 3 + (int)bits(s, 2);
        } else if (sym == 17) {   /* 3..10 zeros */
            repeat = 3 + (int)bits(s, 3);
        } else {   /* 11..138 zeros */
            repeat = 11 + (int)bits(s, 7);
        }
        if (index + repeat > nlen + ndist) {
            s->error = true;
            return;
        }
        while (repeat--) {
            lengths[index++] = value;
        }
    }
    if (s->error || lengths[256] == 0) {   /* there must be an end-of-block code */
        s->error = true;
        return;
    }
    struct huffman dists;
    if (!build(&lencode, lengths, nlen) || !build(&dists, lengths + nlen, ndist)) {
        s->error = true;
        return;
    }
    codes(s, &lencode, &dists);
}

static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    while (len) {
        size_t chunk = len < 5552 ? len : 5552;   /* the most before the sums can overflow */
        len -= chunk;
        while (chunk--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

long zlib_decompress(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    /* zlib header: compression method 8 (deflate), a check value, no preset dictionary. */
    if (in_size < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) {
        return -1;
    }
    struct state s = { .in = in, .in_size = in_size, .in_pos = 2, .out = out, .out_size = out_size };
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (type == 0) {
            stored(&s);
        } else if (type == 1) {
            fixed(&s);
        } else if (type == 2) {
            dynamic(&s);
        } else {
            s.error = true;
        }
    } while (!last && !s.error);
    if (s.error) {
        return -1;
    }
    /* Then the Adler-32 checksum of the output, big-endian, from the next whole byte. */
    if (s.in_pos + 4 <= in_size) {
        uint32_t want = (uint32_t)in[s.in_pos] << 24 | (uint32_t)in[s.in_pos + 1] << 16 |
                        (uint32_t)in[s.in_pos + 2] << 8 | in[s.in_pos + 3];
        if (want != adler32(out, s.out_pos)) {
            return -1;
        }
    }
    return (long)s.out_pos;
}
