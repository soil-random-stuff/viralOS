/*
 * MP3 through minimp3. minimp3 decodes one frame at a time and can't seek,
 * so at open every frame is found (cheaply, without decoding) and indexed;
 * seeking then restarts from the right frame. Title/artist come from the
 * ID3v2 tag, if there is one.
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"

#include "internal.h"

struct frame {
    uint32_t offset;   /* in the file */
    uint32_t sample;   /* first frame (per channel) it holds */
};

struct mp3 {
    mp3dec_t dec;
    struct frame *frames;
    int count, next;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int have, used;    /* decoded frames in pcm, and how many were handed out */
};

/* --- ID3v2 ------------------------------------------------------------------------------------------ */

static size_t put_utf8(char *out, size_t at, size_t size, uint32_t c) {
    char tmp[3];
    int n = 0;
    if (c < 0x80) {
        tmp[n++] = (char)c;
    } else if (c < 0x800) {
        tmp[n++] = (char)(0xC0 | c >> 6);
        tmp[n++] = (char)(0x80 | (c & 0x3F));
    } else {
        tmp[n++] = (char)(0xE0 | c >> 12);
        tmp[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (c & 0x3F));
    }
    if (at + (size_t)n >= size) {
        return at;
    }
    memcpy(out + at, tmp, (size_t)n);
    return at + (size_t)n;
}

/* A text frame's value (encoding byte first) as UTF-8. */
static void id3_text(const uint8_t *p, size_t n, char *out, size_t size) {
    size_t at = 0;
    if (n < 1) {
        return;
    }
    int encoding = p[0];
    p++;
    n--;
    if (encoding == 1 || encoding == 2) {   /* UTF-16 with a BOM (1), or big-endian (2) */
        bool little = false;
        if (encoding == 1 && n >= 2) {
            little = p[0] == 0xFF && p[1] == 0xFE;
            p += 2;
            n -= 2;
        }
        for (size_t i = 0; i + 1 < n; i += 2) {
            uint32_t c = little ? (uint32_t)(p[i] | p[i + 1] << 8) : (uint32_t)(p[i] << 8 | p[i + 1]);
            if (!c) {
                break;
            }
            at = put_utf8(out, at, size, c >= 0xD800 && c < 0xE000 ? '?' : c);
        }
    } else {
        for (size_t i = 0; i < n && p[i]; i++) {
            if (encoding == 3) {   /* UTF-8 */
                if (at + 1 < size) {
                    out[at++] = (char)p[i];
                }
            } else {               /* ISO-8859-1 */
                at = put_utf8(out, at, size, p[i]);
            }
        }
    }
    out[at] = '\0';
}

static uint32_t syncsafe(const uint8_t *p) {
    return (uint32_t)((p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F));
}

/* Read the tag at the start (if any); returns where the audio begins. */
static size_t read_id3(struct audio_file *f) {
    const uint8_t *d = f->data;
    size_t n = f->size;
    if (n < 10 || memcmp(d, "ID3", 3) != 0) {
        return 0;
    }
    int version = d[3];
    size_t end = 10 + syncsafe(d + 6) + (d[5] & 0x10 ? 10 : 0);
    end = end > n ? n : end;
    size_t p = 10;
    if ((d[5] & 0x40) && p + 4 <= end) {   /* an extended header */
        p += version == 4 ? syncsafe(d + p) : (size_t)(d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]) + 4;
    }
    while (version >= 3 && p + 10 <= end && d[p]) {
        size_t size = version == 4 ? syncsafe(d + p + 4)
                                   : (size_t)(d[p + 4] << 24 | d[p + 5] << 16 | d[p + 6] << 8 | d[p + 7]);
        if (p + 10 + size > end) {
            break;
        }
        if (!memcmp(d + p, "TIT2", 4)) {
            id3_text(d + p + 10, size, f->title, sizeof(f->title));
        } else if (!memcmp(d + p, "TPE1", 4)) {
            id3_text(d + p + 10, size, f->artist, sizeof(f->artist));
        }
        p += 10 + size;
    }
    return end;
}

