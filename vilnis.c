/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VILNIS ("wave" in Latvian): a west coast machine for the Digitakt MKI (core 4.4 + machine-pages,
 * OS 1.54), inspired by Majella Audio's Implexus and Compax (a complex oscillator into a wavefolder with
 * HARMONICS and BIAS) and by the Buchla Music Easel (a low pass gate, a pulser and a random voltage).
 *
 * Page 1 VOICE: TUNE  HARM  FOLD  BIAS  RATIO MOD   DCY   LEV
 *   HARM the main oscillator's shape, sine - triangle - saw; FOLD how hard it is folded; BIAS the
 *   folder's symmetry; RATIO the modulation oscillator's pitch against the main one (x1/4 .. x8);
 *   MOD how much it modulates; DCY the low pass gate's decay.
 * Page 2 WEST:  MODE  FINE  GATE  RES   PULS  RAND  FOLD  VIB    (FOLD and VIB draw the waveform)
 *   MODE FM, AM or both; FINE the modulation oscillator's detune (+-1 semitone, bells); GATE the low
 *   pass gate: OFF, LP (it darkens), VCA (it fades) or BOTH; RES its resonance; PULS the pulser, which
 *   strikes the gate again and again (OFF, 0.25 - 32 Hz); RAND a random voltage, new on each strike,
 *   added to FOLD and MOD; VIB a vibrato.
 * Machine 36. */
#include "digitakt-mk1/core3.h"
#include "tables.h"

#define VL_ID 36
enum { K_TUNE, K_HARM, K_FOLD, K_BIAS, K_RATIO, K_MOD, K_DCY, K_LEV,
       K_MODE, K_FINE, K_GATE, K_RES, K_PULS, K_RAND, K_VIB };

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the modulation oscillator's ratios to the main one */
#define RATIOS 16
static const uint8_t r_num[RATIOS] = { 1, 1, 3, 1, 5, 3, 7, 2, 5, 3, 7, 4, 5, 6, 7, 8 };
static const uint8_t r_den[RATIOS] = { 4, 2, 4, 1, 4, 2, 4, 1, 2, 1, 2, 1, 1, 1, 1, 1 };
static int32_t ratio_of(int32_t v) { return clampi(v * RATIOS / 128, 0, RATIOS - 1); }
static int32_t mode_of(int32_t v) { return clampi(v * 3 / 128, 0, 2); }          /* FM AM BOTH */
static int32_t gate_of(int32_t v) { return clampi(v * 4 / 128, 0, 3); }          /* OFF LP VCA BOTH */

static inline int32_t sin_q15(uint32_t ph) { return vl_sin[ph >> 22]; }

/* ---- the sound (ev_render_voices: interrupt level, 1500 blocks a second) ---------------------------- */

static struct vl {
    uint32_t ph, mph, vph, pph;     /* main, modulation, vibrato, pulser phases */
    int32_t lpg, peak, rising;      /* the gate's level (Q30), where a strike rises to, rising or not */
    int32_t low, band;              /* the gate's filter */
    int32_t rnd;                    /* the random voltage, Q15 */
    uint32_t noise;
} vl[8];

static uint32_t inc_for(int32_t pitch)                                  /* 16.16 semitones */
{
    pitch = clampi(pitch, 0, 87 << 16);
    return (23410327u >> 13) * (fw_pitch_tab[pitch / 384] >> 16);
}
/* 0.3: a knob's value straight from the engine's copy (where machine-pages keeps it: base knobs at
 * 0-7, extras 8-14 at slot 46 + k - 8, which is 21 + k past fw_voice_params' start), without its lookup */
static inline int32_t knob(int t, int k)
{
    return clampi(fw_voice_params[53 * t + (k < 8 ? k : 21 + k)] >> 8, 0, 127);
}

static void strike(struct vl *s, int32_t vel)
{
    uint32_t x = s->noise;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->noise = x;
    s->rnd = (int32_t)(x >> 17) - 16384;                                /* +-0.5 */
    s->peak = (1 << 29) + vel * (1 << 22);                              /* 0.5 .. 1.0 by velocity */
    s->rising = 1;
}

