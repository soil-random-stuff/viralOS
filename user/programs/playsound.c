/*
 * playsound FILE: play a sound file (MP3, Ogg Vorbis, FLAC, WAV) to the end, then exit. The system's
 * sounds (system_sound() in <sound.h>) are played this way; it mixes with whatever else is playing.
 */

#include <audiofile.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FRAMES 2048

static uint8_t *read_file(const char *path, size_t *size) {
    struct os_file_info info;
    if (stat(path, &info) != 0 || !info.size) {
        return NULL;
    }
    uint8_t *data = malloc(info.size);
    int fd = data ? open(path, O_RDONLY) : -1;
    if (fd < 0) {
        free(data);
        return NULL;
    }
    size_t got = 0;
    long n;
    while (got < info.size && (n = read(fd, data + got, info.size - got)) > 0) {
        got += (size_t)n;
    }
    close(fd);
    *size = got;
    return data;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        dprintf(STDERR_FILENO, "usage: playsound FILE\n");
        return 2;
    }
    size_t size = 0;
    uint8_t *data = read_file(argv[1], &size);
    const char *error = "can't read it";
    struct audio_file *f = data ? audio_file_open(data, size, &error) : NULL;
    if (!f) {
        dprintf(STDERR_FILENO, "playsound: %s: %s\n", argv[1], error);
        return 1;
    }
    if (audio_open((uint32_t)audio_file_rate(f), 2) < 0) {
        return 1;   /* no sound card: nothing to do */
    }
    static int16_t buf[2 * FRAMES];
    long frames;
    while ((frames = audio_file_read(f, buf, FRAMES)) > 0) {
        if (audio_write(buf, (size_t)frames * 4, 0) < 0) {
            break;
        }
    }
    audio_ctl(OS_AUDIO_DRAIN, 0);
    audio_ctl(OS_AUDIO_CLOSE, 0);
    audio_file_close(f);
    return 0;
}
