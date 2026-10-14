/*
 * VP8 key frames: lossy WebP pictures (RFC 6386).
 *
 * The frame is cut into 16x16 macroblocks. For each, the first partition
 * says how it's predicted (from the pixels above and to the left, as a
 * whole or in 4x4 subblocks); the token partitions hold the DCT
 * coefficients of the residual, arithmetic ("boolean") coded. After
 * reconstruction a loop filter smooths the block edges, and the YUV 4:2:0
 * result is converted to RGB the way libwebp does it.
 *
 * Written from the specification and its reference decoder; the
 * probability tables come straight from the RFC (webp_tables.h).
 */

#include "internal.h"
#include "webp_tables.h"

enum { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED };
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED,
       B_HU_PRED };

static const int kf_ymode_tree[8] = { -B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED };
static const int bmode_tree[18] = {
    -B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10, -B_RD_PRED, -B_VR_PRED,
    -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED,
};
static const int uv_mode_tree[6] = { -DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED };
static const uint8_t kf_ymode_prob[4] = { 145, 156, 163, 128 };
static const uint8_t kf_uv_mode_prob[3] = { 142, 114, 183 };

static const uint8_t coeff_bands[17] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0 };
static const uint8_t zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t pcat1[] = { 159, 0 }, pcat2[] = { 165, 145, 0 }, pcat3[] = { 173, 148, 140, 0 },
                     pcat4[] = { 176, 155, 140, 135, 0 }, pcat5[] = { 180, 157, 141, 134, 130, 0 },
                     pcat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 };

/* --- the boolean decoder --------------------------------------------------------------- */

struct bool_dec {
    const uint8_t *in, *end;
    uint32_t range, value;
    int bit_count;
};

static void bool_init(struct bool_dec *d, const uint8_t *data, size_t size) {
    d->end = data + size;
    if (size >= 2) {
        d->value = (uint32_t)data[0] << 8 | data[1];
        d->in = data + 2;
    } else {
        d->value = 0;
        d->in = d->end;
    }
    d->range = 255;
    d->bit_count = 0;
}

static int bool_get(struct bool_dec *d, int prob) {
    uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8);
    uint32_t big = split << 8;
    int bit;
    if (d->value >= big) {
        bit = 1;
        d->range -= split;
        d->value -= big;
    } else {
        bit = 0;
        d->range = split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bit_count == 8) {
            d->bit_count = 0;
            if (d->in < d->end) {
                d->value |= *d->in++;
            }
        }
    }
    return bit;
}

static int bool_bit(struct bool_dec *d) {
    return bool_get(d, 128);
}

static int bool_uint(struct bool_dec *d, int bits) {
    int v = 0;
    while (bits--) {
        v = v << 1 | bool_bit(d);
    }
    return v;
}

static int bool_maybe_int(struct bool_dec *d, int bits) {
    if (!bool_bit(d)) {
        return 0;
    }
    int v = bool_uint(d, bits);
    return bool_bit(d) ? -v : v;
}

static int bool_tree(struct bool_dec *d, const int *tree, const uint8_t *probs) {
    int i = 0;
    while ((i = tree[i + bool_get(d, probs[i >> 1])]) > 0) {
    }
    return -i;
}

/* --- frame state ------------------------------------------------------------------------- */

#define BORDER 32

struct mb {
    uint8_t ymode, uvmode, segment, skip;
    uint8_t bmodes[16];
    bool has_coeffs;   /* any non-zero coefficient (for the loop filter) */
};

struct vp8 {
    int width, height, mb_cols, mb_rows;
    /* Headers. */
    bool seg_enabled, seg_update_map, seg_abs;
    int seg_quant[4], seg_lf[4];
    uint8_t seg_probs[3];
    bool filter_simple;
    int filter_level, sharpness;
    bool lf_delta;
    int ref_delta[4], mode_delta[4];
    int partitions;
    struct bool_dec part[8];
    uint8_t coeff_probs[4][8][3][11];
    bool skip_enabled;
    int skip_prob;
    int dq[4][3][2];   /* [segment][Y1, Y2, UV][DC, AC] */

    struct mb *mbs;
    /* Planes with a border all round (out-of-frame pixels for prediction). */
    uint8_t *y, *u, *v;
    int ystride, uvstride;
    /* Token contexts: is there anything in the block above / to the left? */
    uint8_t *above_ctx;   /* per macroblock column: 4 Y, 2 U, 2 V, 1 Y2 */
    uint8_t left_ctx[9];
    /* Subblock modes above / to the left, for the B_PRED probabilities. */
    uint8_t *above_bmodes;
    uint8_t left_bmodes[4];
};

static uint8_t *plane_at(uint8_t *plane, int stride, int x, int y) {
    return plane + (size_t)(y + BORDER) * stride + x + BORDER;
}

/* --- headers ---------------------------------------------------------------------------- */

static int clamp_q(int q) {
    return q < 0 ? 0 : q > 127 ? 127 : q;
}

