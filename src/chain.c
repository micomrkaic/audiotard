/* This file is part of audiotard.
 *
 * audiotard -- calibrated audio distortions with blind listening tests
 * Copyright (C) 2026  Mico
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "chain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg)
{
    fprintf(stderr, "audiotard: %s\n", msg);
    exit(1);
}

void chain_defaults(chain_params *cp)
{
    memset(cp, 0, sizeof *cp);
    cp->wsp     = (ws_params){ .shape = WS_TUBE, .drive = 2.0, .bias = 0.2 };
    cp->h2db    = -30.0;
    cp->vp      = VINYL_DEFAULTS;
    cp->tp      = TAPE_DEFAULTS;
    cp->shp     = SHELLAC_DEFAULTS;
    cp->os      = 8;
    cp->gain_db = 0.0;
    cp->spk_zout = 1.0;        /* DF 8 -- typical push-pull tube amp   */
}

static int parse_eq(const char *s, eq_spec *e)
{
    char type[8] = {0};
    e->g = 0.0;
    if (sscanf(s, "%7[a-z]:%lf:%lf:%lf", type, &e->f, &e->Q, &e->g) < 3)
        return -1;
    if      (!strcmp(type, "peak")) e->t = BQ_PEAK;
    else if (!strcmp(type, "ls"))   e->t = BQ_LOWSHELF;
    else if (!strcmp(type, "hs"))   e->t = BQ_HIGHSHELF;
    else if (!strcmp(type, "lp"))   e->t = BQ_LOWPASS;
    else if (!strcmp(type, "hp"))   e->t = BQ_HIGHPASS;
    else if (!strcmp(type, "bp"))   e->t = BQ_BANDPASS;
    else return -1;
    return (e->Q > 0.0 && e->f > 0.0) ? 0 : -1;
}

