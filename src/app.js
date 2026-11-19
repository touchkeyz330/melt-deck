'use strict';
/* Melt Deck — main app: audio engine, decks, mixer, library, recording. */

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const dbToGain = (db) => Math.pow(10, db / 20);
const HOTCUE_COLORS = ['#ff2e4c', '#ffc23a', '#3dff8b', '#b45cff'];

function fmtTime(s, tenths = true) {
  if (!isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const sec = s - m * 60;
  return tenths ? `${m}:${sec.toFixed(1).padStart(4, '0')}` : `${m}:${String(Math.floor(sec)).padStart(2, '0')}`;
}

let toastTimer = null;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}

/* ================= Audio engine ================= */
const ctx = new AudioContext({ latencyHint: 'interactive' });
const masterGain = ctx.createGain();
const limiter = ctx.createDynamicsCompressor(); // keeps the master from clipping
limiter.threshold.value = -1;
limiter.knee.value = 0;
limiter.ratio.value = 20;
limiter.attack.value = 0.002;
limiter.release.value = 0.12;
const masterAnalyser = ctx.createAnalyser();
masterAnalyser.fftSize = 1024;
masterGain.connect(limiter);
limiter.connect(masterAnalyser);
masterAnalyser.connect(ctx.destination);

const resumeAudio = () => { if (ctx.state !== 'running') ctx.resume(); };
window.addEventListener('pointerdown', resumeAudio);
window.addEventListener('keydown', resumeAudio);

/* ================= Knob control ================= */
class Knob {
  constructor(el, { min = -1, max = 1, value = 0, def = value, onChange = () => {} } = {}) {
    Object.assign(this, { el, min, max, def, onChange });
    this.value = value;
    el.addEventListener('pointerdown', (e) => {
      el.setPointerCapture(e.pointerId);
      const startY = e.clientY, startV = this.value;
      const move = (ev) => {
        const fine = ev.shiftKey ? 0.25 : 1;
        this.set(startV + ((startY - ev.clientY) / 160) * (this.max - this.min) * fine);
      };
      const up = () => { el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', up); };
      el.addEventListener('pointermove', move);
      el.addEventListener('pointerup', up);
    });
    el.addEventListener('dblclick', () => this.set(this.def));
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.set(this.value - Math.sign(e.deltaY) * (this.max - this.min) * 0.02);
    }, { passive: false });
    this.render();
  }
  set(v, fire = true) {
    this.value = clamp(v, this.min, this.max);
    if (Math.abs(this.value - this.def) < (this.max - this.min) * 0.012) this.value = this.def; // easy centre
    this.render();
    if (fire) this.onChange(this.value);
  }
  render() {
    const toDeg = (v) => (270 * (v - this.min)) / (this.max - this.min);
    const a = toDeg(this.value), d = toDeg(this.def);
    this.el.style.setProperty('--a', `${a - 135}deg`);
    this.el.style.setProperty('--s', `${Math.min(a, d)}deg`);
    this.el.style.setProperty('--e', `${Math.max(a, d)}deg`);
  }
}

/* ================= Canvas helper ================= */
function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
  const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    return true;
  }
  return false;
}

function hexToRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
// Waveform colour: bass-heavy = deck colour, bright/high = near white.
function waveColor(rgb, lowRatio, alpha = 1) {
  const t = clamp(lowRatio, 0, 1);
  const r = Math.round(255 + (rgb[0] - 255) * t);
  const g = Math.round(240 + (rgb[1] - 240) * t);
  const b = Math.round(235 + (rgb[2] - 235) * t);
  return `rgba(${r},${g},${b},${alpha})`;
}

/* ================= Deck ================= */
class Deck {
  constructor(id, color) {
    this.id = id;
    this.color = color;
    this.rgb = hexToRgb(color);
    this.track = null;
    this.an = null;              // analysis results
    this.bpm = null;
    this.firstBeat = 0;
    this.cuePoint = 0;
    this.hotcues = [null, null, null, null];
    this.loop = null;            // { start, end, beats }
    this.tempo = 0;              // -0.5 .. +0.5
    this.range = 0.08;
    this.nudge = 1;
    this.keylock = false;
    this.cuePreview = false;
    this.taps = [];
    this.visibleSecs = 8;
    this.other = null;

    // --- audio chain ---
    this.audio = new Audio();
    this.audio.preload = 'auto';
    this.audio.preservesPitch = false;
    this.src = ctx.createMediaElementSource(this.audio);
    this.trim = ctx.createGain();
    this.lo = Object.assign(ctx.createBiquadFilter(), { type: 'lowshelf' });
    this.lo.frequency.value = 220;
    this.mid = Object.assign(ctx.createBiquadFilter(), { type: 'peaking' });
    this.mid.frequency.value = 1000; this.mid.Q.value = 0.7;
    this.hi = Object.assign(ctx.createBiquadFilter(), { type: 'highshelf' });
    this.hi.frequency.value = 3500;
    this.hpf = Object.assign(ctx.createBiquadFilter(), { type: 'highpass' });
    this.hpf.frequency.value = 10; this.hpf.Q.value = 0.9;
    this.lpf = Object.assign(ctx.createBiquadFilter(), { type: 'lowpass' });
    this.lpf.frequency.value = 22000; this.lpf.Q.value = 0.9;
    this.fader = ctx.createGain();
    this.xf = ctx.createGain();
    this.analyser = ctx.createAnalyser();
    this.analyser.fftSize = 1024;
    this.src.connect(this.trim).connect(this.lo).connect(this.mid).connect(this.hi)
      .connect(this.hpf).connect(this.lpf).connect(this.fader).connect(this.xf).connect(masterGain);
    this.fader.connect(this.analyser);
    this.meterBuf = new Float32Array(this.analyser.fftSize);
    this.level = 0;

    this.audio.addEventListener('ended', () => this.updateButtons());
    this.audio.addEventListener('play', () => this.updateButtons());
    this.audio.addEventListener('pause', () => this.updateButtons());

    this.buildDeckUI();
    this.buildChannelUI();
    this.bindWaveRow();
  }

