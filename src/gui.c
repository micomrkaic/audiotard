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

/* audiotard GUI v2.3 (GTK3 + ALSA).
 *
 * v2.3:
 *  - two-column Chain tab inside scrolled windows: the window's minimum
 *    height no longer exceeds small screens, and maximize works (the
 *    waveform/spectrum panes absorb the extra space)
 *  - real-time RESIDUAL metric during processed playback: the clean
 *    source is sample-aligned with the processed buffer, so each frame
 *    we fit an optimal gain g and report rms(proc - g*clean)/rms(g*clean)
 *    in dB -- everything the chain added (harmonics, noise, wow
 *    sidebands). On a sine passage this equals THD+N. The clean spectrum
 *    is overlaid in blue in the FFT display.
 *  - parametric filters: high-pass, low-pass, band-pass, and a
 *    band window (HP at f_lo + LP at f_hi), all RBJ biquads appended to
 *    the EQ chain
 *  - tooltips documenting Drive / Bias / H2 semantics and units
 *
 * Build:  make gui     (needs libgtk-3-dev libasound2-dev)
 */
#include "audio_io.h"
#include "chain.h"
#include "fft.h"
#include "playback.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define FFT_N          4096
#define TEST_MAX_SECS  30.0

/* live streaming: block, pre-roll context, crossfade, history */
#define LIVE_B   4096              /*  93 ms                    */
#define LIVE_PR  16384             /* 371 ms filter settling    */
#define LIVE_X    512              /*  12 ms seam crossfade     */
#define LIVE_HIST (1u << 17)       /* ~3 s rendered history     */
#define LIVE_MAP  128

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* spectrum display range and bottom inset (label strip): clamped floor
 * values draw INSIDE the panel instead of riding the widget edge       */
#define SPEC_DB_HI    0.0
#define SPEC_DB_LO (-120.0)
#define SPEC_INSET   12

typedef struct App App;

/* one smoothing state per displayed trace */
typedef struct {
    double sm[FFT_N / 2];
    double ring[32][FFT_N / 2];
    int    head, fill, init, last_mode, last_amt;
} smst;

struct App {
    GtkWidget *win, *in_chooser, *status;
    GtkWidget *wf, *fftw, *shaper_view;
    GtkWidget *fscale_combo, *smooth_combo, *smooth_spin, *metric_label;
    GtkWidget *btn_pause, *sel_label;
    GtkWidget *shape_combo, *os_combo, *drive, *bias, *h2db;
    GtkWidget *vinyl_chk, *tape_chk;
    GtkWidget *wow, *flutter, *hiss, *crk_rate, *crk_db, *hf_loss,
              *bump, *bw;
    GtkWidget *hp_chk, *hp_f, *hp_q;
    GtkWidget *lp_chk, *lp_f, *lp_q;
    GtkWidget *bp_chk, *bp_f, *bp_q;
    GtkWidget *win_chk, *win_lo, *win_hi;
    GtkWidget *bits_combo, *match_chk, *selonly_chk, *render_btn;
    GtkWidget *abx_mode, *abx_trials, *abx_abs, *abx_start, *abx_end,
              *abx_status, *abx_result;
    GtkWidget *btn_p1, *btn_p2, *btn_px, *btn_a1, *btn_a2, *abx_help;

    char      *in_path_loaded;
    audio_buf  in_full;
    uint64_t   wf_gen;
    double    *pkmin, *pkmax;
    int        pk_w;
    uint64_t   pk_gen;
    size_t     sel_a, sel_b;
    int        have_sel, dragging;
    double     drag_x0;
    size_t     cursor;

    player    *pl;
    const audio_buf *playing;
    size_t     region_start;
    int        playing_is_view;
    audio_buf  proc_buf;

    /* live streaming state (producer thread <-> GUI via live_mx) */
    GThread   *live_th;
    GMutex     live_mx;
    int        live_active;
    int        live_stop;
    long       live_seek;                     /* -1 = none             */
    chain_params live_cp;                     /* producer's params     */
    chain_params live_cp_snap;                /* GUI-side change check */
    size_t     live_r0, live_r1;
    double    *live_hist;                     /* LIVE_HIST * ch        */
    struct { uint64_t s; size_t f, n; } live_map[LIVE_MAP];
    int        live_map_n;

    double     spec[FFT_N / 2];               /* display dB            */
    double     pow_cur[FFT_N / 2];
    smst       sm_main, sm_clean;             /* per-trace smoothing   */
    int        spec_valid;
    unsigned   spec_rate;
    double     clean_db[FFT_N / 2];           /* overlay               */
    int        overlay_valid;

    uint64_t   rng;
    int        render_busy;

    int        sess_active;
    audio_buf  ex_clean, ex_proc;
    chain_params sess_cp;
    const audio_buf *x_buf;
    int        trials, t, k, n, proc_is_2;   /* in absolute mode,
                    proc_is_2 means "the single stimulus is processed" */
    int        sess_abs;
    const sc_info *si;
    double     s, step;
    int        dir, ncorr, nrev;
    double     revs[16];

    /* per-trial record for the end-of-session report */
    struct { int stim, ans, correct; double s; } tlog[220];
    int        ntl;
    GtkWidget *abx_log_view;
};

static double frand(uint64_t *st)
{
    uint64_t x = *st;
    x ^= x >> 12;  x ^= x << 25;  x ^= x >> 27;
    *st = x;
    return (double)((x * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

static void set_status(GtkWidget *lbl, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(lbl), buf);
}

/* ---------------------------------------------------------------------- */
/* Params, input, regions                                                 */
/* ---------------------------------------------------------------------- */

static double sv(GtkWidget *scale)
{ return gtk_range_get_value(GTK_RANGE(scale)); }

static double spv(GtkWidget *spin)
{ return gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)); }

static int chk(GtkWidget *c)
{ return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c)); }

static void collect_params(App *a, chain_params *cp)
{
    chain_defaults(cp);
    int sh = gtk_combo_box_get_active(GTK_COMBO_BOX(a->shape_combo));
    cp->use_shape = (sh > 0);
    if (sh == 1) cp->wsp.shape = WS_TANH;
    if (sh == 2) cp->wsp.shape = WS_TUBE;
    if (sh == 3) cp->wsp.shape = WS_H2;
    cp->os = 2 << gtk_combo_box_get_active(GTK_COMBO_BOX(a->os_combo));
    cp->wsp.drive = sv(a->drive);
    cp->wsp.bias  = sv(a->bias);
    cp->h2db      = sv(a->h2db);
    cp->use_vinyl = chk(a->vinyl_chk);
    cp->use_tape  = chk(a->tape_chk);
    cp->vp.wow_cents = cp->tp.wow_cents = sv(a->wow);
    cp->tp.flutter_cents = sv(a->flutter);
    cp->vp.hiss_db = cp->tp.hiss_db = sv(a->hiss);
    cp->vp.crackle_per_s = sv(a->crk_rate);
    cp->vp.crackle_db = sv(a->crk_db);
    cp->tp.hf_loss = sv(a->hf_loss);
    cp->tp.bump_db = sv(a->bump);
    cp->vp.lp_hz = cp->tp.lp_hz = sv(a->bw);

    /* parametric filters -> EQ chain (applied after media stages) */
    cp->neq = 0;
    if (chk(a->hp_chk))
        cp->eq[cp->neq++] = (eq_spec){ BQ_HIGHPASS, spv(a->hp_f),
                                       spv(a->hp_q), 0.0 };
    if (chk(a->lp_chk))
        cp->eq[cp->neq++] = (eq_spec){ BQ_LOWPASS, spv(a->lp_f),
                                       spv(a->lp_q), 0.0 };
    if (chk(a->bp_chk))
        cp->eq[cp->neq++] = (eq_spec){ BQ_BANDPASS, spv(a->bp_f),
                                       spv(a->bp_q), 0.0 };
    if (chk(a->win_chk)) {
        double lo = spv(a->win_lo), hi = spv(a->win_hi);
        if (hi < lo * 1.05) hi = lo * 1.05;
        cp->eq[cp->neq++] = (eq_spec){ BQ_HIGHPASS, lo, 0.7071, 0.0 };
        cp->eq[cp->neq++] = (eq_spec){ BQ_LOWPASS,  hi, 0.7071, 0.0 };
    }
}

static int chain_enabled(const chain_params *cp)
{
    return cp->use_shape || cp->use_vinyl || cp->use_tape || cp->neq > 0;
}

static void live_stop(App *a);

static void stop_all(App *a)
{
    live_stop(a);
    player_stop(a->pl);
    a->playing = NULL;
    a->overlay_valid = 0;
    gtk_label_set_text(GTK_LABEL(a->metric_label), "");
}

static int load_input(App *a)
{
    if (a->render_busy) {
        set_status(a->status, "busy rendering -- try again in a moment");
        return -1;
    }
    gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(a->in_chooser));
    if (!path) { set_status(a->status, "pick an input file first"); return -1; }
    if (a->in_path_loaded && !strcmp(a->in_path_loaded, path)) {
        g_free(path);
        return 0;
    }
    audio_buf nb;
    if (audio_read(path, &nb) != 0) {
        set_status(a->status, "cannot read %s", path);
        g_free(path);
        return -1;
    }
    stop_all(a);
    if (a->in_full.data) audio_free(&a->in_full);
    if (a->proc_buf.data) audio_free(&a->proc_buf);
    a->in_full = nb;
    g_free(a->in_path_loaded);
    a->in_path_loaded = g_strdup(path);
    g_free(path);
    a->wf_gen++;
    a->have_sel = 0;
    a->cursor = 0;
    a->spec_valid = 0;
    set_status(a->status, "loaded: %.1f s, %u ch, %u Hz",
               (double)a->in_full.nframes / a->in_full.rate,
               a->in_full.channels, a->in_full.rate);
    gtk_widget_queue_draw(a->wf);
    gtk_widget_queue_draw(a->fftw);
    return 0;
}

static void get_region(const App *a, size_t *r0, size_t *r1)
{
    if (a->have_sel && a->sel_b > a->sel_a) {
        *r0 = a->sel_a;
        *r1 = a->sel_b;
    } else {
        *r0 = a->cursor;
        *r1 = a->in_full.nframes;
    }
}

static int slice_view(const App *a, size_t r0, size_t r1, audio_buf *out)
{
    *out = a->in_full;
    out->nframes = r1 - r0;
    size_t total = out->nframes * out->channels;
    out->data = malloc(total * sizeof *out->data);
    if (!out->data) return -1;
    memcpy(out->data, a->in_full.data + r0 * a->in_full.channels,
           total * sizeof *out->data);
    return 0;
}

static void update_sel_label(App *a)
{
    if (!a->in_full.data) {
        gtk_label_set_text(GTK_LABEL(a->sel_label), "");
        return;
    }
    double fs = a->in_full.rate;
    if (a->have_sel)
        set_status(a->sel_label, "selection: %.2f s .. %.2f s  (%.2f s)   "
                   "cursor: %.2f s",
                   a->sel_a / fs, a->sel_b / fs,
                   (a->sel_b - a->sel_a) / fs, a->cursor / fs);
    else
        set_status(a->sel_label, "no selection (click-drag on the "
                   "waveform)   cursor: %.2f s", a->cursor / fs);
}

