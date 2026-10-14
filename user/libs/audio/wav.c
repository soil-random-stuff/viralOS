/* WAV through dr_wav (from memory, no stdio): PCM, float, A-law/µ-law, ADPCM. Tags from LIST/INFO. */

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#include "vendor/dr_wav.h"

#include "internal.h"

static void copy_tag(char *dst, size_t size, const drwav_list_info_text *t) {
    if (!dst[0] && t->pString) {
        size_t n = t->stringLength < size - 1 ? t->stringLength : size - 1;
        memcpy(dst, t->pString, n);
        dst[n] = '\0';
    }
}

static bool wav_open(struct audio_file *f) {
    drwav *w = calloc(1, sizeof(*w));
    if (!w) {
        return false;
    }
    f->state = w;
    if (!drwav_init_memory_with_metadata(w, f->data, f->size, 0, NULL)) {
        free(w);
        f->state = NULL;
        f->error = "This WAV file is damaged or uses an unsupported encoding";
        return false;
    }
    f->rate = (int)w->sampleRate;
    f->channels = w->channels;
    f->frames = w->totalPCMFrameCount;
    for (drwav_uint32 i = 0; i < w->metadataCount; i++) {
        const drwav_metadata *m = &w->pMetadata[i];
        if (m->type == drwav_metadata_type_list_info_title) {
            copy_tag(f->title, sizeof(f->title), &m->data.infoText);
        } else if (m->type == drwav_metadata_type_list_info_artist) {
            copy_tag(f->artist, sizeof(f->artist), &m->data.infoText);
        }
    }
    return true;
}

static long wav_read(struct audio_file *f, int16_t *out, long frames) {
    return (long)drwav_read_pcm_frames_s16(f->state, (drwav_uint64)frames, out);
}

static bool wav_seek(struct audio_file *f, uint64_t frame) {
    return drwav_seek_to_pcm_frame(f->state, frame);
}

static void wav_close(struct audio_file *f) {
    if (f->state) {
        drwav_uninit(f->state);
        free(f->state);
    }
}

const struct audio_backend audio_wav = { "WAV", wav_open, wav_read, wav_seek, wav_close };