  get playing() { return !this.audio.paused && !this.audio.ended; }
  get time() { return this.audio.currentTime || 0; }
  get duration() { return (this.an && this.an.duration) || this.audio.duration || 0; }
  get rate() { return (1 + this.tempo) * this.nudge; }
  get effBpm() { return this.bpm ? this.bpm * (1 + this.tempo) : null; }
  get beatLen() { return this.bpm ? 60 / this.bpm : null; }

  /* ---------- UI construction ---------- */
  buildDeckUI() {
    const el = $(`#deck${this.id}`);
    this.el = el;
    el.innerHTML = `
      <div class="deck-head">
        <div class="deck-letter">${this.id}</div>
        <div class="track-info">
          <div class="track-title">No track loaded</div>
          <div class="track-artist">Drag a song onto this deck, or use the library below</div>
        </div>
        <div class="readouts">
          <div><span class="bpm-val">---.-</span><span class="bpm-unit">BPM</span></div>
          <div class="tempo-pct">0.00%</div>
        </div>
      </div>
      <canvas class="overview" title="Click to jump"></canvas>
      <div class="times"><span class="elapsed">0:00.0</span><span class="remain">-0:00.0</span></div>
      <div class="deck-body">
        <div class="controls">
          <div class="row hotcues">
            <span class="row-label">HOT CUE</span>
            ${[0, 1, 2, 3].map((i) => `<button class="pad" data-hc="${i}" title="Click: set / jump · Shift+click: clear">${i + 1}</button>`).join('')}
          </div>
          <div class="row loops">
            <span class="row-label">LOOP</span>
            ${[1, 2, 4, 8, 16].map((b) => `<button class="sbtn" data-loop="${b}" title="${b}-beat loop">${b}</button>`).join('')}
          </div>
          <div class="row bpmtools">
            <span class="row-label">BPM</span>
            <button class="sbtn" data-act="tap" title="Tap along to the beat to set BPM">TAP</button>
            <button class="sbtn" data-act="half" title="Halve BPM">÷2</button>
            <button class="sbtn" data-act="double" title="Double BPM">×2</button>
            <button class="sbtn" data-act="load" title="Load a song from your computer">LOAD</button>
          </div>
          <div class="transport">
            <button class="tbtn cue" title="Cue">CUE</button>
            <button class="tbtn play" title="Play / pause"><span class="play-icon"></span>PLAY</button>
            <button class="tbtn sync" title="Match tempo + beats to the other deck">SYNC</button>
          </div>
        </div>
        <div class="pitch">
          <button class="sbtn range" title="Tempo range">±8%</button>
          <input type="range" class="vslider tempo" min="-1000" max="1000" value="0" title="Tempo (double-click to reset)" />
          <div class="nudges">
            <button class="sbtn" data-nudge="-1" title="Nudge slower (hold)">−</button>
            <button class="sbtn" data-nudge="1" title="Nudge faster (hold)">+</button>
          </div>
          <button class="sbtn keylock" title="Key lock: change tempo without changing pitch">KEY</button>
        </div>
      </div>`;

    this.ui = {
      title: $('.track-title', el), artist: $('.track-artist', el),
      bpm: $('.bpm-val', el), pct: $('.tempo-pct', el),
      overview: $('.overview', el), elapsed: $('.elapsed', el), remain: $('.remain', el),
      pads: $$('.pad', el), loops: $$('[data-loop]', el),
      cue: $('.tbtn.cue', el), play: $('.tbtn.play', el), sync: $('.tbtn.sync', el),
      range: $('.range', el), tempo: $('.tempo', el), keylock: $('.keylock', el),
      tap: $('[data-act="tap"]', el),
    };

    this.ui.play.addEventListener('click', () => this.togglePlay());
    this.ui.cue.addEventListener('pointerdown', () => this.cueDown());
    this.ui.cue.addEventListener('pointerup', () => this.cueUp());
    this.ui.cue.addEventListener('pointerleave', () => this.cueUp());
    this.ui.sync.addEventListener('click', () => this.sync());
    this.ui.pads.forEach((p) => p.addEventListener('click', (e) => this.hotcue(+p.dataset.hc, e.shiftKey)));
    this.ui.loops.forEach((b) => b.addEventListener('click', () => this.toggleLoop(+b.dataset.loop)));
    $('[data-act="tap"]', el).addEventListener('click', () => this.tap());
    $('[data-act="half"]', el).addEventListener('click', () => this.scaleBpm(0.5));
    $('[data-act="double"]', el).addEventListener('click', () => this.scaleBpm(2));
    $('[data-act="load"]', el).addEventListener('click', () => pickFiles(this));

    this.ui.range.addEventListener('click', () => {
      const ranges = [0.08, 0.16, 0.5];
      this.range = ranges[(ranges.indexOf(this.range) + 1) % ranges.length];
      this.ui.range.textContent = `±${Math.round(this.range * 100)}%`;
      this.setTempo(this.tempo);
    });
    this.ui.tempo.addEventListener('input', () => this.setTempo((this.ui.tempo.value / 1000) * this.range, false));
    this.ui.tempo.addEventListener('dblclick', () => this.setTempo(0));
    $$('[data-nudge]', el).forEach((b) => {
      const dir = +b.dataset.nudge;
      b.addEventListener('pointerdown', () => { this.nudge = 1 + dir * 0.04; this.applyRate(); });
      const stop = () => { this.nudge = 1; this.applyRate(); };
      b.addEventListener('pointerup', stop);
      b.addEventListener('pointerleave', stop);
    });
    this.ui.keylock.addEventListener('click', () => {
      this.keylock = !this.keylock;
      this.audio.preservesPitch = this.keylock;
      this.ui.keylock.classList.toggle('on', this.keylock);
    });

    // Click / drag on overview to jump.
    const ov = this.ui.overview;
    const seekFromOv = (e) => {
      if (!this.track) return;
      const r = ov.getBoundingClientRect();
      this.seek(((e.clientX - r.left) / r.width) * this.duration);
    };
    ov.addEventListener('pointerdown', (e) => {
      ov.setPointerCapture(e.pointerId);
      seekFromOv(e);
      const move = (ev) => seekFromOv(ev);
      const up = () => { ov.removeEventListener('pointermove', move); ov.removeEventListener('pointerup', up); };
      ov.addEventListener('pointermove', move);
      ov.addEventListener('pointerup', up);
    });

    // Drag & drop a file (or library row) straight onto the deck.
    el.addEventListener('dragover', (e) => { e.preventDefault(); e.stopPropagation(); el.classList.add('drag-over'); });
    el.addEventListener('dragleave', () => el.classList.remove('drag-over'));
    el.addEventListener('drop', (e) => {
      e.preventDefault(); e.stopPropagation();
      el.classList.remove('drag-over');
      hideDropOverlay();
      const libId = e.dataTransfer.getData('text/melt-track');
      if (libId) { const t = library.find((x) => x.id === +libId); if (t) this.load(t); return; }
      const files = audioFiles(e.dataTransfer.files);
      if (files.length) {
        const added = addToLibrary(files);
        if (added[0]) this.load(added[0]);
      }
    });
  }

