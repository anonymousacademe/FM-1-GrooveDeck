/* SPDX-License-Identifier: GPL-3.0-only */
/* GROVEDECK: a drum machine and groovebox for the M-VAVE FM-1. Eight synthesized drums, a 16-step sequencer, eight patterns.
 * Built on Dinghy (zvenson); the platform is Felucca (Leo Kuroshita, Hugelton Instruments) and SLOOP (isod89).
 *
 *   drums     KICK SNARE CLAP C.HAT O.HAT TOM RIM COWBELL, each four knobs; the closed hat chokes the open one
 *   keys      1-16 pads (the drums twice) or, in STEP mode, the 16 steps | 17-24 pattern | 25 MUTE 26 SOLO 27 CLEAR (hold + pad)
 *   buttons   PLAY REC  SEQ (step mode)  EDIT sound  ENV mix  FX  GLO global  ARP (hold: note repeat)  SEL (hold + pad: select)
 *             OCT- OCT+ pattern  SAVE  HOME (hold: panic and stop)
 *   encoders  KNOB1-4 the page, SELECT the drum, ALGORITHM tempo, PRESETS the kit, MASTER volume
 *
 * Integers only (no float, no 64-bit division). Everything that makes sound or keeps time runs in the audio ISR; the
 * main loop posts events. tests/host.c runs this file on a computer. */
#define APP_NAME "GROVEDECK"
#include "platform.c"
#include "tables.h"

#ifndef FM1_TICKS_PER_US
#define FM1_TICKS_PER_US 24u
#endif

#define NT 8u                                   /* drums */
#define NSTEP 16u
#define NPAT 8u
enum { T_KICK, T_SNARE, T_CLAP, T_CHAT, T_OHAT, T_TOM, T_RIM, T_COW };
static const char *const TNAME[NT] = {"KICK", "SNARE", "CLAP", "C.HAT", "O.HAT", "TOM", "RIM", "COWBL"};
static const char *const TLAB[NT][4] = {
    {"TUNE", "DECAY", "PUNCH", "DRIVE"}, {"TUNE", "DECAY", "TONE", "SNAP"},
    {"TONE", "DECAY", "SPREAD", "DRIVE"}, {"TUNE", "DECAY", "TONE", "NOISE"},
    {"TUNE", "DECAY", "TONE", "NOISE"}, {"TUNE", "DECAY", "PUNCH", "CLICK"},
    {"TUNE", "DECAY", "TONE", "CLICK"}, {"TUNE", "DECAY", "TONE", "DRIVE"}};
static const char *const MLAB[4] = {"LEVEL", "PAN", "SEND", "MUTE"};
static const char *const PGN[4] = {"SOUND", "MIX", "FX", "GLOBAL"};

/* shared state: written by the main loop only (the ISR reads) */
enum { G_BPM, G_SWING, G_ACCENT, G_REPEAT, G_CRUSH, G_DMIX, G_DTIME, G_DFB, NG };
static const char *const GLAB[NG] = {"BPM", "SWING", "ACCENT", "REPEAT", "CRUSH", "DLY MIX", "DLY TIME", "FEEDBACK"};
static volatile uint8_t G[NG];
static volatile uint8_t TP[NT][4];              /* the sound of each drum */
static volatile uint8_t MX[NT][3];              /* level, pan, delay send */
static volatile uint8_t mute_mask, solo_mask;
static volatile uint8_t pat[NPAT][NT][NSTEP];   /* a step: 0 off, bit 0 on, bit 1 accent (the ISR writes while recording) */

/* four kits: tune decay x3 x4 | level pan send */
static const uint8_t KIT[4][NT][7] = {
    {{40, 60, 90, 70, 118, 64, 0}, {60, 55, 70, 60, 105, 60, 30}, {80, 60, 50, 40, 95, 76, 55}, {70, 25, 90, 70, 80, 84, 10},
     {70, 60, 90, 70, 75, 90, 25}, {40, 55, 50, 30, 95, 48, 20}, {70, 30, 60, 50, 85, 70, 25}, {55, 50, 70, 40, 70, 80, 40}},
    {{60, 70, 70, 30, 118, 64, 0}, {80, 50, 80, 50, 105, 60, 25}, {70, 70, 60, 20, 95, 76, 45}, {64, 20, 70, 50, 80, 84, 10},
     {64, 55, 70, 50, 75, 90, 20}, {70, 60, 60, 20, 95, 48, 15}, {64, 25, 50, 30, 85, 70, 20}, {64, 45, 60, 20, 70, 80, 30}},
    {{20, 100, 30, 10, 120, 64, 0}, {40, 60, 40, 70, 100, 60, 20}, {60, 80, 80, 0, 90, 76, 50}, {60, 15, 60, 30, 75, 84, 5},
     {60, 70, 60, 30, 72, 90, 25}, {30, 80, 20, 10, 95, 48, 20}, {50, 35, 40, 20, 80, 70, 20}, {50, 70, 50, 0, 70, 80, 35}},
    {{50, 45, 50, 40, 112, 64, 0}, {55, 40, 40, 50, 100, 60, 25}, {60, 45, 60, 30, 90, 76, 45}, {45, 18, 40, 60, 78, 84, 10},
     {45, 40, 40, 60, 72, 90, 20}, {55, 45, 40, 40, 90, 48, 20}, {60, 22, 40, 70, 80, 70, 20}, {45, 35, 45, 30, 65, 80, 30}}};
static const char *const KITNAME[4] = {"CYBER", "909", "808", "DUST"};