/* the main oscillator's shape at phase ph: sine to triangle (HARM 0-63), triangle to saw (64-127) */
static inline int32_t shape(uint32_t ph, int32_t harm)
{
    int32_t v = (int32_t)(ph >> 16);                                    /* 0 .. 65535 */
    int32_t tri = v < 32768 ? v * 2 - 32768 : (65535 - v) * 2 - 32767;
    if (harm < 64) {
        int32_t sn = sin_q15(ph - 0x40000000u);                         /* in phase with the triangle */
        return sn + (((tri - sn) * harm) >> 6);
    }
    int32_t saw = v - 32768;
    return tri + (((saw - tri) * (harm - 64)) / 63);
}

/* the folder: the signal x gain + bias is a phase into a sine, so whatever passes +-1.0 folds back */
static inline int32_t fold(int32_t x, int32_t fg, int32_t bias)
{
    int32_t v = ((x * fg) >> 8) + bias;
    return sin_q15((uint32_t)v << 15);
}

struct lp_state { uint32_t a, m; int32_t low, band, l15; };
struct lp_args { int32_t *out; uint32_t inc, minc; int32_t pm, am, harm, hk, fg, bias, f, q, open, l15_step, gain; };

/* one block of one voice; lp, vca and use_mod are constants in each copy, so their tests vanish */
static inline __attribute__((always_inline)) void run(struct lp_state *st, const struct lp_args *g,
                                                      const int lp, const int vca, const int use_mod)
{
    uint32_t a = st->a, m = st->m;
    int32_t low = st->low, band = st->band, l15 = st->l15;
    const uint32_t inc = g->inc, minc = g->minc;
    const int32_t pm = g->pm, am = g->am, harm = g->harm, hk = g->hk, fg = g->fg, bias = g->bias;
    const int32_t f = g->f, q = g->q, open = g->open, l15_step = g->l15_step, gain = g->gain;
    int32_t *out = g->out;
    for (int i = 0; i < 32; i++) {
        int32_t ms = use_mod ? sin_q15(m) : 0, x, v;
        uint32_t pa = use_mod ? a + ((uint32_t)(ms * pm) << 10) : a;
        /* the main oscillator: sine to triangle (HARM 0-63), triangle to saw (64-127) */
        v = (int32_t)(pa >> 16);
        int32_t tri = v < 32768 ? v * 2 - 32768 : (65535 - v) * 2 - 32767;
        if (harm < 64) {
            int32_t sn = sin_q15(pa - 0x40000000u);
            x = harm ? sn + (((tri - sn) * harm) >> 6) : sn;
        } else
            x = tri + ((((v - 32768) - tri) * hk) >> 15);
        x = fold(x, fg, bias);
        if (use_mod && am)
            x = (x * (32768 - ((am * (32768 - ms)) >> 8))) >> 15;
        if (lp) {
            low = clampi(low + ((f * band) >> 15), -65535, 65535);
            int32_t high = clampi(x - low - ((q * band) >> 14), -65535, 65535);
            band = clampi(band + ((f * high) >> 15), -65535, 65535);
            x = open ? low + ((((x - low) >> 1) * open) >> 14) : low;
        }
        if (vca) {
            x = (x * l15) >> 15;
            l15 += l15_step;
        }
        out[i] = (x * gain) << 2;                                       /* Q15 x Q14 -> Q31 */
        a += inc;
        m += minc;
    }
    st->a = a; st->m = m; st->low = low; st->band = band;
}