/* ---------------------------------------------------------------------- */
/* Waveform widget                                                        */
/* ---------------------------------------------------------------------- */

static void peaks_ensure(App *a, int w)
{
    if (a->pk_w == w && a->pk_gen == a->wf_gen) return;
    free(a->pkmin); free(a->pkmax);
    a->pkmin = calloc((size_t)w, sizeof *a->pkmin);
    a->pkmax = calloc((size_t)w, sizeof *a->pkmax);
    a->pk_w = w;
    a->pk_gen = a->wf_gen;
    if (!a->in_full.data) return;
    size_t n = a->in_full.nframes;
    unsigned ch = a->in_full.channels;
    for (int x = 0; x < w; x++) {
        size_t f0 = (size_t)((double)x / w * n);
        size_t f1 = (size_t)((double)(x + 1) / w * n);
        if (f1 <= f0) f1 = f0 + 1;
        if (f1 > n) f1 = n;
        double lo = 0.0, hi = 0.0;
        for (size_t f = f0; f < f1; f++)
            for (unsigned c = 0; c < ch; c++) {
                double v = a->in_full.data[f * ch + c];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        a->pkmin[x] = lo;
        a->pkmax[x] = hi;
    }
}

static size_t px_to_frame(const App *a, double x, int w)
{
    if (!a->in_full.data || w <= 0) return 0;
    double f = x / w * (double)a->in_full.nframes;
    if (f < 0) f = 0;
    if (f > (double)a->in_full.nframes) f = (double)a->in_full.nframes;
    return (size_t)f;
}

static gboolean wf_draw(GtkWidget *wd, cairo_t *cr, gpointer u)
{
    App *a = u;
    int w = gtk_widget_get_allocated_width(wd);
    int h = gtk_widget_get_allocated_height(wd);

    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);
    if (!a->in_full.data) {
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, 10, h / 2);
        cairo_show_text(cr, "no file loaded");
        return FALSE;
    }
    peaks_ensure(a, w);

    if (a->have_sel) {
        double x0 = (double)a->sel_a / a->in_full.nframes * w;
        double x1 = (double)a->sel_b / a->in_full.nframes * w;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
        cairo_rectangle(cr, x0, 0, x1 - x0, h);
        cairo_fill(cr);
    }

    cairo_set_source_rgb(cr, 0.35, 0.62, 0.90);
    cairo_set_line_width(cr, 1.0);
    double mid = h / 2.0, half = h / 2.0 - 2.0;
    for (int x = 0; x < a->pk_w; x++) {
        cairo_move_to(cr, x + 0.5, mid - a->pkmax[x] * half);
        cairo_line_to(cr, x + 0.5, mid - a->pkmin[x] * half);
    }
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.30);
    cairo_move_to(cr, 0, mid);
    cairo_line_to(cr, w, mid);
    cairo_stroke(cr);

    double cx = (double)a->cursor / a->in_full.nframes * w;
    cairo_set_source_rgb(cr, 0.95, 0.30, 0.25);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, cx, 0);
    cairo_line_to(cr, cx, h);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean wf_press(GtkWidget *wd, GdkEventButton *e, gpointer u)
{
    (void)wd;
    App *a = u;
    if (!a->in_full.data || e->button != 1) return FALSE;
    a->dragging = 1;
    a->drag_x0 = e->x;
    return TRUE;
}

static gboolean wf_motion(GtkWidget *wd, GdkEventMotion *e, gpointer u)
{
    App *a = u;
    if (!a->dragging || !a->in_full.data) return FALSE;
    int w = gtk_widget_get_allocated_width(wd);
    size_t f0 = px_to_frame(a, a->drag_x0, w);
    size_t f1 = px_to_frame(a, e->x, w);
    a->sel_a = f0 < f1 ? f0 : f1;
    a->sel_b = f0 < f1 ? f1 : f0;
    a->have_sel = (a->sel_b - a->sel_a) > (size_t)(0.01 * a->in_full.rate);
    update_sel_label(a);
    gtk_widget_queue_draw(wd);
    return TRUE;
}

static void fft_at_cursor(App *a);

static gboolean wf_release(GtkWidget *wd, GdkEventButton *e, gpointer u)
{
    App *a = u;
    if (!a->dragging || !a->in_full.data) return FALSE;
    a->dragging = 0;
    int w = gtk_widget_get_allocated_width(wd);
    if (fabs(e->x - a->drag_x0) < 3.0) {
        a->have_sel = 0;
        a->cursor = px_to_frame(a, e->x, w);
        if (a->live_active) {
            if (a->cursor >= a->live_r0 && a->cursor < a->live_r1) {
                g_mutex_lock(&a->live_mx);
                a->live_seek = (long)a->cursor;
                g_mutex_unlock(&a->live_mx);
            }
        } else if (player_state(a->pl) != PL_STOPPED &&
                   a->playing_is_view) {
            if (a->cursor >= a->region_start &&
                a->cursor < a->region_start + a->playing->nframes)
                player_seek(a->pl, a->cursor - a->region_start);
        } else {
            fft_at_cursor(a);
        }
    }
    update_sel_label(a);
    gtk_widget_queue_draw(wd);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Spectrum: capture -> smooth (power domain) -> present                  */
/* ---------------------------------------------------------------------- */

static void smooth_reset(App *a)
{
    a->sm_main.init  = a->sm_main.head  = a->sm_main.fill  = 0;
    a->sm_clean.init = a->sm_clean.head = a->sm_clean.fill = 0;
}

static void mono_window(const audio_buf *b, size_t at, double *mono)
{
    size_t n = b->nframes;
    for (int i = 0; i < FFT_N; i++) {
        size_t f = at + (size_t)i;
        if (f >= n) { mono[i] = 0.0; continue; }
        double s = 0.0;
        for (unsigned c = 0; c < b->channels; c++)
            s += b->data[f * b->channels + c];
        mono[i] = s / b->channels;
    }
}

static void spec_capture(App *a, const audio_buf *b, size_t at)
{
    static double mono[FFT_N];
    if (!b || !b->data || b->nframes == 0) { a->spec_valid = 0; return; }
    mono_window(b, at, mono);
    fft_spectrum_pow(mono, FFT_N, a->pow_cur);
    a->spec_rate  = b->rate;
    a->spec_valid = 1;
}

/* Smooth in the POWER domain (averaging dB underweights peaks). One
 * independent state per trace, same mode/amount for both.               */
static const double *smooth_apply(App *a, smst *st, const double *cur)
{
    int mode = gtk_combo_box_get_active(GTK_COMBO_BOX(a->smooth_combo));
    int amt  = gtk_spin_button_get_value_as_int(
                   GTK_SPIN_BUTTON(a->smooth_spin));
    if (amt < 1)  amt = 1;
    if (amt > 32) amt = 32;
    if (mode != st->last_mode || amt != st->last_amt) {
        st->init = st->head = st->fill = 0;
        st->last_mode = mode;
        st->last_amt  = amt;
    }
    if (mode == 0) return cur;
    if (mode == 1) {
        double al = 1.0 - exp(-1.0 / amt);
        if (!st->init) {
            memcpy(st->sm, cur, sizeof st->sm);
            st->init = 1;
        } else
            for (int k = 0; k < FFT_N / 2; k++)
                st->sm[k] += al * (cur[k] - st->sm[k]);
        return st->sm;
    }
    memcpy(st->ring[st->head], cur, sizeof st->ring[0]);
    st->head = (st->head + 1) % amt;
    if (st->fill < amt) st->fill++;
    for (int k = 0; k < FFT_N / 2; k++) {
        double s = 0.0;
        for (int j = 0; j < st->fill; j++)
            s += st->ring[j][k];
        st->sm[k] = s / st->fill;
    }
    return st->sm;
}

static void spec_present(App *a)
{
    if (!a->spec_valid) return;
    const double *src = smooth_apply(a, &a->sm_main, a->pow_cur);
    for (int k = 0; k < FFT_N / 2; k++)
        a->spec[k] = 10.0 * log10(src[k] + 1e-24);
}

static void spec_from(App *a, const audio_buf *b, size_t at)
{
    spec_capture(a, b, at);
    spec_present(a);
}

static void fft_at_cursor(App *a)
{
    if (!a->in_full.data) return;
    smooth_reset(a);
    a->overlay_valid = 0;
    spec_from(a, &a->in_full, a->cursor);
    gtk_widget_queue_draw(a->fftw);
}

/* Real-time residual: proc vs gain-fitted clean over the FFT window.
 * Only valid while playing proc_buf, which is sample-aligned to the
 * source at region_start.                                               */
static void update_metric(App *a, size_t p)
{
    static double pm[FFT_N], cm[FFT_N];
    if (a->playing != &a->proc_buf || !a->in_full.data) {
        a->overlay_valid = 0;
        return;
    }
    size_t src_at = a->region_start + p;
    if (src_at + FFT_N > a->in_full.nframes ||
        p + FFT_N > a->proc_buf.nframes) {
        return;                                 /* keep last values      */
    }
    mono_window(&a->proc_buf, p, pm);
    mono_window(&a->in_full, src_at, cm);

    double cc = 0.0, cp = 0.0;
    for (int i = 0; i < FFT_N; i++) {
        cc += cm[i] * cm[i];
        cp += cm[i] * pm[i];
    }
    if (cc < 1e-12) {
        gtk_label_set_text(GTK_LABEL(a->metric_label),
                           "residual: --  (source silent)");
        a->overlay_valid = 0;
        return;
    }
    double g = cp / cc;
    double ee = 0.0;
    for (int i = 0; i < FFT_N; i++) {
        double e = pm[i] - g * cm[i];
        ee += e * e;
        cm[i] *= g;                             /* scaled clean          */
    }
    double res_db = 10.0 * log10(ee / (g * g * cc) + 1e-24);
    set_status(a->metric_label,
               "residual (added dist+noise): %.1f dB   gain fit: %+.2f dB",
               res_db, 20.0 * log10(fabs(g) + 1e-24));

    /* clean overlay spectrum (gain-matched) */
    static double cpow[FFT_N / 2];
    fft_spectrum_pow(cm, FFT_N, cpow);
    const double *cs = smooth_apply(a, &a->sm_clean, cpow);
    for (int k = 0; k < FFT_N / 2; k++)
        a->clean_db[k] = 10.0 * log10(cs[k] + 1e-24);
    a->overlay_valid = 1;
}

/* ---------------------------------------------------------------------- */
/* Spectrum drawing                                                       */
/* ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- */
/* Waveshaper transfer view: one cycle of a clean sine vs the same sine
 * through the CURRENT shape settings. Same math as the engine's
 * shape_buf, with the display's mean removed (the engine's DC blocker
 * does the equivalent).                                                  */
/* ---------------------------------------------------------------------- */

#define SHAPER_AMP 0.9

static double shaper_eval(int sh, double drive, double bias, double h2a,
                          double x)
{
    switch (sh) {
    case 1: return tanh(drive * x) / tanh(drive);
    case 2: {                      /* span-normalized, mirrors engine  */
        double t0 = tanh(drive * bias);
        double k  = 0.5 * (tanh(drive * (1.0 + bias))
                           - tanh(drive * (-1.0 + bias)));
        return (tanh(drive * (x + bias)) - t0) / k;
    }
    case 3: return x + h2a * x * x;
    default: return x;
    }
}

static gboolean shaper_draw(GtkWidget *wd, cairo_t *cr, gpointer u)
{
    App *a = u;
    int w = gtk_widget_get_allocated_width(wd);
    int h = gtk_widget_get_allocated_height(wd);

    cairo_set_source_rgb(cr, 0.08, 0.08, 0.10);
    cairo_paint(cr);

    int    sh    = gtk_combo_box_get_active(GTK_COMBO_BOX(a->shape_combo));
    double drive = sv(a->drive);
    double bias  = sv(a->bias);
    double h2a   = ws_h2_coeff(sv(a->h2db), SHAPER_AMP);

    double mid = h / 2.0, half = h / 2.0 - 6.0;

    /* zero line */
    cairo_set_source_rgb(cr, 0.22, 0.22, 0.26);
    cairo_set_line_width(cr, 0.7);
    cairo_move_to(cr, 0, mid);
    cairo_line_to(cr, w, mid);
    cairo_stroke(cr);

    /* mean of the shaped cycle (display-side DC removal) */
    double mean = 0.0;
    for (int i = 0; i < w; i++) {
        double x = SHAPER_AMP * sin(2.0 * M_PI * i / w);
        mean += shaper_eval(sh, drive, bias, h2a, x);
    }
    mean /= w;

    /* common scale: largest excursion of either curve fills the panel,
     * so nothing clips and relative amplitudes stay comparable          */
    double vmax = SHAPER_AMP;
    for (int i = 0; i < w; i++) {
        double x = SHAPER_AMP * sin(2.0 * M_PI * i / w);
        double v = fabs(shaper_eval(sh, drive, bias, h2a, x) - mean);
        if (v > vmax) vmax = v;
    }
    double scale = half / vmax;

    /* clean sine (dim blue) */
    cairo_set_source_rgba(cr, 0.40, 0.60, 0.95, 0.8);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i < w; i++) {
        double y = mid - SHAPER_AMP * sin(2.0 * M_PI * i / w) * scale;
        if (i == 0) cairo_move_to(cr, i, y);
        else        cairo_line_to(cr, i, y);
    }
    cairo_stroke(cr);

    /* shaped sine (yellow) */
    if (sh > 0) {
        cairo_set_source_rgb(cr, 0.98, 0.78, 0.25);
        cairo_set_line_width(cr, 1.4);
        for (int i = 0; i < w; i++) {
            double x = SHAPER_AMP * sin(2.0 * M_PI * i / w);
            double y = mid - (shaper_eval(sh, drive, bias, h2a, x) - mean)
                             * scale;
            if (i == 0) cairo_move_to(cr, i, y);
            else        cairo_line_to(cr, i, y);
        }
        cairo_stroke(cr);
    }

    cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
    cairo_set_font_size(cr, 9);
    cairo_move_to(cr, 4, 11);
    cairo_show_text(cr, sh > 0 ? "shaper: sine 0.9 in (blue) vs out "
                                 "(yellow)"
                               : "shaper: none (clean sine)");

    /* Exact THD of the shaped 0.9 sine: one integer cycle sampled at
     * high resolution -> leak-free harmonic correlation, no window
     * needed. Harmonics 2..64 vs the fundamental.                       */
    if (sh > 0) {
        enum { HN = 2048, KMAX = 64 };
        static double yv[HN];
        for (int i = 0; i < HN; i++)
            yv[i] = shaper_eval(sh, drive, bias, h2a,
                                SHAPER_AMP * sin(2.0 * M_PI * i / HN));
        double hk[KMAX + 1];
        for (int k = 1; k <= KMAX; k++) {
            double sa = 0.0, sb = 0.0;
            for (int i = 0; i < HN; i++) {
                double ph = 2.0 * M_PI * k * i / HN;
                sa += yv[i] * sin(ph);
                sb += yv[i] * cos(ph);
            }
            hk[k] = 2.0 * sqrt(sa * sa + sb * sb) / HN;
        }
        double s2 = 0.0;
        for (int k = 2; k <= KMAX; k++) s2 += hk[k] * hk[k];
        double thd = sqrt(s2) / (hk[1] + 1e-30);
        char txt[128];
        snprintf(txt, sizeof txt,
                 "THD %.2f %%   H2 %.1f dB   H3 %.1f dB",
                 100.0 * thd,
                 20.0 * log10(hk[2] / hk[1] + 1e-12),
                 20.0 * log10(hk[3] / hk[1] + 1e-12));
        cairo_set_source_rgb(cr, 0.98, 0.78, 0.25);
        cairo_move_to(cr, 4, 23);
        cairo_show_text(cr, txt);
    }
    return FALSE;
}

