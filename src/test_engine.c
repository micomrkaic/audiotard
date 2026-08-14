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

/* Engine self-test. No audio files needed: synthesizes sines, measures
 * harmonic and alias components with Goertzel, prints a report.
 *
 *   1. WS_TUBE on 1 kHz: harmonic profile H1..H5 + THD.
 *   2. WS_H2 calibrated for -30 dB H2: verify measurement hits target.
 *   3. WS_TUBE on 15 kHz, drive 4: alias products at 14.1 kHz (folded H2)
 *      and 900 Hz (folded H3), with L=1 (no oversampling) vs L=8.
 */
#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS      44100.0
#define SECS    2
#define NTOT    ((size_t)(FS * SECS))
#define WIN     ((size_t)FS)      /* 1 s window: integer cycles for      */
#define OFF     ((size_t)8192)    /* integer-Hz tones => leak-free       */

static double goertzel_amp(const double *x, size_t n, double f, double fs)
{
    double w  = 2.0 * M_PI * f / fs;
    double c  = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double re = s1 - s2 * cos(w);
    double im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

static double db(double x) { return 20.0 * log10(x + 1e-300); }

static void make_sine(double *x, size_t n, double f, double a, double fs)
{
    for (size_t i = 0; i < n; i++)
        x[i] = a * sin(2.0 * M_PI * f * (double)i / fs);
}

int main(void)
{
    double *x = malloc(NTOT * sizeof *x);
    double *y = malloc(NTOT * sizeof *y);
    if (!x || !y) return 1;

    /* ---- 1: tube harmonic profile ---------------------------------- */
    {
        ws_params p = { .shape = WS_TUBE, .drive = 2.0, .bias = 0.2 };
        make_sine(x, NTOT, 1000.0, 0.5, FS);
        ws_process(x, y, NTOT, FS, 8, &p);

        double h1 = goertzel_amp(y + OFF, WIN, 1000.0, FS);
        printf("[tube g=2 b=0.2, 1 kHz @ 0.5, L=8]\n");
        double thd2 = 0.0;
        for (int k = 2; k <= 5; k++) {
            double hk = goertzel_amp(y + OFF, WIN, 1000.0 * k, FS);
            printf("  H%d: %7.2f dB re H1\n", k, db(hk / h1));
            thd2 += (hk / h1) * (hk / h1);
        }
        printf("  THD(H2..H5): %.3f %%\n\n", 100.0 * sqrt(thd2));
    }

    /* ---- 2: calibrated pure H2 -------------------------------------- */
    {
        double target = -30.0;
        ws_params p = { .shape = WS_H2,
                        .h2    = ws_h2_coeff(target, 0.5) };
        make_sine(x, NTOT, 1000.0, 0.5, FS);
        ws_process(x, y, NTOT, FS, 8, &p);

        double h1 = goertzel_amp(y + OFF, WIN, 1000.0, FS);
        double h2 = goertzel_amp(y + OFF, WIN, 2000.0, FS);
        double h3 = goertzel_amp(y + OFF, WIN, 3000.0, FS);
        printf("[pure H2 calibrated to %.0f dB, 1 kHz @ 0.5, L=8]\n", target);
        printf("  H2: %7.2f dB re H1   (target %.2f)\n", db(h2 / h1), target);
        printf("  H3: %7.2f dB re H1   (should be ~ -inf)\n\n", db(h3 / h1));
    }

    /* ---- 3: aliasing, with vs without oversampling ------------------ */
    {
        ws_params p = { .shape = WS_TUBE, .drive = 4.0, .bias = 0.3 };
        make_sine(x, NTOT, 15000.0, 0.5, FS);

        printf("[tube g=4 b=0.3, 15 kHz @ 0.5 -- alias products]\n");
        printf("  H2=30 kHz folds to 14100 Hz; H3=45 kHz folds to 900 Hz\n");
        for (int L = 1; L <= 8; L += 7) {
            ws_process(x, y, NTOT, FS, L, &p);
            double h1 = goertzel_amp(y + OFF, WIN, 15000.0, FS);
            double a2 = goertzel_amp(y + OFF, WIN, 14100.0, FS);
            double a3 = goertzel_amp(y + OFF, WIN,   900.0, FS);
            printf("  L=%d:  alias(14100): %7.2f dB   alias(900): %7.2f dB"
                   "  (re H1)\n", L, db(a2 / h1), db(a3 / h1));
        }
    }

    free(x);
    free(y);
    return 0;
}
