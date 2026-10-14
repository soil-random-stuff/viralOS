/* FLAC through dr_flac (from memory, no stdio). Tags are Vorbis comments in a metadata block. */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DRFLAC_NO_SSE2
#define DRFLAC_NO_SSE41
#include "vendor/dr_flac.h"

#include "internal.h"

static void on_metadata(void *user, drflac_metadata *meta) {
    struct audio_file *f = user;
    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        return;
    }
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, meta->data.vorbis_comment.commentCount,
                                        meta->data.vorbis_comment.pComments);
    const char *c;
    drflac_uint32 length;
    while ((c = drflac_next_vorbis_comment(&it, &length)) != NULL) {
        audio_vorbis_comment(f, c, length);
    }
}

static bool flac_open(struct audio_file *f) {
    drflac *d = drflac_open_memory_with_metadata(f->data, f->size, on_metadata, f, NULL);
    if (!d) {
        f->error = "This FLAC file is damaged";
        return false;
    }
    f->state = d;
    f->rate = (int)d->sampleRate;
    f->channels = d->channels;
    f->frames = d->totalPCMFrameCount;
    return true;
}

static long flac_read(struct audio_file *f, int16_t *out, long frames) {
    return (long)drflac_read_pcm_frames_s16(f->state, (drflac_uint64)frames, out);
}

static bool flac_seek(struct audio_file *f, uint64_t frame) {
    return drflac_seek_to_pcm_frame(f->state, frame);
}

static void flac_close(struct audio_file *f) {
    if (f->state) {
        drflac_close(f->state);
    }
}

const struct audio_backend audio_flac = { "FLAC", flac_open, flac_read, flac_seek, flac_close };