  buildChannelUI() {
    const ch = $(`#ch${this.id}`);
    ch.innerHTML = `
      <div class="knobs">
        <div class="knob-cell"><div class="knob" data-k="trim"></div><span class="label">TRIM</span></div>
        <div class="knob-cell"><div class="knob" data-k="hi"></div><span class="label">HI</span></div>
        <div class="knob-cell"><div class="knob" data-k="mid"></div><span class="label">MID</span></div>
        <div class="knob-cell"><div class="knob" data-k="lo"></div><span class="label">LOW</span></div>
        <div class="knob-cell"><div class="knob" data-k="filter"></div><span class="label">FILTER</span></div>
      </div>
      <div class="fader-col">
        <canvas class="vu"></canvas>
        <input type="range" class="vslider up chfader" min="0" max="1000" value="800" title="Channel volume" />
      </div>`;

    const eqGain = (v) => (v < 0 ? (v <= -0.98 ? -40 : v * 26) : v * 6); // full left ≈ kill
    const k = (name) => $(`[data-k="${name}"]`, ch);
    this.knobs = {
      trim: new Knob(k('trim'), { onChange: (v) => this.trim.gain.setTargetAtTime(dbToGain(v * 12), ctx.currentTime, 0.01) }),
      hi: new Knob(k('hi'), { onChange: (v) => this.hi.gain.setTargetAtTime(eqGain(v), ctx.currentTime, 0.01) }),
      mid: new Knob(k('mid'), { onChange: (v) => this.mid.gain.setTargetAtTime(eqGain(v), ctx.currentTime, 0.01) }),
      lo: new Knob(k('lo'), { onChange: (v) => this.lo.gain.setTargetAtTime(eqGain(v), ctx.currentTime, 0.01) }),
      filter: new Knob(k('filter'), { onChange: (v) => this.setFilter(v) }),
    };
    this.vu = $('.vu', ch);
    const fader = $('.chfader', ch);
    const applyFader = () => {
      const x = fader.value / 1000;
      this.fader.gain.setTargetAtTime(x * x, ctx.currentTime, 0.01); // natural-feeling curve
    };
    fader.addEventListener('input', applyFader);
    applyFader();
  }

  setFilter(v) {
    const t = ctx.currentTime;
    if (v < 0) { // low-pass sweep down
      this.lpf.frequency.setTargetAtTime(20000 * Math.pow(60 / 20000, -v), t, 0.02);
      this.hpf.frequency.setTargetAtTime(10, t, 0.02);
    } else if (v > 0) { // high-pass sweep up
      this.hpf.frequency.setTargetAtTime(10 * Math.pow(8000 / 10, v), t, 0.02);
      this.lpf.frequency.setTargetAtTime(22000, t, 0.02);
    } else {
      this.lpf.frequency.setTargetAtTime(22000, t, 0.02);
      this.hpf.frequency.setTargetAtTime(10, t, 0.02);
    }
    const q = Math.abs(v) > 0.05 ? 3 : 0.9; // a little resonance when active
    this.lpf.Q.setTargetAtTime(q, t, 0.05);
    this.hpf.Q.setTargetAtTime(q, t, 0.05);
  }