/* ---- the drums (run in the audio ISR: no flash, no screen, no waiting) */
typedef struct {
    int32_t env, env2, dc, dc2, penv, pdc, lp1, lp2, gain, x[3];
    uint32_t ph[6], inc[6], sw, t, b1;
    uint8_t on;
} dv_t;
static dv_t dv[NT];
static volatile uint32_t trig_cnt[NT];

static inline int32_t sclip(int32_t v)          /* soft clip, 1.5v - 0.5v^3 */
{
    int32_t t;
    if (v > 32767) return 32767;
    if (v < -32767) return -32767;
    t = (v * v) >> 15;
    return (v * ((98304 - t) >> 1)) >> 15;
}
static inline int32_t clamp15(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
static uint32_t nz = 0x2545F491u;
static inline int32_t noise(void) { nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5; return (int32_t)nz >> 16; }
static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return clamp15(y);
}
static inline uint32_t inc_hz16(uint32_t f16) { return (uint32_t)(((uint64_t)f16 * 97392u) >> 4); }   /* f in 1/16 Hz */

static void trigger(uint32_t t, uint32_t vel)
{
    dv_t *d = &dv[t];
    uint32_t p0 = TP[t][0], p1 = TP[t][1], p2 = TP[t][2], p3 = TP[t][3], i, m;
#ifdef DINGHY_HOST
    printf("%6u ms  hit %u vel %3u\n", fm1_ms, t, vel);   /* (host only: the trace the tests read) */
#endif
    memset(d, 0, sizeof *d);
    d->on = 1;
    d->gain = (int32_t)vel * 258;
    d->env = 32767;
    switch (t) {
    case T_KICK:
        d->inc[0] = inc_hz16(36u * 16u + p0 * 6u);
        d->sw = (uint32_t)(((uint64_t)d->inc[0] * (p2 * 5u)) >> 7);
        d->penv = 32767; d->pdc = 65464;
        d->dc = (int32_t)DECC[25u + p1 * 75u / 128u];
        d->x[0] = (int32_t)p3;
        break;
    case T_TOM:
        d->inc[0] = inc_hz16(80u * 16u + p0 * 20u);
        d->sw = (uint32_t)(((uint64_t)d->inc[0] * (p2 * 2u)) >> 7);
        d->penv = 32767; d->pdc = 65300;
        d->dc = (int32_t)DECC[20u + p1 * 60u / 128u];
        d->x[0] = (int32_t)p3;
        break;
    case T_SNARE:
        d->inc[0] = inc_hz16(150u * 16u + p0 * 13u);
        d->inc[1] = (d->inc[0] * 13u) >> 3;
        d->sw = d->inc[0] >> 2; d->penv = 32767; d->pdc = 64800;
        d->dc = (int32_t)DECC[12u + p1 * 45u / 128u];
        d->env2 = 32767; d->dc2 = (int32_t)DECC[30u + p3 * 60u / 128u];
        d->x[0] = (int32_t)p2;
        break;
    case T_CLAP:
        d->x[0] = (int32_t)(6000u + p0 * 190u);                /* the filter */
        d->x[1] = (int32_t)DECC[30u + p1 * 55u / 128u];        /* the tail, after three bursts */
        d->x[2] = (int32_t)p3;
        d->b1 = 200u + p2 * 4u;
        d->dc = 65164;
        break;
    case T_CHAT:
    case T_OHAT:
        m = TUNEM[p0];
        for (i = 0; i < 6u; i++) d->inc[i] = (uint32_t)(((uint64_t)HATINC[i] * m) >> 12);
        d->dc = (int32_t)DECC[t == T_CHAT ? p1 * 40u / 128u : 45u + p1 * 55u / 128u];
        d->x[0] = (int32_t)(14000u + p2 * 130u);
        d->x[1] = (int32_t)p3;
        if (t == T_CHAT) dv[T_OHAT].dc = 64000;                /* the closed hat chokes the open one */
        break;
    case T_RIM:
        m = TUNEM[p0];
        d->inc[0] = (uint32_t)(((uint64_t)RIMINC[0] * m) >> 12);
        d->inc[1] = (uint32_t)(((uint64_t)RIMINC[1] * m) >> 12);
        d->dc = (int32_t)DECC[p1 * 35u / 128u];
        d->x[0] = (int32_t)p2; d->x[1] = (int32_t)p3;
        break;
    default:                                    /* cowbell */
        m = TUNEM[p0];
        d->inc[0] = (uint32_t)(((uint64_t)COWINC[0] * m) >> 12);
        d->inc[1] = (uint32_t)(((uint64_t)COWINC[1] * m) >> 12);
        d->dc = (int32_t)DECC[25u + p1 * 70u / 128u];
        d->x[0] = (int32_t)(6000u + p2 * 190u); d->x[1] = (int32_t)p3;
        break;
    }
    trig_cnt[t]++;
}

