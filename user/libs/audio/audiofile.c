/* libaudio: recognising a sound file and handing it to its decoder. */

#include "internal.h"

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static bool same_ignoring_case(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

bool audio_supported_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    static const char *const extensions[] = { "mp3", "ogg", "oga", "flac", "wav", "wave" };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (same_ignoring_case(dot + 1, extensions[i])) {
            return true;
        }
    }
    return false;
}

static const struct audio_backend *backend_for(const uint8_t *d, size_t n) {
    if (n >= 12 && (!memcmp(d, "RIFF", 4) || !memcmp(d, "RIFX", 4) || !memcmp(d, "RF64", 4)) &&
        !memcmp(d + 8, "WAVE", 4)) {
        return &audio_wav;
    }
    if (n >= 4 && !memcmp(d, "fLaC", 4)) {
        return &audio_flac;
    }
    if (n >= 4 && !memcmp(d, "OggS", 4)) {
        return &audio_ogg;   /* (Ogg FLAC and Opus aren't handled; stb_vorbis will say so) */
    }
    /* MP3: an ID3 tag, or a frame sync. */
    if (n >= 3 && !memcmp(d, "ID3", 3)) {
        return &audio_mp3;
    }
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) {
        return &audio_mp3;
    }
    return NULL;
}

const char *audio_format(const uint8_t *data, size_t size) {
    const struct audio_backend *b = backend_for(data, size);
    return b ? b->name : NULL;
}

struct audio_file *audio_file_open(const uint8_t *data, size_t size, const char **error) {
    const struct audio_backend *b = backend_for(data, size);
    if (!b) {
        if (error) {
            *error = "Not a sound file this can play";
        }
        return NULL;
    }
    struct audio_file *f = calloc(1, sizeof(*f));
    if (!f) {
        if (error) {
            *error = "Not enough memory";
        }
        return NULL;
    }
    f->backend = b;
    f->data = data;
    f->size = size;
    if (!b->open(f) || f->rate <= 0 || f->channels <= 0) {
        if (error) {
            *error = f->error ? f->error : "This file is damaged or unsupported";
        }
        b->close(f);
        free(f);
        return NULL;
    }
    return f;
}

void audio_file_close(struct audio_file *f) {
    if (f) {
        f->backend->close(f);
        free(f->scratch);
        free(f);
    }
}

const char *audio_file_format(const struct audio_file *f) { return f->backend->name; }
int audio_file_rate(const struct audio_file *f) { return f->rate; }
int audio_file_channels(const struct audio_file *f) { return f->channels; }
uint64_t audio_file_frames(const struct audio_file *f) { return f->frames; }
const char *audio_file_title(const struct audio_file *f) { return f->title; }
const char *audio_file_artist(const struct audio_file *f) { return f->artist; }

long audio_file_read(struct audio_file *f, int16_t *out, long frames) {
    if (f->channels == 2) {
        return f->backend->read(f, out, frames);
    }
    /* Other layouts: decode into scratch, then make stereo. */
    if (f->scratch_frames < frames) {
        free(f->scratch);
        f->scratch = malloc((size_t)frames * (size_t)f->channels * sizeof(int16_t));
        f->scratch_frames = f->scratch ? frames : 0;
        if (!f->scratch) {
            return 0;
        }
    }
    long n = f->backend->read(f, f->scratch, frames);
    int ch = f->channels;
    for (long i = 0; i < n; i++) {
        out[2 * i] = f->scratch[i * ch];
        out[2 * i + 1] = f->scratch[i * ch + (ch > 1 ? 1 : 0)];
    }
    return n;
}

bool audio_file_seek(struct audio_file *f, uint64_t frame) {
    return f->backend->seek(f, frame);
}

void audio_vorbis_comment(struct audio_file *f, const char *c, size_t n) {
    static const struct {
        const char *key;
        size_t offset;
    } keys[] = {
        { "TITLE=", offsetof(struct audio_file, title) },
        { "ARTIST=", offsetof(struct audio_file, artist) },
    };
    for (size_t k = 0; k < 2; k++) {
        size_t kl = strlen(keys[k].key);
        if (n <= kl) {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < kl && match; i++) {
            match = lower(c[i]) == lower(keys[k].key[i]);
        }
        char *dst = (char *)f + keys[k].offset;
        if (match && !dst[0]) {
            size_t len = n - kl < 127 ? n - kl : 127;
            memcpy(dst, c + kl, len);
            dst[len] = '\0';
        }
    }
}
