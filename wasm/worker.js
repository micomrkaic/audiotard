/* This file is part of audiotard. Copyright (C) 2026 Mico.
 * GPL-3.0-or-later; see COPYING.
 *
 * Streaming producer, browser edition of the GTK live mode: renders
 * LIVE_B-frame blocks with LIVE_PR frames of pre-roll context through
 * the wasm DSP core, crossfading LIVE_X frames at seams and looping
 * the region. The latest parameters land at the next block, so
 * knob-to-ear latency = scheduled lookahead + one block.               */
"use strict";

const LIVE_B = 8192, LIVE_X = 512;      /* bigger blocks: less pre-roll
                                           overhead per emitted sample */
/* pre-roll: media chains need long settling (envelope, noise filters);
 * shaper-only needs little more than the FIR warm-up                  */
function livePR() {
  return (params && (params.vinyl || params.tape)) ? 16384 : 2048;
}

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
    const eq = params.eq || [];
    if (eq.length) {
      const ep = wasm.at_alloc(eq.length * 4);
      heap().set([].concat(...eq), ep / 8);
      wasm.at_eq(eq.length, ep);
      wasm.at_free(ep);
    } else wasm.at_eq(0, 0);
    out = wasm.at_render(ptr, n, ch, rate,
        params.shape, params.drive, params.bias, params.h2db, params.os,
        params.vinyl, params.tape, params.wow, params.flutter,
        params.hiss, params.crkRate, params.crkDb, params.hfLoss,
        params.bumpDb, params.bwHz, params.spkModel | 0,
        params.spkZout || 0, params.shellac | 0, params.am | 0,
        params.amBw || 4500, params.amDepth || 0.95,
        params.bassDb || 0, params.trebleDb || 0, 0, from);
    if (!out) postMessage({ type: "error",
        msg: "DSP render FAILED (out of memory?) -- playing clean" });
  }
  const res = new Float64Array(n * ch);
  if (out) res.set(heap().subarray(out / 8, out / 8 + n * ch));
  else     res.set(heap().subarray(ptr / 8, ptr / 8 + n * ch));
  wasm.at_free(ptr);
  return res;
}

/* ------------------------- internet radio -------------------------- */
/* Push-based live path: chunks arrive, a ring holds recent history for
 * media-effect pre-roll, emission is delayed by R_PAD so the FIR-edge-
 * corrupted render tail never reaches the ear, and consecutive renders
 * crossfade over LIVE_X at the seam exactly like file streaming.      */
const R_PAD = 1024, RINGF = 1 << 18;         /* frames                 */
let rRing = null, rCh = 2, rW = 0, rE = 0, rTail = null;
let rGain = 1, rGainSet = false;

function ringCopy(fromAbs, toAbs, dst) {     /* -> interleaved Float64 */
  for (let f = fromAbs; f < toAbs; f++) {
    const s = (f % RINGF) * rCh, d = (f - fromAbs) * rCh;
    for (let c = 0; c < rCh; c++) dst[d + c] = rRing[s + c];
  }
}

function radioChunk(f32) {
  const n = f32.length / rCh;
  for (let i = 0; i < n; i++) {
    const s = ((rW + i) % RINGF) * rCh;
    for (let c = 0; c < rCh; c++) rRing[s + c] = f32[i * rCh + c];
  }
  rW += n;
  const emitEnd = rW - R_PAD;
  if (emitEnd <= rE) return;                 /* bootstrap              */
  const emitStart = rE;
  const pre = Math.max(rW - RINGF + 8, Math.max(0, emitStart - livePR()));

  const nn = rW - pre;
  const ptr = wasm.at_alloc(nn * rCh);
  {
    const hp = heap();
    const tmp = new Float64Array(nn * rCh);
    ringCopy(pre, rW, tmp);
    hp.set(tmp, ptr / 8);
  }
  let out = 0;
  if (params && !params.bypass && params.enabled) {
    const eq = params.eq || [];
    if (eq.length) {
      const ep = wasm.at_alloc(eq.length * 4);
      heap().set([].concat(...eq), ep / 8);
      wasm.at_eq(eq.length, ep);
      wasm.at_free(ep);
    } else wasm.at_eq(0, 0);
    out = wasm.at_render(ptr, nn, rCh, rate,
        params.shape, params.drive, params.bias, params.h2db, params.os,
        params.vinyl, params.tape, params.wow, params.flutter,
        params.hiss, params.crkRate, params.crkDb, params.hfLoss,
        params.bumpDb, params.bwHz, params.spkModel | 0,
        params.spkZout || 0, params.shellac | 0, params.am | 0,
        params.amBw || 4500, params.amDepth || 0.95,
        params.bassDb || 0, params.trebleDb || 0, 0, pre);
  }
  const src = out ? out : ptr;
  const hp = heap(), base = src / 8;

  if (!rGainSet) {                            /* frozen radio gain     */
    let rs = 0, ro = 0;
    const m = (emitEnd - emitStart) * rCh, o0 = (emitStart - pre) * rCh;
    for (let i = 0; i < m; i++) {
      const cv = rRing[((emitStart + (i / rCh | 0)) % RINGF) * rCh
                       + i % rCh];
      rs += cv * cv; ro += hp[base + o0 + i] ** 2;
    }
    rGain = ro > 1e-12 ? Math.sqrt(rs / ro) * 0.708 : 1;
    rGainSet = true;
  }

  const emitN = emitEnd - emitStart, o0 = (emitStart - pre) * rCh;
  const blk = new Float32Array(emitN * rCh);
  for (let i = 0; i < emitN * rCh; i++) blk[i] = hp[base + o0 + i] * rGain;

  if (rTail) {                                /* seam crossfade        */
    const X = Math.min(LIVE_X, emitN);
    for (let i = 0; i < X; i++) {
      const w = i / X;
      for (let c = 0; c < rCh; c++)
        blk[i * rCh + c] = (1 - w) * rTail[i * rCh + c]
                         + w * blk[i * rCh + c];
    }
  }
  rTail = new Float32Array(LIVE_X * rCh);     /* next seam: processed
      samples at [emitEnd, emitEnd+X) from THIS render                 */
  const t0 = (emitEnd - pre) * rCh;
  const avail = Math.min(LIVE_X, rW - emitEnd);
  for (let i = 0; i < avail * rCh; i++)
    rTail[i] = hp[base + t0 + i] * rGain;

  wasm.at_free(ptr);
  const cln = new Float32Array(emitN * rCh);      /* aligned raw span */
  for (let i = 0; i < emitN; i++) {
    const s = ((emitStart + i) % RINGF) * rCh;
    for (let c = 0; c < rCh; c++) cln[i * rCh + c] = rRing[s + c];
  }
  rE = emitEnd;
  postMessage({ type: "radioblock", buf: blk.buffer, cbuf: cln.buffer },
              [blk.buffer, cln.buffer]);
}

function nextBlock() {
  if (t >= r1) { t = r0; tailOk = false; }
  const emit = Math.min(LIVE_B, r1 - t);
  const pre  = Math.max(0, t - livePR());
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
  else if (m.type === "radiostart") {
    rCh = m.ch; rate = m.rate; ch = m.ch;
    rRing = new Float32Array(RINGF * rCh);
    rW = 0; rE = 0; rTail = null; rGainSet = false;
  }
  else if (m.type === "radio") {
    if (rRing) radioChunk(new Float32Array(m.buf));
  }
  else if (m.type === "radiostop") { rRing = null; rTail = null; }
};
