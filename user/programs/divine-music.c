/*
 * Music: a small player for MP3, Ogg Vorbis, FLAC and WAV files.
 *
 *   music [file]
 *
 * Space plays/pauses, left/right jump 5 seconds, up/down change the
 * volume, L changes the loop mode, M mutes. Click or drag on the bar to
 * move around in the song. The loop button cycles: off (stop at the end),
 * the whole folder (on to the next song, around to the first), this song.
 *
 * Decoding is libaudio (<audiofile.h>); the sound goes out through
 * audio_open/audio_write (the HD Audio driver).
 */

#include <audiofile.h>
#include <fileops.h>
#include <gui.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define WIN_W        460
#define WIN_H        226
#define ART          96           /* the picture on the left */
#define TARGET_QUEUE (OS_AUDIO_BUFFER * 3 / 4)   /* keep about this much queued in the driver */
#define CHUNK        1024         /* frames decoded at a time */
#define TICK_MS      20
#define SEEK_STEP    5.0          /* seconds, arrow keys */
#define SKIP_STEP    10.0         /* seconds, buttons */
#define VOLUME_ICON  20

#define WHITE       GFX_RGB(255, 255, 255)
#define INK         GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT    GFX_RGB(0x78, 0x78, 0x82)
#define INK_FAINT   GFX_RGB(0xC0, 0xC0, 0xC8)
#define TRACK       GFX_RGB(0xE4, 0xE5, 0xEA)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define HOVER       GFX_RGB(0xEB, 0xEC, 0xF0)

enum control { C_NONE = -1, C_LOOP, C_BACK, C_PLAY, C_FORWARD, C_MUTE, C_SEEK, C_VOLUME, C_COUNT };
enum loop { LOOP_OFF, LOOP_ALL, LOOP_ONE };

static struct {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *ui, *bold;
    struct gfx_surface *art;                 /* /usr/share/dde-res/Icons/FilesFolders/Audio.png */
    struct gfx_surface *volume_icons[4];     /* mute, low, medium, high */

    char path[OS_PATH_MAX];
    char title[128], artist[128];
    const char *error;            /* shown instead of the artist */

    uint8_t *data;
    size_t size;
    struct audio_file *file;
    int hz;

    /* The songs in the folder (names, sorted). */
    char (*songs)[OS_NAME_MAX];
    int song_count, song_index;
    enum loop loop;

    /* Playback. */
    bool have_audio, playing, at_end;
    int out_hz;                   /* the driver's rate: hz, or 48000 when resampling */
    uint64_t queued_until;        /* frame position just after what has gone to the driver */
    uint64_t switched_at;         /* (looping the folder) driver bytes still holding the previous song */
    double resample_pos;
    int16_t decoded[2 * CHUNK];
    int16_t pending[2 * CHUNK * 7];   /* converted (and resampled up to 6x), not yet accepted */
    int pending_bytes, pending_offset;
    int volume;                   /* 0-100: this player's own volume */
    bool muted;

    int hover, pressed;
    bool dragging_seek;
    double drag_seconds;
} m;