static void on_shaper_changed(GtkWidget *w, gpointer u)
{
    (void)w;
    gtk_widget_queue_draw(((App *)u)->shaper_view);
}

static double freq_to_x(double f, int logscale, double fmin, double fmax,
                        int w)
{
    if (logscale)
        return (log10(f) - log10(fmin)) / (log10(fmax) - log10(fmin)) * w;
    return f / fmax * w;
}

static void draw_trace(cairo_t *cr, const double *db_arr, int w, int ph,
                       int logscale, double fmin, double fmax, double fs)
{
    const double DB_LO = SPEC_DB_LO, DB_HI = SPEC_DB_HI;
    int nb = FFT_N / 2;
    int started = 0;
    for (int x = 0; x < w; x++) {
        double f0, f1;
        if (logscale) {
            double lmin = log10(fmin), lmax = log10(fmax);
            f0 = pow(10.0, lmin + (lmax - lmin) * x / w);
            f1 = pow(10.0, lmin + (lmax - lmin) * (x + 1) / w);
        } else {
            f0 = fmax * x / w;
            f1 = fmax * (x + 1) / w;
        }
        int b0 = (int)(f0 / fs * FFT_N), b1 = (int)(f1 / fs * FFT_N) + 1;
        if (b0 < 1) b0 = 1;
        if (b1 > nb) b1 = nb;
        if (b0 >= b1) continue;
        double m = -300.0;
        for (int b = b0; b < b1; b++)
            if (db_arr[b] > m) m = db_arr[b];
        if (m < DB_LO) m = DB_LO;
        double y = (DB_HI - m) / (DB_HI - DB_LO) * ph;
        if (!started) { cairo_move_to(cr, x, y); started = 1; }
        else            cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
}

static gboolean fft_draw(GtkWidget *wd, cairo_t *cr, gpointer u)
{
    App *a = u;
    int w = gtk_widget_get_allocated_width(wd);
    int h = gtk_widget_get_allocated_height(wd);

    cairo_set_source_rgb(cr, 0.08, 0.08, 0.10);
    cairo_paint(cr);

    const double DB_LO = SPEC_DB_LO, DB_HI = SPEC_DB_HI;
    int    ph   = h - SPEC_INSET;            /* plot area above labels  */
    int    logscale = gtk_combo_box_get_active(
                          GTK_COMBO_BOX(a->fscale_combo)) == 0;
    double fs   = a->spec_valid ? a->spec_rate : 44100.0;
    double fmax = fs / 2.0;
    double fmin = logscale ? 20.0 : 0.0;

    cairo_set_line_width(cr, 0.7);
    cairo_set_font_size(cr, 9);

    if (logscale) {
        const double marks[] = { 50, 100, 200, 500, 1000, 2000, 5000,
                                 10000, 20000 };
        const char  *mlbl[]  = { "50", "100", "200", "500", "1k", "2k",
                                 "5k", "10k", "20k" };
        for (int i = 0; i < 9; i++) {
            if (marks[i] > fmax) break;
            double x = freq_to_x(marks[i], 1, fmin, fmax, w);
            cairo_set_source_rgb(cr, 0.20, 0.20, 0.24);
            cairo_move_to(cr, x, 0); cairo_line_to(cr, x, ph);
            cairo_stroke(cr);
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
            cairo_move_to(cr, x + 2, h - 3);
            cairo_show_text(cr, mlbl[i]);
        }
    } else {
        for (double f = 5000; f < fmax; f += 5000) {
            double x = freq_to_x(f, 0, fmin, fmax, w);
            char lbl[16];
            snprintf(lbl, sizeof lbl, "%.0fk", f / 1000.0);
            cairo_set_source_rgb(cr, 0.20, 0.20, 0.24);
            cairo_move_to(cr, x, 0); cairo_line_to(cr, x, ph);
            cairo_stroke(cr);
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
            cairo_move_to(cr, x + 2, h - 3);
            cairo_show_text(cr, lbl);
        }
    }

    for (double db = 0; db >= -100; db -= 20) {
        double y = (DB_HI - db) / (DB_HI - DB_LO) * ph;
        if (db < 0) {
            cairo_set_source_rgb(cr, 0.20, 0.20, 0.24);
            cairo_move_to(cr, 0, y); cairo_line_to(cr, w, y);
            cairo_stroke(cr);
        }
        char lbl[16];
        snprintf(lbl, sizeof lbl, "%.0f", db);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
        cairo_move_to(cr, 3, db == 0 ? y + 10 : y - 2);
        cairo_show_text(cr, lbl);
    }
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
    cairo_move_to(cr, 3, 20);
    cairo_show_text(cr, "dB");

    if (!a->spec_valid) {
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, 30, h / 2);
        cairo_show_text(cr, "spectrum appears here during playback "
                            "(or click the waveform)");
        return FALSE;
    }

    if (a->overlay_valid) {                     /* clean, gain-matched   */
        cairo_set_source_rgba(cr, 0.40, 0.60, 0.95, 0.8);
        cairo_set_line_width(cr, 1.0);
        draw_trace(cr, a->clean_db, w, ph, logscale, fmin, fmax, fs);
    }
    cairo_set_source_rgb(cr, 0.98, 0.78, 0.25);  /* playing (processed)  */
    cairo_set_line_width(cr, 1.2);
    draw_trace(cr, a->spec, w, ph, logscale, fmin, fmax, fs);
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Async chain rendering                                                  */
/* ---------------------------------------------------------------------- */

enum { CR_FILE, CR_PLAY, CR_ABX_INIT, CR_STAIR };

typedef struct {
    App         *a;
    audio_buf    in;
    chain_params cp;
    audio_buf    out;
    int          ok, purpose, bits, match;
    size_t       r0;
    char        *outpath;
} cr_job;

static void start_play(App *a, const audio_buf *b, size_t region_start,
                       int is_view, const char *what);
static void sess_buttons(App *a, int on);
static void abx_next_x(App *a);

static gboolean cr_done(gpointer u)
{
    cr_job *j = u;
    App *a = j->a;
    a->render_busy = 0;
    gtk_widget_set_sensitive(a->render_btn, TRUE);

    switch (j->purpose) {
    case CR_FILE:
        set_status(a->status, j->ok ? "rendered %s" : "render FAILED (%s)",
                   j->outpath);
        break;

    case CR_PLAY:
        if (!j->ok) { set_status(a->status, "render failed"); break; }
        stop_all(a);
        if (a->proc_buf.data) audio_free(&a->proc_buf);
        a->proc_buf = j->out;
        j->out.data = NULL;
        start_play(a, &a->proc_buf, j->r0, 1, "processed");
        break;

    case CR_ABX_INIT:
        if (!j->ok || a->sess_active != 1) {
            if (j->ok) audio_free(&j->out);
            else {
                set_status(a->abx_status, "render failed");
                a->sess_active = 0;
            }
            break;
        }
        if (a->ex_proc.data) audio_free(&a->ex_proc);
        a->ex_proc = j->out;
        j->out.data = NULL;
        a->t = 1; a->k = 0; a->n = 0;
        abx_next_x(a);
        sess_buttons(a, 1);
        gtk_widget_set_sensitive(a->btn_px, TRUE);
        if (a->sess_abs) {                 /* no references available   */
            gtk_widget_set_sensitive(a->btn_p1, FALSE);
            gtk_widget_set_sensitive(a->btn_p2, FALSE);
            set_status(a->abx_status, "trial %d/%d -- listen to X only: "
                       "clean or processed?", a->t, a->trials);
        }
        break;

    case CR_STAIR:
        if (!j->ok || a->sess_active != 2) {
            if (j->ok) audio_free(&j->out);
            else {
                set_status(a->abx_status, "render failed");
                a->sess_active = 0;
            }
            break;
        }
        if (a->ex_proc.data) audio_free(&a->ex_proc);
        a->ex_proc = j->out;
        j->out.data = NULL;
        a->proc_is_2 = frand(&a->rng) < 0.5;
        sess_buttons(a, 1);
        if (a->sess_abs) {
            gtk_widget_set_sensitive(a->btn_p2, FALSE);
            set_status(a->abx_status, "trial %d (reversal %d/9 -- ends "
                       "at 9) -- one sound: clean or processed?",
                       a->t, a->nrev);
        } else {
            set_status(a->abx_status, "trial %d (reversal %d/9 -- ends "
                       "at 9) -- which interval is processed?",
                       a->t, a->nrev);
        }
        break;
    }

    if (j->out.data) audio_free(&j->out);
    audio_free(&j->in);
    g_free(j->outpath);
    g_free(j);
    return G_SOURCE_REMOVE;
}

static gpointer cr_worker(gpointer u)
{
    cr_job *j = u;
    j->ok = chain_render(&j->in, &j->out, &j->cp, j->match) == 0;
    if (j->ok && j->purpose == CR_FILE) {
        j->ok = audio_write_wav(j->outpath, &j->out, j->bits, 1) == 0;
        audio_free(&j->out);
    }
    g_idle_add(cr_done, j);
    return NULL;
}

static int cr_launch(App *a, audio_buf in, const chain_params *cp,
                     int purpose, int match, int bits, char *outpath,
                     size_t r0)
{
    if (a->render_busy) {
        free(in.data);
        g_free(outpath);
        set_status(a->status, "busy rendering -- wait a moment");
        return -1;
    }
    cr_job *j = g_new0(cr_job, 1);
    j->a = a;
    j->in = in;
    j->cp = *cp;
    j->purpose = purpose;
    j->match   = match;
    j->bits    = bits;
    j->outpath = outpath;
    j->r0      = r0;
    a->render_busy = 1;
    gtk_widget_set_sensitive(a->render_btn, FALSE);
    g_thread_unref(g_thread_new("chain-render", cr_worker, j));
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Live streaming producer: renders LIVE_B-frame blocks with LIVE_PR of
 * pre-roll context just ahead of the playhead, crossfading LIVE_X at
 * seams and looping the region. Parameter changes are picked up at the
 * next block, so knob-to-ear latency = queued audio + device buffer
 * (roughly 250-350 ms).                                                  */
/* ---------------------------------------------------------------------- */

static gpointer live_worker(gpointer u)
{
    App *a = u;
    unsigned ch   = a->in_full.channels;
    unsigned rate = a->in_full.rate;
    size_t   r0 = a->live_r0, r1 = a->live_r1;
    size_t   t  = r0;
    uint64_t s  = 0;                          /* stream frame index    */

    double *tail = calloc((size_t)LIVE_X * ch, sizeof *tail);
    double *wblk = calloc((size_t)(LIVE_B + LIVE_X) * ch, sizeof *wblk);
    int     tail_ok = 0;
    double  gain = 1.0;
    int     gain_set = 0;
    if (!tail || !wblk) goto out;

    for (;;) {
        g_mutex_lock(&a->live_mx);
        if (a->live_stop) { g_mutex_unlock(&a->live_mx); break; }
        long sk = a->live_seek;
        a->live_seek = -1;
        chain_params cp = a->live_cp;
        g_mutex_unlock(&a->live_mx);

        if (sk >= 0) {
            t = (size_t)sk;
            if (t < r0 || t >= r1) t = r0;
            tail_ok = 0;
        }
        if (t >= r1) { t = r0; tail_ok = 0; }   /* loop the region      */

        size_t emit = r1 - t;
        if (emit > LIVE_B) emit = LIVE_B;
        size_t pre  = t > LIVE_PR ? t - LIVE_PR : 0;
        size_t endr = t + emit + LIVE_X;
        if (endr > r1) endr = r1;

        audio_buf in = a->in_full;              /* slice [pre, endr)    */
        in.nframes = endr - pre;
        in.data = malloc(in.nframes * ch * sizeof *in.data);
        if (!in.data) break;
        memcpy(in.data, a->in_full.data + pre * ch,
               in.nframes * ch * sizeof *in.data);
        cp.pos0 = pre;

        audio_buf outb;
        int rc = chain_render(&in, &outb, &cp, 0);
        free(in.data);
        if (rc != 0) break;

        const double *blk = outb.data + (t - pre) * ch;
        if (!gain_set) {                        /* frozen loudness trim */
            double rs = 0.0, ro = 0.0;
            for (size_t i = 0; i < emit * ch; i++) {
                double sv2 = a->in_full.data[(t) * ch + i];
                rs += sv2 * sv2;
                ro += blk[i] * blk[i];
            }
            gain = (ro > 1e-12) ? sqrt(rs / ro) : 1.0;
            gain_set = 1;
        }
        for (size_t i = 0; i < emit * ch; i++)
            wblk[i] = gain * blk[i];
        if (tail_ok) {
            size_t xf = emit < LIVE_X ? emit : LIVE_X;
            for (size_t i = 0; i < xf; i++) {
                double wgt = (double)i / LIVE_X;
                for (unsigned c = 0; c < ch; c++)
                    wblk[i * ch + c] = tail[i * ch + c] * (1.0 - wgt)
                                     + wblk[i * ch + c] * wgt;
            }
        }
        if (t + emit + LIVE_X <= r1) {          /* tail for next seam   */
            const double *nt = outb.data + (t + emit - pre) * ch;
            for (size_t i = 0; i < (size_t)LIVE_X * ch; i++)
                tail[i] = gain * nt[i];
            tail_ok = 1;
        } else {
            tail_ok = 0;
        }
        audio_free(&outb);

        g_mutex_lock(&a->live_mx);              /* history + block map  */
        for (size_t i = 0; i < emit; i++) {
            size_t slot = (size_t)((s + i) % LIVE_HIST);
            for (unsigned c = 0; c < ch; c++)
                a->live_hist[slot * ch + c] = wblk[i * ch + c];
        }
        if (a->live_map_n == LIVE_MAP) {
            memmove(a->live_map, a->live_map + 1,
                    (LIVE_MAP - 1) * sizeof a->live_map[0]);
            a->live_map_n--;
        }
        a->live_map[a->live_map_n].s = s;
        a->live_map[a->live_map_n].f = t;
        a->live_map[a->live_map_n].n = emit;
        a->live_map_n++;
        g_mutex_unlock(&a->live_mx);

        if (player_stream_write(a->pl, wblk, emit) != 0) break;
        s += emit;
        t += emit;
        (void)rate;
    }
out:
    free(tail);
    free(wblk);
    return NULL;
}

static void live_stop(App *a)
{
    if (!a->live_active) return;
    g_mutex_lock(&a->live_mx);
    a->live_stop = 1;
    g_mutex_unlock(&a->live_mx);
    player_stop(a->pl);                        /* unblocks stream_write */
    g_thread_join(a->live_th);
    a->live_th = NULL;
    a->live_active = 0;
}

/* ---------------------------------------------------------------------- */
/* Transport                                                              */
/* ---------------------------------------------------------------------- */

static void start_play(App *a, const audio_buf *b, size_t region_start,
                       int is_view, const char *what)
{
    a->playing = b;
    a->region_start = region_start;
    a->playing_is_view = is_view;
    smooth_reset(a);
    a->overlay_valid = 0;
    if (player_play(a->pl, b->data, b->nframes, b->channels, b->rate, 0)
        != 0) {
        set_status(a->status, "%s", player_status(a->pl));
        a->playing = NULL;
        return;
    }
    gtk_button_set_label(GTK_BUTTON(a->btn_pause), "Pause");
    set_status(a->status, "playing %s%s", what,
               player_status(a->pl)[0] == 'n' ? " (no audio device!)" : "");
}

static void on_play_clean(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (load_input(a) != 0) return;
    stop_all(a);
    size_t r0, r1;
    get_region(a, &r0, &r1);
    if (r1 <= r0) { r0 = 0; r1 = a->in_full.nframes; }
    static audio_buf view;
    view = a->in_full;
    view.data    = a->in_full.data + r0 * a->in_full.channels;
    view.nframes = r1 - r0;
    start_play(a, &view, r0, 1, "clean");
}

static void on_play_proc(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (load_input(a) != 0) return;
    chain_params cp;
    collect_params(a, &cp);
    if (!chain_enabled(&cp)) {
        set_status(a->status, "no effect enabled");
        return;
    }
    stop_all(a);
    size_t r0, r1;
    get_region(a, &r0, &r1);
    if (r1 <= r0) { r0 = 0; r1 = a->in_full.nframes; }

    unsigned ch = a->in_full.channels;
    if (!a->live_hist)
        a->live_hist = calloc((size_t)LIVE_HIST * ch, sizeof *a->live_hist);
    if (!a->live_hist) return;

    a->live_cp      = cp;
    a->live_cp_snap = cp;
    a->live_r0 = r0;
    a->live_r1 = r1;
    a->live_stop  = 0;
    a->live_seek  = -1;
    a->live_map_n = 0;
    if (player_stream_start(a->pl, ch, a->in_full.rate) != 0) {
        set_status(a->status, "%s", player_status(a->pl));
        return;
    }
    a->playing_is_view = 1;
    smooth_reset(a);
    a->live_th = g_thread_new("live-render", live_worker, a);
    a->live_active = 1;
    gtk_button_set_label(GTK_BUTTON(a->btn_pause), "Pause");
    set_status(a->status, "live: looping %.1f s -- tweak away "
               "(changes land in ~0.3 s)%s",
               (double)(r1 - r0) / a->in_full.rate,
               player_status(a->pl)[0] == 'n' ? " (no audio device!)" : "");
}

static void on_pause(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (player_state(a->pl) == PL_STOPPED) return;
    player_pause_toggle(a->pl);
}

static void on_stop(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    stop_all(a);
    set_status(a->status, "stopped");
}

static void live_tick(App *a)
{
    int st = player_state(a->pl);
    if (st == PL_STOPPED) { a->live_active = 0; return; }
    gtk_button_set_label(GTK_BUTTON(a->btn_pause),
                         st == PL_PAUSED ? "Resume" : "Pause");

    /* poll controls ~30/s; hand changed params to the producer */
    chain_params cp;
    collect_params(a, &cp);
    if (memcmp(&cp, &a->live_cp_snap, sizeof cp) != 0) {
        a->live_cp_snap = cp;
        g_mutex_lock(&a->live_mx);
        a->live_cp = cp;
        g_mutex_unlock(&a->live_mx);
    }

    uint64_t sp = player_pos(a->pl);           /* audible stream frame  */
    unsigned ch = a->in_full.channels;
    static double pm[FFT_N], cm[FFT_N];
    int have = 0;
    size_t fpos = 0;

    g_mutex_lock(&a->live_mx);
    for (int i = a->live_map_n - 1; i >= 0; i--) {
        if (sp >= a->live_map[i].s &&
            sp < a->live_map[i].s + a->live_map[i].n) {
            fpos = a->live_map[i].f + (size_t)(sp - a->live_map[i].s);
            have = 1;
            break;
        }
    }
    if (have) {
        for (int i = 0; i < FFT_N; i++) {
            size_t slot = (size_t)((sp + (uint64_t)i) % LIVE_HIST);
            double sum = 0.0;
            for (unsigned c = 0; c < ch; c++)
                sum += a->live_hist[slot * ch + c];
            pm[i] = sum / ch;
        }
    }
    g_mutex_unlock(&a->live_mx);
    if (!have) return;

    a->cursor = fpos;
    if (a->cursor > a->in_full.nframes) a->cursor = a->in_full.nframes;
    gtk_widget_queue_draw(a->wf);
    update_sel_label(a);

    fft_spectrum_pow(pm, FFT_N, a->pow_cur);
    a->spec_rate  = a->in_full.rate;
    a->spec_valid = 1;
    spec_present(a);

    /* residual vs the source at the mapped position */
    if (fpos + FFT_N <= a->in_full.nframes) {
        mono_window(&a->in_full, fpos, cm);
        double cc = 0.0, cp2 = 0.0;
        for (int i = 0; i < FFT_N; i++) {
            cc  += cm[i] * cm[i];
            cp2 += cm[i] * pm[i];
        }
        if (cc > 1e-12) {
            double g = cp2 / cc, ee = 0.0;
            for (int i = 0; i < FFT_N; i++) {
                double e = pm[i] - g * cm[i];
                ee += e * e;
                cm[i] *= g;
            }
            set_status(a->metric_label,
                       "residual (added dist+noise): %.1f dB   "
                       "gain fit: %+.2f dB",
                       10.0 * log10(ee / (g * g * cc) + 1e-24),
                       20.0 * log10(fabs(g) + 1e-24));
            static double cpow[FFT_N / 2];
            fft_spectrum_pow(cm, FFT_N, cpow);
            const double *cs = smooth_apply(a, &a->sm_clean, cpow);
            for (int k = 0; k < FFT_N / 2; k++)
                a->clean_db[k] = 10.0 * log10(cs[k] + 1e-24);
            a->overlay_valid = 1;
        }
    }
    uint64_t ur = player_stream_underruns(a->pl);
    if (ur > 4)   /* a couple at startup are pipeline priming: ignore   */
        set_status(a->status, "live: %llu underruns -- lower the "
                   "Oversampling setting", (unsigned long long)ur);
    gtk_widget_queue_draw(a->fftw);
}

static gboolean tick(gpointer u)
{
    App *a = u;
    if (a->live_active) { live_tick(a); return G_SOURCE_CONTINUE; }
    int st = player_state(a->pl);
    if (st == PL_STOPPED || !a->playing) {
        if (a->playing && st == PL_STOPPED) {
            a->playing = NULL;
            set_status(a->status, "finished");
            gtk_button_set_label(GTK_BUTTON(a->btn_pause), "Pause");
        }
        return G_SOURCE_CONTINUE;
    }
    gtk_button_set_label(GTK_BUTTON(a->btn_pause),
                         st == PL_PAUSED ? "Resume" : "Pause");
    size_t p = player_pos(a->pl);
    if (a->playing_is_view) {
        a->cursor = a->region_start + p;
        if (a->cursor > a->in_full.nframes) a->cursor = a->in_full.nframes;
        gtk_widget_queue_draw(a->wf);
        update_sel_label(a);
    }
    spec_from(a, a->playing, p);
    update_metric(a, p);
    gtk_widget_queue_draw(a->fftw);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* File render                                                            */
/* ---------------------------------------------------------------------- */

static void on_render(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (a->render_busy) {
        set_status(a->status, "busy rendering -- wait a moment");
        return;
    }
    if (load_input(a) != 0) return;

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Render to", GTK_WINDOW(a->win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg),
                                                   TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "out.wav");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg);
        return;
    }
    gchar *outpath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);

    chain_params cp;
    collect_params(a, &cp);
    int bits  = gtk_combo_box_get_active(GTK_COMBO_BOX(a->bits_combo)) ? 24
                                                                       : 16;
    int match = chk(a->match_chk);
    int selonly = chk(a->selonly_chk) && a->have_sel;
    size_t r0 = selonly ? a->sel_a : 0;
    size_t r1 = selonly ? a->sel_b : a->in_full.nframes;

    audio_buf in;
    if (slice_view(a, r0, r1, &in) != 0) { g_free(outpath); return; }
    if (cr_launch(a, in, &cp, CR_FILE, match, bits, outpath, 0) == 0)
        set_status(a->status, "rendering %.1f s to file...",
                   (double)(r1 - r0) / a->in_full.rate);
}