int chain_parse(chain_params *cp, int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    const char *v = (*i + 1 < argc) ? argv[*i + 1] : NULL;

#define TAKE(dest) do { if (!v) die("missing value"); \
                        (dest) = atof(v); (*i)++; } while (0)

    if (!strcmp(a, "--shape") && v) {
        (*i)++;
        cp->use_shape = 1;
        if      (!strcmp(v, "none")) cp->use_shape = 0;
        else if (!strcmp(v, "tanh")) cp->wsp.shape = WS_TANH;
        else if (!strcmp(v, "tube")) cp->wsp.shape = WS_TUBE;
        else if (!strcmp(v, "h2"))   cp->wsp.shape = WS_H2;
        else die("unknown shape");
        return 1;
    }
    if (!strcmp(a, "--drive"))   { TAKE(cp->wsp.drive); return 1; }
    if (!strcmp(a, "--bias"))    { TAKE(cp->wsp.bias);  return 1; }
    if (!strcmp(a, "--h2db"))    { TAKE(cp->h2db);      return 1; }
    if (!strcmp(a, "--os"))      { if (!v) die("missing value");
                                   cp->os = atoi(v); (*i)++;
                                   if (cp->os < 1 || cp->os > 32)
                                       die("--os must be 1..32");
                                   return 1; }
    if (!strcmp(a, "--gain-in")) { TAKE(cp->gain_db);   return 1; }

    if (!strcmp(a, "--shellac")) {
        if (!v) die("missing value: --shellac acoustic|electric");
        if      (!strcmp(v, "acoustic")) cp->shp.era = 0;
        else if (!strcmp(v, "electric")) cp->shp.era = 1;
        else die("unknown shellac era (acoustic|electric)");
        cp->use_shellac = 1;
        if (cp->use_vinyl)
            die("--shellac and --vinyl are mutually exclusive (one "
                "disc, one format; --tape combines with either)");
        (*i)++;
        return 1;
    }
    if (!strcmp(a, "--spkload")) {
        if (!v) die("missing value: --spkload model:zout_ohms");
        char mn[16] = {0};
        double zo = 1.0;
        if (sscanf(v, "%15[a-z0-9]:%lf", mn, &zo) < 1)
            die("bad --spkload (want model:zout, e.g. reflex8:1.5)");
        if      (!strcmp(mn, "sealed8")) cp->spk_model = 1;
        else if (!strcmp(mn, "reflex8")) cp->spk_model = 2;
        else if (!strcmp(mn, "reflex4")) cp->spk_model = 3;
        else if (!strcmp(mn, "hard4"))   cp->spk_model = 4;
        else die("unknown speaker model (sealed8|reflex8|reflex4|hard4)");
        cp->spk_zout = zo;
        (*i)++;                        /* consumed the value token     */
        return 1;
    }
    if (!strcmp(a, "--vinyl"))   {
        cp->use_vinyl = 1;
        if (cp->use_shellac)
            die("--shellac and --vinyl are mutually exclusive (one "
                "disc, one format; --tape combines with either)");
        return 1;
    }
    if (!strcmp(a, "--tape"))    { cp->use_tape  = 1;   return 1; }

    if (!strcmp(a, "--wow-cents")) {
        if (!v) die("missing value");
        cp->vp.wow_cents = cp->tp.wow_cents = atof(v); (*i)++; return 1;
    }
    if (!strcmp(a, "--wow-rate")) {
        if (!v) die("missing value");
        cp->vp.wow_rate = cp->tp.wow_rate = atof(v); (*i)++; return 1;
    }
    if (!strcmp(a, "--drift-cents")) {
        if (!v) die("missing value");
        cp->vp.drift_cents = cp->tp.drift_cents = atof(v); (*i)++; return 1;
    }
    if (!strcmp(a, "--flutter-cents")) { TAKE(cp->tp.flutter_cents); return 1; }
    if (!strcmp(a, "--flutter-rate"))  { TAKE(cp->tp.flutter_rate);  return 1; }
    if (!strcmp(a, "--hiss-db")) {
        if (!v) die("missing value");
        cp->vp.hiss_db = cp->tp.hiss_db = atof(v); (*i)++; return 1;
    }
    if (!strcmp(a, "--crackle-rate")) { TAKE(cp->vp.crackle_per_s); return 1; }
    if (!strcmp(a, "--crackle-db"))   { TAKE(cp->vp.crackle_db);    return 1; }
    if (!strcmp(a, "--bump-db"))      { TAKE(cp->tp.bump_db);       return 1; }
    if (!strcmp(a, "--bump-hz"))      { TAKE(cp->tp.bump_hz);       return 1; }
    if (!strcmp(a, "--hf-loss"))      { TAKE(cp->tp.hf_loss);
                                        if (cp->tp.hf_loss < 0.0 ||
                                            cp->tp.hf_loss > 1.0)
                                            die("--hf-loss must be 0..1");
                                        return 1; }
    if (!strcmp(a, "--bw-hz")) {
        if (!v) die("missing value");
        cp->vp.lp_hz = cp->tp.lp_hz = atof(v); (*i)++; return 1;
    }

    if (!strcmp(a, "--eq")) {
        if (!v) die("missing value");
        (*i)++;
        if (cp->neq >= CHAIN_MAX_EQ) die("too many EQ bands");
        if (parse_eq(v, &cp->eq[cp->neq]) != 0)
            die("bad --eq spec (want type:freq:Q[:gain])");
        cp->neq++;
        return 1;
    }
#undef TAKE
    return 0;
}