  bindWaveRow() {
    const row = $(`.wave-row[data-deck="${this.id}"]`);
    this.detail = $('canvas', row);
    // Drag the scrolling waveform to scrub; mouse wheel to zoom.
    row.addEventListener('pointerdown', (e) => {
      if (!this.track) return;
      row.setPointerCapture(e.pointerId);
      let lastX = e.clientX;
      const wasPlaying = this.playing;
      if (wasPlaying) this.audio.pause();
      const move = (ev) => {
        const pxPerSec = row.clientWidth / this.visibleSecs;
        this.seek(this.time - (ev.clientX - lastX) / pxPerSec, true);
        lastX = ev.clientX;
      };
      const up = () => {
        row.removeEventListener('pointermove', move);
        row.removeEventListener('pointerup', up);
        if (wasPlaying) this.audio.play();
      };
      row.addEventListener('pointermove', move);
      row.addEventListener('pointerup', up);
    });
    row.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.visibleSecs = clamp(this.visibleSecs * (e.deltaY > 0 ? 1.15 : 0.87), 2, 40);
    }, { passive: false });
  }

  /* ---------- Loading ---------- */
  async load(track) {
    if (this.playing && this.fader.gain.value > 0.05 && this.xf.gain.value > 0.05) {
      toast(`Deck ${this.id} is playing live — pause it before loading a new track`);
      return;
    }
    this.audio.pause();
    this.track = track;
    this.an = null;
    this.bpm = null;
    this.firstBeat = 0;
    this.cuePoint = 0;
    this.hotcues = [null, null, null, null];
    this.loop = null;
    this.ovCache = null;
    this.ui.title.textContent = track.title;
    this.ui.artist.textContent = track.artist || 'Analysing…';
    this.audio.src = track.url;
    this.audio.preservesPitch = this.keylock;
    this.applyRate();
    this.updateButtons();
    renderLibrary();

    try {
      const an = await analyzeTrack(track);
      if (this.track !== track) return; // another track was loaded meanwhile
      this.an = an;
      this.bpm = an.bpm;
      this.firstBeat = an.firstBeat;
      this.ui.artist.textContent = track.artist || ' ';
      if (!an.bpm) toast(`Couldn't detect a BPM for "${track.title}" — use TAP`);
    } catch (err) {
      console.error(err);
      if (this.track === track) this.ui.artist.textContent = track.artist || ' ';
      toast(`Couldn't read "${track.title}"`);
    }
  }

  /* ---------- Transport ---------- */
  togglePlay() {
    if (!this.track) { toast(`Load a track into deck ${this.id} first`); return; }
    resumeAudio();
    if (this.playing) this.audio.pause();
    else { this.cuePreview = false; this.audio.play().catch(() => {}); }
  }
  cueDown() {
    if (!this.track) return;
    if (this.playing && !this.cuePreview) { // back to cue
      this.audio.pause();
      this.seek(this.cuePoint);
    } else if (!this.playing) {
      if (Math.abs(this.time - this.cuePoint) > 0.02) {
        this.cuePoint = this.snapToBeat(this.time, 0.08); // set cue here
      } else { // hold to preview from cue
        this.cuePreview = true;
        resumeAudio();
        this.audio.play().catch(() => {});
      }
    }
    this.updateButtons();
  }
  cueUp() {
    if (this.cuePreview) {
      this.cuePreview = false;
      this.audio.pause();
      this.seek(this.cuePoint);
      this.updateButtons();
    }
  }
  seek(t, keepLoop = false) {
    if (!this.track) return;
    const d = this.duration || 0;
    t = clamp(t, 0, Math.max(0, d - 0.01));
    if (this.loop && !keepLoop && (t < this.loop.start - 0.01 || t > this.loop.end)) this.exitLoop();
    this.audio.currentTime = t;
  }
  snapToBeat(t, tolerance) {
    if (!this.bpm) return t;
    const bl = this.beatLen;
    const snapped = this.firstBeat + Math.round((t - this.firstBeat) / bl) * bl;
    return Math.abs(snapped - t) <= tolerance ? Math.max(0, snapped) : t;
  }

  hotcue(i, clear) {
    if (!this.track) return;
    if (clear) { this.hotcues[i] = null; this.updateButtons(); return; }
    if (this.hotcues[i] == null) {
      this.hotcues[i] = this.snapToBeat(this.time, 0.06);
    } else {
      this.seek(this.hotcues[i]);
      if (!this.playing) { resumeAudio(); this.audio.play().catch(() => {}); }
    }
    this.updateButtons();
  }

  /* ---------- Loops ---------- */
  toggleLoop(beats) {
    if (!this.track) return;
    if (!this.bpm) { toast('Set a BPM first (TAP) to use beat loops'); return; }
    if (this.loop && this.loop.beats === beats) { this.exitLoop(); return; }
    const bl = this.beatLen;
    let start;
    if (this.loop) start = this.loop.start; // resize the current loop
    else {
      start = this.firstBeat + Math.floor((this.time - this.firstBeat) / bl + 0.25) * bl;
      start = Math.max(0, start);
    }
    this.loop = { start, end: start + beats * bl, beats };
    this.updateButtons();
  }
  exitLoop() { this.loop = null; this.updateButtons(); }

  /* ---------- Tempo / BPM ---------- */
  setTempo(t, moveSlider = true) {
    this.tempo = clamp(t, -this.range, this.range);
    if (moveSlider) this.ui.tempo.value = Math.round((this.tempo / this.range) * 1000);
    this.applyRate();
  }
  applyRate() {
    this.audio.playbackRate = clamp(this.rate, 0.25, 4);
    const pct = this.tempo * 100;
    this.ui.pct.textContent = `${pct >= 0 ? '+' : ''}${pct.toFixed(2)}%`;
  }
  tap() {
    const now = performance.now();
    if (this.taps.length && now - this.taps[this.taps.length - 1] > 2000) this.taps = [];
    this.taps.push(now);
    if (this.taps.length > 8) this.taps.shift();
    this.ui.tap.classList.add('on');
    setTimeout(() => this.ui.tap.classList.remove('on'), 90);
    if (this.taps.length >= 4) {
      const iv = (this.taps[this.taps.length - 1] - this.taps[0]) / (this.taps.length - 1) / 1000;
      const tapped = 60 / iv;
      this.bpm = Math.round((tapped / this.rate) * 100) / 100;
      if (this.track && this.playing) this.firstBeat = this.time % this.beatLen;
    }
  }
  scaleBpm(f) {
    if (!this.bpm) return;
    this.bpm = Math.round(this.bpm * f * 100) / 100;
  }
  sync() {
    const o = this.other;
    if (!this.track || !o.track) { toast('Load tracks on both decks to sync'); return; }
    if (!this.bpm || !o.bpm) { toast('Both decks need a BPM to sync'); return; }
    let r = o.effBpm / this.bpm;
    // Allow half/double-time matching (e.g. 70 vs 140).
    const options = [r, r * 2, r / 2];
    r = options.reduce((a, b) => (Math.abs(b - 1) < Math.abs(a - 1) ? b : a));
    const t = r - 1;
    if (Math.abs(t) > 0.5) { toast('Tempos are too far apart to sync'); return; }
    if (Math.abs(t) > this.range) {
      this.range = Math.abs(t) <= 0.16 ? 0.16 : 0.5;
      this.ui.range.textContent = `±${Math.round(this.range * 100)}%`;
    }
    this.setTempo(t);

    // Line up the beats (phase) with the other deck.
    const frac = (x) => x - Math.floor(x);
    const pO = frac((o.time - o.firstBeat) / o.beatLen);
    const pT = frac((this.time - this.firstBeat) / this.beatLen);
    let diff = pO - pT;
    if (diff > 0.5) diff -= 1;
    if (diff < -0.5) diff += 1;
    this.seek(this.time + diff * this.beatLen, true);

    this.ui.sync.classList.add('on');
    setTimeout(() => this.ui.sync.classList.remove('on'), 250);
  }

  /* ---------- UI updates ---------- */
  updateButtons() {
    this.ui.play.classList.toggle('on', this.playing && !this.cuePreview);
    this.ui.cue.classList.toggle('on', !!this.track && !this.playing);
    this.ui.pads.forEach((p, i) => {
      const set = this.hotcues[i] != null;
      p.classList.toggle('set', set);
      p.style.background = set ? HOTCUE_COLORS[i] : '';
    });
    this.ui.loops.forEach((b) => b.classList.toggle('on', !!this.loop && this.loop.beats === +b.dataset.loop));
  }

  tick() {
    // Loop enforcement
    if (this.loop && this.playing) {
      const t = this.time;
      if (t >= this.loop.end) this.audio.currentTime = this.loop.start + ((t - this.loop.end) % (this.loop.end - this.loop.start));
    }

    // Readouts
    const t = this.time, d = this.duration;
    this.ui.elapsed.textContent = fmtTime(t);
    this.ui.remain.textContent = `-${fmtTime(d - t)}`;
    this.ui.remain.classList.toggle('warn', !!this.track && this.playing && d - t < 30);
    this.ui.bpm.textContent = this.effBpm ? this.effBpm.toFixed(1) : '---.-';

    // Meter
    this.analyser.getFloatTimeDomainData(this.meterBuf);
    let pk = 0;
    for (let i = 0; i < this.meterBuf.length; i++) { const a = Math.abs(this.meterBuf[i]); if (a > pk) pk = a; }
    this.level = Math.max(pk, this.level * 0.9);
    drawVu(this.vu, this.level, true);

    this.drawOverview();
    this.drawDetail();
  }

  drawOverview() {
    const c = this.ui.overview;
    const resized = fitCanvas(c);
    const g = c.getContext('2d');
    const W = c.width, H = c.height;
    g.clearRect(0, 0, W, H);
    if (!this.an) {
      g.fillStyle = '#2a2530';
      g.fillRect(0, H / 2 - 0.5, W, 1);
      return;
    }
    if (!this.ovCache || resized || this.ovCache.width !== W) {
      const off = document.createElement('canvas');
      off.width = W; off.height = H;
      const og = off.getContext('2d');
      const n = this.an.overviewAmp.length;
      for (let x = 0; x < W; x++) {
        const i = Math.floor((x / W) * n);
        const a = this.an.overviewAmp[i] * (H / 2 - 2);
        og.fillStyle = waveColor(this.rgb, this.an.overviewLow[i]);
        og.fillRect(x, H / 2 - a, 1, a * 2 || 1);
      }
      this.ovCache = off;
    }
    g.drawImage(this.ovCache, 0, 0);
    const d = this.duration || 1;
    const px = (t) => (t / d) * W;
    // Played part dimmed
    g.fillStyle = 'rgba(11,10,12,0.55)';
    g.fillRect(0, 0, px(this.time), H);
    // Loop
    if (this.loop) {
      g.fillStyle = 'rgba(61,255,139,0.25)';
      g.fillRect(px(this.loop.start), 0, Math.max(2, px(this.loop.end) - px(this.loop.start)), H);
    }
    // Cue + hot cues
    g.fillStyle = '#ffc23a';
    g.fillRect(px(this.cuePoint), 0, 2, H);
    this.hotcues.forEach((h, i) => {
      if (h == null) return;
      g.fillStyle = HOTCUE_COLORS[i];
      g.fillRect(px(h), 0, 2, H);
      g.fillRect(px(h), 0, 8 * (window.devicePixelRatio || 1), 6 * (window.devicePixelRatio || 1));
    });
    // Playhead
    g.fillStyle = '#fff';
    g.fillRect(px(this.time) - 1, 0, 2, H);
  }

  drawDetail() {
    const c = this.detail;
    fitCanvas(c);
    const g = c.getContext('2d');
    const W = c.width, H = c.height, mid = H / 2;
    const dpr = window.devicePixelRatio || 1;
    g.clearRect(0, 0, W, H);

    if (!this.an) {
      g.fillStyle = '#1a171d';
      g.fillRect(0, mid - 0.5, W, 1);
      g.fillStyle = '#5c5464';
      g.font = `${12 * dpr}px system-ui, sans-serif`;
      g.textAlign = 'center';
      g.fillText(this.track ? 'Analysing…' : `Deck ${this.id} — empty`, W / 2, mid - 8 * dpr);
      return;
    }

    const t0 = this.time;
    const pxPerSec = W / this.visibleSecs;
    const tStart = t0 - W / 2 / pxPerSec;
    const { amp, lowRatio, fps } = this.an;
    const n = amp.length;
    const fpp = fps / pxPerSec; // frames per pixel

    // Loop region
    if (this.loop) {
      const x1 = (this.loop.start - tStart) * pxPerSec, x2 = (this.loop.end - tStart) * pxPerSec;
      g.fillStyle = 'rgba(61,255,139,0.13)';
      g.fillRect(x1, 0, x2 - x1, H);
    }

    // Beat grid
    if (this.bpm) {
      const bl = this.beatLen;
      const k0 = Math.ceil((tStart - this.firstBeat) / bl);
      const k1 = Math.floor((tStart + W / pxPerSec - this.firstBeat) / bl);
      for (let k = k0; k <= k1; k++) {
        if (k < 0) continue;
        const x = (this.firstBeat + k * bl - tStart) * pxPerSec;
        g.fillStyle = k % 4 === 0 ? 'rgba(255,255,255,0.28)' : 'rgba(255,255,255,0.09)';
        g.fillRect(Math.round(x), 0, k % 4 === 0 ? 2 : 1, H);
      }
    }

    // Waveform
    for (let x = 0; x < W; x++) {
      const f0 = Math.floor((tStart + x / pxPerSec) * fps);
      const f1 = Math.max(f0 + 1, Math.floor(f0 + fpp));
      if (f1 <= 0 || f0 >= n) continue;
      let m = 0, l = 0, cnt = 0;
      for (let f = Math.max(0, f0); f < f1 && f < n; f++) {
        if (amp[f] > m) m = amp[f];
        l += lowRatio[f]; cnt++;
      }
      const h = m * (mid - 3 * dpr);
      const past = x < W / 2;
      g.fillStyle = waveColor(this.rgb, cnt ? l / cnt : 0, past ? 0.45 : 1);
      g.fillRect(x, mid - h, 1, h * 2 || 1);
    }

    // Cue / hot cue markers
    const marker = (t, color, label) => {
      const x = (t - tStart) * pxPerSec;
      if (x < -20 || x > W + 20) return;
      g.fillStyle = color;
      g.fillRect(x - 1, 0, 2, H);
      if (label) {
        g.fillRect(x, 0, 14 * dpr, 14 * dpr);
        g.fillStyle = '#000';
        g.font = `bold ${10 * dpr}px system-ui, sans-serif`;
        g.textAlign = 'center';
        g.fillText(label, x + 7 * dpr, 11 * dpr);
      }
    };
    marker(this.cuePoint, '#ffc23a', 'C');
    this.hotcues.forEach((h, i) => h != null && marker(h, HOTCUE_COLORS[i], String(i + 1)));

    // Centre playhead
    g.fillStyle = '#fff';
    g.shadowColor = this.color; g.shadowBlur = 8 * dpr;
    g.fillRect(W / 2 - 1, 0, 2 * dpr, H);
    g.shadowBlur = 0;
  }
}