static void render_track(uint32_t t, int32_t *o)   /* 16 samples, Q15 */
{
    dv_t *d = &dv[t];
    uint32_t i, k;
    switch (t) {
    case T_KICK:
    case T_TOM:
        for (i = 0; i < 16u; i++) {
            int32_t s;
            d->ph[0] += d->inc[0] + (uint32_t)(((int64_t)d->sw * d->penv) >> 15);
            d->penv = (d->penv * d->pdc) >> 16;
            s = (sine(d->ph[0]) * d->env) >> 15;
            d->env = (d->env * d->dc) >> 16;
            if (t == T_KICK) s = sclip((s * (256 + d->x[0] * 6)) >> 8);
            else if (d->t < 300u) s += (((noise() * d->x[0]) >> 7) * (int32_t)(300u - d->t)) >> 9;
            d->t++;
            o[i] = clamp15(s);
        }
        break;
    case T_SNARE:
        for (i = 0; i < 16u; i++) {
            int32_t b, n, hp, s;
            d->ph[0] += d->inc[0] + (uint32_t)(((int64_t)d->sw * d->penv) >> 15);
            d->ph[1] += d->inc[1];
            d->penv = (d->penv * d->pdc) >> 16;
            b = ((sine(d->ph[0]) * 2 + sine(d->ph[1])) / 3 * d->env) >> 15;
            n = noise();
            d->lp1 += ((n - d->lp1) * 23000) >> 16;
            hp = n - d->lp1;
            n = (hp * d->env2) >> 15;
            s = ((b * (127 - d->x[0]) + n * (24 + d->x[0])) * 13) >> 10;
            d->env = (d->env * d->dc) >> 16;
            d->env2 = (d->env2 * d->dc2) >> 16;
            d->t++;
            o[i] = clamp15(s);
        }
        break;
    case T_CLAP:
        for (i = 0; i < 16u; i++) {
            int32_t n, s;
            if (d->t == d->b1) d->env = 32767;                              /* three bursts, then the tail */
            if (d->t == 2u * d->b1) { d->env = 32767; d->dc = d->x[1]; }
            n = noise();
            d->lp1 += ((n - d->lp1) * d->x[0]) >> 16;
            d->lp2 += ((d->lp1 - d->lp2) * 3500) >> 16;
            s = clamp15(((d->lp1 - d->lp2) * 3 * d->env) >> 15);
            s = sclip((s * (256 + d->x[2] * 5)) >> 8);
            d->env = (d->env * d->dc) >> 16;
            d->t++;
            o[i] = s;
        }
        break;
    case T_CHAT:
    case T_OHAT:
        for (i = 0; i < 16u; i++) {
            int32_t x = 0, y, y2, n;
            for (k = 0; k < 6u; k++) { d->ph[k] += d->inc[k]; x += (d->ph[k] >> 31) ? 3000 : -3000; }
            y = x - d->lp1; d->lp1 += (y * d->x[0]) >> 16;
            y2 = y - d->lp2; d->lp2 += (y2 * d->x[0]) >> 16;
            n = noise() - d->x[2]; d->x[2] += (n * d->x[0]) >> 16;                  /* the noise, high passed too */
            d->env = (d->env * d->dc) >> 16;
            d->t++;
            o[i] = clamp15(((y2 * 8 + ((n * d->x[1]) >> 8)) * d->env) >> 15);
        }
        break;
    case T_RIM:
        for (i = 0; i < 16u; i++) {
            int32_t s;
            d->ph[0] += d->inc[0]; d->ph[1] += d->inc[1];
            s = (sine(d->ph[0]) * (127 - d->x[0]) + sine(d->ph[1]) * d->x[0]) >> 7;
            if (d->t < 120u) s += (noise() * d->x[1]) >> 8;
            s = (clamp15(s) * d->env) >> 15;
            d->env = (d->env * d->dc) >> 16;
            d->t++;
            o[i] = s;
        }
        break;
    default:                                    /* cowbell: two squares, band passed */
        for (i = 0; i < 16u; i++) {
            int32_t x, s;
            d->ph[0] += d->inc[0]; d->ph[1] += d->inc[1];
            x = ((d->ph[0] >> 31) ? 9000 : -9000) + ((d->ph[1] >> 31) ? 9000 : -9000);
            d->lp1 += ((x - d->lp1) * d->x[0]) >> 16;
            d->lp2 += ((d->lp1 - d->lp2) * 2500) >> 16;
            s = ((d->lp1 - d->lp2) * 3 * d->env) >> 15;
            s = sclip((s * (256 + d->x[1] * 4)) >> 8);
            d->env = (d->env * d->dc) >> 16;
            d->t++;
            o[i] = s;
        }
        break;
    }
    if (d->t > 1500u && d->env < 16 && (t != T_SNARE || d->env2 < 16)) d->on = 0;   /* done: costs nothing now */
}

/* ---- the delay: ping-pong, in the pool (synced to the tempo) */
#define DLEN 32768u
static int16_t dlL[DLEN] __attribute__((section(".pool")));
static int16_t dlR[DLEN] __attribute__((section(".pool")));
static uint32_t step_len_g = 5512;
static volatile uint32_t cpu_pct;
static void engine_tick(void);