void vl_render(int32_t *blocks)
{
    for (int t = 0; t < 8; t++) {
        if (fw_render_machine[t] != VL_ID)
            continue;
        struct vl *s = &vl[t];
        int32_t *out = &blocks[32 * t];
        const volatile int16_t *p = &fw_voice_params[53 * t];
        int start = (fw_voice_start >> t) & 1;
        int32_t vel = clampi(fw_voice_vel[t] >> 8, 0, 127);
        int32_t gate = gate_of(knob(t, K_GATE)), puls = knob(t, K_PULS);
        if (!s->noise)
            s->noise = 0x9e3779b9u + (uint32_t)t * 7919u;

        if (start) {
            s->ph = s->mph = 0;
            s->pph = 0;
            strike(s, vel);
        }
        if (puls) {                                                     /* the pulser strikes again */
            uint32_t before = s->pph;
            s->pph += vl_puls[puls];
            if (s->pph < before)
                strike(s, vel);
        }

        int32_t al = fw_amp_level[2 * t + 1];
        int amp_done = !start && fw_amp_level[2 * t] == FW_AMP_DECAY && al > -(1 << 15) && al < (1 << 15);
        int gate_shut = gate >= 2 && !s->rising && s->lpg < (1 << 22);
        if (amp_done || gate_shut) {
            for (int i = 0; i < 32; i++)
                out[i] = 0;
            if (gate_shut)
                s->lpg = s->low = s->band = 0;
            continue;
        }

        /* pitch, with the vibrato: about 5.5 Hz, up to +-60 cents, once a block */
        int32_t vib = knob(t, K_VIB);
        s->vph += 15748365u;                                            /* 5.5 Hz at 1500 blocks/s */
        int32_t pitch = ((int32_t)p[K_TUNE] - 0x4000) * 256 + fw_voice_note[t] + (3 << 16);
        if (vib)
            pitch += sin_q15(s->vph) * vib / 106;                       /* 32767 x 127 / 106 = 60 cents */
        uint32_t inc = inc_for(pitch);
        int32_t r = ratio_of(knob(t, K_RATIO));
        int32_t fine = (knob(t, K_FINE) - 64) * 1024;                   /* +-1 semitone, 16.16 */
        uint32_t minc = fine ? inc_for(pitch + fine) : inc;
        minc = (minc / r_den[r]) * r_num[r];

        int32_t rnd = s->rnd * knob(t, K_RAND) >> 7;                    /* +-0.5 at the most */
        int32_t harm = knob(t, K_HARM);
        int32_t fg = 256 + clampi(knob(t, K_FOLD) + (rnd >> 8), 0, 127) * 26;    /* x1 .. x14, 8.8 */
        int32_t bias = (knob(t, K_BIAS) - 64) * 512;                    /* +-1.0, Q15 */
        int32_t mod = clampi(knob(t, K_MOD) + (rnd >> 8), 0, 127);
        int32_t mode = mode_of(knob(t, K_MODE));
        int32_t pm = mode != 1 ? mod : 0;                               /* FM depth: up to a turn */
        int32_t am = mode == 1 ? mod : mode == 2 ? mod >> 1 : 0;        /* AM depth */

        int32_t dk = vl_dcy[knob(t, K_DCY)];
        int32_t res = knob(t, K_RES);
        int32_t q = 32768 - res * 242;                                  /* damping 2.0 .. 0.12, Q14 */
        int32_t lev = (p[K_LEV] >> 8) & 0x7f, gain = (lev * lev * (64 + (vel >> 1))) >> 7;   /* Q14 */

        /* 0.2: the gate moves once a block (a ramp across it), not every sample */
        int32_t L = s->lpg, L0 = L;
        if (s->rising) {
            L += (s->peak - L) - ((s->peak - L) >> 2);                            /* a strike: 3/4 of the way a block */
            if (L >= s->peak - (s->peak >> 6))
                s->rising = 0;
        } else {
            L -= (L >> 20) * (dk + ((dk * (L >> 15)) >> 13)) * 32;      /* faster while it is high */
            if (L < 0)
                L = 0;
        }
        int32_t l15 = L0 >> 15, l15_end = L >> 15, l15_step = (l15_end - l15) / 32;
        int32_t lp = gate == 1 || gate == 3, vca = gate >= 2;
        int32_t f = 330 + ((((l15_end * l15_end) >> 15) * 29000) >> 15);
        int32_t open = l15_end > 24576 ? (l15_end - 24576) * 4 : 0;     /* wide open: let the top through */
        int32_t hk = harm >= 64 ? (harm - 64) * 520 : 0;                /* (harm - 64) / 63, Q15 */
        int32_t use_mod = pm || am;

        struct lp_state st = { s->ph, s->mph, s->low, s->band, l15 };
        struct lp_args ar = { out, inc, minc, pm, am, harm, hk, fg, bias, f, q, open, l15_step, gain };
        switch ((lp ? 4 : 0) | (vca ? 2 : 0) | (use_mod ? 1 : 0)) {    /* a loop made for each case */
        case 0: run(&st, &ar, 0, 0, 0); break;
        case 1: run(&st, &ar, 0, 0, 1); break;
        case 2: run(&st, &ar, 0, 1, 0); break;
        case 3: run(&st, &ar, 0, 1, 1); break;
        case 4: run(&st, &ar, 1, 0, 0); break;
        case 5: run(&st, &ar, 1, 0, 1); break;
        case 6: run(&st, &ar, 1, 1, 0); break;
        default: run(&st, &ar, 1, 1, 1); break;
        }
        uint32_t a = st.a, m = st.m;
        int32_t low = st.low, band = st.band;
        s->ph = a; s->mph = m;
        s->lpg = L; s->low = low; s->band = band;
    }
}