/* ---------------------------------------------------------------------- */
/* Listening-test tab                                                     */
/* ---------------------------------------------------------------------- */

static void chain_describe(const chain_params *cp, char *buf, size_t n)
{
    size_t off = 0;
    #define APP(...) off += (size_t)snprintf(buf + off, n - off, __VA_ARGS__)
    APP("chain:");
    if (cp->use_shape) {
        const char *nm = cp->wsp.shape == WS_TANH ? "tanh"
                       : cp->wsp.shape == WS_TUBE ? "tube" : "h2";
        APP(" %s", nm);
        if (cp->wsp.shape == WS_H2) APP(" h2=%.1fdB", cp->h2db);
        else APP(" drive=%.1f bias=%.2f", cp->wsp.drive, cp->wsp.bias);
        APP(" os=%dx", cp->os);
    }
    if (cp->use_tape)
        APP(" | tape wow=%.1fc flut=%.1fc hiss=%.0fdB hf=%.2f",
            cp->tp.wow_cents, cp->tp.flutter_cents, cp->tp.hiss_db,
            cp->tp.hf_loss);
    if (cp->use_vinyl)
        APP(" | vinyl wow=%.1fc hiss=%.0fdB crackle=%.0f/s@%.0fdB",
            cp->vp.wow_cents, cp->vp.hiss_db, cp->vp.crackle_per_s,
            cp->vp.crackle_db);
    if (cp->neq) APP(" | %d filter(s)", cp->neq);
    if (!cp->use_shape && !cp->use_tape && !cp->use_vinyl && !cp->neq)
        APP(" (none)");
    #undef APP
}