static void app_audio(int32_t *out, uint32_t n)
{
    static uint32_t dw, crush_n, cpu_max, cpu_blk;
    static int32_t dt_cur = 5512, lpL, lpR, heldL, heldR, pk, lgain = 32768;
    uint32_t t0 = fm1_ticks(), b, t, i;

    for (b = 0; b < n; b += 16u) {
        int32_t dl[16], dr[16], bus[16], o[16], dtt;
        engine_tick();                          /* events, the clock, the sequencer: they start the drums */
        for (i = 0; i < 16u; i++) dl[i] = dr[i] = bus[i] = 0;
        for (t = 0; t < NT; t++) {
            dv_t *d = &dv[t];
            int32_t lv, pan, g, gl, gr, sd;
            if (!d->on) continue;
            render_track(t, o);
            lv = MX[t][0]; pan = MX[t][1]; sd = MX[t][2];
            g = lv * lv * 2;
            gl = pan <= 64 ? g : (g * (127 - pan) * 519) >> 15;     /* pan: the far side is turned down */
            gr = pan >= 64 ? g : (g * pan) >> 6;
            for (i = 0; i < 16u; i++) {
                int32_t v = (o[i] * d->gain) >> 15;
                dl[i] += (v * gl) >> 16;
                dr[i] += (v * gr) >> 16;
                bus[i] += (v * sd) >> 8;
            }
        }
        {                                       /* the delay time follows the tempo; it glides, like a tape */
            static const uint8_t DM[4] = {1, 2, 3, 4};
            int32_t d, target = (int32_t)(step_len_g * DM[G[G_DTIME] >> 5]);
            target = target > 32700 ? 32700 : target;
            d = (target - dt_cur) >> 6;
            if (d == 0 && target != dt_cur) d = target > dt_cur ? 1 : -1;
            dt_cur += d;
            dtt = (pk > 26000 ? (int32_t)(26000u * 32768u / (uint32_t)pk) : 32768);   /* limiter gain: down at once, up slowly */
            lgain = dtt < lgain ? dtt : lgain + ((dtt - lgain) >> 4);
        }
        for (i = 0; i < 16u; i++) {
            int32_t L = dl[i], R = dr[i], yL, yR, fL, fR, wd, fb = (int32_t)((G[G_DFB] * 113u) >> 7), pkn;
            uint32_t rd;
            if (G[G_CRUSH]) {                   /* bit crusher on the dry mix */
                uint32_t bits = 16u - ((uint32_t)G[G_CRUSH] * 12u >> 7), hold = 1u + ((uint32_t)G[G_CRUSH] * 10u >> 7);
                if (++crush_n >= hold) { crush_n = 0; heldL = L & ~((1 << (16u - bits)) - 1); heldR = R & ~((1 << (16u - bits)) - 1); }
                L = heldL; R = heldR;
            }
            rd = (dw - (uint32_t)dt_cur) & (DLEN - 1u);
            yL = dlL[rd]; yR = dlR[rd];
            lpL += (yL - lpL) >> 2; lpR += (yR - lpR) >> 2;     /* each repeat is darker */
            fL = (lpL * fb) >> 7; fR = (lpR * fb) >> 7;
            wd = bus[i] + fR;                   /* ping-pong: the send and the right repeat feed the left, the left feeds the right */
            dlL[dw] = (int16_t)clamp15(wd);
            dlR[dw] = (int16_t)clamp15(fL);
            dw = (dw + 1u) & (DLEN - 1u);
            L += (yL * (int32_t)G[G_DMIX]) >> 7;
            R += (yR * (int32_t)G[G_DMIX]) >> 7;
            pkn = (L < 0 ? -L : L) > (R < 0 ? -R : R) ? (L < 0 ? -L : L) : (R < 0 ? -R : R);
            pk = pkn > pk ? pkn : pk - (pk >> 11);
            out[2u * (b + i)] += sclip((L * lgain) >> 15);
            out[2u * (b + i) + 1u] += sclip((R * lgain) >> 15);
        }
    }
    {   /* CPU: the worst block of the last 64, in percent of 256 frames (5805 us) */
        uint32_t dt = fm1_ticks() - t0;
        if (dt > cpu_max) cpu_max = dt;
        if (++cpu_blk >= 64u) { cpu_pct = cpu_max / (5805u * FM1_TICKS_PER_US / 100u); cpu_max = 0; cpu_blk = 0; }
    }
}

/* ---- the engine: events from the pads and MIDI, the clock, the sequencer, note repeat (all in the ISR) */
enum { EV_PAD, EV_PADUP, EV_PANIC };
#define EVQ 64u
static volatile uint16_t evq[EVQ];
static volatile uint32_t evq_w, evq_r;
static volatile uint8_t want_play, rec_arm, rpt, want_pat;   /* main -> ISR */
static volatile uint32_t playing, seq_step, cur_pat;         /* ISR -> main */
static uint32_t acc, cur_len, rpt_acc, held_mask;

static void push_ev(uint32_t type, uint32_t trk, uint32_t vel)
{
    uint32_t w = evq_w;
    if (w - evq_r >= EVQ) return;
    evq[w & (EVQ - 1u)] = (uint16_t)(type << 14 | (vel & 127u) << 7 | (trk & 127u));
    evq_w = w + 1u;
}
static int audible(uint32_t t) { return solo_mask ? (solo_mask >> t & 1u) : !(mute_mask >> t & 1u); }

static void pad_hit(uint32_t t, uint32_t vel)
{
    trigger(t, vel);
    if (rec_arm && playing) {                   /* record to the nearest step */
        uint32_t s = seq_step;
        if (acc * 2u >= cur_len) s = s + 1u >= NSTEP ? 0u : s + 1u;
        pat[cur_pat][t][s] = (uint8_t)(1u | (vel >= 110u ? 2u : 0u));
    }
}

