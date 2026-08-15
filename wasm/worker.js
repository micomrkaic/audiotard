/* This file is part of audiotard. Copyright (C) 2026 Mico.
 * GPL-3.0-or-later; see COPYING.
 *
 * Streaming producer, browser edition of the GTK live mode: renders
 * LIVE_B-frame blocks with LIVE_PR frames of pre-roll context through
 * the wasm DSP core, crossfading LIVE_X frames at seams and looping
 * the region. The latest parameters land at the next block, so
 * knob-to-ear latency = scheduled lookahead + one block.               */
"use strict";

const LIVE_B = 4096, LIVE_PR = 16384, LIVE_X = 512;

let wasm = null, clean = null, frames = 0, ch = 1, rate = 44100;
let params = null, t = 0, r0 = 0, r1 = 0;
let tail = null, tailOk = false, gain = 1, gainSet = false;
/* -3 dB master headroom: hot (-0.1 dBFS) masters + added harmonics
 * would clip at the DAC; applied identically to clean and processed
 * so it cannot become a listening cue                                 */
const HEADROOM = 0.708;

function heap() { return new Float64Array(wasm.memory.buffer); }

let expectVer = 0;

async function init(url) {
  const imports = { wasi_snapshot_preview1:
                    new Proxy({}, { get: () => () => 0 }) };
  let r;
  try {
    r = await WebAssembly.instantiateStreaming(fetch(url), imports);
  } catch (e) {
    const buf = await (await fetch(url)).arrayBuffer();
    r = await WebAssembly.instantiate(buf, imports);
  }
  wasm = r.instance.exports;
  if (wasm._initialize) wasm._initialize();
  const v = wasm.at_version ? wasm.at_version() : 0;
  postMessage({ type: "ready", v,
                ok: !expectVer || v === expectVer });
}

function renderSpan(from, to) {           /* -> Float64 interleaved     */
  const n = to - from;
  const ptr = wasm.at_alloc(n * ch);
  {
    const hp = heap(), base = ptr / 8, off = from * ch;
    for (let i = 0; i < n * ch; i++) hp[base + i] = clean[off + i];
  }
  let out = 0;
  if (!params.bypass && params.enabled) {
    out = wasm.at_render(ptr, n, ch, rate,
        params.shape, params.drive, params.bias, params.h2db, params.os,
        params.vinyl, params.tape, params.wow, params.flutter,
        params.hiss, params.crkRate, params.crkDb, params.hfLoss,
        params.bumpDb, params.bwHz, 0, from);
    if (!out) postMessage({ type: "error",
        msg: "DSP render FAILED (out of memory?) -- playing clean" });
  }
  const res = new Float64Array(n * ch);
  if (out) res.set(heap().subarray(out / 8, out / 8 + n * ch));
  else     res.set(heap().subarray(ptr / 8, ptr / 8 + n * ch));
  wasm.at_free(ptr);
  return res;
}

function nextBlock() {
  if (t >= r1) { t = r0; tailOk = false; }
  const emit = Math.min(LIVE_B, r1 - t);
  const pre  = Math.max(0, t - LIVE_PR);
  /* +512 pad past the crossfade tail: the last ~140 samples of any
   * render are FIR edge-corrupted, so the tail must come from clean
   * interior                                                          */
  const endr = Math.min(t + emit + LIVE_X + 512, r1);
  const filePos = t;

  const rend = renderSpan(pre, endr);
  const off = (t - pre) * ch;
  const blk = new Float32Array(emit * ch);

  if (!gainSet) {
    let rs = 0, ro = 0;
    for (let i = 0; i < emit * ch; i++) {
      const s = clean[t * ch + i];
      rs += s * s;
      ro += rend[off + i] * rend[off + i];
    }
    gain = (ro > 1e-12 ? Math.sqrt(rs / ro) : 1) * HEADROOM;
    gainSet = true;
  }
  for (let i = 0; i < emit * ch; i++) blk[i] = gain * rend[off + i];

  if (tailOk) {
    const xf = Math.min(emit, LIVE_X);
    for (let i = 0; i < xf; i++) {
      const w = i / LIVE_X;
      for (let c = 0; c < ch; c++)
        blk[i * ch + c] = tail[i * ch + c] * (1 - w)
                        + blk[i * ch + c] * w;
    }
  }
  if (t + emit + LIVE_X <= r1) {
    tail = new Float32Array(LIVE_X * ch);
    const toff = (t + emit - pre) * ch;
    for (let i = 0; i < LIVE_X * ch; i++)
      tail[i] = gain * rend[toff + i];
    tailOk = true;
  } else {
    tailOk = false;
  }
  t += emit;
  postMessage({ type: "block", buf: blk.buffer, frames: emit, ch,
                filePos }, [blk.buffer]);
}

onmessage = e => {
  const m = e.data;
  if (m.type === "init")  { expectVer = m.expect | 0; init(m.url); }
  else if (m.type === "audio") {
    clean = new Float32Array(m.buf);
    frames = m.frames; ch = m.ch; rate = m.rate;
    let s = 0;
    const probe = Math.min(clean.length, 65536);
    for (let i = 0; i < probe; i++) s += clean[i] * clean[i];
    if (frames > 0 && s < 1e-12)
      postMessage({ type: "error",
          msg: "worker received silent audio -- stale files? "
             + "hard-refresh (Ctrl+Shift+R)" });
  }
  else if (m.type === "params") { params = m.p; gainSet = false; }
  else if (m.type === "start") {
    t = m.pos; r0 = m.r0; r1 = m.r1;
    tailOk = false; gainSet = false;
  }
  else if (m.type === "need") nextBlock();
};