static void setup_dequant(struct vp8 *f, int q_index, const int delta[5]) {
    for (int s = 0; s < (f->seg_enabled ? 4 : 1); s++) {
        int q = q_index;
        if (f->seg_enabled) {
            q = f->seg_abs ? f->seg_quant[s] : q + f->seg_quant[s];
        }
        f->dq[s][0][0] = dc_qlookup[clamp_q(q + delta[0])];
        f->dq[s][0][1] = ac_qlookup[clamp_q(q)];
        f->dq[s][1][0] = dc_qlookup[clamp_q(q + delta[1])] * 2;
        f->dq[s][1][1] = ac_qlookup[clamp_q(q + delta[2])] * 155 / 100;
        if (f->dq[s][1][1] < 8) {
            f->dq[s][1][1] = 8;
        }
        f->dq[s][2][0] = dc_qlookup[clamp_q(q + delta[3])];
        if (f->dq[s][2][0] > 132) {
            f->dq[s][2][0] = 132;
        }
        f->dq[s][2][1] = ac_qlookup[clamp_q(q + delta[4])];
    }
}

/* --- modes --------------------------------------------------------------------------------- */

static uint8_t implied_bmode(int ymode) {
    switch (ymode) {
    case V_PRED:  return B_VE_PRED;
    case H_PRED:  return B_HE_PRED;
    case TM_PRED: return B_TM_PRED;
    default:      return B_DC_PRED;
    }
}

static void read_modes(struct vp8 *f, struct bool_dec *d, struct mb *m, int col) {
    if (f->seg_update_map) {
        m->segment = (uint8_t)(bool_get(d, f->seg_probs[0]) ? 2 + bool_get(d, f->seg_probs[2])
                                                            : bool_get(d, f->seg_probs[1]));
    }
    m->skip = f->skip_enabled ? (uint8_t)bool_get(d, f->skip_prob) : 0;
    m->ymode = (uint8_t)bool_tree(d, kf_ymode_tree, kf_ymode_prob);
    uint8_t *above = f->above_bmodes + col * 4;
    if (m->ymode == B_PRED) {
        for (int i = 0; i < 16; i++) {
            int a = above[i & 3], l = f->left_bmodes[i >> 2];
            int b = bool_tree(d, bmode_tree, &kf_bmode_prob[(a * 10 + l) * 9]);
            m->bmodes[i] = (uint8_t)b;
            above[i & 3] = (uint8_t)b;
            f->left_bmodes[i >> 2] = (uint8_t)b;
        }
    } else {
        uint8_t b = implied_bmode(m->ymode);
        memset(above, b, 4);
        memset(f->left_bmodes, b, 4);
    }
    m->uvmode = (uint8_t)bool_tree(d, uv_mode_tree, kf_uv_mode_prob);
}

/* --- tokens ---------------------------------------------------------------------------------- */

static int extra(struct bool_dec *d, const uint8_t *p) {
    int v = 0;
    while (*p) {
        v = v + v + bool_get(d, *p++);
    }
    return v;
}

/*
 * One 4x4 block's coefficients (dequantized, into out[] in natural order).
 * Returns 1 if it has any, 0 if not.
 */
static int read_block(struct bool_dec *d, uint8_t probs[8][3][11], int ctx, int first, const int dq[2],
                      int16_t *out) {
    int c = first;
    const uint8_t *p = probs[coeff_bands[c]][ctx];
    if (!bool_get(d, p[0])) {
        return 0;   /* nothing at all */
    }
    for (;;) {
        /* Not the end: zeros first (no end check after a zero). */
        while (!bool_get(d, p[1])) {
            if (++c == 16) {
                return 1;
            }
            p = probs[coeff_bands[c]][0];
        }
        int v, next_ctx;
        if (!bool_get(d, p[2])) {
            v = 1;
            next_ctx = 1;
        } else {
            if (!bool_get(d, p[3])) {
                v = !bool_get(d, p[4]) ? 2 : 3 + bool_get(d, p[5]);
            } else if (!bool_get(d, p[6])) {
                v = !bool_get(d, p[7]) ? 5 + extra(d, pcat1) : 7 + extra(d, pcat2);
            } else if (!bool_get(d, p[8])) {
                v = !bool_get(d, p[9]) ? 11 + extra(d, pcat3) : 19 + extra(d, pcat4);
            } else {
                v = !bool_get(d, p[10]) ? 35 + extra(d, pcat5) : 67 + extra(d, pcat6);
            }
            next_ctx = 2;
        }
        if (bool_bit(d)) {
            v = -v;
        }
        out[zigzag[c]] = (int16_t)(v * dq[c > 0]);
        if (++c == 16) {
            return 1;
        }
        p = probs[coeff_bands[c]][next_ctx];
        if (!bool_get(d, p[0])) {
            return 1;   /* end of block */
        }
    }
}