/* ---- the page: value texts --------------------------------------------------------------------------- */

static int32_t put(char *buf, const char *s)
{
    int i = 0;
    while ((buf[i] = s[i]))
        i++;
    return 1;
}
static char *put_int(char *b, int32_t v)
{
    char d[8];
    int n = 0;
    if (v < 0) { *b++ = '-'; v = -v; }
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *b++ = d[--n];
    *b = 0;
    return b;
}
static int32_t v127(int32_t value) { return clampi(value >> 8, 0, 127); }
static int32_t shown(int k, int32_t fallback)
{
    int32_t v = cm_src_shown(k);
    return v == CM_NO_VALUE ? fallback : v127(v);
}

static int32_t num_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    (void)ctx; (void)m;
    put_int(buf, v127(value));
    return 1;
}
static int32_t bias_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    int32_t v = v127(value) - 64;
    (void)ctx; (void)m;
    if (v > 0) *buf++ = '+';
    put_int(buf, v);
    return 1;
}
static int32_t ratio_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    int32_t r = ratio_of(v127(value)), n = r_num[r], d = r_den[r];
    char *b = buf;
    (void)m;
    if (ctx) b += put(b, "Ratio ") * 6;
    *b++ = 'x';
    b = put_int(b, n / d);
    if (n % d) {                                                        /* .25 .5 .75 */
        int32_t f = (n % d) * 100 / d;
        *b++ = '.';
        if (f % 10) b = put_int(b, f); else b = put_int(b, f / 10);
    }
    return 1;
}
static int32_t fine_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    int32_t c = (v127(value) - 64) * 100 / 64;
    char *b = buf;
    (void)m;
    if (c > 0) *b++ = '+';
    b = put_int(b, c);
    if (ctx) put(b, " cents");
    return 1;
}
static int32_t mode_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    static const char *const s[3] = { "FM", "AM", "BOTH" }, *const l[3] = { "FM", "AM", "FM + AM" };
    (void)m;
    return put(buf, (ctx ? l : s)[mode_of(v127(value))]);
}
static int32_t gate_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    static const char *const s[4] = { "OFF", "LP", "VCA", "BOTH" };
    static const char *const l[4] = { "Gate off", "Low pass", "VCA", "Low pass + VCA" };
    (void)m;
    return put(buf, (ctx ? l : s)[gate_of(v127(value))]);
}
static int32_t puls_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    int32_t v = v127(value), c = vl_puls_chz[v];
    char *b = buf;
    (void)m;
    if (!v)
        return put(buf, "OFF");
    if (c < 1000) {                                                     /* 0.25 .. 9.9 */
        b = put_int(b, c / 100);
        *b++ = '.';
        b = put_int(b, c / 10 % 10);
    } else
        b = put_int(b, (c + 50) / 100);
    if (ctx) put(b, " Hz");
    return 1;
}

/* ---- the page: graphics (UI task; y = 0 is the bottom row) ------------------------------------------- */

/* One cycle of the sound as the knobs set it, w columns, h rows */
static void draw_wave(void *bmp, int32_t x, int32_t y, int32_t w, int32_t h, int full)
{
    int32_t harm = shown(K_HARM, 0), fg = 256 + shown(K_FOLD, 30) * 26;
    int32_t bias = (shown(K_BIAS, 64) - 64) * 512;
    int32_t r = ratio_of(shown(K_RATIO, 56)), pm = 0, prev = -1;
    if (full && mode_of(shown(K_MODE, 0)) != 1)
        pm = shown(K_MOD, 20);
    for (int32_t c = 0; c < w; c++) {
        uint32_t ph = (uint32_t)(c * (int32_t)(0x10000 / w)) << 16;
        uint32_t mph = (ph / r_den[r]) * r_num[r];
        int32_t ms = sin_q15(mph);
        int32_t v = fold(shape(ph + ((uint32_t)(ms * pm) << 10), harm), fg, bias);
        int32_t row = (v + 32768) * (h - 1) / 65535;
        int32_t lo = prev < 0 ? row : prev < row ? prev : row, hi = prev < 0 ? row : prev > row ? prev : row;
        fw_fillrect(bmp, x + c, y + lo, x + c, y + hi, 1);
        prev = row;
    }
}

static int32_t g_x = -1, h_x = -1;                                     /* FOLD (page 2) and VIB */

