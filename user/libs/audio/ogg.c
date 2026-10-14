/* Ogg Vorbis through stb_vorbis (from memory, no stdio). Tags are Vorbis comments. */

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "vendor/stb_vorbis.c"

#include "internal.h"

static bool ogg_open(struct audio_file *f) {
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(f->data, (int)f->size, &err, NULL);
    if (!v) {
        f->error = err == VORBIS_invalid_first_page || err == VORBIS_invalid_setup
                       ? "This Ogg file isn't Vorbis audio" : "This Ogg file is damaged";
        return false;
    }
    f->state = v;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    f->rate = (int)info.sample_rate;
    f->channels = info.channels;
    f->frames = stb_vorbis_stream_length_in_samples(v);
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; i++) {
        audio_vorbis_comment(f, c.comment_list[i], strlen(c.comment_list[i]));
    }
    return true;
}

static long ogg_read(struct audio_file *f, int16_t *out, long frames) {
    /* stb_vorbis can mix any layout down to what we ask for: ask for the file's own. */
    return stb_vorbis_get_samples_short_interleaved(f->state, f->channels, out, (int)(frames * f->channels));
}

static bool ogg_seek(struct audio_file *f, uint64_t frame) {
    return stb_vorbis_seek(f->state, (unsigned int)frame) != 0;
}

static void ogg_close(struct audio_file *f) {
    if (f->state) {
        stb_vorbis_close(f->state);
    }
}

const struct audio_backend audio_ogg = { "Ogg Vorbis", ogg_open, ogg_read, ogg_seek, ogg_close };
