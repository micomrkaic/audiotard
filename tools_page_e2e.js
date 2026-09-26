const fs = require('fs');
const { JSDOM } = require('jsdom');

const html = fs.readFileSync('wasm/index.html', 'utf8');
const workerMsgs = [];
const errors = [];

const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  url: 'http://localhost/',
  beforeParse(w) {
    w.devicePixelRatio = 1;
    w.requestAnimationFrame = () => 0;
    w.HTMLCanvasElement.prototype.getContext = () => ({
      setTransform(){}, fillRect(){}, fillText(){}, beginPath(){},
      moveTo(){}, lineTo(){}, stroke(){}, fill(){},
    });
    w.Worker = class {
      constructor(url) { this.url = url; workerMsgs.push(['NEW', url]); }
      postMessage(m) { workerMsgs.push([m.type, m]); }
      addEventListener() {}
      terminate() {}
    };
    const fakeBuf = (n, ch, rate) => ({
      length: n, numberOfChannels: ch, sampleRate: rate, duration: n/rate,
      getChannelData: () => new Float32Array(n).fill(0.1),
    });
    class FakeCtx {
      constructor(opts) { this.sampleRate = 44100; this.currentTime = 0;
        this.destination = {}; }
      resume() { return Promise.resolve(); }
      decodeAudioData() { return Promise.resolve(fakeBuf(44100*5, 2, 44100)); }
      createBuffer(ch, n, rate) { return fakeBuf(n, ch, rate); }
      createBufferSource() {
        return { connect(){}, start(){ workerMsgs.push(['SRC.START']); },
                 stop(){}, buffer: null };
      }
    }
    FakeCtx.prototype.createMediaElementSource = function() {
      return { connect(){}, disconnect(){} }; };
    FakeCtx.prototype.createScriptProcessor = function(n, a, b) {
      w.__proc = { connect(){}, disconnect(){}, onaudioprocess: null,
                   _n: n };
      return w.__proc; };
    w.Audio = class {
      constructor() { this.listeners = {}; }
      addEventListener(t, f) { this.listeners[t] = f; }
      play() { setTimeout(() => this.listeners.playing &&
                          this.listeners.playing(), 10);
               return Promise.resolve(); }
      pause() {}
    };
    w.AudioContext = FakeCtx;
    w.OfflineAudioContext = class extends FakeCtx {
      constructor(ch, len, rate) { super(); this.sampleRate = rate; }
    };
  },
});
dom.window.addEventListener('error', e =>
  errors.push(e.message + ' @ ' + e.filename + ':' + e.lineno));

setTimeout(async () => {
  const d = dom.window.document;
  // simulate choosing a file
  const file = { name: 't.flac',
                 arrayBuffer: async () => new ArrayBuffer(64) };
  const inp = d.getElementById('file');
  Object.defineProperty(inp, 'files', { value: [file] });
  inp.onchange({ target: inp });
  await new Promise(r => setTimeout(r, 200));
  console.log('after load, status:',
    JSON.stringify(d.getElementById('status').textContent.slice(0, 60)));
  console.log('play disabled?', d.getElementById('play').disabled);
  // trap null lookups
  const orig = d.getElementById.bind(d);
  d.getElementById = id => { const e = orig(id);
    if (!e) console.log('NULL ID LOOKUP:', id); return e; };
  // click play
  try { d.getElementById('play').onclick(); }
  catch (e) { errors.push('PLAY THREW: ' + e.message); }
  await new Promise(r => setTimeout(r, 100));
  console.log('worker messages:', workerMsgs.map(m => m[0]).join(' '));
  // ---- radio path ----
  try { d.getElementById('rconnect').onclick(); } catch (e) {
    errors.push('RCONNECT THREW: ' + e.message); }
  await new Promise(r => setTimeout(r, 60));
  const proc = dom.window.__proc;
  if (!proc || !proc.onaudioprocess) errors.push('radio tap not wired');
  else {
    const n = proc._n, mkch = () => new Float32Array(n).fill(0.2);
    const fakeEv = {
      inputBuffer: { length: n, numberOfChannels: 2,
                     getChannelData: mkch },
      outputBuffer: { length: n, numberOfChannels: 2,
                      getChannelData: () => new Float32Array(n) },
    };
    try { for (let k = 0; k < 3; k++) proc.onaudioprocess(fakeEv); }
    catch (e) { errors.push('TAP THREW: ' + e.message); }
  }
  const radioSeq = workerMsgs.map(m => m[0])
      .filter(t => t.startsWith('radio')).join(' ');
  console.log('radio worker sequence:', radioSeq || 'NONE');
  // capture path
  try { d.getElementById('rcapture').onclick(); } catch (e) {
    errors.push('CAPTURE THREW: ' + e.message); }
  await new Promise(r => setTimeout(r, 30));
  console.log('after capture, status:',
    JSON.stringify(d.getElementById('status').textContent.slice(0, 50)));
  console.log('page errors:', errors.length ? errors : 'none');
}, 300);
