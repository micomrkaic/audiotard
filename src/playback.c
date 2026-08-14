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

#define _POSIX_C_SOURCE 200809L   /* nanosleep + sane ALSA headers under
                                     -std=c17 (strict ANSI)             */
#include "playback.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 1024
#define SRING (1u << 14)          /* stream ring: 16384 frames (~370 ms) */

struct player {
    pthread_t     th;
    int           th_active;

    const double *data;
    size_t        n;
    unsigned      ch, rate;
    size_t        start;

    _Atomic size_t pos_play;      /* audible frame estimate              */
    _Atomic long   seek_req;      /* -1 = none                           */
    _Atomic int    stop_req;
    _Atomic int    pause_req;
    _Atomic int    state;

    /* stream mode */
    int             mode;         /* 0 = buffer, 1 = stream              */
    double         *sring;        /* SRING * ch interleaved              */
    uint64_t        s_wr, s_rd;   /* frame indices                       */
    uint64_t        s_underruns;
    pthread_mutex_t smx;
    pthread_cond_t  scv;

    char status[160];
};

player *player_new(void)
{
    player *p = calloc(1, sizeof *p);
    if (p) {
        atomic_store(&p->seek_req, -1);
        pthread_mutex_init(&p->smx, NULL);
        pthread_cond_init(&p->scv, NULL);
        strcpy(p->status, "idle");
    }
    return p;
}

const char *player_status(const player *p) { return p->status; }
int    player_state(const player *p) { return atomic_load(&p->state); }
size_t player_pos(const player *p)   { return atomic_load(&p->pos_play); }

