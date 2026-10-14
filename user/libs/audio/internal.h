#pragma once

/* Shared by libaudio's format backends. */

#include <audiofile.h>
#include <stdlib.h>
#include <string.h>

struct audio_file;

struct audio_backend {
    const char *name;
    bool (*open)(struct audio_file *f);
    /* Up to `frames` frames in the file's own channel layout into out. */
    long (*read)(struct audio_file *f, int16_t *out, long frames);
    bool (*seek)(struct audio_file *f, uint64_t frame);
    void (*close)(struct audio_file *f);
};

struct audio_file {
    const struct audio_backend *backend;
    const uint8_t *data;
    size_t size;
    const char *error;
    int rate, channels;
    uint64_t frames;
    char title[128], artist[128];
    void *state;                     /* the backend's */
    int16_t *scratch;                /* for files with other than 2 channels */
    long scratch_frames;
};

extern const struct audio_backend audio_mp3, audio_ogg, audio_flac, audio_wav;

/* "TITLE=..." / "ARTIST=..." style tags (Vorbis comments: Ogg, FLAC). */
void audio_vorbis_comment(struct audio_file *f, const char *comment, size_t length);