function drawVu(canvas, level, vertical) {
  fitCanvas(canvas);
  const g = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  g.clearRect(0, 0, W, H);
  const SEG = vertical ? 18 : 30;
  const db = 20 * Math.log10(Math.max(level, 1e-5));
  const lit = clamp(Math.round(((db + 42) / 42) * SEG), 0, SEG); // -42 dB .. 0 dB
  for (let i = 0; i < SEG; i++) {
    const on = i < lit;
    const col = i >= SEG - 2 ? '#ff2e4c' : i >= SEG - 5 ? '#ffc23a' : '#3dff8b';
    g.fillStyle = on ? col : '#1d1a21';
    if (vertical) {
      const sh = H / SEG;
      g.fillRect(0, H - (i + 1) * sh + 1, W, sh - 2);
    } else {
      const sw = W / SEG;
      g.fillRect(i * sw + 1, 0, sw - 2, H);
    }
  }
}

/* ================= Library ================= */
const library = [];
let nextTrackId = 1;
let selectedId = null;
const AUDIO_EXT = /\.(mp3|wav|aiff?|flac|m4a|aac|ogg|opus|webm)$/i;

function audioFiles(fileList) {
  return [...fileList].filter((f) => (f.type && f.type.startsWith('audio/')) || AUDIO_EXT.test(f.name));
}