static void engine_tick(void)                   /* every 16 frames */
{
    static const uint8_t RK[4] = {32, 16, 11, 8};     /* repeat: 1/8, 1/16, 1/16 triplet, 1/32, in 16ths of a step */
    uint32_t bpm = 60u + G[G_BPM], step_len = 661500u / bpm, swd = (step_len * G[G_SWING]) >> 9, wp, t;
    uint32_t nv = 127u - ((G[G_ACCENT] * 77u) >> 7);

    step_len_g = step_len;
    while (evq_r != evq_w) {
        uint32_t e = evq[evq_r & (EVQ - 1u)], type = e >> 14, vel = e >> 7 & 127u, trk = e & 127u;
        evq_r++;
        if (type == EV_PAD && trk < NT) { pad_hit(trk, vel); held_mask |= 1u << trk; rpt_acc = 0; }
        else if (type == EV_PADUP && trk < NT) held_mask &= ~(1u << trk);
        else if (type == EV_PANIC) { for (t = 0; t < NT; t++) dv[t].on = 0; held_mask = 0; }
    }

    wp = want_play;
    if (wp && !playing) {                       /* start: the first step sounds at once */
        seq_step = NSTEP - 1u;
        cur_len = step_len + (((NSTEP - 1u) & 1u) ? 0u - swd : swd);
        acc = cur_len;
        playing = 1;
    } else if (!wp && playing)
        playing = 0;
    if (!playing) cur_pat = want_pat & 7u;      /* while stopped the pattern changes at once, else at the end of the bar */

    if (rpt && held_mask) {                     /* note repeat: the held pads roll */
        uint32_t rl = (step_len * RK[G[G_REPEAT] >> 5]) >> 4;
        rpt_acc += 16u;
        if (rpt_acc >= rl) {
            rpt_acc -= rl;
            if (rpt_acc >= rl) rpt_acc = 0;
            for (t = 0; t < NT; t++) if (held_mask >> t & 1u) pad_hit(t, 100);
        }
    } else
        rpt_acc = 0;

    if (playing) {
        acc += 16u;
        if (acc >= cur_len) {
            uint32_t s;
            acc -= cur_len;
            if (acc >= step_len) acc = 0;
            s = seq_step + 1u;
            if (s >= NSTEP) { s = 0; cur_pat = want_pat & 7u; }
            seq_step = s;
            cur_len = step_len + ((s & 1u) ? 0u - swd : swd);
            for (t = 0; t < NT; t++) {
                uint32_t v = pat[cur_pat][t][s];
                if ((v & 1u) && audible(t)) trigger(t, v & 2u ? 127u : nv);
            }
        }
    }
}

/* ---- the project: the patterns, the sounds, the mixer (one flash sector, platform.c: project_load / project_save) */
#define PROJ_MAGIC 0x47u
static struct { uint8_t magic, cur, kit, mode; uint8_t pat[NPAT][NT][NSTEP], tp[NT][4], mx[NT][3], g[NG]; } pr;   /* 1092 B */
static uint32_t kit_n;

static void load_kit(uint32_t n)
{
    uint32_t t, j;
    kit_n = n;
    for (t = 0; t < NT; t++) {
        for (j = 0; j < 4u; j++) TP[t][j] = KIT[n][t][j];
        for (j = 0; j < 3u; j++) MX[t][j] = KIT[n][t][4 + j];
    }
}
static void put_pat(uint32_t p, uint32_t t, uint32_t mask, uint32_t acc_mask)
{
    uint32_t s;
    for (s = 0; s < NSTEP; s++) pat[p][t][s] = (uint8_t)((mask >> s & 1u) ? (acc_mask >> s & 1u ? 3u : 1u) : 0u);
}
static void defaults(void)
{
    static const uint8_t G0[NG] = {60, 0, 60, 40, 0, 40, 40, 45};     /* 120 bpm, straight, accent, repeat 1/16, no crush, delay 1/8 */
    uint32_t p, t, s;
    for (p = 0; p < NPAT; p++) for (t = 0; t < NT; t++) for (s = 0; s < NSTEP; s++) pat[p][t][s] = 0;
    put_pat(0, T_KICK, 0x1111, 0x0001);         /* a four-on-the-floor to start with */
    put_pat(0, T_SNARE, 0x1010, 0);
    put_pat(0, T_CLAP, 0x1000, 0);
    put_pat(0, T_CHAT, 0x1555, 0x0011);
    put_pat(0, T_OHAT, 0x4000, 0);
    put_pat(0, T_RIM, 0x0080, 0);
    put_pat(1, T_KICK, 0x0509, 0x0001);
    put_pat(1, T_SNARE, 0x1010, 0);
    put_pat(1, T_CLAP, 0x1010, 0);
    put_pat(1, T_CHAT, 0x5555, 0x1111);
    put_pat(1, T_OHAT, 0x2000, 0);
    put_pat(1, T_TOM, 0xC000, 0);
    put_pat(1, T_COW, 0x0040, 0);
    for (s = 0; s < NG; s++) G[s] = G0[s];
    mute_mask = solo_mask = 0;
    load_kit(0);
}

/* ---- the screen: a neon HUD, the step grid, four knobs per page */
#define BG RGB(6, 4, 14)
#define C_CY RGB(0, 235, 255)
#define C_MG RGB(255, 0, 160)
#define C_YE RGB(255, 225, 0)
#define C_PU RGB(60, 38, 100)
#define C_GR RGB(24, 16, 48)
#define C_TX RGB(200, 200, 230)
#define C_SELBG RGB(34, 0, 26)

static void itoa3(int32_t v, char *s)
{
    uint32_t i = 0, u = (uint32_t)(v < 0 ? -v : v), n = 0;
    char t[4];
    if (v < 0) s[i++] = '-';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) s[i++] = t[--n];
    s[i] = 0;
}
static void scopy(char *d, const char *s) { do { *d++ = *s; } while (*s++); }

