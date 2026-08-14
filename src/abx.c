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

/* ABX / threshold harness.
 *
 *   audiotard abx in.(wav|flac) [chain options] [abx options]
 *
 * Two procedures:
 *
 * FIXED-LEVEL ABX (default): renders A (clean) and B (processed,
 * RMS-matched), then N trials in which X is randomly A or B. You listen
 * and answer. Score is tested against chance with an exact binomial test.
 *
 * ADAPTIVE STAIRCASE (--staircase PARAM): each trial renders two
 * intervals -- one clean, one processed at the current level -- in random
 * order; you say which was processed (2IFC, criterion-free). A 2-down /
 * 1-up rule tracks the ~70.7%-correct point; the step halves at each
 * reversal (6 -> 1 dB), stops after 9 reversals, and reports the mean of
 * the last 6 as your detection threshold, in the parameter's own units.
 *
 * PARAM: h2db | hiss-db | crackle-db | wow-cents | flutter-cents | drive
 * (dB-valued params step in dB; the others step in dB of their value.)
 *
 * ABX options:
 *   --trials N       fixed-mode trial count (default 16)
 *   --staircase P    adaptive mode on parameter P (start = its CLI value)
 *   --player CMD     shell command to play a file; %s -> path. If absent
 *                    you play the files yourself from the session dir.
 *   --outdir D       session directory (default ./abx_session)
 *   --seed N         randomization seed (default: time-based)
 *   --sim S          no listener: simulate one with threshold S (in the
 *                    staircase scalar units) to validate the procedure
 *
 * All trial files are 16-bit dithered with per-file seeds, so identical
 * program material never yields bit-identical files.
 */
#include "audio_io.h"
#include "chain.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* ---------------------------------------------------------------------- */

static void die(const char *msg)
{
    fprintf(stderr, "audiotard abx: %s\n", msg);
    exit(1);
}