function parseName(filename) {
  const base = filename.replace(/\.[^.]+$/, '').replace(/_/g, ' ').trim();
  const m = base.match(/^(.+?)\s+[-–—]\s+(.+)$/);
  return m ? { artist: m[1].trim(), title: m[2].trim() } : { artist: '', title: base };
}

function addToLibrary(files) {
  const added = [];
  for (const file of files) {
    if (library.some((t) => t.file.name === file.name && t.file.size === file.size)) continue;
    const { artist, title } = parseName(file.name);
    const track = { id: nextTrackId++, file, title, artist, url: URL.createObjectURL(file), analysis: null, analyzing: null };
    library.push(track);
    added.push(track);
  }
  if (added.length) {
    renderLibrary();
    toast(`Added ${added.length} track${added.length > 1 ? 's' : ''}`);
    queueAnalysis(added);
  }
  return added;
}

async function analyzeTrack(track) {
  if (track.analysis) return track.analysis;
  if (!track.analyzing) {
    track.analyzing = (async () => {
      const buf = await track.file.arrayBuffer();
      const audio = await ctx.decodeAudioData(buf);
      const channels = [audio.getChannelData(0).slice()];
      if (audio.numberOfChannels > 1) channels.push(audio.getChannelData(1).slice());
      const r = await MeltAnalyzer.analyze(channels, audio.sampleRate);
      track.analysis = r;
      renderLibrary();
      return r;
    })();
    track.analyzing.catch(() => { track.analyzing = null; });
  }
  return track.analyzing;
}

// Analyse new tracks one at a time in the background (fills in BPM + time).
let analysisChain = Promise.resolve();
function queueAnalysis(tracks) {
  for (const t of tracks) {
    analysisChain = analysisChain.then(() => analyzeTrack(t)).catch(() => {
      t.failed = true;
      renderLibrary();
    });
  }
}