/* --- files ------------------------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *size) {
    struct os_file_info info;
    if (stat(path, &info) < 0 || info.type != OS_FILE) {
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    uint8_t *buf = malloc(info.size ? info.size : 1);
    size_t got = 0;
    while (buf && got < info.size) {
        long n = read(fd, buf + got, info.size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != info.size) {
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

static struct gfx_surface *load_icon(const char *path, int size) {
    struct gfx_surface *img = image_load(path, NULL);
    if (!img || (img->width == size && img->height == size)) {
        return img;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, size, size);
    gfx_surface_destroy(img);
    return s;
}

static int compare_names(const void *a, const void *b) {
    return name_compare((const char *)a, (const char *)b);
}

static void load_folder(void) {
    free(m.songs);
    m.songs = NULL;
    m.song_count = 0;
    m.song_index = -1;
    char dir[256];
    path_parent(m.path, dir, sizeof(dir));
    int fd = open(dir, O_RDONLY);
    if (fd < 0) {
        return;
    }
    int cap = 0;
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) >= 0; i++) {
        if (info.type != OS_FILE || !audio_supported_name(info.name)) {
            continue;
        }
        if (m.song_count == cap) {
            cap = cap ? cap * 2 : 32;
            void *bigger = realloc(m.songs, (size_t)cap * sizeof(*m.songs));
            if (!bigger) {
                break;
            }
            m.songs = bigger;
        }
        strlcpy(m.songs[m.song_count++], info.name, OS_NAME_MAX);
    }
    close(fd);
    if (m.song_count) {
        qsort(m.songs, (size_t)m.song_count, sizeof(*m.songs), compare_names);
    }
    for (int i = 0; i < m.song_count; i++) {
        if (strcmp(m.songs[i], path_base(m.path)) == 0) {
            m.song_index = i;
        }
    }
}

/* --- playback ----------------------------------------------------------------------------------- */

static double total_seconds(void) {
    return m.file && m.hz ? audio_file_frames(m.file) / (double)m.hz : 0;
}

static double seconds_now(void) {
    if (!m.hz) {
        return 0;
    }
    double t = m.queued_until / (double)m.hz;
    if (m.have_audio) {
        long queued = audio_ctl(OS_AUDIO_QUEUED, 0);
        if (queued > (long)m.switched_at) {
            t -= (queued - (long)m.switched_at) / (4.0 * m.out_hz);   /* 16-bit stereo */
        }
    }
    double total = total_seconds();
    return t < 0 ? 0 : total && t > total ? total : t;
}

static void reset_pipeline(uint64_t frame) {
    m.queued_until = frame;
    m.pending_bytes = m.pending_offset = 0;
    m.resample_pos = 0;
    m.at_end = false;
    m.switched_at = 0;
}

static void seek_to(double seconds) {
    if (!m.file) {
        return;
    }
    double total = total_seconds();
    seconds = seconds < 0 ? 0 : total && seconds > total ? total : seconds;
    uint64_t frame = (uint64_t)(seconds * m.hz);
    if (m.have_audio) {
        audio_ctl(OS_AUDIO_FLUSH, 0);
    }
    audio_file_seek(m.file, frame);
    reset_pipeline(frame);
}

static void close_song(void) {
    audio_file_close(m.file);
    m.file = NULL;
    free(m.data);
    m.data = NULL;
}

/* Read and open a song (the sound output is separate). Returns false on failure. */
static bool load_song(const char *path) {
    close_song();
    m.error = NULL;
    m.title[0] = m.artist[0] = '\0';
    strlcpy(m.path, path, sizeof(m.path));
    char window_title[160];
    snprintf(window_title, sizeof(window_title), "%s - Music", path_base(path));
    gui_window_set_title(m.w, window_title);

    m.data = read_file(path, &m.size);
    if (!m.data) {
        m.error = "This file can't be read";
        return false;
    }
    m.file = audio_file_open(m.data, m.size, &m.error);
    if (!m.file) {
        free(m.data);
        m.data = NULL;
        return false;
    }
    m.hz = audio_file_rate(m.file);
    strlcpy(m.title, audio_file_title(m.file), sizeof(m.title));
    strlcpy(m.artist, audio_file_artist(m.file), sizeof(m.artist));
    if (!m.title[0]) {
        /* No tag: the file name without its extension. */
        strlcpy(m.title, path_base(path), sizeof(m.title));
        char *dot = strrchr(m.title, '.');
        if (dot && dot != m.title) {
            *dot = '\0';
        }
    }
    return true;
}