int chain_render(const audio_buf *in, audio_buf *out,
                 const chain_params *cp_in, int match_rms)
{
    chain_params cp = *cp_in;             /* local: h2 calibration       */

    if (cp.use_shape && cp.wsp.shape == WS_H2) {
        double pk = audio_peak(in);
        if (pk <= 0.0) return -1;
        cp.wsp.h2 = ws_h2_coeff(cp.h2db, pk);
    }

    *out = *in;
    out->data = malloc(in->nframes * in->channels * sizeof *out->data);
    double *chan = malloc(in->nframes * sizeof *chan);
    double *tmp  = malloc(in->nframes * sizeof *tmp);
    if (!out->data || !chan || !tmp) {
        free(out->data); free(chan); free(tmp);
        return -1;
    }

    double g_in = pow(10.0, cp.gain_db / 20.0);

    for (unsigned c = 0; c < in->channels; c++) {
        for (size_t i = 0; i < in->nframes; i++)
            chan[i] = g_in * in->data[i * in->channels + c];
        if (cp.use_shellac && in->channels > 1)   /* a 78 is mono      */
            for (size_t i = 0; i < in->nframes; i++) {
                double s = 0.0;
                for (unsigned cc = 0; cc < in->channels; cc++)
                    s += in->data[i * in->channels + cc];
                chan[i] = g_in * s / in->channels;
            }

        if (cp.use_shape) {
            if (ws_process(chan, tmp, in->nframes, (double)in->rate,
                           cp.os, &cp.wsp))
                goto fail;
            memcpy(chan, tmp, in->nframes * sizeof *chan);
        }
        double t0 = (double)cp.pos0 / (double)in->rate;
        if (cp.use_shellac &&
            shellac_process(chan, in->nframes, (double)in->rate,
                            &cp.shp, t0))
            goto fail;
        if (cp.use_tape &&
            tape_process(chan, in->nframes, (double)in->rate, &cp.tp, c,
                         t0))
            goto fail;
        if (cp.use_vinyl &&
            vinyl_process(chan, in->nframes, (double)in->rate, &cp.vp, c,
                          t0))
            goto fail;
        for (int b = 0; b < cp.neq; b++) {
            biquad q;
            bq_design(&q, cp.eq[b].t, (double)in->rate,
                      cp.eq[b].f, cp.eq[b].Q, cp.eq[b].g);
            bq_process(&q, chan, in->nframes);
        }

        /* amp output impedance x speaker load: the amplifier-speaker
         * interface, so it is applied last                             */
        {
            eq_spec spk[SPK_MAX_SECTIONS];
            double mk_db = 0.0;
            int nspk = spk_sections(cp.spk_model, cp.spk_zout, spk,
                                    &mk_db);
            for (int s = 0; s < nspk; s++) {
                biquad q;
                bq_design(&q, spk[s].t, (double)in->rate,
                          spk[s].f, spk[s].Q, spk[s].g);
                bq_process(&q, chan, in->nframes);
            }
            if (nspk) {
                double mk = pow(10.0, mk_db / 20.0);
                for (size_t i = 0; i < in->nframes; i++) chan[i] *= mk;
            }
        }

        for (size_t i = 0; i < in->nframes; i++)
            out->data[i * in->channels + c] = chan[i];
    }
    free(chan);
    free(tmp);

    if (match_rms) {
        double r0 = audio_rms(in), r1 = audio_rms(out);
        if (r1 > 0.0) {
            double s = r0 / r1;
            size_t total = out->nframes * out->channels;
            for (size_t i = 0; i < total; i++) out->data[i] *= s;
        }
    }

    /* Headroom guard: modern masters peak at -0.1 dBFS; the h2 shape
     * (x + a*x^2 > 1 at peaks), added noise, and the RMS match can all
     * push past full scale, which hard-clips at the 16/24-bit write.
     * A global trim is transparent: pure gain, no waveshape change.   */
    if (!cp.no_trim) {
        double pk = audio_peak(out);
        if (pk > 0.999) {
            double s = 0.999 / pk;
            size_t total = out->nframes * out->channels;
            for (size_t i = 0; i < total; i++) out->data[i] *= s;
            fprintf(stderr, "audiotard: output trimmed %.2f dB to avoid "
                    "clipping (hot master + added harmonics)\n",
                    20.0 * log10(s));
        }
    }
    return 0;

fail:
    free(out->data); free(chan); free(tmp);
    return -1;
}

/* ====================================================================== */
/* Staircase parameter plumbing                                           */
/* ====================================================================== */

static const sc_info SC_TABLE[] = {
    { "h2db",          SC_H2DB,  1, -90.0,   0.0, "dB re fundamental" },
    { "hiss-db",       SC_HISS,  1, -110.0, -10.0, "dBFS"             },
    { "crackle-db",    SC_CRK,   1, -90.0,   0.0, "dBFS"              },
    { "wow-cents",     SC_WOW,   0, -26.0,  40.0, "cents"             },
    { "flutter-cents", SC_FLUT,  0, -26.0,  40.0, "cents"             },
    { "drive",         SC_DRIVE, 0,  -6.0,  30.0, "(drive)"           },
};

const sc_info *sc_find(const char *name)
{
    for (size_t i = 0; i < sizeof SC_TABLE / sizeof *SC_TABLE; i++)
        if (!strcmp(SC_TABLE[i].name, name)) return &SC_TABLE[i];
    return NULL;
}

