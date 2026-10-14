#pragma once

/*
 * libaudio: reading sound files, through well-known open-source decoders
 * (in user/libs/audio/vendor, unchanged):
 *
 *   MP3          minimp3      (lieff, CC0)
 *   Ogg Vorbis   stb_vorbis   (Sean Barrett, public domain)
 *   FLAC         dr_flac      (David Reid, public domain)
 *   WAV          dr_wav       (David Reid, public domain): PCM, float, ADPCM, ...
 *
 * Whatever the file holds, it comes out as 16-bit stereo at the file's own
 * rate (mono on both sides; with more channels, the first two).
 *
 *     struct audio_file *f = audio_file_open(data, size, &error);
 *     int16_t buf[2 * 1024];
 *     long frames = audio_file_read(f, buf, 1024);   // 0 at the end
 *     audio_file_close(f);   // (data must stay around until then)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct audio_file;

struct audio_file *audio_file_open(const uint8_t *data, size_t size, const char **error);
void audio_file_close(struct audio_file *f);

/* "MP3", "Ogg Vorbis", "FLAC", "WAV" for data in that format, or NULL. */
const char *audio_format(const uint8_t *data, size_t size);
/* Does the name end in an extension we can play (.mp3, .ogg, .oga, .flac, .wav)? */
bool audio_supported_name(const char *name);

const char *audio_file_format(const struct audio_file *f);
int audio_file_rate(const struct audio_file *f);
int audio_file_channels(const struct audio_file *f);          /* in the file (output is always 2) */
uint64_t audio_file_frames(const struct audio_file *f);       /* the length; 0 if unknown */
const char *audio_file_title(const struct audio_file *f);     /* from the tags; "" if none */
const char *audio_file_artist(const struct audio_file *f);

/* Up to `frames` stereo frames into out (2 * frames samples). Returns how many; 0 at the end. */
long audio_file_read(struct audio_file *f, int16_t *out, long frames);
/* Continue from this frame. */
bool audio_file_seek(struct audio_file *f, uint64_t frame);