/* All of a macroblock's coefficients: 16 Y, 4 U, 4 V and (unless B_PRED) the Y2 block. */
static void read_tokens(struct vp8 *f, struct bool_dec *d, struct mb *m, int col, int16_t coeffs[25][16]) {
    memset(coeffs, 0, 25 * 16 * sizeof(int16_t));
    uint8_t *above = f->above_ctx + col * 9, *left = f->left_ctx;
    if (m->skip) {
        /* Nothing coded: the contexts become "empty" (Y2's only if this block would have had one). */
        memset(above, 0, 8);
        memset(left, 0, 8);
        if (m->ymode != B_PRED) {
            above[8] = left[8] = 0;
        }
        m->has_coeffs = false;
        return;
    }
    const int (*dq)[2] = f->dq[f->seg_enabled ? m->segment : 0];
    int any = 0, first = 0, ytype = 3;
    if (m->ymode != B_PRED) {
        int nz = read_block(d, f->coeff_probs[1], above[8] + left[8], 0, dq[1], coeffs[24]);
        above[8] = left[8] = (uint8_t)nz;
        any |= nz;
        first = 1;
        ytype = 0;
    }
    for (int by = 0; by < 4; by++) {
        for (int bx = 0; bx < 4; bx++) {
            int nz = read_block(d, f->coeff_probs[ytype], above[bx] + left[by], first, dq[0], coeffs[by * 4 + bx]);
            above[bx] = left[by] = (uint8_t)nz;
            any |= nz;
        }
    }
    for (int plane = 0; plane < 2; plane++) {
        for (int by = 0; by < 2; by++) {
            for (int bx = 0; bx < 2; bx++) {
                int a = 4 + plane * 2 + bx, l = 4 + plane * 2 + by;
                int nz = read_block(d, f->coeff_probs[2], above[a] + left[l], 0, dq[2],
                                    coeffs[16 + plane * 4 + by * 2 + bx]);
                above[a] = left[l] = (uint8_t)nz;
                any |= nz;
            }
        }
    }
    m->has_coeffs = any;
}

/* --- transforms ------------------------------------------------------------------------------ */

static void walsh(const int16_t *in, int16_t *out) {
    int16_t t[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i];
        int c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        t[i] = (int16_t)(a1 + b1);
        t[4 + i] = (int16_t)(c1 + d1);
        t[8 + i] = (int16_t)(a1 - b1);
        t[12 + i] = (int16_t)(d1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        const int16_t *r = t + 4 * i;
        int a1 = r[0] + r[3], b1 = r[1] + r[2], c1 = r[1] - r[2], d1 = r[0] - r[3];
        out[4 * i + 0] = (int16_t)((a1 + b1 + 3) >> 3);
        out[4 * i + 1] = (int16_t)((c1 + d1 + 3) >> 3);
        out[4 * i + 2] = (int16_t)((a1 - b1 + 3) >> 3);
        out[4 * i + 3] = (int16_t)((d1 - c1 + 3) >> 3);
    }
}

#define C1 20091   /* cos(pi/8) * sqrt(2) - 1, in 16.16 */
#define S1 35468   /* sin(pi/8) * sqrt(2) */

/* Inverse DCT of a 4x4 block, added to the prediction already in place. */
static void idct_add(const int16_t *in, uint8_t *dst, int stride) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
        int c1 = ((in[4 + i] * S1) >> 16) - (in[12 + i] + ((in[12 + i] * C1) >> 16));
        int d1 = (in[4 + i] + ((in[4 + i] * C1) >> 16)) + ((in[12 + i] * S1) >> 16);
        t[i] = a1 + d1;
        t[12 + i] = a1 - d1;
        t[4 + i] = b1 + c1;
        t[8 + i] = b1 - c1;
    }
    for (int i = 0; i < 4; i++) {
        const int *r = t + 4 * i;
        int a1 = r[0] + r[2], b1 = r[0] - r[2];
        int c1 = ((r[1] * S1) >> 16) - (r[3] + ((r[3] * C1) >> 16));
        int d1 = (r[1] + ((r[1] * C1) >> 16)) + ((r[3] * S1) >> 16);
        uint8_t *o = dst + i * stride;
        o[0] = image_clamp255(o[0] + ((a1 + d1 + 4) >> 3));
        o[3] = image_clamp255(o[3] + ((a1 - d1 + 4) >> 3));
        o[1] = image_clamp255(o[1] + ((b1 + c1 + 4) >> 3));
        o[2] = image_clamp255(o[2] + ((b1 - c1 + 4) >> 3));
    }
}

/* --- prediction ------------------------------------------------------------------------------- */

static void predict_dc(uint8_t *p, int stride, int n, int shift) {
    int dc = 0;
    for (int i = 0; i < n; i++) {
        dc += p[i * stride - 1] + p[i - stride];
    }
    dc = (dc + (1 << (shift - 1))) >> shift;
    for (int i = 0; i < n; i++) {
        memset(p + i * stride, dc, (size_t)n);
    }
}

static void predict_v(uint8_t *p, int stride, int n) {
    for (int i = 0; i < n; i++) {
        memcpy(p + i * stride, p - stride, (size_t)n);
    }
}

static void predict_h(uint8_t *p, int stride, int n) {
    for (int i = 0; i < n; i++) {
        memset(p + i * stride, p[i * stride - 1], (size_t)n);
    }
}

static void predict_tm(uint8_t *p, int stride, int n) {
    const uint8_t *above = p - stride;
    int corner = above[-1];
    for (int i = 0; i < n; i++) {
        int left = p[i * stride - 1];
        for (int j = 0; j < n; j++) {
            p[i * stride + j] = image_clamp255(left + above[j] - corner);
        }
    }
}

#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) ((uint8_t)(((a) + (b) + 1) >> 1))

