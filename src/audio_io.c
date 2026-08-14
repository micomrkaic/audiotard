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

#include "audio_io.h"

#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Reading                                                                */
/* ---------------------------------------------------------------------- */

static int sniff_flac(const char *path)
{
    unsigned char magic[4] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    return got == 4 && memcmp(magic, "fLaC", 4) == 0;
}

int audio_read(const char *path, audio_buf *out)
{
    memset(out, 0, sizeof *out);

    if (sniff_flac(path)) {
        unsigned int ch, rate;
        drflac_uint64 nframes;
        float *f32 = drflac_open_file_and_read_pcm_frames_f32(
                         path, &ch, &rate, &nframes, NULL);
        if (!f32) return -1;

        out->data = malloc((size_t)nframes * ch * sizeof *out->data);
        if (!out->data) { drflac_free(f32, NULL); return -1; }
        for (size_t i = 0; i < (size_t)nframes * ch; i++)
            out->data[i] = (double)f32[i];
        drflac_free(f32, NULL);

        out->nframes  = (size_t)nframes;
        out->channels = ch;
        out->rate     = rate;
        return 0;
    }

    /* WAV path: dr_wav converts any source format (PCM 8/16/24/32,
     * float, ADPCM...) to f32 for us.                                    */
    unsigned int ch, rate;
    drwav_uint64 nframes;
    float *f32 = drwav_open_file_and_read_pcm_frames_f32(
                     path, &ch, &rate, &nframes, NULL);
    if (!f32) return -1;

    out->data = malloc((size_t)nframes * ch * sizeof *out->data);
    if (!out->data) { drwav_free(f32, NULL); return -1; }
    for (size_t i = 0; i < (size_t)nframes * ch; i++)
        out->data[i] = (double)f32[i];
    drwav_free(f32, NULL);

    out->nframes  = (size_t)nframes;
    out->channels = ch;
    out->rate     = rate;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Writing (16/24-bit PCM WAV with TPDF dither)                           */
/* ---------------------------------------------------------------------- */

/* xorshift64* -- deterministic runs; uniform in [0,1). */
static double frand(drwav_uint64 *s)
{
    drwav_uint64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return (double)((x * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

/* Quantize v in [-1,1] to a signed integer with 'levels' = 2^(bits-1),
 * with optional TPDF dither of +/-1 LSB, symmetric clamp.               */
static long quantize(double v, double levels, int dither, drwav_uint64 *seed)
{
    double d = dither ? (frand(seed) - frand(seed)) : 0.0;   /* TPDF     */
    double q = floor(v * levels + d + 0.5);
    double lim = levels - 1.0;
    if (q >  lim) q =  lim;
    if (q < -levels) q = -levels;
    return (long)q;
}

int audio_write_wav(const char *path, const audio_buf *b,
                    int bits, int dither)
{
    return audio_write_wav_seeded(path, b, bits, dither,
                                  0x9e3779b97f4a7c15ULL);
}

int audio_write_wav_seeded(const char *path, const audio_buf *b,
                           int bits, int dither, unsigned long long seed0)
{
    if (bits != 16 && bits != 24) return -1;

    drwav_data_format fmt = {
        .container     = drwav_container_riff,
        .format        = DR_WAVE_FORMAT_PCM,
        .channels      = b->channels,
        .sampleRate    = b->rate,
        .bitsPerSample = (drwav_uint32)bits
    };
    drwav w;
    if (!drwav_init_file_write(&w, path, &fmt, NULL)) return -1;

    size_t total = b->nframes * b->channels;
    drwav_uint64 seed = (drwav_uint64)seed0 | 1u;   /* xorshift needs != 0 */
    int rc = 0;

    if (bits == 16) {
        drwav_int16 *tmp = malloc(total * sizeof *tmp);
        if (!tmp) { drwav_uninit(&w); return -1; }
        for (size_t i = 0; i < total; i++)
            tmp[i] = (drwav_int16)quantize(b->data[i], 32768.0,
                                           dither, &seed);
        if (drwav_write_pcm_frames(&w, b->nframes, tmp) != b->nframes)
            rc = -1;
        free(tmp);
    } else {
        unsigned char *tmp = malloc(total * 3);
        if (!tmp) { drwav_uninit(&w); return -1; }
        for (size_t i = 0; i < total; i++) {
            long q = quantize(b->data[i], 8388608.0, dither, &seed);
            tmp[3*i + 0] = (unsigned char)( q        & 0xFF);
            tmp[3*i + 1] = (unsigned char)((q >>  8) & 0xFF);
            tmp[3*i + 2] = (unsigned char)((q >> 16) & 0xFF);
        }
        if (drwav_write_pcm_frames(&w, b->nframes, tmp) != b->nframes)
            rc = -1;
        free(tmp);
    }

    drwav_uninit(&w);
    return rc;
}

void audio_free(audio_buf *b)
{
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

double audio_peak(const audio_buf *b)
{
    double p = 0.0;
    size_t total = b->nframes * b->channels;
    for (size_t i = 0; i < total; i++) {
        double a = fabs(b->data[i]);
        if (a > p) p = a;
    }
    return p;
}

double audio_rms(const audio_buf *b)
{
    double s = 0.0;
    size_t total = b->nframes * b->channels;
    if (!total) return 0.0;
    for (size_t i = 0; i < total; i++)
        s += b->data[i] * b->data[i];
    return sqrt(s / (double)total);
}