/* The driver plays the song's own rate if it can; otherwise we resample to 48 kHz. */
static void open_output(void) {
    if (m.have_audio) {
        audio_ctl(OS_AUDIO_CLOSE, 0);
        m.have_audio = false;
    }
    m.out_hz = m.hz;
    int err = audio_open((uint32_t)m.hz, 2);
    if (err == -EINVAL) {
        m.out_hz = 48000;
        err = audio_open(48000, 2);
    }
    m.have_audio = err == 0;
    if (err == -ENODEV) {
        m.error = "No sound card found";
    } else if (err == -EBUSY) {
        m.error = "Another program is playing sound";
    } else if (err) {
        m.error = "The sound card can't play this";
    }
}

static void set_playing(bool on);

static void open_song(const char *path) {
    m.playing = false;
    if (load_song(path)) {
        load_folder();
        open_output();
        reset_pipeline(0);
        set_playing(true);
    } else if (m.have_audio) {
        audio_ctl(OS_AUDIO_CLOSE, 0);
        m.have_audio = false;
    }
}

/* At the end of a song with the folder looping: straight on to the next one. */
static bool next_song(void) {
    char dir[OS_PATH_MAX], path[OS_PATH_MAX];
    path_parent(m.path, dir, sizeof(dir));
    for (int tries = 0; tries < m.song_count; tries++) {
        m.song_index = (m.song_index + 1) % m.song_count;
        path_join(path, sizeof(path), dir, m.songs[m.song_index]);
        int old_hz = m.out_hz;
        long queued = m.have_audio ? audio_ctl(OS_AUDIO_QUEUED, 0) : 0;
        if (!load_song(path)) {
            continue;
        }
        if (m.hz != old_hz && m.have_audio) {
            audio_ctl(OS_AUDIO_DRAIN, 0);   /* let the last one finish before changing the rate */
            open_output();
            queued = 0;
        }
        reset_pipeline(0);
        m.switched_at = queued > 0 ? (uint64_t)queued : 0;   /* the old song's tail still in the driver */
        return true;
    }
    return false;
}

/* The next chunk of sound into `pending`: volume applied, resampled if needed. */
static bool decode_next(void) {
    long n = audio_file_read(m.file, m.decoded, CHUNK);
    if (n <= 0) {
        if (m.loop == LOOP_ONE) {
            audio_file_seek(m.file, 0);
            m.queued_until = 0;
            n = audio_file_read(m.file, m.decoded, CHUNK);
        } else if (m.loop == LOOP_ALL && next_song()) {
            n = audio_file_read(m.file, m.decoded, CHUNK);
        }
        if (n <= 0) {
            m.at_end = true;
            return false;
        }
    }
    m.queued_until += (uint64_t)n;
    int scale = m.muted ? 0 : m.volume * m.volume * 65536 / 10000;   /* a gentler curve at the low end */
    int out = 0;
    if (m.out_hz == m.hz) {
        for (long i = 0; i < 2 * n; i++) {
            m.pending[out++] = (int16_t)((m.decoded[i] * scale) >> 16);
        }
    } else {
        double step = (double)m.hz / m.out_hz;   /* linear resampling */
        for (; m.resample_pos < n; m.resample_pos += step) {
            long i = (long)m.resample_pos;
            double frac = m.resample_pos - i;
            for (int c = 0; c < 2; c++) {
                int a = m.decoded[2 * i + c], b = i + 1 < n ? m.decoded[2 * (i + 1) + c] : a;
                m.pending[out++] = (int16_t)(((int)(a + (b - a) * frac) * scale) >> 16);
            }
        }
        m.resample_pos -= n;
    }
    m.pending_bytes = out * 2;
    m.pending_offset = 0;
    return true;
}

