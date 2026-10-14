/*
 * Intel High Definition Audio, after Linux's sound/hda:
 *
 *   core/controller.c   link reset, CORB/RIRB command rings     (reset_link, init_cmd_io)
 *   core/stream.c       stream descriptor reset and setup       (stream_reset, stream_setup)
 *   core/device.c       the SDnFMT encoding of rates            (rate_bits[])
 *   codecs/generic.c    finding a DAC -> ... -> output pin path, unmuting it, pin control
 *
 * Much simplified: one hardware output stream, no interrupts. Several programs can play at once
 * (music and the system's sounds): each has a stream of its own here, a queue in its own format,
 * and a mixer adds them up (resampled to the hardware's rate, linearly) into the ring a little ahead
 * of the hardware. The hardware runs at the rate of the first stream opened, so that one plays
 * untouched. A kernel thread polls the position (LPIB) every few milliseconds, silences what has
 * been played and mixes more. The ring is OS_AUDIO_BUFFER bytes in 4 KiB pages, one Buffer
 * Descriptor List entry per page.
 */

#include <stddef.h>

#include "drivers/hda.h"
#include "drivers/pci.h"
#include <viralos.h>
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "proc/usercopy.h"
#include "sched/sched.h"
#include "sched/sync.h"

/* --- controller registers ------------------------------------------------------------------------ */

#define GCAP       0x00
#define GCTL       0x08
#define  GCTL_RESET  0x01
#define STATESTS   0x0E
#define INTCTL     0x20
#define INTSTS     0x24
#define CORBLBASE  0x40
#define CORBUBASE  0x44
#define CORBWP     0x48
#define CORBRP     0x4A
#define  CORBRP_RST  0x8000
#define CORBCTL    0x4C
#define  CORBCTL_RUN 0x02
#define CORBSIZE   0x4E
#define RIRBLBASE  0x50
#define RIRBUBASE  0x54
#define RIRBWP     0x58
#define  RIRBWP_RST  0x8000
#define RINTCNT    0x5A
#define RIRBCTL    0x5C
#define  RIRBCTL_DMA 0x02
#define RIRBSTS    0x5D
#define RIRBSIZE   0x5E
#define DPLBASE    0x70
#define DPUBASE    0x74

/* Stream descriptors: input streams first, then output, then bidirectional. */
#define SD_BASE    0x80
#define SD_SIZE    0x20
#define SD_CTL     0x00     /* 3 bytes; byte 2 holds the stream tag */
#define  SD_CTL_SRST 0x01
#define  SD_CTL_RUN  0x02
#define SD_STS     0x03
#define SD_LPIB    0x04
#define SD_CBL     0x08
#define SD_LVI     0x0C
#define SD_FIFOS   0x10
#define SD_FMT     0x12
#define SD_BDPL    0x18
#define SD_BDPU    0x1C

/* --- codec verbs and parameters (hda_verbs.h) ------------------------------------------------------------ */

#define VERB_GET_PARAMETER      0xF00
#define VERB_GET_CONNECT_LIST   0xF02
#define VERB_SET_CONNECT_SEL    0x701
#define VERB_SET_POWER_STATE    0x705
#define VERB_SET_CHANNEL_STREAMID 0x706
#define VERB_SET_PIN_WIDGET_CONTROL 0x707
#define VERB_SET_EAPD_BTLENABLE 0x70C
#define VERB_GET_CONFIG_DEFAULT 0xF1C
#define VERB4_SET_STREAM_FORMAT 0x2   /* 4-bit verbs carry a 16-bit payload */
#define VERB4_SET_AMP_GAIN_MUTE 0x3

#define PAR_NODE_COUNT    0x04
#define PAR_FUNCTION_TYPE 0x05
#define PAR_AUDIO_WIDGET_CAP 0x09
#define PAR_PCM           0x0A
#define PAR_PIN_CAP       0x0C
#define PAR_AMP_IN_CAP    0x0D
#define PAR_CONNLIST_LEN  0x0E
#define PAR_AMP_OUT_CAP   0x12

#define WCAP_IN_AMP    (1u << 1)
#define WCAP_OUT_AMP   (1u << 2)
#define WCAP_AMP_OVRD  (1u << 3)
#define WCAP_CONN_LIST (1u << 8)
#define WCAP_TYPE(c)   (((c) >> 20) & 0xF)
enum { WID_AUD_OUT = 0, WID_AUD_IN, WID_AUD_MIX, WID_AUD_SEL, WID_PIN };

#define PINCAP_OUT  (1u << 4)
#define PINCAP_HP   (1u << 3)
#define PINCAP_EAPD (1u << 16)
#define PINCTL_OUT_EN 0x40
#define PINCTL_HP_EN  0x80