/* the panel across G and H: the waveform, and the gate's level on the active track as a bar */
static void draw_panel(void *bmp, int32_t y)
{
    int32_t x0 = (g_x >= 0 ? g_x : h_x - 16) + 1, x1 = (h_x >= 0 ? h_x : g_x + 16) + 16;
    int32_t t = clampi(fw_active_track, 0, 7), lv = clampi(vl[t].lpg >> 26, 0, 15);
    fw_fillrect(bmp, x0, y, x1, y + 16, 0);
    draw_wave(bmp, x0, y + 1, x1 - x0 - 4, 15, 1);
    fw_framerect(bmp, x1 - 2, y + 1, x1, y + 15, 1);
    if (lv)
        fw_fillrect(bmp, x1 - 2, y + 1, x1, y + lv, 1);
}

static int32_t draw_fold(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    (void)value; (void)flag;
    if (x >= 88) {                                                      /* page 2, knob G */
        g_x = x;
        draw_panel(bmp, y);
        return 1;
    }
    fw_fillrect(bmp, x + 1, y + 1, x + 16, y + 15, 0);
    draw_wave(bmp, x + 1, y + 1, 16, 15, 0);                            /* page 1: the folded shape */
    return 1;
}
static int32_t draw_vib(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    (void)value; (void)flag;
    h_x = x;
    draw_panel(bmp, y);
    return 1;
}

/* ---- the page ------------------------------------------------------------------------------------------ */

#define KNOB(nm, ln, d, f) { .name = nm, .lname = ln, .look = 0x86, .flags = CM_RANGE | CM_DEFAULT, \
                             .max = 0x7f00, .def = (d) << 8, .fmt = f }
#define DRAWN(nm, ln, d, f, dr) { .name = nm, .lname = ln, .look = 0x86, \
                                  .flags = CM_RANGE | CM_DEFAULT | CM_DRAW, .max = 0x7f00, .def = (d) << 8, \
                                  .fmt = f, .draw = dr }

static const struct cm_src_page vl_pages[2] = {
    { "VOICE", { 0, 1, 2, 3, 4, 5, 6, 7 }, 0, { 0, 0, 0 } },
    { "WEST", { 8, 9, 10, 11, 12, 13, 2, 14 }, 0, { 0, 0, 0 } },
};

static const struct cm_ui32 vl_page = {
    .base = {
        .abi = cm_ui_v32,
        .page_from = 3,
        .group = "VILNIS",
        .knob = {
            [K_HARM]  = KNOB("HARM", "Harmonics", 0, num_text),
            [K_FOLD]  = DRAWN("FOLD", "Wavefolder", 30, num_text, draw_fold),
            [K_BIAS]  = { .name = "BIAS", .lname = "Folder bias", .look = 0x86,
                          .flags = CM_RANGE | CM_DEFAULT | CM_NOT_SAMPLE, .max = 0x7f00, .def = 64 << 8,
                          .fmt = bias_text },
            [K_RATIO] = KNOB("RATI", "Mod osc ratio", 56, ratio_text),     /* x2 */
            [K_MOD]   = KNOB("MOD", "Modulation", 20, num_text),
            [K_DCY]   = KNOB("DCY", "Gate decay", 50, num_text),
        },
    },
    .extra = {
        KNOB("MODE", "FM, AM or both", 0, mode_text),
        KNOB("FINE", "Mod osc detune", 64, fine_text),
        KNOB("GATE", "Low pass gate", 127, gate_text),
        KNOB("RES", "Gate resonance", 20, num_text),
        KNOB("PULS", "Pulser", 0, puls_text),
        KNOB("RAND", "Random voltage", 0, num_text),
        DRAWN("VIB", "Vibrato", 0, num_text, draw_vib),
    },
    .page_count = 2,
    .pages = vl_pages,
};

/* The menu's 11 x 7 icon (top row bit 25): a folded wave. */
static const uint32_t vl_px[11] = {
    0x10000000, 0x28000000, 0x44000000, 0x82000000, 0x44000000, 0x28000000,
    0x10000000, 0x28000000, 0x44000000, 0x82000000, 0x44000000,
};
static const uint32_t vl_mask[11] = {
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
};
static const struct {
    const void *vt;
    int32_t width, height, one;
    const uint32_t *px, *mask;
    int32_t zero;
} vl_icon = { fw_bitmap_vt, 11, 7, 1, vl_px, vl_mask, 0 };

const struct cm_machine vl_machine = {
    VL_ID, "VILNIS", "VLNS", &vl_icon, 3, VL_ID, CM_UI_TAG, (const struct cm_ui *)&vl_page,
};