const sc_info *sc_table(size_t *count)
{
    *count = sizeof SC_TABLE / sizeof *SC_TABLE;
    return SC_TABLE;
}

double sc_get(const chain_params *cp, sc_param id)
{
    switch (id) {
    case SC_H2DB:  return cp->h2db;
    case SC_HISS:  return cp->vp.hiss_db;
    case SC_CRK:   return cp->vp.crackle_db;
    case SC_WOW:   return 20.0 * log10(cp->use_tape ? cp->tp.wow_cents
                                                    : cp->vp.wow_cents);
    case SC_FLUT:  return 20.0 * log10(cp->tp.flutter_cents);
    case SC_DRIVE: return 20.0 * log10(cp->wsp.drive);
    default:       return 0.0;
    }
}

void sc_set(chain_params *cp, sc_param id, double s)
{
    double v = pow(10.0, s / 20.0);
    switch (id) {
    case SC_H2DB:  cp->h2db = s; break;
    case SC_HISS:  cp->vp.hiss_db = cp->tp.hiss_db = s; break;
    case SC_CRK:   cp->vp.crackle_db = s; break;
    case SC_WOW:   cp->vp.wow_cents = cp->tp.wow_cents = v; break;
    case SC_FLUT:  cp->tp.flutter_cents = v; break;
    case SC_DRIVE: cp->wsp.drive = v; break;
    default: break;
    }
}

const char *sc_check_enabled(const chain_params *cp, sc_param id)
{
    switch (id) {
    case SC_H2DB:
        if (!cp->use_shape || cp->wsp.shape != WS_H2)
            return "staircase on h2db needs the h2 shape enabled";
        return NULL;
    case SC_DRIVE:
        if (!cp->use_shape)
            return "staircase on drive needs the tanh or tube shape";
        return NULL;
    case SC_HISS: case SC_WOW:
        if (!cp->use_vinyl && !cp->use_tape)
            return "this staircase parameter needs vinyl or tape enabled";
        return NULL;
    case SC_CRK:
        if (!cp->use_vinyl)
            return "staircase on crackle-db needs vinyl enabled";
        return NULL;
    case SC_FLUT:
        if (!cp->use_tape)
            return "staircase on flutter-cents needs tape enabled";
        return NULL;
    default:
        return "unknown staircase parameter";
    }
}

void sc_print_value(const sc_info *si, double s, char *buf, size_t n)
{
    if (si->is_db) snprintf(buf, n, "%.1f %s", s, si->unit);
    else           snprintf(buf, n, "%.2f %s", pow(10.0, s / 20.0), si->unit);
}

/* Parametric loudspeaker impedance models: voice-coil Re + Le plus up
 * to two motional resonances (parallel-RLC humps). Sealed: one hump.
 * Bass-reflex: twin humps straddling the port tuning. "Difficult":
 * low Re, big Le, deep humps -- the load that makes tube amps audible. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double f0, rp, q; } spk_res;
typedef struct {
    double  re, le;
    spk_res r[2];
    int     nres;
} spk_model_t;

static const spk_model_t SPK[] = {
    { 0,   0,      {{0,0,0},{0,0,0}},               0 },  /* off      */
    { 6.4, 0.6e-3, {{55, 34, 3.0},{0,0,0}},         1 },  /* sealed8  */
    { 6.4, 0.7e-3, {{28, 24, 4.0},{68, 28, 4.0}},   2 },  /* reflex8  */
    { 3.2, 0.4e-3, {{25, 14, 4.0},{62, 16, 4.0}},   2 },  /* reflex4  */
    { 2.9, 0.9e-3, {{24, 12, 5.0},{55, 14, 5.0}},   2 },  /* hard4    */
};

/* |H(f)| = |Z/(Z+Zout)| from the complex impedance                    */
static double spk_h(const spk_model_t *m, double zout, double f)
{
    double zr = m->re, zi = 2.0 * M_PI * f * m->le;
    for (int k = 0; k < m->nres; k++) {
        double d   = f / m->r[k].f0 - m->r[k].f0 / f;
        double qd  = m->r[k].q * d;
        double den = 1.0 + qd * qd;
        zr += m->r[k].rp / den;
        zi -= m->r[k].rp * qd / den;
    }
    double dr = zr + zout;
    return sqrt((zr * zr + zi * zi) / (dr * dr + zi * zi));
}