/* end-of-session report: one line per trial */
static void fill_session_log(App *a, const char *scoreline)
{
    GString *g = g_string_new(NULL);
    char cb[256];
    chain_describe(&a->sess_cp, cb, sizeof cb);
    g_string_append_printf(g, "%s\n\n", cb);

    int stair = (a->sess_active == 2);
    int cmp_stair = stair && !a->sess_abs;
    g_string_append_printf(g, "%-6s %-14s %-14s %-8s %s\n",
        "trial",
        cmp_stair ? "processed in" : "X was",
        cmp_stair ? "you said"     : "you said",
        "result",
        stair ? "level" : "level (fixed)");
    for (int i = 0; i < a->ntl; i++) {
        char lv[64] = "-";
        if (stair) sc_print_value(a->si, a->tlog[i].s, lv, sizeof lv);
        const char *stim, *ans;
        if (cmp_stair) {
            stim = a->tlog[i].stim ? "interval 2" : "interval 1";
            ans  = a->tlog[i].ans  ? "interval 2" : "interval 1";
        } else {
            stim = a->tlog[i].stim ? "processed" : "clean";
            ans  = a->tlog[i].ans  ? "processed" : "clean";
        }
        g_string_append_printf(g, "%-6d %-14s %-14s %-8s %s\n",
            i + 1, stim, ans,
            a->tlog[i].correct ? "correct" : "WRONG", lv);
    }
    g_string_append_printf(g, "\n%s\n", scoreline);
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->abx_log_view)),
        g->str, -1);
    g_string_free(g, TRUE);
}

/* Session errors were easy to miss as small labels: raise a dialog. */
static void abx_fail(App *a, const char *msg)
{
    set_status(a->abx_status, "%s", msg);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->win),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void sess_buttons(App *a, int on)
{
    gtk_widget_set_sensitive(a->btn_p1, on);
    gtk_widget_set_sensitive(a->btn_p2, on);
    gtk_widget_set_sensitive(a->btn_a1, on);
    gtk_widget_set_sensitive(a->btn_a2, on);
    if (!on) gtk_widget_set_sensitive(a->btn_px, 0);
}

static void sess_free(App *a)
{
    stop_all(a);
    if (a->ex_clean.data) audio_free(&a->ex_clean);
    if (a->ex_proc.data)  audio_free(&a->ex_proc);
    a->x_buf = NULL;
    a->sess_active = 0;
    sess_buttons(a, 0);
}

static void abx_next_x(App *a)
{
    a->x_buf = (frand(&a->rng) < 0.5) ? &a->ex_proc : &a->ex_clean;
    set_status(a->abx_status,
               a->sess_abs ? "trial %d/%d -- listen to X only: clean or "
                             "processed?"
                           : "trial %d/%d -- is X clean or processed?",
               a->t, a->trials);
}

