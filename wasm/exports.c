/* This file is part of audiotard.  Copyright (C) 2026 Mico.
 * GPL-3.0-or-later. See COPYING.
 *
 * WebAssembly surface: the whole DSP core behind one render call.
 * Built with clang --target=wasm32-wasi (no emscripten needed).      */
#include "../src/chain.h"
#include <stdlib.h>
#include <string.h>

/* audio_io.c (file I/O) is not linked in the browser build; provide
 * the one symbol the chain uses from it.                              */
void audio_free(audio_buf *b)
{
    free(b->data);
    b->data = NULL;
}

double audio_rms(const audio_buf *b)
{
    if (!b->data || b->nframes == 0) return 0.0;
    double s = 0.0;
    size_t n = b->nframes * b->channels;
    for (size_t i = 0; i < n; i++) s += b->data[i] * b->data[i];
    return __builtin_sqrt(s / (double)n);
}

double audio_peak(const audio_buf *b)
{
    double pk = 0.0;
    size_t n = b->nframes * b->channels;
    for (size_t i = 0; i < n; i++) {
        double v = b->data[i] < 0 ? -b->data[i] : b->data[i];
        if (v > pk) pk = v;
    }
    return pk;
}

__attribute__((export_name("at_version")))
int at_version(void) { return 600; }   /* 0.5.0 -> maj*10000+min*100+p */

__attribute__((export_name("at_alloc")))
double *at_alloc(int n) { return malloc((size_t)n * sizeof(double)); }

__attribute__((export_name("at_free")))
void at_free(double *p) { free(p); }

static audio_buf g_out;

/* Render 'frames' interleaved frames through the chain. Returns a
 * pointer to frames*ch doubles (valid until the next call), or 0.     */
__attribute__((export_name("at_render")))
double *at_render(double *in, int frames, int ch, int rate,
                  int shape, double drive, double bias, double h2db,
                  int os, int vinyl, int tape, double wow,
                  double flutter, double hiss, double crk_rate,
                  double crk_db, double hf_loss, double bump_db,
                  double bw_hz, int match_rms, int pos0)
{
    audio_buf ib = { .data = in, .nframes = (size_t)frames,
                     .channels = (unsigned)ch, .rate = (unsigned)rate };
    chain_params cp;
    chain_defaults(&cp);
    cp.use_shape = shape > 0;
    if (shape == 1) cp.wsp.shape = WS_TANH;
    if (shape == 2) cp.wsp.shape = WS_TUBE;
    if (shape == 3) cp.wsp.shape = WS_H2;
    cp.wsp.drive = drive;
    cp.wsp.bias  = bias;
    cp.h2db      = h2db;
    cp.os        = os;
    cp.use_vinyl = vinyl;
    cp.use_tape  = tape;
    cp.vp.wow_cents = cp.tp.wow_cents = wow;
    cp.tp.flutter_cents = flutter;
    cp.vp.hiss_db = cp.tp.hiss_db = hiss;
    cp.vp.crackle_per_s = crk_rate;
    cp.vp.crackle_db    = crk_db;
    cp.tp.hf_loss = hf_loss;
    cp.tp.bump_db = bump_db;
    cp.vp.lp_hz = cp.tp.lp_hz = bw_hz;
    cp.pos0 = (size_t)pos0;
    /* Random drift cannot stay continuous across independently rendered
     * streaming blocks (it is a random walk); wow + flutter carry the
     * audible pitch character and ARE phase-continuous via pos0.       */
    cp.vp.drift_cents = cp.tp.drift_cents = 0.0;

    if (g_out.data) audio_free(&g_out);
    if (chain_render(&ib, &g_out, &cp, match_rms) != 0) return 0;
    return g_out.data;
}
