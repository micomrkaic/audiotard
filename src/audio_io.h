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

/* audiotard I/O layer.
 *
 * Read WAV or FLAC into interleaved doubles; write WAV at 16 or 24 bit
 * with TPDF dither. FLAC output is not supported (dr_flac decodes only) --
 * for a listening-test tool, lossless WAV out is what you want anyway.
 */
#ifndef AUDIOTARD_IO_H
#define AUDIOTARD_IO_H

#include <stddef.h>

typedef struct {
    double  *data;      /* interleaved, [-1, 1] nominal        */
    size_t   nframes;
    unsigned channels;
    unsigned rate;      /* Hz                                  */
} audio_buf;

/* Detects WAV vs FLAC by content, not extension. Returns 0 on success. */
int  audio_read(const char *path, audio_buf *out);

/* bits: 16 or 24. dither: apply +/-1 LSB TPDF before quantizing (do it
 * unless you have a reason not to). Returns 0 on success.              */
int  audio_write_wav(const char *path, const audio_buf *b,
                     int bits, int dither);

/* Same, with an explicit dither seed. The ABX harness writes each trial
 * file with a fresh seed so identical program material never produces
 * bit-identical files (defeats md5-based "listening").                 */
int  audio_write_wav_seeded(const char *path, const audio_buf *b,
                            int bits, int dither,
                            unsigned long long seed);

void audio_free(audio_buf *b);

/* Interleaved-buffer helpers used by the CLI. */
double audio_peak(const audio_buf *b);
double audio_rms(const audio_buf *b);

#endif