static void stair_launch_trial(App *a)
{
    if (a->s < a->si->lo) a->s = a->si->lo;
    if (a->s > a->si->hi) a->s = a->si->hi;
    a->t++;
    sc_set(&a->sess_cp, a->si->id, a->s);
    stop_all(a);
    sess_buttons(a, 0);
    audio_buf in;
    in = a->ex_clean;
    size_t total = in.nframes * in.channels;
    in.data = malloc(total * sizeof *in.data);
    if (!in.data) return;
    memcpy(in.data, a->ex_clean.data, total * sizeof *in.data);
    if (cr_launch(a, in, &a->sess_cp, CR_STAIR, 1, 0, NULL, 0) == 0) {
        set_status(a->abx_status, "rendering trial %d... buttons enable "
                   "when ready", a->t);
        if (a->t == 1)
            set_status(a->status, "listening test: rendering trials -- "
                       "each takes a moment at high oversampling");
    }
}

static const char HELP_IDLE[] =
    "How to run a test:\n\n"
    "1. On the Chain tab, enable the effect you want to test and set "
    "its level.\n"
    "2. Optionally drag a selection on the waveform (otherwise the "
    "first 10 s are used).\n"
    "3. Pick a mode above and press Start session.\n\n"
    "ABX runs a fixed number of trials and gives a p-value. A "
    "staircase hunts for your detection threshold and decides its own "
    "length.";

static const char HELP_ABX[] =
    "ABX, fixed level:\n\n"
    "A is always the clean reference, B the processed one. Each trial, "
    "X is secretly either A or B. Listen to A, B and X as many times "
    "as you like -- there is no time pressure -- then answer what X "
    "was.\n\n"
    "The test ends by itself after the set number of trials and "
    "reports how many you got right with an exact binomial p-value: "
    "p < 0.05 means you are reliably hearing the difference.";

static const char HELP_ABX_ABS[] =
    "Absolute identification, fixed level:\n\n"
    "Each trial plays ONE sound, X -- secretly clean or processed. "
    "There are no references to compare against; that is the point. "
    "Replay X as often as you like, then judge it.\n\n"
    "Ends after the set number of trials with a p-value. Expect this "
    "to be much harder than ABX: within seconds the ear adopts "
    "whatever it hears as the new normal.";

static const char HELP_STAIR[] =
    "Adaptive staircase (finds your threshold):\n\n"
    "Each trial has two intervals -- one clean, one processed at the "
    "CURRENT level -- in random order. Replay both freely, then say "
    "which was processed.\n\n"
    "Two correct answers in a row lower the level (harder); one wrong "
    "raises it. The first trials are deliberately easy -- the level "
    "starts high and walks down, so a long run of correct answers at "
    "the beginning is normal.\n\n"
    "Every change of direction is a 'reversal'. The test ends itself "
    "at 9 reversals (usually 25-45 trials); your threshold is the "
    "average of the last 6 reversal levels. End session stops early "
    "(6+ reversals still yield an estimate).\n\n"
    "The staircase ISOLATES its parameter: all other distortion cues "
    "(crackle, wow, bandwidth...) are switched off for the session, so "
    "the threshold measures only the varied quantity.";

static const char HELP_STAIR_ABS[] =
    "Adaptive staircase, absolute identification:\n\n"
    "Each trial plays ONE sound at the current level -- secretly clean "
    "or processed, no reference. Replay it freely, then judge "
    "it.\n\n"
    "Two correct in a row lower the level; one wrong raises it. Early "
    "trials are easy by design. Ends itself at 9 reversals (usually "
    "25-45 trials); threshold = mean of the last 6 reversals. Compare "
    "against the same staircase WITHOUT this mode to measure how much "
    "a reference is worth to your ears.";

static void set_help(App *a, const char *txt)
{
    gtk_label_set_text(GTK_LABEL(a->abx_help), txt);
}

static void stair_finish(App *a);

static void on_abx_end(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (a->sess_active == 2) {
        stair_finish(a);                  /* partial threshold if >= 6
                                             reversals, else says so    */
    } else if (a->sess_active == 1) {
        char msg[200];
        double p = a->n ? binom_p(a->k, a->n) : 1.0;
        snprintf(msg, sizeof msg,
                 "ended early: %d/%d correct, exact binomial p = %.4f",
                 a->k, a->n, p);
        gtk_label_set_text(GTK_LABEL(a->abx_result), msg);
        fill_session_log(a, msg);
        set_status(a->abx_status, "session ended");
        sess_free(a);
    }
}

static void on_abx_start(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (a->render_busy) {
        set_status(a->abx_status, "busy rendering -- wait a moment");
        return;
    }
    sess_free(a);
    gtk_label_set_text(GTK_LABEL(a->abx_result), "");
    a->ntl = 0;
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->abx_log_view)), "", -1);
    if (load_input(a) != 0) return;

    size_t r0, r1;
    get_region(a, &r0, &r1);
    if (r1 <= r0) { r0 = 0; r1 = a->in_full.nframes; }
    /* no selection: default to a 10 s excerpt (fast to render, and
     * short loops make better test material anyway). An explicit
     * selection is honored up to 30 s.                                  */
    double cap_s = a->have_sel ? TEST_MAX_SECS : 10.0;
    size_t maxf = (size_t)(cap_s * a->in_full.rate);
    if (r1 - r0 > maxf) r1 = r0 + maxf;
    if (slice_view(a, r0, r1, &a->ex_clean) != 0) return;

    collect_params(a, &a->sess_cp);
    if (!chain_enabled(&a->sess_cp)) {
        audio_free(&a->ex_clean);
        abx_fail(a, "No effect is enabled on the Chain tab -- there is "
                    "nothing to test. Enable a shape, vinyl, tape, or a "
                    "filter first.");
        return;
    }
    int mode = gtk_combo_box_get_active(GTK_COMBO_BOX(a->abx_mode));
    a->sess_abs = chk(a->abx_abs);

    if (mode == 0) {
        a->sess_active = 1;
        set_help(a, a->sess_abs ? HELP_ABX_ABS : HELP_ABX);
        a->trials = gtk_spin_button_get_value_as_int(
                        GTK_SPIN_BUTTON(a->abx_trials));
        gtk_button_set_label(GTK_BUTTON(a->btn_p1), "Play A (clean)");
        gtk_button_set_label(GTK_BUTTON(a->btn_p2), "Play B (processed)");
        gtk_button_set_label(GTK_BUTTON(a->btn_a1),
                             a->sess_abs ? "X was clean" : "X = A");
        gtk_button_set_label(GTK_BUTTON(a->btn_a2),
                             a->sess_abs ? "X was processed" : "X = B");
        audio_buf in;
        in = a->ex_clean;
        size_t total = in.nframes * in.channels;
        in.data = malloc(total * sizeof *in.data);
        if (!in.data) { sess_free(a); return; }
        memcpy(in.data, a->ex_clean.data, total * sizeof *in.data);
        if (cr_launch(a, in, &a->sess_cp, CR_ABX_INIT, 1, 0, NULL, 0)
            == 0) {
            double secs = (double)(r1 - r0) / a->in_full.rate;
            set_status(a->abx_status, "rendering references (%.0f s of "
                       "audio)... buttons enable when ready", secs);
            set_status(a->status, "listening test: rendering %.0f s -- "
                       "this can take a while at 8x oversampling", secs);
        }
    } else {
        size_t nsc;
        a->si = &sc_table(&nsc)[mode - 1];
        const char *err = sc_check_enabled(&a->sess_cp, a->si->id);
        if (err) {
            audio_free(&a->ex_clean);
            abx_fail(a, err);
            return;
        }
        a->sess_active = 2;
        sc_isolate(&a->sess_cp, a->si->id);
        set_help(a, a->sess_abs ? HELP_STAIR_ABS : HELP_STAIR);
        a->s = sc_get(&a->sess_cp, a->si->id);
        set_status(a->status, "staircase isolates %s: other distortion "
                   "cues are disabled during the test", a->si->name);
        a->step = 6.0;
        a->dir = 0; a->ncorr = 0; a->nrev = 0; a->t = 0;
        if (a->sess_abs) {
            gtk_button_set_label(GTK_BUTTON(a->btn_p1),
                                 "Play the sound");
            gtk_button_set_label(GTK_BUTTON(a->btn_p2), "--");
            gtk_button_set_label(GTK_BUTTON(a->btn_a1), "It was clean");
            gtk_button_set_label(GTK_BUTTON(a->btn_a2),
                                 "It was processed");
        } else {
            gtk_button_set_label(GTK_BUTTON(a->btn_p1),
                                 "Play interval 1");
            gtk_button_set_label(GTK_BUTTON(a->btn_p2),
                                 "Play interval 2");
            gtk_button_set_label(GTK_BUTTON(a->btn_a1),
                                 "Processed was 1");
            gtk_button_set_label(GTK_BUTTON(a->btn_a2),
                                 "Processed was 2");
        }
        stair_launch_trial(a);
    }
}

static void on_play_ref(GtkWidget *w, gpointer u)
{
    App *a = u;
    if (!a->sess_active) return;
    int two = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "idx")) == 2;
    const audio_buf *b;
    if (a->sess_active == 1)
        b = two ? &a->ex_proc : &a->ex_clean;
    else if (a->sess_abs)                     /* single stimulus        */
        b = a->proc_is_2 ? &a->ex_proc : &a->ex_clean;
    else
        b = (two == a->proc_is_2) ? &a->ex_proc : &a->ex_clean;
    start_play(a, b, 0, 0, two ? "interval 2" : "interval 1");
}

static void on_play_x(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (a->sess_active != 1 || !a->x_buf) return;
    start_play(a, a->x_buf, 0, 0, "X");
}

static void stair_finish(App *a)
{
    char vbuf[64];
    if (a->nrev >= 6) {
        double t = 0.0;
        for (int i = a->nrev - 6; i < a->nrev; i++) t += a->revs[i];
        sc_print_value(a->si, t / 6.0, vbuf, sizeof vbuf);
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s threshold (%s): %s  [%d trials]",
                 a->sess_abs ? "absolute-ID" : "detection",
                 a->si->name, vbuf, a->t);
        gtk_label_set_text(GTK_LABEL(a->abx_result), msg);
        fill_session_log(a, msg);
    } else {
        gtk_label_set_text(GTK_LABEL(a->abx_result),
                           "too few reversals for an estimate");
        fill_session_log(a, "too few reversals for an estimate");
    }
    set_status(a->abx_status, "session finished");
    sess_free(a);
}

