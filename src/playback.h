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

/* In-process ALSA playback with pause, seek, and a live play-position.
 *
 * The player does NOT copy the buffer: the caller guarantees the data
 * stays alive until player_stop() returns (the GUI stops before freeing
 * or replacing any buffer it handed over).
 *
 * Position semantics: player_pos() is the estimated *audible* frame
 * (frames written minus the device's queued delay), so the GUI cursor
 * and spectrum track what the ears hear, not what was queued.
 */
#ifndef AUDIOTARD_PLAYBACK_H
#define AUDIOTARD_PLAYBACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct player player;

enum { PL_STOPPED = 0, PL_PLAYING = 1, PL_PAUSED = 2 };

player *player_new(void);
void    player_free(player *p);            /* stops first                */

/* Start playing interleaved doubles [0, nframes). Stops any current
 * playback. Returns 0 on success; on failure player_status() explains. */
int     player_play(player *p, const double *data, size_t nframes,
                    unsigned channels, unsigned rate, size_t start_frame);

void    player_pause_toggle(player *p);
void    player_stop(player *p);            /* joins the audio thread     */
void    player_seek(player *p, size_t frame);

int     player_state(const player *p);
size_t  player_pos(const player *p);       /* current audible frame      */
const char *player_status(const player *p);

/* Stream mode: a producer pushes frames; underruns emit silence.
 * player_pos() then reports the audible STREAM frame index.            */
int      player_stream_start(player *p, unsigned channels, unsigned rate);
int      player_stream_write(player *p, const double *frames, size_t n);
uint64_t player_stream_underruns(player *p);

#endif