static double frand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;  x ^= x << 25;  x ^= x >> 27;
    *s = x;
    return (double)((x * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

static void play(const char *player, const char *path)
{
    if (!player) return;
    char cmd[1024];
    const char *pct = strstr(player, "%s");
    if (pct)
        snprintf(cmd, sizeof cmd, "%.*s%s%s",
                 (int)(pct - player), player, path, pct + 2);
    else
        snprintf(cmd, sizeof cmd, "%s %s", player, path);
    if (system(cmd) != 0)
        fprintf(stderr, "(player command failed)\n");
}

static char ask(const char *prompt, const char *valid)
{
    char line[64];
    for (;;) {
        fprintf(stderr, "%s", prompt);
        fflush(stderr);
        if (!fgets(line, sizeof line, stdin)) die("stdin closed");
        char c = line[0];
        if (c && strchr(valid, c)) return c;
    }
}

/* ---------------------------------------------------------------------- */
/* Staircase parameter plumbing                                           */
/* ---------------------------------------------------------------------- */

/* Simulated listener: P(correct) = 0.5 + 0.5 * sigmoid((s - thr)/2).
 * ~75% correct at threshold, so the 70.7% staircase converges near it.  */
static int sim_answer_correct(double s, double thr, uint64_t *rng)
{
    double pc = 0.5 + 0.5 / (1.0 + exp(-(s - thr) / 2.0));
    return frand(rng) < pc;
}

/* ---------------------------------------------------------------------- */

int abx_main(int argc, char **argv)
{
    if (argc < 2) die("usage: audiotard abx in.(wav|flac) [options]");
    const char *inpath = argv[1];

    chain_params cp;
    chain_defaults(&cp);

    int         trials  = 16;
    const char *sc_name = NULL;
    const char *player  = NULL;
    const char *outdir  = "abx_session";
    uint64_t    seed    = (uint64_t)time(NULL) * 2654435761u + 1;
    int         use_sim = 0;
    double      sim_thr = 0.0;

    for (int i = 2; i < argc; i++) {
        if (chain_parse(&cp, argc, argv, &i)) continue;
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--trials")    && v) { trials  = atoi(v); i++; }
        else if (!strcmp(a, "--staircase") && v) { sc_name = v;       i++; }
        else if (!strcmp(a, "--player")    && v) { player  = v;       i++; }
        else if (!strcmp(a, "--outdir")    && v) { outdir  = v;       i++; }
        else if (!strcmp(a, "--seed")      && v) {
            seed = (uint64_t)strtoull(v, NULL, 10) | 1u; i++;
        }
        else if (!strcmp(a, "--sim") && v) {
            use_sim = 1; sim_thr = atof(v); i++;
        }
        else die("unknown or incomplete option");
    }
    if (!cp.use_shape && !cp.use_vinyl && !cp.use_tape && cp.neq == 0)
        die("no effect enabled -- nothing to test");
    if (trials < 4 || trials > 200) die("--trials must be 4..200");

    audio_buf in;
    if (audio_read(inpath, &in) != 0) die("cannot read input");
    if (in.nframes > 30u * in.rate)
        fprintf(stderr, "note: input is %.0f s; short loops (5-15 s) make "
                "for much better ABX trials\n",
                (double)in.nframes / in.rate);

    MKDIR(outdir);
    char path_a[512], path_b[512], path_x[512], path_1[512], path_2[512],
         path_log[512];
    snprintf(path_a, sizeof path_a, "%s/a_clean.wav",     outdir);
    snprintf(path_b, sizeof path_b, "%s/b_processed.wav", outdir);
    snprintf(path_x, sizeof path_x, "%s/x.wav",           outdir);
    snprintf(path_1, sizeof path_1, "%s/int1.wav",        outdir);
    snprintf(path_2, sizeof path_2, "%s/int2.wav",        outdir);
    snprintf(path_log, sizeof path_log, "%s/log.txt",     outdir);
    FILE *log = fopen(path_log, "a");

    /* ================= adaptive staircase ============================ */
    if (sc_name) {
        const sc_info *si = sc_find(sc_name);
        if (!si) die("unknown staircase parameter");
        const char *err = sc_check_enabled(&cp, si->id);
        if (err) die(err);

        double s     = sc_get(&cp, si->id);
        double step  = 6.0;
        int    ncorr = 0, nrev = 0, trial = 0;
        int    dir   = 0;                    /* -1 down, +1 up            */
        double revs[16];
        char   vbuf[64];

        fprintf(stderr, "\n== adaptive threshold: %s ==\n", si->name);
        if (!use_sim && !player)
            fprintf(stderr, "no --player given: play %s and %s yourself "
                    "each trial.\n", path_1, path_2);

        while (nrev < 9 && trial < 120) {
            trial++;
            if (s < si->lo) s = si->lo;
            if (s > si->hi) s = si->hi;
            sc_set(&cp, si->id, s);
            sc_print_value(si, s, vbuf, sizeof vbuf);

            int proc_is_2 = frand(&seed) < 0.5;
            int correct;

            if (use_sim) {
                correct = sim_answer_correct(s, sim_thr, &seed);
            } else {
                audio_buf b;
                if (chain_render(&in, &b, &cp, 1) != 0) die("render failed");
                audio_write_wav_seeded(proc_is_2 ? path_1 : path_2, &in,
                                       16, 1, seed ^ 0xA1);
                audio_write_wav_seeded(proc_is_2 ? path_2 : path_1, &b,
                                       16, 1, seed ^ 0xB2);
                audio_free(&b);

                fprintf(stderr, "\ntrial %d  [level: %s]\n", trial, vbuf);
                char c;
                do {
                    if (player) { play(player, path_1); play(player, path_2); }
                    c = ask("which interval was processed? "
                            "(1/2, r=replay, q=quit): ", "12rq");
                } while (c == 'r');
                if (c == 'q') break;
                correct = (c == '2') == (proc_is_2 != 0);
            }

            if (log) fprintf(log, "staircase %s trial %d level %s %s\n",
                             si->name, trial, vbuf,
                             correct ? "correct" : "wrong");

            int move = 0;                    /* +1 harder(down), -1 easier */
            if (correct) {
                if (++ncorr >= 2) { ncorr = 0; move = +1; }
            } else { ncorr = 0; move = -1; }

            if (move) {
                int nd = (move > 0) ? -1 : +1;      /* scalar direction  */
                if (dir != 0 && nd != dir && nrev < 16) {
                    revs[nrev++] = s;
                    step *= 0.5;
                    if (step < 1.0) step = 1.0;
                }
                dir = nd;
                s  += nd * step;
            }
        }

        if (nrev >= 6) {
            double t = 0.0;
            for (int i = nrev - 6; i < nrev; i++) t += revs[i];
            t /= 6.0;
            sc_print_value(si, t, vbuf, sizeof vbuf);
            fprintf(stderr, "\n== detection threshold (%s): %s "
                    "(%d trials, %d reversals) ==\n",
                    si->name, vbuf, trial, nrev);
            if (log) fprintf(log, "RESULT %s threshold %s\n", si->name, vbuf);
        } else {
            fprintf(stderr, "\nnot enough reversals for an estimate "
                    "(%d) -- session too short or level pinned at a "
                    "bound.\n", nrev);
        }
    }
    /* ================= fixed-level ABX =============================== */
    else {
        audio_buf b;
        if (chain_render(&in, &b, &cp, 1) != 0) die("render failed");
        audio_write_wav_seeded(path_a, &in, 16, 1, seed ^ 0x0A);
        audio_write_wav_seeded(path_b, &b,  16, 1, seed ^ 0x0B);

        fprintf(stderr, "\n== ABX: %d trials ==\nreferences: %s (clean), "
                "%s (processed, RMS-matched)\n", trials, path_a, path_b);
        if (!use_sim && !player)
            fprintf(stderr, "no --player given: play the files yourself "
                    "each trial.\n");

        int k = 0, n = 0;
        for (int t = 1; t <= trials; t++) {
            int x_is_b = frand(&seed) < 0.5;
            int correct;

            if (use_sim) {
                double s = 0.0;   /* crude: reuse h2db as the level      */
                s = cp.h2db;
                correct = sim_answer_correct(s, sim_thr, &seed);
            } else {
                audio_write_wav_seeded(path_x, x_is_b ? &b : &in,
                                       16, 1, seed ^ (uint64_t)t);
                fprintf(stderr, "\ntrial %d/%d -- X written to %s\n",
                        t, trials, path_x);
                char c;
                do {
                    if (player) play(player, path_x);
                    c = ask("is X the clean (a) or processed (b) file? "
                            "(a/b, r=replay, A/B=play refs, q=quit): ",
                            "abrABq");
                    if (c == 'A') { play(player, path_a); c = 'r'; }
                    else if (c == 'B') { play(player, path_b); c = 'r'; }
                } while (c == 'r');
                if (c == 'q') break;
                correct = (c == 'b') == (x_is_b != 0);
            }
            n++;
            k += correct;
            if (log) fprintf(log, "abx trial %d %s\n", t,
                             correct ? "correct" : "wrong");
        }

        if (n > 0) {
            double p = binom_p(k, n);
            fprintf(stderr, "\n== result: %d/%d correct, exact binomial "
                    "p = %.4f ==\n", k, n, p);
            fprintf(stderr, p < 0.05
                ? "you are reliably distinguishing them (p < 0.05).\n"
                : "no reliable evidence you can tell them apart -- "
                  "consistent with guessing.\n");
            if (log) fprintf(log, "RESULT abx %d/%d p=%.4f\n", k, n, p);
        }
        audio_free(&b);
    }

    if (log) fclose(log);
    audio_free(&in);
    return 0;
}
