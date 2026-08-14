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

/* audiotard CLI.
 *
 * File processing:
 *   audiotard in.(wav|flac) out.wav [chain options] [output options]
 *
 * Listening tests:
 *   audiotard abx in.(wav|flac) [chain options] [abx options]
 *   (see src/abx.c header for the abx option list)
 *
 * Chain options (both modes; chain: gain-in -> shaper -> tape -> vinyl -> EQ):
 *   --shape none|tanh|tube|h2   --drive G --bias B --h2db D --os L
 *   --vinyl --tape
 *   --wow-cents C --wow-rate HZ --drift-cents C
 *   --flutter-cents C --flutter-rate HZ
 *   --hiss-db D --crackle-rate N --crackle-db D
 *   --bump-db D --bump-hz F --hf-loss S --bw-hz F
 *   --eq TYPE:FREQ:Q:GAIN_DB    (peak|ls|hs|lp|hp|bp, repeatable)
 *   --gain-in DB
 *
 * Output options (file mode):
 *   --bits 16|24  --no-dither  --match-rms
 */
#include "audio_io.h"
#include "chain.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int abx_main(int argc, char **argv);

static void die(const char *msg)
{
    fprintf(stderr, "audiotard: %s\n", msg);
    exit(1);
}

static double lin2db(double v) { return 20.0 * log10(v + 1e-300); }

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--version") ||
                     !strcmp(argv[1], "-V"))) {
        printf("audiotard %s\n", AUDIOTARD_VERSION);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "abx"))
        return abx_main(argc - 1, argv + 1);

    if (argc < 3)
        die("usage: audiotard in.(wav|flac) out.wav [options]\n"
            "       audiotard abx in.(wav|flac) [options]");

    const char *inpath  = argv[1];
    const char *outpath = argv[2];

    chain_params cp;
    chain_defaults(&cp);

    int bits = 16, dither = 1, match = 0;

    for (int i = 3; i < argc; i++) {
        if (chain_parse(&cp, argc, argv, &i)) continue;
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--bits") && v) { bits = atoi(v); i++; }
        else if (!strcmp(a, "--no-dither"))   dither = 0;
        else if (!strcmp(a, "--match-rms"))   match  = 1;
        else die("unknown or incomplete option");
    }

    audio_buf in;
    if (audio_read(inpath, &in) != 0) die("cannot read input");
    fprintf(stderr, "in : %zu frames, %u ch, %u Hz, peak %.2f dBFS, "
            "rms %.2f dBFS\n", in.nframes, in.channels, in.rate,
            lin2db(audio_peak(&in)), lin2db(audio_rms(&in)));

    audio_buf out;
    if (chain_render(&in, &out, &cp, match) != 0)
        die("render failed (silent input with --shape h2?)");

    double pk = audio_peak(&out);
    if (pk > 1.0)
        fprintf(stderr, "warning: output peaks at %+.2f dBFS -- clipping "
                "at the quantizer; consider --gain-in %.1f\n",
                lin2db(pk), -ceil(lin2db(pk) * 10.0) / 10.0);
    fprintf(stderr, "out: peak %.2f dBFS, rms %.2f dBFS\n",
            lin2db(pk), lin2db(audio_rms(&out)));

    if (audio_write_wav(outpath, &out, bits, dither) != 0)
        die("cannot write output");

    audio_free(&out);
    audio_free(&in);
    return 0;
}
