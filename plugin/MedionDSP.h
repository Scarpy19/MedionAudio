/*
 * MedionDSP.h — output processing of the HAL plugin (48 kHz stereo, float).
 *
 * Both profiles are deliberately flat, matching the Linux SOF pipeline:
 *   volume -> look-ahead peak limiter (speakers -1.0 dBFS, headphones -0.5 dBFS)
 * No EQ, no virtual bass and no bass limiter: the MEDION speakers are driven
 * through the external amp and are left as the codec/DAC deliver them.
 *
 * All state is static/preallocated: no malloc and no lock on the IO thread.
 */
#ifndef MEDION_DSP_H
#define MEDION_DSP_H

#include <math.h>
#include <string.h>

#define MBDSP_SR            48000.0
#define MBDSP_LA            120          /* limiter look-ahead: 2.5 ms */

#define MBDSP_SPK_CEIL_DB   (-1.0)
#define MBDSP_HP_CEIL_DB    (-0.5)

/* Linear amplitude curve: usable low levels without changing maximum. */
static inline float MBVol_ScalarToDB(float s) {
    if (s <= 0.0f) return -60.0f;
    float d = 20.0f * log10f(s);
    return d < -60.0f ? -60.0f : (d > 0.0f ? 0.0f : d);
}
static inline float MBVol_DBToScalar(float d) {
    if (d <= -60.0f) return 0.0f;
    if (d >= 0.0f) return 1.0f;
    return powf(10.0f, d / 20.0f);
}
static inline float MBVol_Gain(float s) { return s > 0.0f ? s : 0.0f; }

/* ---- look-ahead peak limiter (linked stereo, guaranteed no overshoot) ---- */
typedef struct {
    float  dl[2][MBDSP_LA];        /* audio delay line */
    float  req[MBDSP_LA];          /* required gain (window) */
    int    dq[MBDSP_LA];           /* deque indices for sliding min */
    int    dqh, dqn;
    float  box[MBDSP_LA];          /* boxcar over released gain */
    double boxSum;
    float  rel;                    /* gain after release */
    int    pos;
    float  ceil, relCoef;
} MBLimiter;

static void MBLim_Init(MBLimiter *l, double ceilDb, double releaseMs) {
    memset(l, 0, sizeof(*l));
    l->ceil = (float)pow(10.0, ceilDb / 20.0);
    l->relCoef = (float)(1.0 - exp(-1.0 / (releaseMs * 0.001 * MBDSP_SR)));
    l->rel = 1.0f;
    for (int i = 0; i < MBDSP_LA; i++) { l->req[i] = 1.0f; l->box[i] = 1.0f; }
    l->boxSum = MBDSP_LA;
}

static inline void MBLim_Run(MBLimiter *l, float *L, float *R) {
    const int N = MBDSP_LA, p = l->pos;
    float pk = fmaxf(fabsf(*L), fabsf(*R));
    float r = pk > l->ceil ? l->ceil / pk : 1.0f;
    l->req[p] = r;
    /* sliding min over N samples (monotonic deque, circular indices) */
    while (l->dqn > 0) {
        int last = l->dq[(l->dqh + l->dqn - 1) % N];
        if (l->req[last] >= r) l->dqn--; else break;
    }
    if (l->dqn > 0 && l->dq[l->dqh] == p) { l->dqh = (l->dqh + 1) % N; l->dqn--; }
    l->dq[(l->dqh + l->dqn) % N] = p; l->dqn++;
    float hold = l->req[l->dq[l->dqh]];
    /* exponential release, instant attack on the held value */
    if (hold < l->rel) l->rel = hold; else l->rel += (hold - l->rel) * l->relCoef;
    /* N-tap boxcar -> smooth attack ramp */
    l->boxSum += l->rel - l->box[p]; l->box[p] = l->rel;
    float g = (float)(l->boxSum / N);
    /* output delayed by N-1: the oldest sample in the window */
    int o = (p + 1) % N;
    float oL = l->dl[0][o], oR = l->dl[1][o];
    l->dl[0][p] = *L; l->dl[1][p] = *R;
    /* the element leaving the window on the next step */
    if (l->dqn > 0 && l->dq[l->dqh] == o) { l->dqh = (l->dqh + 1) % N; l->dqn--; }
    l->pos = o;
    *L = oL * g; *R = oR * g;
}

/* ---- global state ---- */
typedef struct {
    MBLimiter limSpk, limHp;
    int       profile;          /* 0 = speakers, 1 = headphones */
    int       ready;
    float     currentGain;
} MBDSP;

static void MBDSP_Init(MBDSP *d) {
    memset(d, 0, sizeof(*d));
    MBLim_Init(&d->limSpk, MBDSP_SPK_CEIL_DB, 120.0);
    MBLim_Init(&d->limHp, MBDSP_HP_CEIL_DB, 80.0);
    d->ready = 1;
}

/* Process n interleaved frames in -> out (full-scale S32). */
static void MBDSP_Process(MBDSP *d, const float *in, int32_t *out, unsigned n, float gain, int profile) {
    if (!d->ready) MBDSP_Init(d);
    if (profile != d->profile) {           /* output changed: start clean */
        int keep = profile; MBDSP_Init(d); d->profile = keep;
    }
    MBLimiter *lim = (profile == 0) ? &d->limSpk : &d->limHp;
    for (unsigned i = 0; i < n; i++) {
        /* Smooth volume changes and fade in on start/profile switches. */
        d->currentGain += (gain - d->currentGain) * 0.002f;
        float L = in[2 * i] * d->currentGain, R = in[2 * i + 1] * d->currentGain;
        MBLim_Run(lim, &L, &R);
        if (L > 1.0f) L = 1.0f; if (L < -1.0f) L = -1.0f;
        if (R > 1.0f) R = 1.0f; if (R < -1.0f) R = -1.0f;
        out[2 * i]     = (int32_t)(L * 2147483647.0f);
        out[2 * i + 1] = (int32_t)(R * 2147483647.0f);
    }
}

#endif