static void predict_subblock(uint8_t *p, int s, int mode) {
    const uint8_t *A = p - s;   /* above: A[-1] is the corner, A[0..7] above and above-right */
    int L[4] = { p[-1], p[s - 1], p[2 * s - 1], p[3 * s - 1] };
    int P = A[-1];
    #define PUT(x, y, v) (p[(y) * s + (x)] = (uint8_t)(v))
    switch (mode) {
    case B_DC_PRED: {
        int dc = 4;
        for (int i = 0; i < 4; i++) {
            dc += A[i] + L[i];
        }
        for (int y = 0; y < 4; y++) {
            memset(p + y * s, dc >> 3, 4);
        }
        break;
    }
    case B_TM_PRED:
        predict_tm(p, s, 4);
        break;
    case B_VE_PRED:
        for (int x = 0; x < 4; x++) {
            uint8_t v = AVG3(A[x - 1], A[x], A[x + 1]);
            for (int y = 0; y < 4; y++) {
                PUT(x, y, v);
            }
        }
        break;
    case B_HE_PRED: {
        uint8_t r[4] = { AVG3(P, L[0], L[1]), AVG3(L[0], L[1], L[2]), AVG3(L[1], L[2], L[3]),
                         AVG3(L[2], L[3], L[3]) };
        for (int y = 0; y < 4; y++) {
            memset(p + y * s, r[y], 4);
        }
        break;
    }
    case B_LD_PRED:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int i = x + y;
                PUT(x, y, i < 6 ? AVG3(A[i], A[i + 1], A[i + 2]) : AVG3(A[6], A[7], A[7]));
            }
        }
        break;
    case B_RD_PRED: {
        /* The edge from bottom-left, round the corner, to top-right: E[0..8]. */
        int E[9] = { L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3] };
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int i = 4 - y + x;   /* centre index into E */
                PUT(x, y, AVG3(E[i - 1], E[i], E[i + 1]));
            }
        }
        break;
    }
    case B_VR_PRED: {
        int E[9] = { L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3] };
        PUT(0, 3, AVG3(E[1], E[2], E[3]));
        PUT(0, 2, AVG3(E[2], E[3], E[4]));
        PUT(1, 3, AVG3(E[3], E[4], E[5])); PUT(0, 1, AVG3(E[3], E[4], E[5]));
        PUT(1, 2, AVG2(E[4], E[5])); PUT(0, 0, AVG2(E[4], E[5]));
        PUT(2, 3, AVG3(E[4], E[5], E[6])); PUT(1, 1, AVG3(E[4], E[5], E[6]));
        PUT(2, 2, AVG2(E[5], E[6])); PUT(1, 0, AVG2(E[5], E[6]));
        PUT(3, 3, AVG3(E[5], E[6], E[7])); PUT(2, 1, AVG3(E[5], E[6], E[7]));
        PUT(3, 2, AVG2(E[6], E[7])); PUT(2, 0, AVG2(E[6], E[7]));
        PUT(3, 1, AVG3(E[6], E[7], E[8]));
        PUT(3, 0, AVG2(E[7], E[8]));
        break;
    }
    case B_VL_PRED:
        PUT(0, 0, AVG2(A[0], A[1]));
        PUT(0, 1, AVG3(A[0], A[1], A[2]));
        PUT(0, 2, AVG2(A[1], A[2])); PUT(1, 0, AVG2(A[1], A[2]));
        PUT(1, 1, AVG3(A[1], A[2], A[3])); PUT(0, 3, AVG3(A[1], A[2], A[3]));
        PUT(1, 2, AVG2(A[2], A[3])); PUT(2, 0, AVG2(A[2], A[3]));
        PUT(1, 3, AVG3(A[2], A[3], A[4])); PUT(2, 1, AVG3(A[2], A[3], A[4]));
        PUT(2, 2, AVG2(A[3], A[4])); PUT(3, 0, AVG2(A[3], A[4]));
        PUT(2, 3, AVG3(A[3], A[4], A[5])); PUT(3, 1, AVG3(A[3], A[4], A[5]));
        PUT(3, 2, AVG3(A[4], A[5], A[6]));
        PUT(3, 3, AVG3(A[5], A[6], A[7]));
        break;
    case B_HD_PRED: {
        int E[9] = { L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3] };
        PUT(0, 3, AVG2(E[0], E[1]));
        PUT(1, 3, AVG3(E[0], E[1], E[2]));
        PUT(0, 2, AVG2(E[1], E[2])); PUT(2, 3, AVG2(E[1], E[2]));
        PUT(1, 2, AVG3(E[1], E[2], E[3])); PUT(3, 3, AVG3(E[1], E[2], E[3]));
        PUT(2, 2, AVG2(E[2], E[3])); PUT(0, 1, AVG2(E[2], E[3]));
        PUT(3, 2, AVG3(E[2], E[3], E[4])); PUT(1, 1, AVG3(E[2], E[3], E[4]));
        PUT(2, 1, AVG2(E[3], E[4])); PUT(0, 0, AVG2(E[3], E[4]));
        PUT(3, 1, AVG3(E[3], E[4], E[5])); PUT(1, 0, AVG3(E[3], E[4], E[5]));
        PUT(2, 0, AVG3(E[4], E[5], E[6]));
        PUT(3, 0, AVG3(E[5], E[6], E[7]));
        break;
    }
    default:   /* B_HU_PRED */
        PUT(0, 0, AVG2(L[0], L[1]));
        PUT(1, 0, AVG3(L[0], L[1], L[2]));
        PUT(2, 0, AVG2(L[1], L[2])); PUT(0, 1, AVG2(L[1], L[2]));
        PUT(3, 0, AVG3(L[1], L[2], L[3])); PUT(1, 1, AVG3(L[1], L[2], L[3]));
        PUT(2, 1, AVG2(L[2], L[3])); PUT(0, 2, AVG2(L[2], L[3]));
        PUT(3, 1, AVG3(L[2], L[3], L[3])); PUT(1, 2, AVG3(L[2], L[3], L[3]));
        PUT(2, 2, L[3]); PUT(3, 2, L[3]);
        for (int x = 0; x < 4; x++) {
            PUT(x, 3, L[3]);
        }
        break;
    }
    #undef PUT
}