function renderLibrary() {
  const body = $('#libBody');
  const q = $('#search').value.trim().toLowerCase();
  const rows = library.filter((t) => !q || `${t.title} ${t.artist}`.toLowerCase().includes(q));
  body.innerHTML = rows.map((t) => {
    const a = t.analysis;
    const cls = [t.id === selectedId ? 'sel' : '', decks.A?.track === t ? 'on-A' : '', decks.B?.track === t ? 'on-B' : ''].join(' ');
    const bpm = a ? (a.bpm ? a.bpm.toFixed(1) : '?') : t.failed ? '✕' : '…';
    return `<tr class="${cls}" data-id="${t.id}" draggable="true">
      <td class="c-num">${t.id}</td>
      <td>${escapeHtml(t.title)}</td>
      <td>${escapeHtml(t.artist)}</td>
      <td class="c-bpm">${bpm}</td>
      <td class="c-len">${a ? fmtTime(a.duration, false) : ''}</td>
      <td class="c-load"><button class="load-btn A" data-load="A">A</button><button class="load-btn B" data-load="B">B</button></td>
    </tr>`;
  }).join('');
  $('#libEmpty').style.display = library.length ? 'none' : 'flex';
  $('#libCount').textContent = `${library.length} track${library.length === 1 ? '' : 's'}`;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/* ================= Recording (WAV) ================= */
const recorder = {
  node: null, chunks: [], frames: 0, on: false, startedAt: 0,
  async init() {
    if (this.node) return;
    const code = `
      class MeltRec extends AudioWorkletProcessor {
        constructor() { super(); this.on = false; this.buf = new Int16Array(16384 * 2); this.n = 0;
          this.port.onmessage = (e) => { if (e.data === 'flush') this.flush(); else this.on = e.data; }; }
        flush() { if (this.n) { this.port.postMessage(this.buf.slice(0, this.n * 2)); this.n = 0; } this.port.postMessage('done'); }
        process(inputs) {
          const inp = inputs[0];
          if (this.on && inp && inp.length) {
            const L = inp[0], R = inp[1] || inp[0];
            for (let i = 0; i < L.length; i++) {
              const l = Math.max(-1, Math.min(1, L[i])), r = Math.max(-1, Math.min(1, R[i]));
              this.buf[this.n * 2] = l < 0 ? l * 0x8000 : l * 0x7fff;
              this.buf[this.n * 2 + 1] = r < 0 ? r * 0x8000 : r * 0x7fff;
              if (++this.n === 16384) { this.port.postMessage(this.buf.slice()); this.n = 0; }
            }
          }
          return true;
        }
      }
      registerProcessor('melt-rec', MeltRec);`;
    await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([code], { type: 'text/javascript' })));
    this.node = new AudioWorkletNode(ctx, 'melt-rec', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2] });
    this.node.port.onmessage = (e) => {
      if (e.data === 'done') { this.finish(); return; }
      this.chunks.push(e.data);
      this.frames += e.data.length / 2;
    };
    const silent = ctx.createGain();
    silent.gain.value = 0;
    limiter.connect(this.node);
    this.node.connect(silent).connect(ctx.destination);
  },
  async toggle() {
    resumeAudio();
    if (!this.on) {
      try { await this.init(); } catch (err) { console.error(err); toast('Recording is not available'); return; }
      this.chunks = []; this.frames = 0;
      this.node.port.postMessage(true);
      this.on = true; this.startedAt = performance.now();
      $('#recBtn').classList.add('on');
      $('#recLabel').textContent = 'STOP';
      toast('Recording your mix…');
    } else {
      this.on = false;
      this.node.port.postMessage(false);
      this.node.port.postMessage('flush');
      $('#recBtn').classList.remove('on');
      $('#recLabel').textContent = 'REC';
    }
  },
  finish() {
    if (!this.frames) { toast('Nothing was recorded'); return; }
    const sr = ctx.sampleRate, dataBytes = this.frames * 4;
    const h = new DataView(new ArrayBuffer(44));
    const str = (o, s) => [...s].forEach((ch, i) => h.setUint8(o + i, ch.charCodeAt(0)));
    str(0, 'RIFF'); h.setUint32(4, 36 + dataBytes, true); str(8, 'WAVE');
    str(12, 'fmt '); h.setUint32(16, 16, true); h.setUint16(20, 1, true); h.setUint16(22, 2, true);
    h.setUint32(24, sr, true); h.setUint32(28, sr * 4, true); h.setUint16(32, 4, true); h.setUint16(34, 16, true);
    str(36, 'data'); h.setUint32(40, dataBytes, true);
    const blob = new Blob([h.buffer, ...this.chunks], { type: 'audio/wav' });
    this.chunks = [];
    const d = new Date();
    const stamp = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}_${String(d.getHours()).padStart(2, '0')}${String(d.getMinutes()).padStart(2, '0')}`;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `Melt-Deck-Mix_${stamp}.wav`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);
  },
};

/* ================= Wiring ================= */
const decks = {};
decks.A = new Deck('A', '#ff6a1f');
decks.B = new Deck('B', '#33d0ff');
decks.A.other = decks.B;
decks.B.other = decks.A;

// Master volume
const masterKnob = new Knob($('#masterKnob'), {
  min: 0, max: 1, value: 0.8, def: 0.8,
  onChange: (v) => masterGain.gain.setTargetAtTime(v * v * 1.5, ctx.currentTime, 0.01),
});
masterKnob.onChange(masterKnob.value);

// Crossfader
let curve = 'smooth';
const xfader = $('#xfader');
function applyXfader() {
  const x = xfader.value / 1000; // -1 (A) .. +1 (B)
  let a, b;
  if (curve === 'smooth') {
    const p = (x + 1) / 2;
    a = Math.cos((p * Math.PI) / 2);
    b = Math.sin((p * Math.PI) / 2);
    // Keep full volume at centre for a smoother blend.
    a = Math.min(1, a * Math.SQRT2);
    b = Math.min(1, b * Math.SQRT2);
  } else { // cut: sharp scratch curve
    a = x < 0.9 ? 1 : clamp((1 - x) / 0.1, 0, 1);
    b = x > -0.9 ? 1 : clamp((x + 1) / 0.1, 0, 1);
  }
  decks.A.xf.gain.setTargetAtTime(a, ctx.currentTime, 0.005);
  decks.B.xf.gain.setTargetAtTime(b, ctx.currentTime, 0.005);
}
xfader.addEventListener('input', applyXfader);
xfader.addEventListener('dblclick', () => { xfader.value = 0; applyXfader(); });
$('#curveBtn').addEventListener('click', () => {
  curve = curve === 'smooth' ? 'cut' : 'smooth';
  $('#curveBtn').textContent = curve.toUpperCase();
  applyXfader();
});
applyXfader();

// Recording
$('#recBtn').addEventListener('click', () => recorder.toggle());

// Library: add, search, load, drag
const fileInput = $('#fileInput');
let pickTarget = null;
function pickFiles(deck = null) { pickTarget = deck; fileInput.click(); }
$('#addBtn').addEventListener('click', () => pickFiles(null));
fileInput.addEventListener('change', () => {
  const added = addToLibrary(audioFiles(fileInput.files));
  if (pickTarget && added[0]) pickTarget.load(added[0]);
  pickTarget = null;
  fileInput.value = '';
});
$('#search').addEventListener('input', renderLibrary);
$('#libBody').addEventListener('click', (e) => {
  const tr = e.target.closest('tr');
  if (!tr) return;
  const track = library.find((t) => t.id === +tr.dataset.id);
  const btn = e.target.closest('[data-load]');
  if (btn) { decks[btn.dataset.load].load(track); return; }
  selectedId = track.id;
  renderLibrary();
});
$('#libBody').addEventListener('dblclick', (e) => {
  const tr = e.target.closest('tr');
  if (!tr || e.target.closest('[data-load]')) return;
  const track = library.find((t) => t.id === +tr.dataset.id);
  const target = !decks.A.playing ? decks.A : !decks.B.playing ? decks.B : null;
  if (target) target.load(track);
  else toast('Both decks are playing — pause one first');
});
$('#libBody').addEventListener('dragstart', (e) => {
  const tr = e.target.closest('tr');
  if (tr) e.dataTransfer.setData('text/melt-track', tr.dataset.id);
});

// Window-wide drop = add to library
const overlay = $('#dropOverlay');
let dragDepth = 0;
function hideDropOverlay() { dragDepth = 0; overlay.classList.remove('show'); }
window.addEventListener('dragenter', (e) => {
  if (![...e.dataTransfer.types].includes('Files')) return;
  dragDepth++;
  overlay.classList.add('show');
});
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) hideDropOverlay(); });
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => {
  e.preventDefault();
  hideDropOverlay();
  if (e.dataTransfer.files.length) {
    const files = audioFiles(e.dataTransfer.files);
    if (files.length) addToLibrary(files);
    else toast('Those files are not audio files');
  }
});

// Keyboard shortcuts
const held = new Set();
window.addEventListener('keydown', (e) => {
  if (e.target.matches('input[type="search"]')) return;
  if (held.has(e.code)) return; // ignore key-repeat
  held.add(e.code);
  const A = decks.A, B = decks.B;
  const map = {
    KeyQ: () => A.togglePlay(), KeyW: () => A.cueDown(), KeyS: () => A.sync(),
    Digit1: () => A.hotcue(0, e.shiftKey), Digit2: () => A.hotcue(1, e.shiftKey),
    Digit3: () => A.hotcue(2, e.shiftKey), Digit4: () => A.hotcue(3, e.shiftKey),
    KeyP: () => B.togglePlay(), KeyO: () => B.cueDown(), KeyL: () => B.sync(),
    Digit7: () => B.hotcue(0, e.shiftKey), Digit8: () => B.hotcue(1, e.shiftKey),
    Digit9: () => B.hotcue(2, e.shiftKey), Digit0: () => B.hotcue(3, e.shiftKey),
  };
  if (map[e.code]) { e.preventDefault(); map[e.code](); }
});
window.addEventListener('keyup', (e) => {
  held.delete(e.code);
  if (e.code === 'KeyW') decks.A.cueUp();
  if (e.code === 'KeyO') decks.B.cueUp();
});
// Arrow keys glide the crossfader (key-repeat allowed).
window.addEventListener('keydown', (e) => {
  if (e.target.matches('input')) return;
  if (e.code === 'ArrowLeft' || e.code === 'ArrowRight') {
    e.preventDefault();
    xfader.value = clamp(+xfader.value + (e.code === 'ArrowLeft' ? -80 : 80), -1000, 1000);
    applyXfader();
  }
});

// Animation loop
const masterBuf = new Float32Array(masterAnalyser.fftSize);
let masterLevel = 0;
function frame() {
  decks.A.tick();
  decks.B.tick();
  masterAnalyser.getFloatTimeDomainData(masterBuf);
  let pk = 0;
  for (let i = 0; i < masterBuf.length; i++) { const a = Math.abs(masterBuf[i]); if (a > pk) pk = a; }
  masterLevel = Math.max(pk, masterLevel * 0.9);
  drawVu($('#masterVu'), masterLevel, false);
  if (recorder.on) $('#recTime').textContent = fmtTime((performance.now() - recorder.startedAt) / 1000, false);
  requestAnimationFrame(frame);
}
renderLibrary();
requestAnimationFrame(frame);