static void on_answer(GtkWidget *w, gpointer u)
{
    App *a = u;
    if (!a->sess_active) return;
    stop_all(a);
    int second = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "idx")) == 2;

    if (a->sess_active == 1) {
        int correct = (second == (a->x_buf == &a->ex_proc));
        if (a->ntl < 220) {
            a->tlog[a->ntl].stim    = (a->x_buf == &a->ex_proc);
            a->tlog[a->ntl].ans     = second;
            a->tlog[a->ntl].correct = correct;
            a->tlog[a->ntl].s       = 0.0;
            a->ntl++;
        }
        a->k += correct;
        a->n++;
        if (a->t >= a->trials) {
            double p = binom_p(a->k, a->n);
            char msg[200];
            snprintf(msg, sizeof msg,
                     "%d/%d correct, exact binomial p = %.4f -- %s",
                     a->k, a->n, p,
                     p < 0.05 ? "you can reliably hear it"
                              : "consistent with guessing");
            gtk_label_set_text(GTK_LABEL(a->abx_result), msg);
            fill_session_log(a, msg);
            set_status(a->abx_status, "session finished");
            sess_free(a);
        } else {
            a->t++;
            abx_next_x(a);
        }
        return;
    }

    int correct = (second == a->proc_is_2);
    if (a->ntl < 220) {
        a->tlog[a->ntl].stim    = a->proc_is_2;
        a->tlog[a->ntl].ans     = second;
        a->tlog[a->ntl].correct = correct;
        a->tlog[a->ntl].s       = a->s;
        a->ntl++;
    }
    int move = 0;
    if (correct) {
        if (++a->ncorr >= 2) { a->ncorr = 0; move = +1; }
    } else { a->ncorr = 0; move = -1; }
    if (move) {
        int nd = (move > 0) ? -1 : +1;
        if (a->dir != 0 && nd != a->dir && a->nrev < 16) {
            a->revs[a->nrev++] = a->s;
            a->step *= 0.5;
            if (a->step < 1.0) a->step = 1.0;
        }
        a->dir = nd;
        a->s += nd * a->step;
    }
    if (a->nrev >= 9 || a->t >= 120) { stair_finish(a); return; }
    stair_launch_trial(a);
}

/* ---------------------------------------------------------------------- */
/* UI construction                                                        */
/* ---------------------------------------------------------------------- */

static GtkWidget *add_scale(GtkWidget *grid, int row, const char *label,
                            double lo, double hi, double step, double init,
                            const char *tip)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                            lo, hi, step);
    gtk_range_set_value(GTK_RANGE(s), init);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(s, TRUE);
    if (tip) {
        gtk_widget_set_tooltip_text(l, tip);
        gtk_widget_set_tooltip_text(s, tip);
    }
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s, 1, row, 1, 1);
    return s;
}

static void frame_grid(GtkWidget *box, const char *title, GtkWidget **grid)
{
    GtkWidget *f = gtk_frame_new(title);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_grid_set_row_spacing(GTK_GRID(g), 2);
    gtk_container_set_border_width(GTK_CONTAINER(g), 6);
    gtk_container_add(GTK_CONTAINER(f), g);
    gtk_box_pack_start(GTK_BOX(box), f, FALSE, FALSE, 4);
    *grid = g;
}

/* one filter row: [check] [freq spin] [second spin] */
static void filter_row(GtkWidget *grid, int row, const char *name,
                       GtkWidget **c, GtkWidget **s1, double lo1,
                       double hi1, double def1, GtkWidget **s2,
                       double lo2, double hi2, double def2,
                       const char *l2, const char *tip)
{
    *c = gtk_check_button_new_with_label(name);
    if (tip) gtk_widget_set_tooltip_text(*c, tip);
    gtk_grid_attach(GTK_GRID(grid), *c, 0, row, 1, 1);
    *s1 = gtk_spin_button_new_with_range(lo1, hi1, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*s1), def1);
    gtk_grid_attach(GTK_GRID(grid), *s1, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hz"), 2, row, 1, 1);
    *s2 = gtk_spin_button_new_with_range(lo2, hi2,
                                         hi2 > 100 ? 10 : 0.05);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*s2), def2);
    gtk_grid_attach(GTK_GRID(grid), *s2, 3, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(l2), 4, row, 1, 1);
}

static void on_specopt_changed(GtkWidget *w, gpointer u)
{
    (void)w;
    App *a = u;
    if (a->spec_valid) spec_present(a);
    gtk_widget_queue_draw(a->fftw);
}