static volatile uint8_t *param_ptr(uint32_t page, uint32_t r, uint32_t sel)
{
    if (page == 0u) return &TP[sel][r];
    if (page == 1u) return r < 3u ? &MX[sel][r] : 0;
    return &G[(page == 2u ? 4u : 0u) + r];
}
static const char *row_label(uint32_t page, uint32_t r, uint32_t sel)
{
    return page == 0u ? TLAB[sel][r] : page == 1u ? MLAB[r] : GLAB[(page == 2u ? 4u : 0u) + r];
}
static uint32_t row_val(uint32_t page, uint32_t r, uint32_t sel)      /* 0..127: the bar */
{
    volatile uint8_t *p = param_ptr(page, r, sel);
    return p ? *p : (mute_mask >> sel & 1u) ? 127u : 0u;
}
static void row_text(uint32_t page, uint32_t r, uint32_t sel, char *s)
{
    uint32_t v = row_val(page, r, sel);
    static const char *const RN[4] = {"1/8", "1/16", "1/16T", "1/32"}, *const DN[4] = {"1/16", "1/8", "1/8.", "1/4"};
    if (page == 1u && r == 3u) { scopy(s, v ? "MUTED" : "-"); return; }
    if (page == 1u && r == 1u) { int32_t x = (int32_t)v - 64; itoa3(x, s); return; }
    if (page == 3u && r == 3u) { scopy(s, RN[v >> 5]); return; }
    if (page == 2u && r == 2u) { scopy(s, DN[v >> 5]); return; }
    if (page == 3u && r == 0u) { itoa3(60 + (int32_t)v, s); return; }
    if (page == 3u && r == 1u) { itoa3(50 + (int32_t)(v * 25u / 127u), s); return; }
    itoa3((int32_t)v, s);
}

static void draw_title(uint32_t patn, uint32_t pl, uint32_t rc, uint32_t ban)   /* ban: 1..8 shows that drum big */
{
    char s[12], n[6], big[12];
    int32_t x;
    cv_begin(240, 40, BG);
    if (ban) {                                  /* the drum just selected, large, where the eye already is */
        big[0] = (char)('0' + ban); big[1] = ' '; scopy(big + 2, TNAME[ban - 1u]);
        cv_text(10, 0, &FONT_L, big, C_MG);
        cv_text(6, 0, &FONT_L, big, C_CY);
        cv_text(8, 0, &FONT_L, big, C_YE);
    } else {
        cv_text(10, 0, &FONT_L, "GROVEDECK", C_MG);                       /* chromatic aberration: magenta, cyan, then white on top */
        cv_text(6, 0, &FONT_L, "GROVEDECK", C_CY);
        cv_text(8, 0, &FONT_L, "GROVEDECK", C_WHITE);
    }
    itoa3(60 + G[G_BPM], n);
    scopy(s, "PAT ");
    s[4] = (char)('1' + patn); s[5] = 0;
    x = 236 - text_w(&FONT_S, s);
    cv_text(x, 0, &FONT_S, s, C_YE);
    x = 236 - text_w(&FONT_S, n) - (pl ? 17 : 0) - (rc ? text_w(&FONT_S, "REC") + 8 : 0);
    x = cv_text(x, 18, &FONT_S, n, C_TX) + 8;
    if (pl) x = cv_text(x, 18, &FONT_S, ">", C_CY) + 8;
    if (rc) cv_text(x, 18, &FONT_S, "REC", C_MG);
    cv_blit(0, 0);
}

static void draw_grid(uint32_t half, uint32_t sel)      /* four drums of the current pattern; the selected one is lit up */
{
    uint32_t r, s, cp = cur_pat;
    cv_begin(240, 48, BG);
    for (r = 0; r < 4u; r++) {
        uint32_t t = half * 4u + r, muted = !audible(t), isel = t == sel;
        int32_t y = (int32_t)r * 12 + 1;
        if (isel) {                             /* a tinted band and a bar on the left edge */
            cv_rect(0, y - 1, 240, 12, C_SELBG);
            cv_rect(0, y - 1, 2, 12, C_MG);
        }
        for (s = 0; s < NSTEP; s++) {
            int32_t x = (int32_t)s * 15 + 2;
            uint32_t v = pat[cp][t][s];
            uint16_t off = isel ? C_MG : (s & 3u) == 0 ? C_PU : C_GR;
            if (v & 1u) cv_rect(x, y, 12, 10, muted ? C_PU : (v & 2u) ? C_YE : C_CY);
            else { cv_rect(x, y, 12, 1, off); cv_rect(x, y + 9, 12, 1, off); cv_rect(x, y, 1, 10, off); cv_rect(x + 11, y, 1, 10, off); }
        }
    }
    cv_blit(0, 44u + half * 48u);
}
static void draw_head(uint32_t s, uint32_t pl)          /* the playhead above the grid */
{
    cv_begin(240, 4, BG);
    if (pl) cv_rect((int32_t)s * 15 + 2, 0, 12, 3, C_MG);
    cv_blit(0, 40);
}

static void draw_row(uint32_t page, uint32_t r, uint32_t sel, uint32_t hot)
{
    char s[12];
    uint32_t v = row_val(page, r, sel);
    cv_begin(240, 20, BG);
    cv_text(8, 2, &FONT_S, row_label(page, r, sel), r == hot ? C_YE : C_CY);
    cv_rect(112, 6, 90, 9, C_PU);
    cv_rect(113, 7, 88, 7, BG);
    cv_rect(113, 7, (int32_t)(v * 88u / 127u), 7, r == hot ? C_YE : C_MG);
    row_text(page, r, sel, s);
    cv_text(236 - text_w(&FONT_S, s), 2, &FONT_S, s, C_WHITE);
    cv_blit(0, 142u + r * 20u);
}

static void draw_status(uint32_t page, uint32_t sel, uint32_t stepmode, const char *msg)
{
    char l[12], r[40], n[8];
    uint32_t i = 0, j;
    const char *p;
    l[0] = (char)('1' + sel); l[1] = ' '; scopy(l + 2, TNAME[sel]);     /* the selected drum, bright, on the left */
    if (msg) scopy(r, msg);
    else {
        for (p = PGN[page]; *p; ) r[i++] = *p++;
        if (stepmode) { r[i++] = ' '; r[i++] = 'S'; r[i++] = 'T'; r[i++] = 'E'; r[i++] = 'P'; }
        r[i++] = ' '; r[i++] = 'C'; r[i++] = 'P'; r[i++] = 'U';
        itoa3((int32_t)cpu_pct, n);
        for (j = 0; n[j]; j++) r[i++] = n[j];
        r[i++] = '%'; r[i] = 0;
    }
    cv_begin(240, 16, BG);
    cv_text(4, 0, &FONT_S, l, C_YE);
    cv_text(236 - text_w(&FONT_S, r), 0, &FONT_S, r, msg ? C_CY : C_PU);
    cv_blit(0, 224);
}