/* The frame's out-of-picture edges, set up the way the reference decoder does (see its predict.c). */
static void fixup_left(uint8_t *p, int n, int stride, int row, int mode) {
    if (mode == DC_PRED && row) {
        for (int i = 0; i < n; i++) {
            p[i * stride - 1] = p[i - stride];   /* DC with no left: average the top twice */
        }
    } else {
        for (int i = -1; i < n; i++) {
            p[i * stride - 1] = 129;
        }
    }
}

static void fixup_above(uint8_t *p, int n, int stride, int col, int mode) {
    uint8_t *above = p - stride;
    if (mode == DC_PRED && col) {
        for (int i = 0; i < n; i++) {
            above[i] = p[i * stride - 1];
        }
    } else {
        memset(above - 1, 127, (size_t)n + 1);
    }
    memset(above + n, 127, 4);   /* for the above-right of subblock modes */
}

static void predict_mb(struct vp8 *f, struct mb *m, int row, int col, int16_t coeffs[25][16]) {
    uint8_t *y = plane_at(f->y, f->ystride, col * 16, row * 16);
    uint8_t *u = plane_at(f->u, f->uvstride, col * 8, row * 8);
    uint8_t *v = plane_at(f->v, f->uvstride, col * 8, row * 8);
    int ys = f->ystride, uvs = f->uvstride;
    if (col == 0) {
        fixup_left(y, 16, ys, row, m->ymode);
        fixup_left(u, 8, uvs, row, m->uvmode);
        fixup_left(v, 8, uvs, row, m->uvmode);
        if (row == 0) {
            y[-ys - 1] = 127;
        }
    }
    if (row == 0) {
        fixup_above(y, 16, ys, col, m->ymode);
        fixup_above(u, 8, uvs, col, m->uvmode);
        fixup_above(v, 8, uvs, col, m->uvmode);
    }

    if (m->ymode == B_PRED) {
        /* Subblocks 7, 11 and 15 use the macroblock's above-right pixels (copied down). */
        uint32_t above_right;
        memcpy(&above_right, y - ys + 16, 4);
        for (int r = 1; r < 4; r++) {
            memcpy(y + (4 * r - 1) * ys + 16, &above_right, 4);
        }
        for (int i = 0; i < 16; i++) {
            uint8_t *b = y + (i >> 2) * 4 * ys + (i & 3) * 4;
            predict_subblock(b, ys, m->bmodes[i]);
            idct_add(coeffs[i], b, ys);
        }
    } else {
        switch (m->ymode) {
        case DC_PRED: predict_dc(y, ys, 16, 5); break;
        case V_PRED:  predict_v(y, ys, 16); break;
        case H_PRED:  predict_h(y, ys, 16); break;
        default:      predict_tm(y, ys, 16); break;
        }
        int16_t dc[16];
        walsh(coeffs[24], dc);
        for (int i = 0; i < 16; i++) {
            coeffs[i][0] = dc[i];
            idct_add(coeffs[i], y + (i >> 2) * 4 * ys + (i & 3) * 4, ys);
        }
    }
    uint8_t *planes[2] = { u, v };
    for (int pl = 0; pl < 2; pl++) {
        uint8_t *c = planes[pl];
        switch (m->uvmode) {
        case DC_PRED: predict_dc(c, uvs, 8, 4); break;
        case V_PRED:  predict_v(c, uvs, 8); break;
        case H_PRED:  predict_h(c, uvs, 8); break;
        default:      predict_tm(c, uvs, 8); break;
        }
        for (int i = 0; i < 4; i++) {
            idct_add(coeffs[16 + pl * 4 + i], c + (i >> 1) * 4 * uvs + (i & 1) * 4, uvs);
        }
    }
}

/* --- the loop filter ------------------------------------------------------------------------------ */

static int s8(int x) {
    return x < -128 ? -128 : x > 127 ? 127 : x;
}

static int u2s(int x) {
    return x - 128;
}

#define ABS(x) ((x) < 0 ? -(x) : (x))

/* Pixels p3 p2 p1 p0 | q0 q1 q2 q3 across an edge, `step` apart. */
struct edge {
    uint8_t *q;
    int step;
};

#define PX(e, i) ((e).q[(i) * (e).step])

static bool simple_ok(struct edge e, int limit) {
    return ABS(PX(e, -1) - PX(e, 0)) * 2 + (ABS(PX(e, -2) - PX(e, 1)) >> 1) <= limit;
}