#define AMP_SET_OUTPUT (1u << 15)
#define AMP_SET_INPUT  (1u << 14)
#define AMP_SET_BOTH   (3u << 12)    /* left and right */
#define AMPCAP_OFFSET(c) ((c) & 0x7F)

/* Default configuration (pin "jack" description from the BIOS). */
#define DEFCFG_CONN(c)   (((c) >> 30) & 3)    /* 1: nothing connected */
#define DEFCFG_DEVICE(c) (((c) >> 20) & 0xF)  /* 0 line out, 1 speaker, 2 headphones */

/* --- state ------------------------------------------------------------------------------------------- */

#define MAX_NODES   128
#define RING_PAGES  (OS_AUDIO_BUFFER / 4096)
#define MARGIN      1024        /* stay this far ahead of the hardware when restarting after an underrun */
#define POLL_MS     10

static volatile uint8_t *regs;
static int codec = -1;          /* address of the codec we use */
static uint8_t afg;             /* its audio function group node */
static uint32_t *corb, *rirb;
static uint16_t rirb_rp;
static int out_stream = -1;     /* descriptor index */
static uint8_t dac;
static struct output {
    uint8_t path[8];             /* pin first, DAC last */
    int len;
    int device;                  /* DEFCFG_DEVICE: 0 line out, 1 speaker, 2 headphones */
    char name[48];
} outputs[4];
static int pin_count, current_output;

/* Volume: on the codec's amplifier when the path has one with steps, else in software. */
static uint8_t volume_nid;
static uint32_t volume_cap;
static uint32_t volume = 40;     /* 0-100 */
static bool muted;
static uint32_t dac_rates;      /* PAR_PCM rate bits the converter supports */
static uint64_t ring_phys[RING_PAGES];
static bool present;
static uint32_t codec_id;

static struct mutex lock = MUTEX_INIT;
static bool running;            /* the hardware stream */
static uint32_t hw_rate;        /* its rate (always 16-bit stereo) */
static uint64_t written, played; /* ring bytes mixed / played since it started (they only grow) */
static uint32_t last_lpib;

/* A program's stream: what it has written, in its own format, waiting to be mixed. */
#define MAX_STREAMS 8
#define QUEUE_BYTES OS_AUDIO_BUFFER
#define AHEAD       (2048 * 4)  /* mix this far ahead of the hardware (~45 ms) */

static struct stream {
    uint64_t pid;               /* 0: free */
    uint32_t rate, channels;
    bool paused;
    int16_t *queue;             /* QUEUE_BYTES, a ring of samples */
    uint32_t head, count;       /* in samples */
    uint64_t pos;               /* 32.32: where the mixer is, in frames past head */
    uint64_t step;              /* 32.32: frames of ours per hardware frame */
} streams[MAX_STREAMS];