/* --- decoding ------------------------------------------------------------------------------------------- */

static bool mp3_open(struct audio_file *f) {
    struct mp3 *m = calloc(1, sizeof(*m));
    if (!m) {
        return false;
    }
    f->state = m;
    size_t pos = read_id3(f);
    mp3dec_init(&m->dec);
    int cap = 0;
    uint32_t samples = 0;
    while (pos < f->size) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&m->dec, f->data + pos, (int)(f->size - pos), NULL, &info);
        if (!info.frame_bytes) {
            break;
        }
        if (n) {
            if (m->count == cap) {
                cap = cap ? cap * 2 : 1024;
                struct frame *bigger = realloc(m->frames, (size_t)cap * sizeof(*bigger));
                if (!bigger) {
                    return false;
                }
                m->frames = bigger;
            }
            if (!m->count) {
                f->rate = info.hz;
                f->channels = info.channels;
            }
            m->frames[m->count].offset = (uint32_t)(pos + (size_t)info.frame_offset);
            m->frames[m->count].sample = samples;
            m->count++;
            samples += (uint32_t)n;
        }
        pos += (size_t)info.frame_bytes;
    }
    f->frames = samples;
    mp3dec_init(&m->dec);
    if (!m->count) {
        f->error = "This isn't an MP3 file";
        return false;
    }
    return true;
}

static long mp3_read(struct audio_file *f, int16_t *out, long frames) {
    struct mp3 *m = f->state;
    int ch = f->channels;
    long done = 0;
    while (done < frames) {
        if (m->used == m->have) {
            if (m->next >= m->count) {
                break;
            }
            const struct frame *fr = &m->frames[m->next++];
            mp3dec_frame_info_t info;
            int n = mp3dec_decode_frame(&m->dec, f->data + fr->offset, (int)(f->size - fr->offset), m->pcm, &info);
            m->have = info.channels == ch ? n : 0;   /* (a stray frame with another layout: skip it) */
            m->used = 0;
            continue;
        }
        long n = m->have - m->used < frames - done ? m->have - m->used : frames - done;
        memcpy(out + done * ch, m->pcm + m->used * ch, (size_t)(n * ch) * sizeof(int16_t));
        m->used += (int)n;
        done += n;
    }
    return done;
}

static bool mp3_seek(struct audio_file *f, uint64_t target) {
    struct mp3 *m = f->state;
    int lo = 0, hi = m->count - 1;
    while (lo < hi) {   /* the last frame starting at or before the target */
        int mid = (lo + hi + 1) / 2;
        if (m->frames[mid].sample <= target) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    /* Layer III borrows bits from earlier frames: decode a couple first, then throw them away. */
    mp3dec_init(&m->dec);
    int warmup = lo < 2 ? lo : 2;
    for (int k = lo - warmup; k < lo; k++) {
        mp3dec_frame_info_t info;
        mp3dec_decode_frame(&m->dec, f->data + m->frames[k].offset, (int)(f->size - m->frames[k].offset), m->pcm,
                            &info);
    }
    m->next = lo;
    m->have = m->used = 0;
    if (target >= f->frames) {
        m->next = m->count;
        return true;
    }
    /* Skip to the exact frame within it. */
    long skip = (long)(target - m->frames[lo].sample);
    while (skip > 0) {
        int16_t junk[2 * 256];
        long n = mp3_read(f, junk, skip < 256 / f->channels ? skip : 256 / f->channels);
        if (n <= 0) {
            break;
        }
        skip -= n;
    }
    return true;
}

static void mp3_close(struct audio_file *f) {
    struct mp3 *m = f->state;
    if (m) {
        free(m->frames);
        free(m);
    }
}

const struct audio_backend audio_mp3 = { "MP3", mp3_open, mp3_read, mp3_seek, mp3_close };
