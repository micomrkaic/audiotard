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
    cp->os      = 8;
    cp->gain_db = 0.0;
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

    if (!strcmp(a, "--vinyl"))   { cp->use_vinyl = 1;   return 1; }
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

        if (cp.use_shape) {
            if (ws_process(chan, tmp, in->nframes, (double)in->rate,
                           cp.os, &cp.wsp))
                goto fail;
            memcpy(chan, tmp, in->nframes * sizeof *chan);
        }
        double t0 = (double)cp.pos0 / (double)in->rate;
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

void sc_isolate(chain_params *cp, sc_param id)
{
    cp->neq = 0;
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