/* Keep the driver's buffer topped up. */
static void feed(void) {
    if (!m.have_audio || !m.playing || !m.file) {
        return;
    }
    for (;;) {
        long queued = audio_ctl(OS_AUDIO_QUEUED, 0);
        if (queued < 0 || queued >= TARGET_QUEUE) {
            return;
        }
        if (m.switched_at > (uint64_t)queued) {
            m.switched_at = (uint64_t)queued;   /* the old song's tail is playing out */
        }
        if (m.pending_offset >= m.pending_bytes && !decode_next()) {
            return;
        }
        long n = audio_write((uint8_t *)m.pending + m.pending_offset, (size_t)(m.pending_bytes - m.pending_offset),
                             OS_AUDIO_NOWAIT);
        if (n <= 0) {
            return;
        }
        m.pending_offset += (int)n;
    }
}

static void set_playing(bool on) {
    if (!m.file || !m.have_audio) {
        return;
    }
    if (on && m.at_end && m.pending_offset >= m.pending_bytes) {
        seek_to(0);   /* play again from the start */
    }
    m.playing = on;
    audio_ctl(on ? OS_AUDIO_RESUME : OS_AUDIO_PAUSE, 0);
    feed();
}

/* --- drawing ---------------------------------------------------------------------------------- */

static struct gfx_rect control_rect(int c) {
    int cy = WIN_H - 40;
    switch (c) {
    case C_LOOP:    return gfx_rect(24, cy - 17, 34, 34);
    case C_BACK:    return gfx_rect(WIN_W / 2 - 74, cy - 18, 36, 36);
    case C_PLAY:    return gfx_rect(WIN_W / 2 - 24, cy - 24, 48, 48);
    case C_FORWARD: return gfx_rect(WIN_W / 2 + 38, cy - 18, 36, 36);
    case C_MUTE:    return gfx_rect(WIN_W - 24 - 80 - 36, cy - 15, 30, 30);
    case C_SEEK:    return gfx_rect(24, 128, WIN_W - 48, 14);
    case C_VOLUME:  return gfx_rect(WIN_W - 24 - 80, cy - 7, 80, 14);
    default:        return gfx_rect(0, 0, 0, 0);
    }
}

static int control_at(int x, int y) {
    for (int c = 0; c < C_COUNT; c++) {
        if (gfx_rect_contains(gfx_rect_inset(control_rect(c), c >= C_SEEK ? -6 : 0), x, y)) {
            return c;
        }
    }
    return C_NONE;
}