static void app_main(void)
{
    uint32_t prev = 0, k, t, page = 0, sel = 0, stepmode = 0, redraw = 1, hot = 99, hot_drawn = 98, pend_save = 0;
    uint32_t grid_sig = ~0u, head_drawn = ~0u, t_stat = 0, t_msg = 0, t_home = 0, home_long = 0, sig_drawn = ~0u, led_drawn = ~0u;
    uint32_t seen[NT], flash_until[NT], banner_t = 0;
    uint8_t shown[4];
    const char *msg = 0;

    for (t = 0; t < NT; t++) { seen[t] = 0; flash_until[t] = 0; }
    if (project_load(&pr, sizeof pr) == 0 && pr.magic == PROJ_MAGIC && pr.cur < NPAT && pr.kit < 4u) {
        uint32_t p, j, s;
        for (p = 0; p < NPAT; p++) for (t = 0; t < NT; t++) for (s = 0; s < NSTEP; s++) pat[p][t][s] = pr.pat[p][t][s];
        for (t = 0; t < NT; t++) { for (j = 0; j < 4u; j++) TP[t][j] = pr.tp[t][j]; for (j = 0; j < 3u; j++) MX[t][j] = pr.mx[t][j]; }
        for (j = 0; j < NG; j++) G[j] = pr.g[j];
        kit_n = pr.kit;
        want_pat = pr.cur;
        stepmode = pr.mode & 1u;
    } else
        defaults();
    for (k = 0; k < 4u; k++) shown[k] = 255;
    lcd_fill(0, 0, 240, 240, BG);

    for (;;) {
        uint32_t keys, down, up, rel, b, ledmask = 0, r, sig, ban, patn = cur_pat, pl = playing, rc = rec_arm, sh = (fm1_in.buttons >> BTN_SEL) & 1u;
        int32_t d;
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            uint32_t mod_m = keys >> 24 & 1u, mod_s = keys >> 25 & 1u, mod_c = keys >> 26 & 1u;
            if (down >> k & 1u) {
                if (k < 16u) {
                    t = k & 7u;
                    if (mod_m) { mute_mask ^= (uint8_t)(1u << t); }                       /* hold MUTE + a pad */
                    else if (mod_s) { solo_mask ^= (uint8_t)(1u << t); }                  /* hold SOLO + a pad */
                    else if (mod_c) { for (r = 0; r < NSTEP; r++) pat[cur_pat][t][r] = 0; msg = "CLEARED"; t_msg = fm1_ms; }
                    else if (sh) { sel = t; redraw = 1; banner_t = fm1_ms; }                                 /* hold SEL + a pad */
                    else if (stepmode) { uint8_t v = pat[cur_pat][sel][k]; pat[cur_pat][sel][k] = (uint8_t)(v == 0u ? 1u : v == 1u ? 3u : 0u); }
                    else push_ev(EV_PAD, t, 100);
                } else if (k < 24u) {
                    if (mod_c) { for (t = 0; t < NT; t++) for (r = 0; r < NSTEP; r++) pat[k - 16u][t][r] = 0; msg = "PATTERN CLEARED"; t_msg = fm1_ms; }
                    else want_pat = (uint8_t)(k - 16u);
                }
            }
            if (up >> k & 1u && k < 16u && !stepmode) push_ev(EV_PADUP, k & 7u, 0);
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_EDIT)) { page = 0; redraw = 1; }
        if (b & (1u << BTN_ENV)) { page = 1; redraw = 1; }
        if (b & (1u << BTN_FX)) { page = 2; redraw = 1; }
        if (b & (1u << BTN_GLO)) { page = 3; redraw = 1; }
        if (b & (1u << BTN_SEQ)) { stepmode = !stepmode; msg = stepmode ? "STEP EDIT" : "PLAY"; t_msg = fm1_ms; }
        if (b & (1u << BTN_PLAY)) want_play = (uint8_t)!want_play;
        if (b & (1u << BTN_REC)) { rec_arm = (uint8_t)!rec_arm; msg = rec_arm ? "REC" : "REC OFF"; t_msg = fm1_ms; }
        if (b & (1u << BTN_OCT_DOWN)) want_pat = (uint8_t)((want_pat + 7u) & 7u);
        if (b & (1u << BTN_OCT_UP)) want_pat = (uint8_t)((want_pat + 1u) & 7u);
        if (b & (1u << BTN_SAVE)) { pend_save = 1; msg = "SAVING"; t_msg = fm1_ms; }
        rpt = (uint8_t)((fm1_in.buttons >> BTN_ARP) & 1u);
        if (b & (1u << BTN_HOME)) { t_home = fm1_ms; home_long = 0; }
        if (t_home && !home_long && (fm1_in.buttons >> BTN_HOME & 1u) && fm1_ms - t_home > 600u) {    /* held: panic and stop */
            home_long = 1; want_play = 0; push_ev(EV_PANIC, 0, 0); msg = "PANIC"; t_msg = fm1_ms;
        }
        if (rel & (1u << BTN_HOME)) { if (t_home && !home_long) { page = 0; redraw = 1; } t_home = 0; }

        for (k = 0; k < 4u; k++) {              /* the four knobs of the page */
            d = fm1_enc_take(ENC_KNOB1 + k);
            if (d) {
                volatile uint8_t *p = param_ptr(page, k, sel);
                hot = k;
                if (p) { int32_t v = (int32_t)*p + d * (d > 2 || d < -2 ? 6 : 2); *p = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
                else if (d > 0) mute_mask |= (uint8_t)(1u << sel);
                else mute_mask &= (uint8_t)~(1u << sel);
            }
        }
        d = fm1_enc_take(ENC_SELECT);
        if (d) { sel = (sel + NT + (d > 0 ? 1u : NT - 1u)) % NT; redraw = 1; banner_t = fm1_ms; }
        d = fm1_enc_take(ENC_ALGORITHM);
        if (d) { int32_t v = (int32_t)G[G_BPM] + d * (d > 2 || d < -2 ? 6 : 2); G[G_BPM] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
        d = fm1_enc_take(ENC_PRESETS);
        if (d) { load_kit((kit_n + 4u + (d > 0 ? 1u : 3u)) % 4u); msg = KITNAME[kit_n]; t_msg = fm1_ms; redraw = 1; }

        for (t = 0; t < NT; t++)
            if (trig_cnt[t] != seen[t]) { seen[t] = trig_cnt[t]; flash_until[t] = fm1_ms + 70u; }
        if (stepmode) {                         /* the 16 keys are the steps; the playhead inverts the one it is on */
            for (k = 0; k < NSTEP; k++) {
                uint32_t on = pat[cur_pat][sel][k] & 1u;
                if (on != (pl && seq_step == k ? 1u : 0u)) ledmask |= 1u << k;
            }
        } else
            for (t = 0; t < NT; t++) if (fm1_ms < flash_until[t]) ledmask |= 0x101u << t;
        ledmask |= 1u << (16u + cur_pat);
        if (want_pat != cur_pat && (fm1_ms >> 7 & 1u)) ledmask |= 1u << (16u + (want_pat & 7u));   /* the next pattern blinks */
        if (mute_mask) ledmask |= 1u << 24;
        if (solo_mask) ledmask |= 1u << 25;
        if (ledmask != led_drawn) { led_drawn = ledmask; for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), ledmask >> k & 1u); }
        fm1_led_key(BTN_PLAY, pl && (seq_step & 3u) == 0u);
        fm1_led_key(BTN_REC, rc);
        fm1_led_key(BTN_SEQ, stepmode);
        fm1_led_key(BTN_EDIT, page == 0u); fm1_led_key(BTN_ENV, page == 1u); fm1_led_key(BTN_FX, page == 2u); fm1_led_key(BTN_GLO, page == 3u);
        fm1_led_key(BTN_ARP, rpt);

        /* the screen: only what changed */
        ban = banner_t && fm1_ms - banner_t < 1400u ? sel + 1u : 0u;
        sig = patn | (pl ? 8u : 0u) | (rc ? 16u : 0u) | (uint32_t)G[G_BPM] << 8 | ban << 20;
        if (redraw) { for (k = 0; k < 4u; k++) shown[k] = 255; sig_drawn = ~0u; grid_sig = ~0u; head_drawn = ~0u; t_stat = fm1_ms - 1000u; redraw = 0; }
        if (sig != sig_drawn) { sig_drawn = sig; draw_title(patn, pl, rc, ban); }
        {
            uint32_t h = sel * 7u + mute_mask * 131u + solo_mask * 977u + cur_pat * 31u, s, tt;
            for (tt = 0; tt < NT; tt++) for (s = 0; s < NSTEP; s++) h = h * 33u + pat[cur_pat][tt][s];
            if (h != grid_sig) { grid_sig = h; draw_grid(0, sel); draw_grid(1, sel); }
        }
        if ((pl ? seq_step : 99u) != head_drawn) { head_drawn = pl ? seq_step : 99u; draw_head(seq_step, pl); }
        for (r = 0; r < 4u; r++) {
            uint32_t v = row_val(page, r, sel);
            if (v != shown[r]) { shown[r] = (uint8_t)v; draw_row(page, r, sel, hot); }
        }
        if (hot != hot_drawn) { hot_drawn = hot; for (r = 0; r < 4u; r++) shown[r] = 255; }
        if (msg && fm1_ms - t_msg > 1500u) { msg = 0; t_stat = fm1_ms - 1000u; }
        if (fm1_ms - t_stat >= 500u || msg) { if (fm1_ms - t_stat >= 250u) { t_stat = fm1_ms; draw_status(page, sel, stepmode, msg); } }

        if (pend_save && !playing) {            /* an erase stalls the sound: only when stopped */
            uint32_t p, j, s;
            for (p = 0; p < NPAT; p++) for (t = 0; t < NT; t++) for (s = 0; s < NSTEP; s++) pr.pat[p][t][s] = pat[p][t][s];
            for (t = 0; t < NT; t++) { for (j = 0; j < 4u; j++) pr.tp[t][j] = TP[t][j]; for (j = 0; j < 3u; j++) pr.mx[t][j] = MX[t][j]; }
            for (j = 0; j < NG; j++) pr.g[j] = G[j];
            pr.magic = PROJ_MAGIC; pr.cur = want_pat & 7u; pr.kit = (uint8_t)kit_n; pr.mode = (uint8_t)stepmode;
            project_save(&pr, sizeof pr);
            pend_save = 0; msg = "SAVED"; t_msg = fm1_ms; t_stat = fm1_ms - 1000u;
        }
    }
}