static void *audio_thread(void *u)
{
    player *p = u;

    const char *dev = getenv("AUDIOTARD_ALSA_DEV");
    if (!dev) dev = "default";

    snd_pcm_t *pcm = NULL;
    int paced = 0;                       /* null sink: emulate real time */
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0 && strcmp(dev, "null") != 0) {
        /* headless / no sound server: fall back to the null device so
         * the transport still works (silently)                          */
        rc = snd_pcm_open(&pcm, "null", SND_PCM_STREAM_PLAYBACK, 0);
        if (rc == 0) {
            paced = 1;
            snprintf(p->status, sizeof p->status,
                     "no audio device -- playing to null sink");
        }
    }
    if (rc < 0 ||
        snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           p->ch, p->rate, 1, 100000 /* 100 ms */) < 0) {
        snprintf(p->status, sizeof p->status, "ALSA open failed: %s",
                 snd_strerror(rc));
        if (pcm) snd_pcm_close(pcm);
        atomic_store(&p->state, PL_STOPPED);
        return NULL;
    }

    if (strcmp(dev, "null") == 0) paced = 1;

    int16_t buf[CHUNK * 8];
    size_t  pos = p->start;
    if (pos > p->n) pos = p->n;
    int prev_paused = 0;

    while (!atomic_load(&p->stop_req)) {
        long sk = atomic_exchange(&p->seek_req, -1);
        if (sk >= 0) {
            pos = (size_t)sk > p->n ? p->n : (size_t)sk;
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
            atomic_store(&p->pos_play, pos);
        }

        if (atomic_load(&p->pause_req)) {
            if (!prev_paused) {
                snd_pcm_drop(pcm);       /* silence immediately          */
                snd_pcm_prepare(pcm);
                prev_paused = 1;
                atomic_store(&p->state, PL_PAUSED);
                atomic_store(&p->pos_play, pos);
            }
            struct timespec ts = { 0, 15 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        if (prev_paused) {
            prev_paused = 0;
            atomic_store(&p->state, PL_PLAYING);
        }

        size_t left = p->n - pos;
        if (left == 0) {
            snd_pcm_drain(pcm);
            break;
        }
        size_t frames = left < CHUNK ? left : CHUNK;
        for (size_t i = 0; i < frames * p->ch; i++) {
            double v = p->data[pos * p->ch + i];
            if (v >  1.0) v =  1.0;
            if (v < -1.0) v = -1.0;
            buf[i] = (int16_t)lrint(v * 32767.0);
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, frames);
        if (w == -EPIPE) {               /* underrun                     */
            snd_pcm_prepare(pcm);
            continue;
        }
        if (w < 0) {
            snprintf(p->status, sizeof p->status, "ALSA write failed: %s",
                     snd_strerror((int)w));
            break;
        }
        pos += (size_t)w;
        if (paced) {                       /* null sink: real-time pace  */
            struct timespec ts = { 0,
                (long)((double)w / p->rate * 1e9) };
            nanosleep(&ts, NULL);
        }

        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0) delay = 0;
        size_t audible = ((size_t)delay > pos) ? 0 : pos - (size_t)delay;
        atomic_store(&p->pos_play, audible);
    }

    snd_pcm_close(pcm);
    atomic_store(&p->pos_play, pos);
    atomic_store(&p->state, PL_STOPPED);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Stream mode: producer pushes frames with player_stream_write; this
 * thread drains the ring to ALSA, emitting silence on underrun so the
 * transport keeps time even if the producer falls behind.               */
/* ---------------------------------------------------------------------- */

static void *stream_thread(void *u)
{
    player *p = u;
    const char *dev = getenv("AUDIOTARD_ALSA_DEV");
    if (!dev) dev = "default";

    snd_pcm_t *pcm = NULL;
    int paced = 0;
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0 && strcmp(dev, "null") != 0) {
        rc = snd_pcm_open(&pcm, "null", SND_PCM_STREAM_PLAYBACK, 0);
        if (rc == 0) {
            paced = 1;
            snprintf(p->status, sizeof p->status,
                     "no audio device -- playing to null sink");
        }
    }
    if (rc < 0 ||
        snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           p->ch, p->rate, 1, 60000 /* 60 ms */) < 0) {
        snprintf(p->status, sizeof p->status, "ALSA open failed: %s",
                 snd_strerror(rc));
        if (pcm) snd_pcm_close(pcm);
        atomic_store(&p->state, PL_STOPPED);
        pthread_cond_broadcast(&p->scv);
        return NULL;
    }
    if (strcmp(dev, "null") == 0) paced = 1;

    int16_t buf[512 * 8];
    int prev_paused = 0;

    while (!atomic_load(&p->stop_req)) {
        if (atomic_load(&p->pause_req)) {
            if (!prev_paused) {
                snd_pcm_drop(pcm);
                snd_pcm_prepare(pcm);
                prev_paused = 1;
                atomic_store(&p->state, PL_PAUSED);
            }
            struct timespec ts = { 0, 15 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        if (prev_paused) {
            prev_paused = 0;
            atomic_store(&p->state, PL_PLAYING);
        }

        size_t frames = 512;
        pthread_mutex_lock(&p->smx);
        uint64_t avail = p->s_wr - p->s_rd;
        if (avail == 0) {
            p->s_underruns++;
            memset(buf, 0, frames * p->ch * sizeof *buf);
        } else {
            if (frames > avail) frames = (size_t)avail;
            for (size_t i = 0; i < frames; i++) {
                size_t slot = (size_t)((p->s_rd + i) % SRING);
                for (unsigned cch = 0; cch < p->ch; cch++) {
                    double v = p->sring[slot * p->ch + cch];
                    if (v >  1.0) v =  1.0;
                    if (v < -1.0) v = -1.0;
                    buf[i * p->ch + cch] = (int16_t)lrint(v * 32767.0);
                }
            }
            p->s_rd += frames;
        }
        pthread_cond_broadcast(&p->scv);
        pthread_mutex_unlock(&p->smx);

        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, frames);
        if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (w < 0) break;
        if (paced) {
            struct timespec ts = { 0, (long)((double)w / p->rate * 1e9) };
            nanosleep(&ts, NULL);
        }
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0) delay = 0;
        uint64_t rd = p->s_rd;
        atomic_store(&p->pos_play,
                     (size_t)(rd > (uint64_t)delay ? rd - delay : 0));
    }
    snd_pcm_close(pcm);
    atomic_store(&p->state, PL_STOPPED);
    pthread_cond_broadcast(&p->scv);
    return NULL;
}