static inline uint8_t r8(uint32_t o) { return *(volatile uint8_t *)(regs + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void w8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(regs + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
static inline uint32_t sd(uint32_t reg) { return SD_BASE + (uint32_t)out_stream * SD_SIZE + reg; }

static bool wait_for(uint32_t reg, uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i <= ms; i++) {
        if ((r32(reg) & mask) == want) {
            return true;
        }
        sched_sleep_ms(1);
    }
    return false;
}

static bool wait_for8(uint32_t reg, uint8_t mask, uint8_t want, int ms) {
    for (int i = 0; i <= ms; i++) {
        if ((r8(reg) & mask) == want) {
            return true;
        }
        sched_sleep_ms(1);
    }
    return false;
}

/* --- codec commands through the CORB/RIRB rings (controller.c) ----------------------------------------------- */

static void init_cmd_io(void) {
    uint64_t page = pmm_alloc_page();
    memset(phys_to_virt(page), 0, 4096);
    corb = phys_to_virt(page);
    rirb = phys_to_virt(page + 2048);

    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait_for8(CORBCTL, CORBCTL_RUN, 0, 10);
    w32(CORBLBASE, (uint32_t)page);
    w32(CORBUBASE, (uint32_t)(page >> 32));
    w8(CORBSIZE, 0x02);                      /* 256 entries */
    w16(CORBWP, 0);
    w16(CORBRP, CORBRP_RST);                 /* reset the read pointer, then clear the bit */
    for (int i = 0; i < 10 && !(r16(CORBRP) & CORBRP_RST); i++) {
        sched_sleep_ms(1);
    }
    w16(CORBRP, 0);
    for (int i = 0; i < 10 && (r16(CORBRP) & CORBRP_RST); i++) {
        sched_sleep_ms(1);
    }
    w8(CORBCTL, CORBCTL_RUN);

    w32(RIRBLBASE, (uint32_t)(page + 2048));
    w32(RIRBUBASE, (uint32_t)((page + 2048) >> 32));
    w8(RIRBSIZE, 0x02);
    w16(RIRBWP, RIRBWP_RST);
    w16(RINTCNT, 0xFF);
    w8(RIRBCTL, RIRBCTL_DMA);                /* no interrupts: we poll */
    rirb_rp = 0;
}

/* Send one verb, wait for its answer (0xFFFFFFFF if none came). */
static uint32_t command(uint32_t cad, uint32_t nid, uint32_t verb, uint32_t payload) {
    uint32_t cmd = cad << 28 | nid << 20;
    cmd |= verb <= 0xF ? (verb << 16 | (payload & 0xFFFF)) : (verb << 8 | (payload & 0xFF));
    uint16_t wp = (uint16_t)((r16(CORBWP) + 1) & 0xFF);
    corb[wp] = cmd;
    w16(CORBWP, wp);
    for (int tries = 0; tries < 200; tries++) {
        uint16_t hw = r16(RIRBWP) & 0xFF;
        while (rirb_rp != hw) {
            rirb_rp = (uint16_t)((rirb_rp + 1) & 0xFF);
            uint32_t response = rirb[rirb_rp * 2], extra = rirb[rirb_rp * 2 + 1];
            /* Acknowledge, as Linux's interrupt handler does: until we do, the
               controller stops after RINTCNT responses. */
            w8(RIRBSTS, 0x05);
            if (!(extra & 0x10)) {   /* skip unsolicited responses */
                return response;
            }
        }
        if (tries < 50) {
            for (volatile int spin = 0; spin < 2000; spin++) {
            }
        } else {
            sched_sleep_ms(1);
        }
    }
    return 0xFFFFFFFFu;
}

static uint32_t param(uint32_t nid, uint32_t which) {
    return command((uint32_t)codec, nid, VERB_GET_PARAMETER, which);
}

static uint32_t verb(uint32_t nid, uint32_t v, uint32_t payload) {
    return command((uint32_t)codec, nid, v, payload);
}

/* --- finding an output path (generic.c, much reduced) --------------------------------------------------- */

static uint32_t wcaps[MAX_NODES];
static uint8_t node_start, node_end;   /* widgets of the audio function group: [start, end) */

/* A node's inputs, ranges expanded (snd_hda_get_raw_connections). */
static int connections(uint8_t nid, uint8_t *out, int max) {
    if (!(wcaps[nid] & WCAP_CONN_LIST)) {
        return 0;
    }
    uint32_t len = param(nid, PAR_CONNLIST_LEN);
    bool long_form = len & 0x80;
    int count = (int)(len & 0x7F), n = 0, per = long_form ? 2 : 4;
    uint32_t shift = long_form ? 16 : 8, mask = long_form ? 0x7FFF : 0x7F, range = long_form ? 0x8000 : 0x80;
    int prev = 0;
    for (int i = 0; i < count; i += per) {
        uint32_t entries = verb(nid, VERB_GET_CONNECT_LIST, (uint32_t)i);
        for (int k = 0; k < per && i + k < count; k++) {
            uint32_t e = (entries >> (k * shift)) & ((1u << shift) - 1);
            int value = (int)(e & mask);
            if ((e & range) && prev) {
                for (int v = prev + 1; v <= value && n < max; v++) {
                    out[n++] = (uint8_t)v;
                }
            } else if (n < max) {
                out[n++] = (uint8_t)value;
            }
            prev = value;
        }
    }
    return n;
}

/* Depth-first from a pin towards a DAC. path[0] is the pin; returns the length, 0 if none. */
static int find_dac(uint8_t nid, uint8_t *path, int depth) {
    if (depth >= 8 || nid >= MAX_NODES) {
        return 0;
    }
    path[depth] = nid;
    int type = WCAP_TYPE(wcaps[nid]);
    if (type == WID_AUD_OUT) {
        return depth + 1;
    }
    if (depth > 0 && type != WID_AUD_MIX && type != WID_AUD_SEL) {
        return 0;
    }
    uint8_t in[32];
    int n = connections(nid, in, 32);
    for (int i = 0; i < n; i++) {
        int len = find_dac(in[i], path, depth + 1);
        if (len) {
            return len;
        }
    }
    return 0;
}

static void unmute_output(uint8_t nid) {
    if (!(wcaps[nid] & WCAP_OUT_AMP)) {
        return;
    }
    uint32_t cap = param(wcaps[nid] & WCAP_AMP_OVRD ? nid : afg, PAR_AMP_OUT_CAP);
    verb(nid, VERB4_SET_AMP_GAIN_MUTE, AMP_SET_OUTPUT | AMP_SET_BOTH | AMPCAP_OFFSET(cap));   /* 0 dB */
}

static void unmute_input(uint8_t nid, int index) {
    if (!(wcaps[nid] & WCAP_IN_AMP)) {
        return;
    }
    uint32_t cap = param(wcaps[nid] & WCAP_AMP_OVRD ? nid : afg, PAR_AMP_IN_CAP);
    verb(nid, VERB4_SET_AMP_GAIN_MUTE,
         AMP_SET_INPUT | AMP_SET_BOTH | (uint32_t)index << 8 | AMPCAP_OFFSET(cap));
}

/* Power up, select and unmute every widget on the way, then enable the pin. */
static void enable_path(const uint8_t *path, int len, bool headphone) {
    if (!len) {
        return;
    }
    for (int i = 0; i < len; i++) {
        uint8_t nid = path[i];
        verb(nid, VERB_SET_POWER_STATE, 0);   /* D0 */
        unmute_output(nid);
        if (i + 1 < len) {
            uint8_t in[32];
            int n = connections(nid, in, 32);
            for (int k = 0; k < n; k++) {
                if (in[k] == path[i + 1]) {
                    if (WCAP_TYPE(wcaps[nid]) != WID_AUD_MIX) {
                        verb(nid, VERB_SET_CONNECT_SEL, (uint32_t)k);
                    }
                    unmute_input(nid, k);
                }
            }
        }
    }
    uint8_t pin = path[0];
    uint32_t pincap = param(pin, PAR_PIN_CAP);
    verb(pin, VERB_SET_PIN_WIDGET_CONTROL, PINCTL_OUT_EN | (headphone && (pincap & PINCAP_HP) ? PINCTL_HP_EN : 0));
    if (pincap & PINCAP_EAPD) {
        verb(pin, VERB_SET_EAPD_BTLENABLE, 0x02);   /* external amplifier on */
    }
}

/* The amplifier gain for a volume: perceptual (squared), with 0 dB at 100%. */
static void apply_volume(void) {
    if (!volume_nid) {
        return;   /* software volume: applied as samples are queued */
    }
    uint32_t offset = AMPCAP_OFFSET(volume_cap), step = ((volume_cap >> 16) & 0x7F) + 1;   /* quarter dB */
    uint32_t gain;
    if (codec_id >> 16 == 0x1AF4) {
        /* QEMU's codec scales linearly with the step instead of by decibels. */
        gain = offset * volume * volume / 10000;
    } else {
        /* Up to 60 dB of attenuation, more of it at the low end. */
        uint32_t quarter_db = 240 * (100 - volume) * (100 - volume) / 10000;
        uint32_t steps = (quarter_db + step / 2) / step;
        gain = steps > offset ? 0 : offset - steps;
    }
    bool silent = muted || volume == 0;
    verb(volume_nid, VERB4_SET_AMP_GAIN_MUTE, AMP_SET_OUTPUT | AMP_SET_BOTH | (silent ? 0x80 : 0) | gain);
}

/* Sound to one output: its pin on, the others off; find the amplifier for the volume on its path. */
static void select_output(int index) {
    for (int i = 0; i < pin_count; i++) {
        if (i != index) {
            verb(outputs[i].path[0], VERB_SET_PIN_WIDGET_CONTROL, 0);
        }
    }
    struct output *out = &outputs[index];
    enable_path(out->path, out->len, out->device == 2);
    current_output = index;
    volume_nid = 0;
    for (int i = out->len - 1; i >= 0 && !volume_nid; i--) {   /* from the DAC towards the pin */
        uint8_t nid = out->path[i];
        if (!(wcaps[nid] & WCAP_OUT_AMP)) {
            continue;
        }
        uint32_t cap = param(wcaps[nid] & WCAP_AMP_OVRD ? nid : afg, PAR_AMP_OUT_CAP);
        if ((cap >> 8) & 0x7F) {   /* it has steps */
            volume_nid = nid;
            volume_cap = cap;
        }
    }
    apply_volume();
}

static bool setup_codec(void) {
    /* The audio function group among the root's children. */
    uint32_t count = param(0, PAR_NODE_COUNT);
    uint8_t start = (uint8_t)(count >> 16), n = (uint8_t)count;
    afg = 0;
    for (uint8_t fg = start; fg < start + n; fg++) {
        if ((param(fg, PAR_FUNCTION_TYPE) & 0xFF) == 1) {
            afg = fg;
            break;
        }
    }
    if (!afg) {
        return false;
    }
    verb(afg, VERB_SET_POWER_STATE, 0);
    sched_sleep_ms(10);
    count = param(afg, PAR_NODE_COUNT);
    node_start = (uint8_t)(count >> 16);
    node_end = (uint8_t)(node_start + (uint8_t)count);
    if (node_end > MAX_NODES) {
        node_end = MAX_NODES;
    }
    for (int nid = node_start; nid < node_end; nid++) {
        wcaps[nid] = param((uint32_t)nid, PAR_AUDIO_WIDGET_CAP);
    }

    /* Output pins, best first: speakers and line out, then headphones (all of them get the sound). */
    static const int order[] = { 1, 0, 2 };
    dac = 0;
    pin_count = 0;
    for (int o = 0; o < 3; o++) {
        for (int nid = node_start; nid < node_end && pin_count < 4; nid++) {
            if (WCAP_TYPE(wcaps[nid]) != WID_PIN || !(param((uint32_t)nid, PAR_PIN_CAP) & PINCAP_OUT)) {
                continue;
            }
            uint32_t cfg = verb((uint32_t)nid, VERB_GET_CONFIG_DEFAULT, 0);
            if (DEFCFG_CONN(cfg) == 1 || (int)DEFCFG_DEVICE(cfg) != order[o]) {
                continue;
            }
            uint8_t path[8];
            int len = find_dac((uint8_t)nid, path, 0);
            if (!len || (dac && path[len - 1] != dac)) {
                continue;   /* one stream, so only pins reachable from the same DAC */
            }
            dac = path[len - 1];
            struct output *out = &outputs[pin_count++];
            memcpy(out->path, path, (size_t)len);
            out->len = len;
            out->device = order[o];
            static const char *const names[] = { "Line out", "Speakers", "Headphones" };
            size_t at = strlcpy(out->name, names[order[o]], sizeof(out->name));
            strlcpy(out->name + at, " (HD Audio)", sizeof(out->name) - at);
        }
    }
    if (!dac) {
        return false;
    }
    select_output(0);
    dac_rates = param(dac, PAR_PCM);
    if (!(dac_rates & 0xFFF)) {
        dac_rates = param(afg, PAR_PCM);   /* the group's defaults */
    }
    return true;
}

/* --- the stream (stream.c) ----------------------------------------------------------------------------- */

/* rate = base * mult / div, with its PAR_PCM bit (device.c's rate_bits[]). */
static const struct {
    uint32_t rate, bit;
    uint16_t fmt;
} rates[] = {
    { 8000, 0, 0x0500 }, { 11025, 1, 0x4300 }, { 16000, 2, 0x0200 }, { 22050, 3, 0x4100 },
    { 32000, 4, 0x0A00 }, { 44100, 5, 0x4000 }, { 48000, 6, 0x0000 }, { 88200, 7, 0x4800 },
    { 96000, 8, 0x0800 }, { 176400, 9, 0x5800 }, { 192000, 10, 0x1800 },
};

static void stream_reset(void) {
    w8(sd(SD_CTL), (uint8_t)(r8(sd(SD_CTL)) & ~SD_CTL_RUN));
    wait_for8(sd(SD_CTL), SD_CTL_RUN, 0, 20);
    w8(sd(SD_CTL), (uint8_t)(r8(sd(SD_CTL)) | SD_CTL_SRST));
    wait_for8(sd(SD_CTL), SD_CTL_SRST, SD_CTL_SRST, 20);
    w8(sd(SD_CTL), (uint8_t)(r8(sd(SD_CTL)) & ~SD_CTL_SRST));
    wait_for8(sd(SD_CTL), SD_CTL_SRST, 0, 20);
    w8(sd(SD_STS), 0x1C);   /* clear status bits */
}

static void ring_clear(void) {
    for (int i = 0; i < RING_PAGES; i++) {
        memset(phys_to_virt(ring_phys[i]), 0, 4096);
    }
}

static bool stream_start(uint16_t fmt) {
    stream_reset();
    ring_clear();
    static uint64_t bdl_phys;
    if (!bdl_phys) {
        bdl_phys = pmm_alloc_page();
        if (!bdl_phys) {
            return false;
        }
    }
    uint32_t *bdl = phys_to_virt(bdl_phys);
    memset(bdl, 0, 4096);
    for (int i = 0; i < RING_PAGES; i++) {
        bdl[i * 4 + 0] = (uint32_t)ring_phys[i];
        bdl[i * 4 + 1] = (uint32_t)(ring_phys[i] >> 32);
        bdl[i * 4 + 2] = 4096;
        bdl[i * 4 + 3] = 0;   /* no interrupt on completion */
    }
    const uint8_t tag = 1;
    w8(sd(SD_CTL + 2), (uint8_t)((r8(sd(SD_CTL + 2)) & 0x0F) | tag << 4));
    w32(sd(SD_CBL), OS_AUDIO_BUFFER);
    w16(sd(SD_FMT), fmt);
    w16(sd(SD_LVI), RING_PAGES - 1);
    w32(sd(SD_BDPL), (uint32_t)bdl_phys);
    w32(sd(SD_BDPU), (uint32_t)(bdl_phys >> 32));

    /* The converter: same format, listening to our stream tag. */
    verb(dac, VERB4_SET_STREAM_FORMAT, fmt);
    verb(dac, VERB_SET_CHANNEL_STREAMID, (uint32_t)tag << 4);
    sched_sleep_ms(1);

    written = played = MARGIN;   /* the first data goes a little ahead of the start */
    last_lpib = 0;
    w8(sd(SD_CTL), (uint8_t)(r8(sd(SD_CTL)) | SD_CTL_RUN));
    running = true;
    return true;
}

static void stream_stop(void) {
    if (!running) {
        return;
    }
    w8(sd(SD_CTL), (uint8_t)(r8(sd(SD_CTL)) & ~SD_CTL_RUN));
    wait_for8(sd(SD_CTL), SD_CTL_RUN, 0, 20);
    verb(dac, VERB_SET_CHANNEL_STREAMID, 0);
    running = false;
}

static void mix(void);

/* Follow the hardware: silence what it has played since last time. Call with the lock held. */
static void update_position(void) {
    if (!running) {
        return;
    }
    uint32_t lpib = r32(sd(SD_LPIB)) % OS_AUDIO_BUFFER;
    uint32_t delta = (lpib + OS_AUDIO_BUFFER - last_lpib) % OS_AUDIO_BUFFER;
    for (uint32_t pos = last_lpib, left = delta; left;) {
        uint32_t page = pos / 4096, off = pos % 4096, n = 4096 - off;
        n = n > left ? left : n;
        memset((uint8_t *)phys_to_virt(ring_phys[page]) + off, 0, n);
        pos = (pos + n) % OS_AUDIO_BUFFER;
        left -= n;
    }
    last_lpib = lpib;
    played += delta;
    if (written < played + MARGIN / 2) {
        written = played + MARGIN;   /* ran dry: new data goes just ahead of the hardware */
    }
}

/* --- the mixer -------------------------------------------------------------------------------------------- */

static uint32_t queue_samples(void) {
    return QUEUE_BYTES / 2;
}

/* Frame f (past the stream's head) as left/right; false if it isn't there yet. */
static bool frame_at(const struct stream *st, uint32_t f, int32_t *l, int32_t *r) {
    uint32_t n = f * st->channels;
    if (n + st->channels > st->count) {
        return false;
    }
    uint32_t i = (st->head + n) % queue_samples();
    *l = st->queue[i];
    *r = st->channels == 2 ? st->queue[(i + 1) % queue_samples()] : *l;
    return true;
}

/* One hardware frame of a stream, added to l/r; false when it has nothing more. */
static bool mix_frame(struct stream *st, int32_t *l, int32_t *r) {
    uint32_t f = (uint32_t)(st->pos >> 32);
    int32_t al, ar, bl, br;
    if (!frame_at(st, f, &al, &ar)) {
        return false;
    }
    uint32_t frac = (uint32_t)(st->pos >> 16) & 0xFFFF;
    if (frac && frame_at(st, f + 1, &bl, &br)) {
        al += (int32_t)(((int64_t)(bl - al) * frac) >> 16);
        ar += (int32_t)(((int64_t)(br - ar) * frac) >> 16);
    }
    *l += al;
    *r += ar;
    st->pos += st->step;
    /* Whole frames behind the position are done with. */
    uint32_t used = (uint32_t)(st->pos >> 32);
    if (used) {
        uint32_t n = used * st->channels;
        n = n > st->count ? st->count : n;
        st->head = (st->head + n) % queue_samples();
        st->count -= n;
        st->pos -= (uint64_t)used << 32;
    }
    return true;
}

/* Fill the ring up to AHEAD past the hardware with every stream added up. Call with the lock held. */
static void mix(void) {
    if (!running) {
        return;
    }
    int32_t scale = !volume_nid && (muted || volume < 100) ? (muted ? 0 : (int32_t)(volume * volume * 65536 / 10000))
                                                           : 65536;
    while (written < played + AHEAD) {
        uint32_t pos = (uint32_t)(written % OS_AUDIO_BUFFER);
        int16_t *dst = (int16_t *)((uint8_t *)phys_to_virt(ring_phys[pos / 4096]) + pos % 4096);
        uint32_t frames = (4096 - pos % 4096) / 4;
        uint64_t want = (played + AHEAD - written) / 4;
        frames = frames > want ? (uint32_t)want : frames;
        bool any = false;
        for (uint32_t i = 0; i < frames; i++) {
            int32_t l = 0, r = 0;
            for (int k = 0; k < MAX_STREAMS; k++) {
                if (streams[k].pid && !streams[k].paused) {
                    any |= mix_frame(&streams[k], &l, &r);
                }
            }
            if (scale != 65536) {
                l = (int32_t)(((int64_t)l * scale) >> 16);
                r = (int32_t)(((int64_t)r * scale) >> 16);
            }
            dst[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            dst[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        if (!any) {
            break;   /* nothing to play: leave the silence (written stays put, so new sound starts soon) */
        }
        written += (uint64_t)frames * 4;
    }
}

static void poll_thread(void *arg) {
    (void)arg;
    for (;;) {
        sched_sleep_ms(POLL_MS);
        mutex_lock(&lock);
        update_position();
        mix();
        mutex_unlock(&lock);
    }
}

/* --- setup ---------------------------------------------------------------------------------------------- */

bool audio_init(void) {
    pci_init();
    struct pci_device dev;
    if (!pci_find_class(0x04, 0x03, 0, &dev)) {   /* multimedia / HD Audio */
        return false;
    }
    uint64_t bar = pci_bar_address(&dev, 0);
    if (!bar) {
        return false;
    }
    pci_write16(&dev, PCI_COMMAND, (uint16_t)(pci_read16(&dev, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                              PCI_COMMAND_MASTER | PCI_COMMAND_INTX_OFF));
    regs = vmm_map_mmio(bar, 0x4000);
    if (!regs) {
        return false;
    }

    /* Link reset (reset_link): into reset, wait, out of reset, wait for the codecs to speak up. */
    w16(STATESTS, 0x7FFF);
    w32(GCTL, r32(GCTL) & ~GCTL_RESET);
    wait_for(GCTL, GCTL_RESET, 0, 100);
    sched_sleep_ms(1);
    w32(GCTL, r32(GCTL) | GCTL_RESET);
    if (!wait_for(GCTL, GCTL_RESET, GCTL_RESET, 100)) {
        return false;
    }
    sched_sleep_ms(2);
    uint16_t codecs = r16(STATESTS);
    w32(INTCTL, 0);   /* we poll */

    init_cmd_io();

    uint16_t gcap = r16(GCAP);
    int iss = (gcap >> 8) & 15, oss = (gcap >> 12) & 15;
    if (!oss) {
        return false;
    }
    out_stream = iss;   /* the first output stream */

    for (int c = 0; c < 15 && codec < 0; c++) {
        if (codecs & (1u << c)) {
            codec = c;
            codec_id = param(0, 0x00);   /* vendor << 16 | device */
            if (!setup_codec()) {
                codec = -1;
            }
        }
    }
    if (codec < 0) {
        return false;
    }
    for (int i = 0; i < RING_PAGES; i++) {
        ring_phys[i] = pmm_alloc_page();
        if (!ring_phys[i]) {
            return false;
        }
    }
    thread_create("audio", poll_thread, NULL);
    present = true;
    return true;
}

void audio_info(uint32_t *codec, int *count) {
    *codec = codec_id;
    *count = pin_count;
}

int64_t audio_mixer(struct process *p, uint64_t op, uint64_t value, uint64_t umixer) {
    if (!present) {
        return -ENODEV;
    }
    mutex_lock(&lock);
    int64_t err = 0;
    switch (op) {
    case OS_MIXER_GET:
        break;
    case OS_MIXER_SET_VOLUME:
        volume = value > 100 ? 100 : (uint32_t)value;
        apply_volume();
        break;
    case OS_MIXER_SET_MUTE:
        muted = value != 0;
        apply_volume();
        break;
    case OS_MIXER_SET_OUTPUT:
        if (value < (uint64_t)pin_count) {
            select_output((int)value);
        } else {
            err = -EINVAL;
        }
        break;
    default:
        err = -EINVAL;
        break;
    }
    struct os_audio_mixer m;
    memset(&m, 0, sizeof(m));
    m.volume = volume;
    m.muted = muted;
    m.output = current_output;
    m.output_count = pin_count;
    for (int i = 0; i < pin_count; i++) {
        strlcpy(m.outputs[i], outputs[i].name, sizeof(m.outputs[i]));
    }
    mutex_unlock(&lock);
    if (!err && umixer && !copy_to_user(p->space, umixer, &m, sizeof(m))) {
        err = -EFAULT;
    }
    return err;
}

/* --- what programs see --------------------------------------------------------------------------------------- */

static struct stream *stream_of(uint64_t pid) {
    for (int k = 0; k < MAX_STREAMS; k++) {
        if (streams[k].pid == pid) {
            return &streams[k];
        }
    }
    return NULL;
}

static bool anyone_playing(void) {
    for (int k = 0; k < MAX_STREAMS; k++) {
        if (streams[k].pid) {
            return true;
        }
    }
    return false;
}

static uint16_t format_for(uint32_t rate) {
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i].rate == rate && (dac_rates & (1u << rates[i].bit))) {
            return (uint16_t)(rates[i].fmt | 1 << 4 | 1);   /* 16 bits, stereo */
        }
    }
    return 0xFFFF;
}

int audio_open(uint64_t pid, uint32_t rate, uint32_t channels) {
    if (!present) {
        return -ENODEV;
    }
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) {
        return -EINVAL;
    }
    mutex_lock(&lock);
    int err = 0;
    struct stream *st = stream_of(pid);
    if (!st) {
        st = stream_of(0);
    }
    if (!st) {
        err = -EBUSY;   /* (MAX_STREAMS programs playing at once) */
    } else {
        if (!st->queue) {
            st->queue = kmalloc(QUEUE_BYTES);
        }
        if (!st->queue) {
            err = -ENOMEM;
        }
    }
    if (!err) {
        st->pid = 0;   /* (out of the mix while the hardware may change rate) */
        /* The hardware follows the first program: it plays at its rate, the others are resampled. */
        if (!anyone_playing() || !running) {
            uint32_t want = format_for(rate) != 0xFFFF ? rate : format_for(48000) != 0xFFFF ? 48000 : 44100;
            if (!running || want != hw_rate) {
                stream_stop();
                if (format_for(want) == 0xFFFF || !stream_start(format_for(want))) {
                    err = -EINVAL;
                } else {
                    hw_rate = want;
                }
            }
        }
    }
    if (!err) {
        st->pid = pid;
        st->rate = rate;
        st->channels = channels;
        st->paused = false;
        st->head = st->count = 0;
        st->pos = 0;
        st->step = ((uint64_t)rate << 32) / hw_rate;
    }
    mutex_unlock(&lock);
    return err;
}

static void close_locked(struct stream *st) {
    st->pid = 0;
    st->count = 0;
    if (!anyone_playing()) {
        stream_stop();
    }
}

int64_t audio_write(struct process *p, uint64_t buf, uint64_t len, uint64_t flags) {
    uint64_t done = 0;
    len &= ~1ull;   /* whole samples */
    while (done < len) {
        mutex_lock(&lock);
        struct stream *st = stream_of(p->pid);
        if (!st || !running) {
            mutex_unlock(&lock);
            return done ? (int64_t)done : -EBADF;
        }
        update_position();
        uint32_t frame = 2 * st->channels;
        uint64_t room = (uint64_t)(queue_samples() - st->count) * 2;
        room -= room % frame;
        uint64_t n = len - done < room ? len - done : room;
        n -= n % frame;
        while (n) {
            uint32_t tail = (st->head + st->count) % queue_samples();
            uint32_t chunk = (queue_samples() - tail) * 2;
            chunk = chunk > n ? (uint32_t)n : chunk;
            if (!copy_from_user(p->space, st->queue + tail, buf + done, chunk)) {
                mutex_unlock(&lock);
                return done ? (int64_t)done : -EFAULT;
            }
            st->count += chunk / 2;
            done += chunk;
            n -= chunk;
        }
        mix();
        mutex_unlock(&lock);
        if (done >= len - len % 4 || (flags & OS_AUDIO_NOWAIT)) {
            break;
        }
        if (process_current_killed()) {
            return done ? (int64_t)done : -EINTR;
        }
        sched_sleep_ms(POLL_MS);   /* full: wait for the mixer to make room */
    }
    return (int64_t)done;
}

/* What's queued but not heard yet, in the stream's own bytes: its queue, plus about what's mixed ahead. */
static int64_t queued_bytes(const struct stream *st) {
    uint64_t ahead = written > played ? (written - played) / 4 : 0;   /* hardware frames */
    uint64_t ours = st->count ? ahead * st->rate / hw_rate : 0;
    return (int64_t)((uint64_t)st->count * 2 + ours * 2 * st->channels);
}

int64_t audio_ctl(uint64_t pid, uint64_t op, uint64_t arg) {
    (void)arg;
    mutex_lock(&lock);
    struct stream *st = stream_of(pid);
    if (!st) {
        mutex_unlock(&lock);
        return present ? -EBADF : -ENODEV;
    }
    int64_t ret = 0;
    switch (op) {
    case OS_AUDIO_CLOSE:
        close_locked(st);
        break;
    case OS_AUDIO_PAUSE:
        st->paused = true;
        break;
    case OS_AUDIO_RESUME:
        st->paused = false;
        break;
    case OS_AUDIO_FLUSH:
        st->head = st->count = 0;
        st->pos = 0;
        break;
    case OS_AUDIO_DRAIN:
        /* Until its queue is empty and what was mixed of it has played. */
        while (running && !st->paused && st->pid == pid && st->count) {
            mutex_unlock(&lock);
            if (process_current_killed()) {
                return -EINTR;
            }
            sched_sleep_ms(POLL_MS);
            mutex_lock(&lock);
            update_position();
            mix();
        }
        {
            uint64_t until = written;
            while (running && st->pid == pid && played < until) {
                mutex_unlock(&lock);
                sched_sleep_ms(POLL_MS);
                mutex_lock(&lock);
                update_position();
                mix();
            }
        }
        break;
    case OS_AUDIO_QUEUED:
        update_position();
        ret = queued_bytes(st);
        break;
    default:
        ret = -EINVAL;
        break;
    }
    mutex_unlock(&lock);
    return ret;
}

void audio_process_exited(uint64_t pid) {
    if (!present) {
        return;
    }
    mutex_lock(&lock);
    struct stream *st = stream_of(pid);
    if (st) {
        close_locked(st);
    }
    mutex_unlock(&lock);
}