static void format_time(double s, char *out, size_t n) {
    int t = (int)s;
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void draw_skip(struct gfx_surface *s, float cx, float cy, bool forward, gfx_color ink) {
    float d = forward ? 1 : -1;
    for (int k = 0; k < 2; k++) {
        float x = cx + d * (k * 8 - 4);
        float pts[6] = { x - d * 6, cy - 7, x + d * 5, cy, x - d * 6, cy + 7 };
        gfx_fill_polygon(s, pts, 3, ink);
    }
}

/* Two arrows chasing each other around a rounded loop; a "1" badge for one song. */
static void draw_loop(struct gfx_surface *s, float cx, float cy, gfx_color ink, bool one) {
    const float w = 1.8f;
    gfx_draw_line(s, cx - 7, cy - 6, cx + 5, cy - 6, w, ink);
    gfx_draw_line(s, cx + 7, cy + 6, cx - 5, cy + 6, w, ink);
    gfx_draw_line(s, cx - 10, cy + 3, cx - 10, cy - 3, w, ink);
    gfx_draw_line(s, cx + 10, cy - 3, cx + 10, cy + 3, w, ink);
    gfx_draw_line(s, cx - 10, cy - 3, cx - 7, cy - 6, w, ink);
    gfx_draw_line(s, cx + 10, cy + 3, cx + 7, cy + 6, w, ink);
    float a1[6] = { cx + 3, cy - 10, cx + 9, cy - 6, cx + 3, cy - 2 };
    float a2[6] = { cx - 3, cy + 2, cx - 9, cy + 6, cx - 3, cy + 10 };
    gfx_fill_polygon(s, a1, 3, ink);
    gfx_fill_polygon(s, a2, 3, ink);
    if (one) {
        gfx_fill_circle(s, cx + 10, cy - 9, 6.5f, ACCENT);
        gfx_fill_circle(s, cx + 10, cy - 9, 5.5f, WHITE);
        float tw = ttf_text_width(m.bold, 9, "1");
        ttf_draw_text(s, m.bold, 9, cx + 10 - tw / 2, cy - 5.8f, ACCENT, "1");
    }
}

static void draw_slider(struct gfx_surface *s, struct gfx_rect r, double fraction, bool active) {
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    int cy = r.y + r.h / 2;
    gfx_fill_rounded_rect(s, gfx_rect(r.x, cy - 2, r.w, 4), 2, TRACK);
    int fill = (int)(r.w * fraction + 0.5);
    if (fill > 0) {
        gfx_fill_rounded_rect(s, gfx_rect(r.x, cy - 2, fill, 4), 2, ACCENT);
    }
    gfx_fill_circle(s, r.x + fill, cy, active ? 7 : 6, ACCENT);
    gfx_fill_circle(s, r.x + fill, cy, active ? 3 : 2.5f, WHITE);
}

static void draw(void) {
    struct gfx_surface *s = gui_window_surface(m.w);
    gfx_clear(s, WHITE);
    if (m.art) {
        gfx_blit(s, 24, 20, m.art, NULL);
    }

    /* Title, artist, and format / place in the folder. */
    int tx = 24 + ART + 20;
    gfx_set_clip(s, gfx_rect(tx, 0, WIN_W - tx - 20, WIN_H));
    if (!m.path[0]) {
        ttf_draw_text(s, m.bold, 16, tx, 56, INK, "Nothing playing");
        ttf_draw_text(s, m.ui, 13, tx, 80, INK_SOFT, "Open a song from Files, or drop one here.");
    } else {
        ttf_draw_text(s, m.bold, 16, tx, 56, INK, m.file ? m.title : path_base(m.path));
        ttf_draw_text(s, m.ui, 13, tx, 80, INK_SOFT, m.error ? m.error : m.artist[0] ? m.artist : "Unknown artist");
        if (m.file) {
            char where[64];
            if (m.song_count > 1 && m.song_index >= 0) {
                snprintf(where, sizeof(where), "%s  -  %d of %d", audio_file_format(m.file), m.song_index + 1,
                         m.song_count);
            } else {
                strlcpy(where, audio_file_format(m.file), sizeof(where));
            }
            ttf_draw_text(s, m.ui, 12, tx, 102, INK_FAINT, where);
        }
    }
    gfx_reset_clip(s);

    /* Seek bar and times. */
    double now = m.dragging_seek ? m.drag_seconds : seconds_now(), total = total_seconds();
    struct gfx_rect bar = control_rect(C_SEEK);
    draw_slider(s, bar, total > 0 ? now / total : 0, m.hover == C_SEEK || m.dragging_seek);
    char t1[16], t2[16];
    format_time(now, t1, sizeof(t1));
    format_time(total, t2, sizeof(t2));
    ttf_draw_text(s, m.ui, 12, bar.x, bar.y + 30, INK_SOFT, t1);
    ttf_draw_text(s, m.ui, 12, bar.x + bar.w - ttf_text_width(m.ui, 12, t2), bar.y + 30, INK_SOFT, t2);

    /* Buttons. */
    bool usable = m.file && m.have_audio;
    for (int c = C_LOOP; c <= C_FORWARD; c++) {
        struct gfx_rect r = control_rect(c);
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
        if (c == C_PLAY) {
            gfx_color fill = !usable ? INK_FAINT : m.pressed == c ? GFX_RGB(0x20, 0x55, 0xC0)
                           : m.hover == c ? GFX_RGB(0x45, 0x80, 0xF5) : ACCENT;
            gfx_fill_circle(s, cx, cy, 24, fill);
            if (m.playing) {
                gfx_fill_rounded_rect(s, gfx_rect((int)cx - 8, (int)cy - 9, 6, 18), 2, WHITE);
                gfx_fill_rounded_rect(s, gfx_rect((int)cx + 2, (int)cy - 9, 6, 18), 2, WHITE);
            } else {
                float pts[6] = { cx - 6, cy - 10, cx + 11, cy, cx - 6, cy + 10 };
                gfx_fill_polygon(s, pts, 3, WHITE);
            }
            continue;
        }
        if (m.hover == c || m.pressed == c) {
            gfx_fill_circle(s, cx, cy, r.w / 2.0f, HOVER);
        }
        if (c == C_LOOP) {
            draw_loop(s, cx, cy, m.loop == LOOP_OFF ? INK_SOFT : ACCENT, m.loop == LOOP_ONE);
        } else {
            draw_skip(s, cx, cy, c == C_FORWARD, usable ? INK : INK_FAINT);
        }
    }

    /* Volume: the speaker (click to mute) and a slider. */
    struct gfx_rect mute = control_rect(C_MUTE);
    if (m.hover == C_MUTE || m.pressed == C_MUTE) {
        gfx_fill_circle(s, mute.x + mute.w / 2.0f, mute.y + mute.h / 2.0f, mute.w / 2.0f, HOVER);
    }
    int level = m.muted || m.volume == 0 ? 0 : m.volume < 34 ? 1 : m.volume < 67 ? 2 : 3;
    if (m.volume_icons[level]) {
        gfx_blit(s, mute.x + (mute.w - VOLUME_ICON) / 2, mute.y + (mute.h - VOLUME_ICON) / 2, m.volume_icons[level],
                 NULL);
    }
    draw_slider(s, control_rect(C_VOLUME), m.muted ? 0 : m.volume / 100.0,
                m.hover == C_VOLUME || m.pressed == C_VOLUME);

    gui_window_invalidate(m.w, NULL);
}

/* --- input -------------------------------------------------------------------------------------- */

static double seek_fraction(int x) {
    struct gfx_rect r = control_rect(C_SEEK);
    double f = (x - r.x) / (double)r.w;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void set_volume_at(int x) {
    struct gfx_rect r = control_rect(C_VOLUME);
    int v = (int)((x - r.x) * 100.0 / r.w + 0.5);
    m.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    m.muted = false;
}

static void change_volume(int delta) {
    m.volume += delta;
    m.volume = m.volume < 0 ? 0 : m.volume > 100 ? 100 : m.volume;
    m.muted = false;
}

static void mouse_down(const struct gui_event *ev) {
    if (ev->button != OS_MOUSE_LEFT) {
        return;
    }
    int c = control_at(ev->x, ev->y);
    m.pressed = c;
    if (c == C_SEEK && m.file) {
        m.dragging_seek = true;
        m.drag_seconds = seek_fraction(ev->x) * total_seconds();
    } else if (c == C_VOLUME) {
        set_volume_at(ev->x);
    }
}

static void mouse_up(const struct gui_event *ev) {
    int c = m.pressed;
    m.pressed = C_NONE;
    if (m.dragging_seek) {
        m.dragging_seek = false;
        seek_to(seek_fraction(ev->x) * total_seconds());
        feed();
        return;
    }
    if (c == C_NONE || control_at(ev->x, ev->y) != c) {
        return;
    }
    switch (c) {
    case C_PLAY:    set_playing(!m.playing); break;
    case C_BACK:    seek_to(seconds_now() - SKIP_STEP); feed(); break;
    case C_FORWARD: seek_to(seconds_now() + SKIP_STEP); feed(); break;
    case C_LOOP:    m.loop = (enum loop)((m.loop + 1) % 3); break;
    case C_MUTE:    m.muted = !m.muted; break;
    default:        break;
    }
}

static void mouse_move(const struct gui_event *ev) {
    if (m.dragging_seek) {
        m.drag_seconds = seek_fraction(ev->x) * total_seconds();
    } else if (m.pressed == C_VOLUME && (ev->buttons & OS_MOUSE_LEFT)) {
        set_volume_at(ev->x);
    }
    m.hover = control_at(ev->x, ev->y);
    gui_window_set_cursor(m.w, m.hover != C_NONE ? GUI_CURSOR_HAND : GUI_CURSOR_ARROW);
}

static void key(const struct gui_event *ev) {
    switch (ev->key) {
    case OS_KEY_SPACE: set_playing(!m.playing); return;
    case OS_KEY_LEFT:  seek_to(seconds_now() - SEEK_STEP); feed(); return;
    case OS_KEY_RIGHT: seek_to(seconds_now() + SEEK_STEP); feed(); return;
    case OS_KEY_UP:    change_volume(5); return;
    case OS_KEY_DOWN:  change_volume(-5); return;
    case OS_KEY_HOME:  seek_to(0); feed(); return;
    default:           break;
    }
    if (ev->ch == 'l' || ev->ch == 'L') {
        m.loop = (enum loop)((m.loop + 1) % 3);
    } else if (ev->ch == 'm' || ev->ch == 'M') {
        m.muted = !m.muted;
    }
}

int main(int argc, char **argv) {
    m.g = gui_open();
    if (!m.g) {
        return 1;
    }
    m.ui = gui_font(m.g, GUI_FONT_REGULAR);
    m.bold = gui_font(m.g, GUI_FONT_BOLD);
    if (!m.ui || !m.bold) {
        return 1;
    }
    m.art = load_icon(_PATH_DDE_RES "/Icons/FilesFolders/Audio.png", ART);
    static const char *const levels[] = { "Mute", "Low", "Medium", "High" };
    for (int i = 0; i < 4; i++) {
        char path[96];
        snprintf(path, sizeof(path), _PATH_DDE_RES "/Icons/Status/Volume%s.png", levels[i]);
        m.volume_icons[i] = load_icon(path, VOLUME_ICON);
    }
    m.hover = m.pressed = C_NONE;
    m.volume = 100;   /* the panel's volume control sets the overall level */
    m.w = gui_window_create(m.g, "Music", -1, -1, WIN_W, WIN_H);
    if (!m.w) {
        return 1;
    }
    if (argc > 1) {
        open_song(argv[1]);
    }
    draw();

    for (;;) {
        struct gui_event ev;
        bool got = gui_wait_event(m.g, &ev, m.playing ? TICK_MS : -1);
        if (got) {
            switch (ev.type) {
            case GUI_EVENT_QUIT:
            case GUI_EVENT_CLOSE:
                if (m.have_audio) {
                    audio_ctl(OS_AUDIO_CLOSE, 0);
                }
                gui_close(m.g);
                return 0;
            case GUI_EVENT_MOUSE_DOWN: mouse_down(&ev); break;
            case GUI_EVENT_MOUSE_UP:   mouse_up(&ev); break;
            case GUI_EVENT_MOUSE_MOVE: mouse_move(&ev); break;
            case GUI_EVENT_KEY:        key(&ev); break;
            case GUI_EVENT_SCROLL:     change_volume(ev.wheel < 0 ? 5 : -5); break;
            case GUI_EVENT_DROP: {
                char paths[1][OS_PATH_MAX];
                if (gui_drop_paths(paths, 1) == 1) {
                    open_song(paths[0]);
                }
                break;
            }
            default:
                break;
            }
        }
        feed();
        /* The song ran out (and nothing loops): stop once the last of it has played. */
        if (m.playing && m.at_end && m.pending_offset >= m.pending_bytes &&
            audio_ctl(OS_AUDIO_QUEUED, 0) <= 1024) {
            m.playing = false;
            audio_ctl(OS_AUDIO_PAUSE, 0);
        }
        draw();
    }
}