int player_stream_start(player *p, unsigned channels, unsigned rate)
{
    player_stop(p);
    free(p->sring);
    p->sring = calloc((size_t)SRING * channels, sizeof *p->sring);
    if (!p->sring) return -1;
    p->mode = 1;
    p->ch   = channels;
    p->rate = rate;
    p->s_wr = p->s_rd = 0;
    p->s_underruns = 0;
    atomic_store(&p->stop_req, 0);
    atomic_store(&p->pause_req, 0);
    atomic_store(&p->pos_play, 0);
    atomic_store(&p->state, PL_PLAYING);
    strcpy(p->status, "streaming");
    if (pthread_create(&p->th, NULL, stream_thread, p) != 0) {
        atomic_store(&p->state, PL_STOPPED);
        return -1;
    }
    p->th_active = 1;
    return 0;
}

int player_stream_write(player *p, const double *frames, size_t n)
{
    pthread_mutex_lock(&p->smx);
    size_t done = 0;
    while (done < n) {
        if (atomic_load(&p->stop_req) ||
            atomic_load(&p->state) == PL_STOPPED) {
            pthread_mutex_unlock(&p->smx);
            return -1;
        }
        uint64_t space = SRING - (p->s_wr - p->s_rd);
        if (space == 0) {
            pthread_cond_wait(&p->scv, &p->smx);
            continue;
        }
        size_t take = n - done;
        if (take > space) take = (size_t)space;
        for (size_t i = 0; i < take; i++) {
            size_t slot = (size_t)((p->s_wr + i) % SRING);
            for (unsigned cch = 0; cch < p->ch; cch++)
                p->sring[slot * p->ch + cch] =
                    frames[(done + i) * p->ch + cch];
        }
        p->s_wr += take;
        done    += take;
    }
    pthread_mutex_unlock(&p->smx);
    return 0;
}

uint64_t player_stream_underruns(player *p)
{
    pthread_mutex_lock(&p->smx);
    uint64_t u = p->s_underruns;
    pthread_mutex_unlock(&p->smx);
    return u;
}

int player_play(player *p, const double *data, size_t nframes,
                unsigned channels, unsigned rate, size_t start_frame)
{
    player_stop(p);
    p->mode  = 0;
    p->data  = data;
    p->n     = nframes;
    p->ch    = channels;
    p->rate  = rate;
    p->start = start_frame;
    atomic_store(&p->stop_req, 0);
    atomic_store(&p->pause_req, 0);
    atomic_store(&p->seek_req, -1);
    atomic_store(&p->pos_play, start_frame);
    atomic_store(&p->state, PL_PLAYING);
    strcpy(p->status, "playing");
    if (pthread_create(&p->th, NULL, audio_thread, p) != 0) {
        atomic_store(&p->state, PL_STOPPED);
        strcpy(p->status, "cannot start audio thread");
        return -1;
    }
    p->th_active = 1;
    return 0;
}

void player_pause_toggle(player *p)
{
    if (atomic_load(&p->state) == PL_STOPPED) return;
    atomic_store(&p->pause_req, !atomic_load(&p->pause_req));
}

void player_seek(player *p, size_t frame)
{
    if (atomic_load(&p->state) == PL_STOPPED) return;
    atomic_store(&p->seek_req, (long)frame);
}

void player_stop(player *p)
{
    if (!p->th_active) return;
    atomic_store(&p->stop_req, 1);
    pthread_cond_broadcast(&p->scv);   /* wake a blocked stream writer  */
    pthread_join(p->th, NULL);
    p->th_active = 0;
    atomic_store(&p->state, PL_STOPPED);
}

void player_free(player *p)
{
    if (!p) return;
    player_stop(p);
    free(p->sring);
    pthread_mutex_destroy(&p->smx);
    pthread_cond_destroy(&p->scv);
    free(p);
}
