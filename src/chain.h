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

/* Shared effect chain: one struct holding every stage's parameters, one
 * parser for the chain-related CLI options (used by both the file
 * processor and the ABX harness), and one render function.
 */
#ifndef AUDIOTARD_CHAIN_H
#define AUDIOTARD_CHAIN_H

#include "audio_io.h"
#include "effects.h"
#include "engine.h"

#define CHAIN_MAX_EQ 16

typedef struct { bq_type t; double f, Q, g; } eq_spec;

typedef struct {
    int          use_shape;
    ws_params    wsp;
    double       h2db;        /* calibrated at render time vs input peak */
    int          use_vinyl;
    vinyl_params vp;
    int          use_tape;
    tape_params  tp;
    eq_spec      eq[CHAIN_MAX_EQ];
    int          neq;
    int          os;
    double       gain_db;
    size_t       pos0;        /* start frame of this buffer within the
                                 source (streaming block renders); 0 for
                                 whole-file renders                      */
    int          no_trim;     /* 1 = skip the output headroom trim:
                                 streaming producers manage level with a
                                 constant gain instead (a per-block trim
                                 would pump audibly between blocks)      */
} chain_params;

void chain_defaults(chain_params *cp);

/* Try to consume argv[*i] (and its value) as a chain option. Returns 1 if
 * handled (advancing *i past any value), 0 if the option is not ours.
 * Exits with a message on a malformed value.                             */
int chain_parse(chain_params *cp, int argc, char **argv, int *i);

/* Render in -> out through the chain. Allocates out->data. If match_rms,
 * scales the result to the input's RMS (do this for listening tests).
 * Returns 0 on success.                                                  */
int chain_render(const audio_buf *in, audio_buf *out,
                 const chain_params *cp, int match_rms);

/* ---- staircase parameter plumbing (shared by CLI + GUI harnesses) ---- */

typedef enum { SC_NONE, SC_H2DB, SC_HISS, SC_CRK, SC_WOW, SC_FLUT, SC_DRIVE }
        sc_param;

typedef struct {
    const char *name;
    sc_param    id;
    int         is_db;      /* scalar == value (dB) vs 20log10(value)    */
    double      lo, hi;     /* scalar bounds                             */
    const char *unit;
} sc_info;

const sc_info *sc_find(const char *name);
const sc_info *sc_table(size_t *count);
double         sc_get(const chain_params *cp, sc_param id);
void           sc_set(chain_params *cp, sc_param id, double scalar);
/* NULL if the parameter's effect stage is enabled, else an error string. */
const char    *sc_check_enabled(const chain_params *cp, sc_param id);
/* Strip everything from the chain except the staircase parameter's own
 * mechanism, so a threshold measures that parameter and not a constant
 * confound (crackle riding along in a hiss test, etc.).                */
void           sc_isolate(chain_params *cp, sc_param id);
void           sc_print_value(const sc_info *si, double scalar,
                              char *buf, size_t n);

/* Exact binomial tail P(K >= k | n, 0.5). */
double binom_p(int k, int n);

#endif