static bool normal_ok(struct edge e, int E, int I) {
    return simple_ok(e, 2 * E + I) && ABS(PX(e, -4) - PX(e, -3)) <= I && ABS(PX(e, -3) - PX(e, -2)) <= I &&
           ABS(PX(e, -2) - PX(e, -1)) <= I && ABS(PX(e, 3) - PX(e, 2)) <= I && ABS(PX(e, 2) - PX(e, 1)) <= I &&
           ABS(PX(e, 1) - PX(e, 0)) <= I;
}

static bool high_variance(struct edge e, int t) {
    return ABS(PX(e, -2) - PX(e, -1)) > t || ABS(PX(e, 1) - PX(e, 0)) > t;
}

/* The pixels are worked on as signed values (x - 128), as the specification does. */
static void common_adjust(struct edge e, bool outer_taps) {
    int p1 = u2s(PX(e, -2)), p0 = u2s(PX(e, -1)), q0 = u2s(PX(e, 0)), q1 = u2s(PX(e, 1));
    int a = s8((outer_taps ? s8(p1 - q1) : 0) + 3 * (q0 - p0));
    int f1 = (a + 4 > 127 ? 127 : a + 4) >> 3, f2 = (a + 3 > 127 ? 127 : a + 3) >> 3;
    PX(e, 0) = (uint8_t)(s8(q0 - f1) + 128);
    PX(e, -1) = (uint8_t)(s8(p0 + f2) + 128);
    if (!outer_taps) {
        a = (f1 + 1) >> 1;
        PX(e, 1) = (uint8_t)(s8(q1 - a) + 128);
        PX(e, -2) = (uint8_t)(s8(p1 + a) + 128);
    }
}

static void mb_adjust(struct edge e) {
    int p2 = u2s(PX(e, -3)), p1 = u2s(PX(e, -2)), p0 = u2s(PX(e, -1));
    int q0 = u2s(PX(e, 0)), q1 = u2s(PX(e, 1)), q2 = u2s(PX(e, 2));
    int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
    int a = s8((27 * w + 63) >> 7);
    PX(e, 0) = (uint8_t)(s8(q0 - a) + 128);
    PX(e, -1) = (uint8_t)(s8(p0 + a) + 128);
    a = s8((18 * w + 63) >> 7);
    PX(e, 1) = (uint8_t)(s8(q1 - a) + 128);
    PX(e, -2) = (uint8_t)(s8(p1 + a) + 128);
    a = s8((9 * w + 63) >> 7);
    PX(e, 2) = (uint8_t)(s8(q2 - a) + 128);
    PX(e, -3) = (uint8_t)(s8(p2 + a) + 128);
}

/*
 * One edge, `count` pixels long. `along` steps to the next pixel of the
 * edge, `across` crosses it. mb: a macroblock edge (stronger filter).
 */
static void filter_edge(uint8_t *at, int along, int across, int count, int E, int I, int hev, bool mb) {
    for (int i = 0; i < count; i++) {
        struct edge e = { at + i * along, across };
        if (!normal_ok(e, E, I)) {
            continue;
        }
        if (mb) {
            if (high_variance(e, hev)) {
                common_adjust(e, true);
            } else {
                mb_adjust(e);
            }
        } else {
            common_adjust(e, high_variance(e, hev));
        }
    }
}

static void filter_edge_simple(uint8_t *at, int along, int across, int limit) {
    for (int i = 0; i < 16; i++) {
        struct edge e = { at + i * along, across };
        if (simple_ok(e, limit)) {
            common_adjust(e, true);
        }
    }
}

static void loop_filter(struct vp8 *f) {
    for (int row = 0; row < f->mb_rows; row++) {
        for (int col = 0; col < f->mb_cols; col++) {
            struct mb *m = &f->mbs[row * f->mb_cols + col];
            int level = f->filter_level;
            if (f->seg_enabled) {
                level = f->seg_abs ? f->seg_lf[m->segment] : level + f->seg_lf[m->segment];
            }
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            if (f->lf_delta) {
                level += f->ref_delta[0];   /* intra frame */
                if (m->ymode == B_PRED) {
                    level += f->mode_delta[0];
                }
                level = level < 0 ? 0 : level > 63 ? 63 : level;
            }
            if (!level) {
                continue;
            }
            int interior = level;
            if (f->sharpness) {
                interior >>= f->sharpness > 4 ? 2 : 1;
                if (interior > 9 - f->sharpness) {
                    interior = 9 - f->sharpness;
                }
            }
            if (interior < 1) {
                interior = 1;
            }
            int hev = (level >= 15) + (level >= 40);
            bool inner = m->has_coeffs || m->ymode == B_PRED;

            uint8_t *y = plane_at(f->y, f->ystride, col * 16, row * 16);
            int ys = f->ystride;
            if (f->filter_simple) {
                int mb_limit = (level + 2) * 2 + interior, b_limit = level * 2 + interior;
                if (col) {
                    filter_edge_simple(y, ys, 1, mb_limit);
                }
                if (inner) {
                    for (int x = 4; x < 16; x += 4) {
                        filter_edge_simple(y + x, ys, 1, b_limit);
                    }
                }
                if (row) {
                    filter_edge_simple(y, 1, ys, mb_limit);
                }
                if (inner) {
                    for (int r = 4; r < 16; r += 4) {
                        filter_edge_simple(y + r * ys, 1, ys, b_limit);
                    }
                }
                continue;
            }
            uint8_t *u = plane_at(f->u, f->uvstride, col * 8, row * 8);
            uint8_t *v = plane_at(f->v, f->uvstride, col * 8, row * 8);
            int uvs = f->uvstride;
            if (col) {
                filter_edge(y, ys, 1, 16, level + 2, interior, hev, true);
                filter_edge(u, uvs, 1, 8, level + 2, interior, hev, true);
                filter_edge(v, uvs, 1, 8, level + 2, interior, hev, true);
            }
            if (inner) {
                for (int x = 4; x < 16; x += 4) {
                    filter_edge(y + x, ys, 1, 16, level, interior, hev, false);
                }
                filter_edge(u + 4, uvs, 1, 8, level, interior, hev, false);
                filter_edge(v + 4, uvs, 1, 8, level, interior, hev, false);
            }
            if (row) {
                filter_edge(y, 1, ys, 16, level + 2, interior, hev, true);
                filter_edge(u, 1, uvs, 8, level + 2, interior, hev, true);
                filter_edge(v, 1, uvs, 8, level + 2, interior, hev, true);
            }
            if (inner) {
                for (int r = 4; r < 16; r += 4) {
                    filter_edge(y + r * ys, 1, ys, 16, level, interior, hev, false);
                }
                filter_edge(u + 4 * uvs, 1, uvs, 8, level, interior, hev, false);
                filter_edge(v + 4 * uvs, 1, uvs, 8, level, interior, hev, false);
            }
        }
    }
}

