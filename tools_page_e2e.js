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
  console.log('page errors:', errors.length ? errors : 'none');
}, 300);