/* |H| of a designed biquad at frequency f (z-transform on the unit
 * circle) -- lets us anchor the shelf exactly                          */
static double bq_mag_at(const eq_spec *e, double fs, double f)
{
    biquad q;
    bq_design(&q, e->t, fs, e->f, e->Q, e->g);
    double w = 2.0 * M_PI * f / fs;
    double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = q.b0 + q.b1 * c1 + q.b2 * c2;
    double ni = -(q.b1 * s1 + q.b2 * s2);
    double dr = 1.0 + q.a1 * c1 + q.a2 * c2;
    double di = -(q.a1 * s1 + q.a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

int spk_sections(int model, double zout, eq_spec *out, double *makeup_db)
{
    *makeup_db = 0.0;
    if (model <= 0 || model > 4 || zout <= 0.001) return 0;
    const spk_model_t *m = &SPK[model];
    double h1k = spk_h(m, zout, 1000.0);

    /* target curve on a log grid */
    enum { NF = 80 };
    double fg[NF], tg[NF];
    for (int i = 0; i < NF; i++) {
        fg[i] = 22.0 * pow(21000.0 / 22.0, (double)i / (NF - 1));
        tg[i] = 20.0 * log10(spk_h(m, zout, fg[i]) / h1k);
    }

    /* basis layout: resonance peaks, midpoint correctors, shelves     */
    eq_spec b[SPK_MAX_SECTIONS];
    int nb = 0;
    b[nb++] = (eq_spec){ BQ_LOWSHELF, 34.0, 0.7071, 1.0 };
    for (int k = 0; k < m->nres; k++)
        b[nb++] = (eq_spec){ BQ_PEAK, m->r[k].f0, m->r[k].q, 1.0 };
    if (m->nres == 2)
        b[nb++] = (eq_spec){ BQ_PEAK,
                             sqrt(m->r[0].f0 * m->r[1].f0), 2.2, 1.0 };
    double flast = m->nres ? m->r[m->nres - 1].f0 : 60.0;
    b[nb++] = (eq_spec){ BQ_PEAK, sqrt(flast * 400.0), 1.0, 1.0 };
    b[nb++] = (eq_spec){ BQ_PEAK, 400.0, 0.9, 1.0 };
    double fsh = (m->re + zout) / (2.0 * M_PI * m->le);
    b[nb++] = (eq_spec){ BQ_HIGHSHELF, fsh, 0.7071, 1.0 };

    /* two rounds: fit gains by least squares on unit-gain dB shapes
     * (cascade dB adds exactly; shape-vs-gain is near-linear here)    */
    double g[SPK_MAX_SECTIONS] = { 0 };
    for (int round = 0; round < 2; round++) {
        double res[NF];
        for (int i = 0; i < NF; i++) {
            double have = 0.0;
            for (int j = 0; j < nb; j++) {
                if (fabs(g[j]) < 1e-9) continue;
                eq_spec e = b[j]; e.g = g[j];
                have += 20.0 * log10(bq_mag_at(&e, 96000.0, fg[i]));
            }
            res[i] = tg[i] - have;
        }
        double S[SPK_MAX_SECTIONS][NF];
        for (int j = 0; j < nb; j++)
            for (int i = 0; i < NF; i++)
                S[j][i] = 20.0 * log10(bq_mag_at(&b[j], 96000.0, fg[i]));
        double A[SPK_MAX_SECTIONS][SPK_MAX_SECTIONS + 1];
        for (int j = 0; j < nb; j++) {
            for (int k = 0; k < nb; k++) {
                double s = 0;
                for (int i = 0; i < NF; i++) s += S[j][i] * S[k][i];
                A[j][k] = s + (j == k ? 1e-6 : 0.0);
            }
            double s = 0;
            for (int i = 0; i < NF; i++) s += S[j][i] * res[i];
            A[j][nb] = s;
        }
        for (int p = 0; p < nb; p++) {          /* Gauss elimination   */
            int piv = p;
            for (int r = p + 1; r < nb; r++)
                if (fabs(A[r][p]) > fabs(A[piv][p])) piv = r;
            for (int k = 0; k <= nb; k++) {
                double tswap = A[p][k]; A[p][k] = A[piv][k];
                A[piv][k] = tswap;
            }
            for (int r = p + 1; r < nb; r++) {
                double f = A[r][p] / A[p][p];
                for (int k = p; k <= nb; k++) A[r][k] -= f * A[p][k];
            }
        }
        for (int p = nb - 1; p >= 0; p--) {
            double s = A[p][nb];
            for (int k = p + 1; k < nb; k++) s -= A[p][k] * (g[k] - 0.0);
            /* solve for delta, accumulate */
            double delta = s;
            for (int k = p + 1; k < nb; k++) ;
            (void)delta;
            double d = A[p][nb];
            for (int k = p + 1; k < nb; k++) d -= A[p][k] * A[k][SPK_MAX_SECTIONS];
            A[p][SPK_MAX_SECTIONS] = d / A[p][p];
        }
        for (int j = 0; j < nb; j++) g[j] += A[j][SPK_MAX_SECTIONS];
    }

    /* exact 0 dB at 1 kHz via makeup */
    double at1k = 0.0;
    for (int j = 0; j < nb; j++) {
        if (fabs(g[j]) < 1e-9) continue;
        eq_spec e = b[j]; e.g = g[j];
        at1k += 20.0 * log10(bq_mag_at(&e, 96000.0, 1000.0));
    }
    *makeup_db = -at1k;

    int n = 0;
    for (int j = 0; j < nb; j++)
        if (fabs(g[j]) > 0.01) {
            out[n] = b[j];
            out[n].g = g[j];
            n++;
        }
    return n;
}

void sc_isolate(chain_params *cp, sc_param id)
{
    cp->neq = 0;
    cp->spk_model = 0;
    cp->use_shellac = 0;
    switch (id) {
    case SC_H2DB:
    case SC_DRIVE:
        cp->use_vinyl = cp->use_tape = 0;
        break;
    case SC_HISS:
        cp->use_shape = 0;
        if (cp->use_vinyl && cp->use_tape) cp->use_vinyl = 0;
        cp->vp.wow_cents = cp->vp.drift_cents = 0.0;
        cp->vp.crackle_per_s = 0.0;
        cp->vp.hp_hz = 10.0;
        cp->vp.lp_hz = 20000.0;
        cp->tp.wow_cents = cp->tp.flutter_cents = cp->tp.drift_cents = 0.0;
        cp->tp.bump_db = 0.0;
        cp->tp.hf_loss = 0.0;
        cp->tp.lp_hz = 20000.0;
        break;
    case SC_CRK:
        cp->use_shape = 0;
        cp->use_tape  = 0;
        cp->vp.wow_cents = cp->vp.drift_cents = 0.0;
        cp->vp.hiss_db = -150.0;
        cp->vp.hp_hz = 10.0;
        cp->vp.lp_hz = 20000.0;
        break;
    case SC_WOW:
        cp->use_shape = 0;
        if (cp->use_vinyl && cp->use_tape) cp->use_tape = 0;
        cp->vp.drift_cents = 0.0;
        cp->vp.crackle_per_s = 0.0;
        cp->vp.hiss_db = -150.0;
        cp->vp.hp_hz = 10.0;
        cp->vp.lp_hz = 20000.0;
        cp->tp.flutter_cents = cp->tp.drift_cents = 0.0;
        cp->tp.hiss_db = -150.0;
        cp->tp.bump_db = 0.0;
        cp->tp.hf_loss = 0.0;
        cp->tp.lp_hz = 20000.0;
        break;
    case SC_FLUT:
        cp->use_shape = 0;
        cp->use_vinyl = 0;
        cp->tp.wow_cents = cp->tp.drift_cents = 0.0;
        cp->tp.hiss_db = -150.0;
        cp->tp.bump_db = 0.0;
        cp->tp.hf_loss = 0.0;
        cp->tp.lp_hz = 20000.0;
        break;
    default:
        break;
    }
}

double binom_p(int k, int n)
{
    double p = 0.0;
    for (int i = k; i <= n; i++)
        p += exp(lgamma(n + 1.0) - lgamma(i + 1.0) - lgamma(n - i + 1.0)
                 - n * log(2.0));
    return p > 1.0 ? 1.0 : p;
}