/* --- YUV to RGB (libwebp's conversion and "fancy" chroma upsampling) ------------------------------------ */

static int mult_hi(int v, int c) {
    return (v * c) >> 8;
}

static uint8_t clip8(int v) {
    return (v & ~16383) == 0 ? (uint8_t)(v >> 6) : v < 0 ? 0 : 255;
}

/* Chroma at a luma position: 9-3-3-1 weighting of the four nearest chroma samples. */
static int upsample(const uint8_t *plane, int stride, int cw, int ch, int x, int y) {
    int cx = x >> 1, cy = y >> 1;
    int nx = x & 1 ? cx + 1 : cx - 1, ny = y & 1 ? cy + 1 : cy - 1;
    nx = nx < 0 ? 0 : nx >= cw ? cw - 1 : nx;
    ny = ny < 0 ? 0 : ny >= ch ? ch - 1 : ny;
    const uint8_t *r0 = plane + (size_t)(cy + BORDER) * stride + BORDER, *r1 = plane + (size_t)(ny + BORDER) * stride + BORDER;
    return (9 * r0[cx] + 3 * r0[nx] + 3 * r1[cx] + r1[nx] + 8) >> 4;
}

static void to_rgb(struct vp8 *f, struct gfx_surface *s) {
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    for (int y = 0; y < f->height; y++) {
        uint32_t *out = gfx_row(s, y);
        const uint8_t *yr = plane_at(f->y, f->ystride, 0, y);
        for (int x = 0; x < f->width; x++) {
            int Y = mult_hi(yr[x], 19077);
            int U = upsample(f->u, f->uvstride, cw, ch, x, y), V = upsample(f->v, f->uvstride, cw, ch, x, y);
            uint8_t r = clip8(Y + mult_hi(V, 26149) - 14234);
            uint8_t g = clip8(Y - mult_hi(U, 6419) - mult_hi(V, 13320) + 8708);
            uint8_t b = clip8(Y + mult_hi(U, 33050) - 17685);
            out[x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
    }
}

/* --- the frame -------------------------------------------------------------------------------------------- */

static void cleanup(struct vp8 *f) {
    free(f->mbs);
    free(f->y);
    free(f->u);
    free(f->v);
    free(f->above_ctx);
    free(f->above_bmodes);
    free(f);
}

struct gfx_surface *vp8_decode(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *vp8_decode(const uint8_t *data, size_t size, const char **error) {
    if (size < 10) {
        return image_fail(error, "truncated VP8 data");
    }
    uint32_t tag = data[0] | data[1] << 8 | data[2] << 16;
    bool key = !(tag & 1);
    uint32_t part0 = (tag >> 5) & 0x7FFFF;
    if (!key || data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A) {
        return image_fail(error, "not a VP8 key frame");
    }
    int w = image_le16(data + 6) & 0x3FFF, h = image_le16(data + 8) & 0x3FFF;
    if (!image_size_ok((uint32_t)w, (uint32_t)h) || 10 + (size_t)part0 > size) {
        return image_fail(error, "bad VP8 frame");
    }
    struct vp8 *f = calloc(1, sizeof(*f));
    if (!f) {
        return image_fail(error, "not enough memory");
    }
    f->width = w;
    f->height = h;
    f->mb_cols = (w + 15) / 16;
    f->mb_rows = (h + 15) / 16;
    f->ystride = f->mb_cols * 16 + 2 * BORDER;
    f->uvstride = f->mb_cols * 8 + 2 * BORDER;
    f->mbs = calloc((size_t)f->mb_cols * f->mb_rows, sizeof(struct mb));
    f->y = calloc((size_t)f->ystride, (size_t)f->mb_rows * 16 + 2 * BORDER);
    f->u = calloc((size_t)f->uvstride, (size_t)f->mb_rows * 8 + 2 * BORDER);
    f->v = calloc((size_t)f->uvstride, (size_t)f->mb_rows * 8 + 2 * BORDER);
    f->above_ctx = calloc((size_t)f->mb_cols, 9);
    f->above_bmodes = calloc((size_t)f->mb_cols, 4);
    if (!f->mbs || !f->y || !f->u || !f->v || !f->above_ctx || !f->above_bmodes) {
        cleanup(f);
        return image_fail(error, "not enough memory");
    }

    struct bool_dec hdr;
    bool_init(&hdr, data + 10, part0);
    bool_bit(&hdr);   /* colour space */
    bool_bit(&hdr);   /* clamping type */
    f->seg_enabled = bool_bit(&hdr);
    if (f->seg_enabled) {
        f->seg_update_map = bool_bit(&hdr);
        bool update_data = bool_bit(&hdr);
        if (update_data) {
            f->seg_abs = bool_bit(&hdr);
            for (int i = 0; i < 4; i++) {
                f->seg_quant[i] = bool_maybe_int(&hdr, 7);
            }
            for (int i = 0; i < 4; i++) {
                f->seg_lf[i] = bool_maybe_int(&hdr, 6);
            }
        }
        if (f->seg_update_map) {
            for (int i = 0; i < 3; i++) {
                f->seg_probs[i] = (uint8_t)(bool_bit(&hdr) ? bool_uint(&hdr, 8) : 255);
            }
        }
    }
    f->filter_simple = bool_bit(&hdr);
    f->filter_level = bool_uint(&hdr, 6);
    f->sharpness = bool_uint(&hdr, 3);
    f->lf_delta = bool_bit(&hdr);
    if (f->lf_delta && bool_bit(&hdr)) {
        for (int i = 0; i < 4; i++) {
            f->ref_delta[i] = bool_maybe_int(&hdr, 6);
        }
        for (int i = 0; i < 4; i++) {
            f->mode_delta[i] = bool_maybe_int(&hdr, 6);
        }
    }
    /* Token partitions: their sizes follow the first partition. */
    f->partitions = 1 << bool_uint(&hdr, 2);
    const uint8_t *p = data + 10 + part0, *end = data + size;
    if (p + 3 * (f->partitions - 1) > end) {
        cleanup(f);
        return image_fail(error, "truncated VP8 data");
    }
    const uint8_t *pdata = p + 3 * (f->partitions - 1);
    for (int i = 0; i < f->partitions; i++) {
        size_t len = i < f->partitions - 1 ? (size_t)(p[3 * i] | p[3 * i + 1] << 8 | p[3 * i + 2] << 16)
                                           : (size_t)(end - pdata);
        if (pdata + len > end) {
            len = (size_t)(end - pdata);
        }
        bool_init(&f->part[i], pdata, len);
        pdata += len;
    }
    int q_index = bool_uint(&hdr, 7), deltas[5];
    for (int i = 0; i < 5; i++) {
        deltas[i] = bool_maybe_int(&hdr, 4);   /* y1 dc, y2 dc, y2 ac, uv dc, uv ac */
    }
    setup_dequant(f, q_index, deltas);
    bool_bit(&hdr);   /* refresh entropy probabilities: there's only this frame */
    memcpy(f->coeff_probs, default_coeff_probs, sizeof(f->coeff_probs));
    for (int i = 0; i < 4 * 8 * 3 * 11; i++) {
        if (bool_get(&hdr, coeff_update_probs[i])) {
            ((uint8_t *)f->coeff_probs)[i] = (uint8_t)bool_uint(&hdr, 8);
        }
    }
    f->skip_enabled = bool_bit(&hdr);
    if (f->skip_enabled) {
        f->skip_prob = bool_uint(&hdr, 8);
    }

    /* Out-of-frame subblock modes count as B_DC_PRED. */
    memset(f->above_bmodes, B_DC_PRED, (size_t)f->mb_cols * 4);
    int16_t coeffs[25][16];
    for (int row = 0; row < f->mb_rows; row++) {
        struct bool_dec *tokens = &f->part[row % f->partitions];
        memset(f->left_ctx, 0, sizeof(f->left_ctx));
        memset(f->left_bmodes, B_DC_PRED, 4);
        for (int col = 0; col < f->mb_cols; col++) {
            struct mb *m = &f->mbs[row * f->mb_cols + col];
            read_modes(f, &hdr, m, col);
            read_tokens(f, tokens, m, col, coeffs);
            predict_mb(f, m, row, col, coeffs);
        }
        /* The four pixels right of the last macroblock's bottom row: its last pixel repeated
           (the above-right of the next row's last macroblock). */
        uint8_t *last = plane_at(f->y, f->ystride, f->mb_cols * 16, row * 16 + 15);
        memset(last, last[-1], 4);
    }
    if (f->filter_level) {
        loop_filter(f);
    }
    struct gfx_surface *s = gfx_surface_create(w, h);
    if (s) {
        to_rgb(f, s);
    }
    cleanup(f);
    return s ? s : image_fail(error, "not enough memory");
}