static void on_file_set(GtkFileChooserButton *b, gpointer u)
{
    (void)b;
    load_input((App *)u);
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static void build_ui(App *a)
{
    a->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->win), "audiotard");
    gtk_window_set_default_size(GTK_WINDOW(a->win), 1000, 700);
    g_signal_connect(a->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(a->win), outer);

    GtkWidget *ib = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(ib), gtk_label_new("Input"), FALSE, FALSE, 0);
    a->in_chooser = gtk_file_chooser_button_new(
                        "Input file", GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_widget_set_hexpand(a->in_chooser, TRUE);
    g_signal_connect(a->in_chooser, "file-set", G_CALLBACK(on_file_set), a);
    gtk_box_pack_start(GTK_BOX(ib), a->in_chooser, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), ib, FALSE, FALSE, 0);

    /* waveform + spectrum: absorb extra vertical space when maximized   */
    a->wf = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->wf, -1, 110);
    gtk_widget_add_events(a->wf, GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(a->wf, "draw", G_CALLBACK(wf_draw), a);
    g_signal_connect(a->wf, "button-press-event", G_CALLBACK(wf_press), a);
    g_signal_connect(a->wf, "motion-notify-event", G_CALLBACK(wf_motion), a);
    g_signal_connect(a->wf, "button-release-event",
                     G_CALLBACK(wf_release), a);
    gtk_widget_set_size_request(a->wf, -1, 80);

    a->fftw = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->fftw, 300, 80);
    g_signal_connect(a->fftw, "draw", G_CALLBACK(fft_draw), a);
    a->shaper_view = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->shaper_view, 220, 80);
    g_signal_connect(a->shaper_view, "draw", G_CALLBACK(shaper_draw), a);

    /* drag the divider between spectrum and shaper panel */
    GtkWidget *hp = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hp), a->fftw, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(hp), a->shaper_view, FALSE, TRUE);
    gtk_paned_set_position(GTK_PANED(hp), 660);

    /* spectrum options + metric */
    GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(fb), gtk_label_new("Spectrum:"),
                       FALSE, FALSE, 0);
    a->fscale_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->fscale_combo),
                                   "Log freq");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->fscale_combo),
                                   "Linear freq");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->fscale_combo), 0);
    gtk_box_pack_start(GTK_BOX(fb), a->fscale_combo, FALSE, FALSE, 0);
    a->smooth_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->smooth_combo),
                                   "No smoothing");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->smooth_combo),
                                   "Exponential");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->smooth_combo),
                                   "Moving avg");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->smooth_combo), 0);
    gtk_box_pack_start(GTK_BOX(fb), a->smooth_combo, FALSE, FALSE, 0);
    a->smooth_spin = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->smooth_spin), 8);
    gtk_box_pack_start(GTK_BOX(fb), a->smooth_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fb), gtk_label_new("frames"),
                       FALSE, FALSE, 0);
    a->metric_label = gtk_label_new("");
    gtk_widget_set_halign(a->metric_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand(a->metric_label, TRUE);
    gtk_widget_set_tooltip_text(a->metric_label,
        "Level of (processed - gain-fitted clean) over the current FFT "
        "window, relative to the clean signal. Everything the chain "
        "added: harmonics, noise, wow sidebands. On a sine passage this "
        "equals THD+N.");
    gtk_box_pack_start(GTK_BOX(fb), a->metric_label, TRUE, TRUE, 0);
    g_signal_connect(a->fscale_combo, "changed",
                     G_CALLBACK(on_specopt_changed), a);
    g_signal_connect(a->smooth_combo, "changed",
                     G_CALLBACK(on_specopt_changed), a);
    g_signal_connect(a->smooth_spin, "value-changed",
                     G_CALLBACK(on_specopt_changed), a);

    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *bc = gtk_button_new_with_label("Play clean");
    GtkWidget *bp = gtk_button_new_with_label("Play processed");
    a->btn_pause  = gtk_button_new_with_label("Pause");
    GtkWidget *bs = gtk_button_new_with_label("Stop");
    g_signal_connect(bc, "clicked", G_CALLBACK(on_play_clean), a);
    g_signal_connect(bp, "clicked", G_CALLBACK(on_play_proc), a);
    g_signal_connect(a->btn_pause, "clicked", G_CALLBACK(on_pause), a);
    g_signal_connect(bs, "clicked", G_CALLBACK(on_stop), a);
    gtk_box_pack_start(GTK_BOX(tb), bc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), bp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), a->btn_pause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), bs, FALSE, FALSE, 0);
    a->sel_label = gtk_label_new("");
    gtk_widget_set_halign(a->sel_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand(a->sel_label, TRUE);
    gtk_box_pack_start(GTK_BOX(tb), a->sel_label, TRUE, TRUE, 0);

    GtkWidget *nb = gtk_notebook_new();

    /* lower area: spectrum options + transport + tabs */
    GtkWidget *lower = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(lower), fb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lower), tb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lower), nb, TRUE, TRUE, 0);

    /* nested vertical panes: waveform / spectrum row / controls, all
     * resizable by dragging the dividers                                */
    GtkWidget *vp2 = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(vp2), hp, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(vp2), lower, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(vp2), 160);
    GtkWidget *vp1 = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(vp1), a->wf, TRUE, TRUE);
    gtk_paned_pack2(GTK_PANED(vp1), vp2, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(vp1), 150);
    gtk_box_pack_start(GTK_BOX(outer), vp1, TRUE, TRUE, 0);

    /* ---- Chain tab: two columns inside a scrolled window ------------ */
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(cols), 6);
    GtkWidget *col1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *col2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(cols), col1, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cols), col2, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(cols),
                             gtk_label_new("Chain"));

    /* column 1: waveshaper + filters */
    GtkWidget *g1;
    frame_grid(col1, "Waveshaper", &g1);
    a->shape_combo = gtk_combo_box_text_new();
    const char *shapes[] = { "none", "tanh (odd)", "tube (even+odd)",
                             "h2 (pure 2nd)" };
    for (int i = 0; i < 4; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->shape_combo),
                                       shapes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->shape_combo), 0);
    gtk_widget_set_tooltip_text(a->shape_combo,
        "Static nonlinearity applied to the signal (oversampled).\n"
        "tanh: y=tanh(g*x)/g, symmetric, odd harmonics (3rd, 5th...)\n"
        "tube: biased tanh, asymmetric, even+odd harmonics\n"
        "h2: y=x+a*x^2, pure 2nd harmonic, level set by the H2 slider");
    gtk_grid_attach(GTK_GRID(g1), gtk_label_new("Shape"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), a->shape_combo, 1, 0, 1, 1);
    a->drive = add_scale(g1, 1, "Drive", 0.5, 10.0, 0.1, 2.0,
        "Dimensionless pre-gain g into the nonlinearity (tanh/tube). "
        "Sets how hard the signal pushes into saturation; output is "
        "renormalized so level stays comparable. ~1 = subtle, 2-3 = "
        "warm, 5+ = obvious distortion.");
    a->bias = add_scale(g1, 2, "Bias", 0.0, 1.0, 0.01, 0.2,
        "Dimensionless DC offset b inside the tube shape "
        "tanh(g*(x+b)). Breaks waveform symmetry, which is what "
        "creates EVEN harmonics (the 'tube' character). 0 = symmetric "
        "(odd only, same as tanh); 0.2-0.4 typical.");
    a->h2db = add_scale(g1, 3, "H2 (dB re f0)", -60.0, -10.0, 1.0, -30.0,
        "h2 shape only: target level of the 2nd harmonic in dB below "
        "the fundamental, calibrated at the file's PEAK level -- a sine "
        "at peak amplitude gets exactly this H2; quieter passages get "
        "proportionally less (x^2 scales with level).");
    a->os_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->os_combo),
                                   "2x (fast)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->os_combo), "4x");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->os_combo),
                                   "8x (best)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->os_combo), 2);
    gtk_widget_set_tooltip_text(a->os_combo,
        "Oversampling around the nonlinearity, to keep generated "
        "harmonics from aliasing back into the audio band. Higher = "
        "cleaner but slower to render.");
    gtk_grid_attach(GTK_GRID(g1), gtk_label_new("Oversampling"),
                    0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), a->os_combo, 1, 4, 1, 1);
    g_signal_connect(a->shape_combo, "changed",
                     G_CALLBACK(on_shaper_changed), a);
    g_signal_connect(a->drive, "value-changed",
                     G_CALLBACK(on_shaper_changed), a);
    g_signal_connect(a->bias, "value-changed",
                     G_CALLBACK(on_shaper_changed), a);
    g_signal_connect(a->h2db, "value-changed",
                     G_CALLBACK(on_shaper_changed), a);

    GtkWidget *g6;
    frame_grid(col1, "Filters (RBJ biquads, applied after media)", &g6);
    filter_row(g6, 0, "High-pass", &a->hp_chk,
               &a->hp_f, 20, 2000, 80, &a->hp_q, 0.3, 4.0, 0.71, "Q",
               "2nd-order high-pass: removes content below the corner "
               "frequency (12 dB/oct).");
    filter_row(g6, 1, "Low-pass", &a->lp_chk,
               &a->lp_f, 500, 20000, 12000, &a->lp_q, 0.3, 4.0, 0.71, "Q",
               "2nd-order low-pass: removes content above the corner "
               "frequency (12 dB/oct).");
    filter_row(g6, 2, "Band-pass", &a->bp_chk,
               &a->bp_f, 50, 15000, 1000, &a->bp_q, 0.3, 8.0, 1.5, "Q",
               "2nd-order band-pass around the center frequency; higher "
               "Q = narrower band.");
    filter_row(g6, 3, "Band window", &a->win_chk,
               &a->win_lo, 20, 5000, 300, &a->win_hi, 200, 20000, 3000,
               "Hz (hi)",
               "Passes only [f_lo, f_hi]: a high-pass at the first "
               "frequency plus a low-pass at the second (Butterworth Q). "
               "Try 300..3000 Hz for the 'telephone' band.");

    /* column 2: media + render */
    GtkWidget *g2;
    frame_grid(col2,
               "Media (both on = tape then vinyl, like vinyl cut from a "
               "tape master)", &g2);
    a->vinyl_chk = gtk_check_button_new_with_label("Vinyl");
    a->tape_chk  = gtk_check_button_new_with_label("Tape");
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(hb), a->vinyl_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), a->tape_chk, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g2), hb, 0, 0, 2, 1);
    a->wow      = add_scale(g2, 1, "Wow (cents)",      0.0, 30.0, 0.5, 8.0,
        "Peak pitch deviation of the slow speed instability "
        "(0.55 Hz for vinyl, i.e. 33 rpm eccentricity).");
    a->flutter  = add_scale(g2, 2, "Flutter (cents)",  0.0, 15.0, 0.25, 2.5,
        "Peak pitch deviation of the fast (9 Hz) tape speed "
        "instability.");
    a->hiss     = add_scale(g2, 3, "Hiss (dBFS)",   -100.0, -40.0, 1.0,
                            -60.0,
        "RMS level of the medium's noise floor (pink for vinyl, "
        "white-ish for tape).");
    a->crk_rate = add_scale(g2, 4, "Crackle (/s)",     0.0, 50.0, 1.0, 12.0,
        "Mean vinyl tick rate (Poisson process).");
    a->crk_db   = add_scale(g2, 5, "Crackle (dBFS)", -60.0, -10.0, 1.0,
                            -33.0, "Peak level of vinyl ticks.");
    a->hf_loss  = add_scale(g2, 6, "Tape HF loss",     0.0, 1.0, 0.01, 0.35,
        "Strength (0..1) of level-dependent treble loss: loud passages "
        "dull, quiet passages stay open -- the cassette signature.");
    a->bump     = add_scale(g2, 7, "Head bump (dB)",   0.0, 8.0, 0.1, 3.0,
        "Low-frequency resonance of the tape head (peaking EQ at "
        "65 Hz).");
    a->bw       = add_scale(g2, 8, "Bandwidth (Hz)", 8000.0, 20000.0,
                            250.0, 15000.0,
        "Overall bandwidth of the simulated medium (low-pass).");

    GtkWidget *g3;
    frame_grid(col2, "Render", &g3);
    GtkWidget *rh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->bits_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->bits_combo),
                                   "16-bit");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->bits_combo),
                                   "24-bit");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->bits_combo), 0);
    a->match_chk   = gtk_check_button_new_with_label("Match RMS");
    a->selonly_chk = gtk_check_button_new_with_label("Selection only");
    a->render_btn  = gtk_button_new_with_label("Render to file...");
    g_signal_connect(a->render_btn, "clicked", G_CALLBACK(on_render), a);
    gtk_box_pack_start(GTK_BOX(rh), a->bits_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rh), a->match_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rh), a->selonly_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rh), a->render_btn, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g3), rh, 0, 0, 1, 1);

    /* ---- Listening-test tab ----------------------------------------- */
    GtkWidget *tab2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(tab2), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), scrolled(tab2),
                             gtk_label_new("Listening test"));

    GtkWidget *g4;
    frame_grid(tab2, "Session (uses the waveform selection, max 30 s)",
               &g4);
    a->abx_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->abx_mode),
                                   "ABX (fixed level)");
    size_t nsc;
    const sc_info *tabsc = sc_table(&nsc);
    for (size_t i = 0; i < nsc; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "Staircase: %s", tabsc[i].name);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->abx_mode), nm);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->abx_mode), 0);
    gtk_grid_attach(GTK_GRID(g4), gtk_label_new("Mode"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g4), a->abx_mode, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g4), gtk_label_new("Trials (ABX)"),
                    0, 1, 1, 1);
    a->abx_trials = gtk_spin_button_new_with_range(4, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->abx_trials), 16);
    gtk_grid_attach(GTK_GRID(g4), a->abx_trials, 1, 1, 1, 1);
    a->abx_abs = gtk_check_button_new_with_label(
        "Absolute ID (no reference -- judge a single sound)");
    gtk_widget_set_tooltip_text(a->abx_abs,
        "Instead of comparing against references, each trial plays ONE "
        "sound (randomly clean or processed) and you say which it was. "
        "Measures detection without a comparison anchor -- run the same "
        "staircase with and without this to measure your adaptation "
        "gap.");
    gtk_grid_attach(GTK_GRID(g4), a->abx_abs, 0, 2, 2, 1);
    a->abx_start = gtk_button_new_with_label("Start session");
    g_signal_connect(a->abx_start, "clicked", G_CALLBACK(on_abx_start), a);
    gtk_grid_attach(GTK_GRID(g4), a->abx_start, 0, 3, 1, 1);
    a->abx_end = gtk_button_new_with_label("End session");
    gtk_widget_set_tooltip_text(a->abx_end,
        "Staircase sessions run until 9 reversals (usually 25-45 "
        "trials), not a fixed count. This ends one early: with 6+ "
        "reversals you still get a threshold estimate.");
    g_signal_connect(a->abx_end, "clicked", G_CALLBACK(on_abx_end), a);
    gtk_grid_attach(GTK_GRID(g4), a->abx_end, 1, 3, 1, 1);

    GtkWidget *g5;
    frame_grid(tab2, "Trial", &g5);
    a->btn_p1 = gtk_button_new_with_label("Play A");
    a->btn_p2 = gtk_button_new_with_label("Play B");
    a->btn_px = gtk_button_new_with_label("Play X");
    a->btn_a1 = gtk_button_new_with_label("X = A");
    a->btn_a2 = gtk_button_new_with_label("X = B");
    g_object_set_data(G_OBJECT(a->btn_p1), "idx", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(a->btn_p2), "idx", GINT_TO_POINTER(2));
    g_object_set_data(G_OBJECT(a->btn_a1), "idx", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(a->btn_a2), "idx", GINT_TO_POINTER(2));
    g_signal_connect(a->btn_p1, "clicked", G_CALLBACK(on_play_ref), a);
    g_signal_connect(a->btn_p2, "clicked", G_CALLBACK(on_play_ref), a);
    g_signal_connect(a->btn_px, "clicked", G_CALLBACK(on_play_x), a);
    g_signal_connect(a->btn_a1, "clicked", G_CALLBACK(on_answer), a);
    g_signal_connect(a->btn_a2, "clicked", G_CALLBACK(on_answer), a);
    GtkWidget *th1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(th1), a->btn_p1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(th1), a->btn_p2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(th1), a->btn_px, FALSE, FALSE, 0);
    GtkWidget *th2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(th2), a->btn_a1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(th2), a->btn_a2, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g5), th1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g5), th2, 0, 1, 1, 1);
    a->abx_status = gtk_label_new("no session");
    gtk_widget_set_halign(a->abx_status, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g5), a->abx_status, 0, 2, 1, 1);
    a->abx_help = gtk_label_new(HELP_IDLE);
    gtk_label_set_line_wrap(GTK_LABEL(a->abx_help), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->abx_help), 52);
    gtk_widget_set_halign(a->abx_help, GTK_ALIGN_START);
    gtk_widget_set_valign(a->abx_help, GTK_ALIGN_START);
    gtk_widget_set_hexpand(a->abx_help, TRUE);
    gtk_widget_set_margin_start(a->abx_help, 16);
    gtk_grid_attach(GTK_GRID(g5), a->abx_help, 1, 0, 1, 3);
    a->abx_result = gtk_label_new("");
    gtk_widget_set_halign(a->abx_result, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(a->abx_result), TRUE);
    gtk_grid_attach(GTK_GRID(g5), a->abx_result, 0, 3, 2, 1);
    a->abx_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->abx_log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->abx_log_view), TRUE);
    GtkWidget *lsw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lsw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(lsw, -1, 170);
    gtk_container_add(GTK_CONTAINER(lsw), a->abx_log_view);
    gtk_grid_attach(GTK_GRID(g5), lsw, 0, 4, 2, 1);

    a->status = gtk_label_new("ready");
    gtk_widget_set_halign(a->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), a->status, FALSE, FALSE, 0);

    sess_buttons(a, 0);
    gtk_widget_show_all(a->win);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    static App a;
    a.rng = (uint64_t)time(NULL) * 2654435761u + 12345u;
    a.pl  = player_new();
    g_mutex_init(&a.live_mx);
    a.live_seek = -1;
    build_ui(&a);
    if (argc > 1) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(a.in_chooser),
                                      argv[1]);
        load_input(&a);
    }
    g_timeout_add(33, tick, &a);
    gtk_main();
    player_free(a.pl);
    return 0;
}
