'use strict';
// Track analysis: waveform data (with bass colouring), BPM and beat grid.
// Runs in a background worker so the app never freezes while analysing.

function meltAnalyze(channels, sampleRate) {
  const HOP = 256;
  const c0 = channels[0];
  const c1 = channels.length > 1 ? channels[1] : channels[0];
  const len = c0.length;
  const frames = Math.max(1, Math.floor(len / HOP));
  const fps = sampleRate / HOP;

  const amp = new Float32Array(frames);      // peak level per frame
  const lowRatio = new Float32Array(frames); // how "bassy" each frame is (0..1)
  const lowEnv = new Float32Array(frames);   // bass energy (used for beat detection)

  // Two-pole low-pass at ~150 Hz to isolate kicks / 808s.
  const a = 1 - Math.exp((-2 * Math.PI * 150) / sampleRate);
  let lp1 = 0, lp2 = 0, maxAmp = 1e-9;

  for (let f = 0; f < frames; f++) {
    const start = f * HOP;
    let peak = 0, sum = 0, lowSum = 0;
    for (let i = start; i < start + HOP; i++) {
      const x = (c0[i] + c1[i]) * 0.5;
      lp1 += a * (x - lp1);
      lp2 += a * (lp1 - lp2);
      const ax = x < 0 ? -x : x;
      if (ax > peak) peak = ax;
      sum += x * x;
      lowSum += lp2 * lp2;
    }
    const rms = Math.sqrt(sum / HOP);
    const lowRms = Math.sqrt(lowSum / HOP);
    amp[f] = peak;
    lowRatio[f] = rms > 1e-6 ? Math.min(1, (lowRms / rms) * 1.3) : 0;
    lowEnv[f] = Math.log(1 + 200 * lowRms);
    if (peak > maxAmp) maxAmp = peak;
  }
  for (let f = 0; f < frames; f++) amp[f] /= maxAmp;

  // ---- Overview (whole-track) waveform ----
  const OV = 1600;
  const overviewAmp = new Float32Array(OV);
  const overviewLow = new Float32Array(OV);
  for (let b = 0; b < OV; b++) {
    const s = Math.floor((b * frames) / OV);
    const e = Math.max(s + 1, Math.floor(((b + 1) * frames) / OV));
    let m = 0, l = 0, n = 0;
    for (let f = s; f < e && f < frames; f++) {
      if (amp[f] > m) m = amp[f];
      l += lowRatio[f]; n++;
    }
    overviewAmp[b] = m;
    overviewLow[b] = n ? l / n : 0;
  }

  // ---- BPM detection (onset autocorrelation on the bass band) ----
  const onset = new Float32Array(frames);
  for (let f = 1; f < frames; f++) {
    const d = lowEnv[f] - lowEnv[f - 1];
    onset[f] = d > 0 ? d : 0;
  }
  // Analyse up to ~4 minutes, skipping the very start (intros are often beatless).
  const aStart = Math.min(Math.floor(frames * 0.05), Math.floor(fps * 15));
  const aEnd = Math.min(frames, aStart + Math.floor(fps * 240));
  const n = aEnd - aStart;

  let bpm = null, firstBeat = 0;
  const MIN_BPM = 70, MAX_BPM = 180;
  const lagMin = Math.floor((60 * fps) / MAX_BPM);
  const lagMax = Math.ceil((60 * fps) / MIN_BPM);

  if (n > lagMax * 4) {
    let mean = 0;
    for (let i = aStart; i < aEnd; i++) mean += onset[i];
    mean /= n;
    const o = new Float32Array(n);
    for (let i = 0; i < n; i++) o[i] = onset[aStart + i] - mean;

    const maxLag = Math.min(n - 1, lagMax * 2 + 2);
    const ac = new Float32Array(maxLag + 1);
    for (let lag = lagMin; lag <= maxLag; lag++) {
      let s = 0;
      for (let i = 0; i + lag < n; i++) s += o[i] * o[i + lag];
      ac[lag] = s / (n - lag);
    }
    const score = (lag) => {
      if (lag < lagMin || lag > lagMax) return -Infinity;
      const l2 = lag * 2;
      return ac[lag] + (l2 <= maxLag ? 0.5 * ac[l2] : 0);
    };
    let best = lagMin, bestS = -Infinity;
    for (let lag = lagMin; lag <= lagMax; lag++) {
      const s = score(lag);
      if (s > bestS) { bestS = s; best = lag; }
    }
    if (bestS > 0) {
      // Refine between frames for a precise tempo.
      let delta = 0;
      const sl = score(best - 1), sr = score(best + 1);
      if (isFinite(sl) && isFinite(sr)) {
        const den = sl - 2 * bestS + sr;
        if (den < 0) delta = Math.max(-0.5, Math.min(0.5, (0.5 * (sl - sr)) / den));
      }
      let rough = (60 * fps) / (best + delta);
      while (rough < 78) rough *= 2;
      while (rough >= 180) rough /= 2;

      // Comb score: how strongly onsets line up with a beat grid at this tempo.
      const comb = (tempo) => {
        const beatFrames = (60 * fps) / tempo;
        let bestOff = 0, bestSum = -Infinity;
        const steps = Math.max(1, Math.floor(beatFrames));
        for (let off = 0; off < steps; off++) {
          let s = 0, c = 0;
          for (let p = aStart + off; p < aEnd; p += beatFrames) {
            const i = Math.round(p);
            s += (onset[i] || 0) + 0.5 * (onset[i - 1] || 0) + 0.5 * (onset[i + 1] || 0);
            c++;
          }
          s /= c || 1;
          if (s > bestSum) { bestSum = s; bestOff = off; }
        }
        return { score: bestSum, off: bestOff, beatFrames };
      };

      // Fine search around the rough tempo (0.01 BPM steps) for precision.
      let fine = { score: -Infinity }, fineBpm = rough;
      for (let t = rough * 0.985; t <= rough * 1.015; t += 0.01) {
        const r = comb(t);
        if (r.score > fine.score) { fine = r; fineBpm = t; }
      }
      // Most music sits on a whole-number BPM: prefer it if it fits as well.
      const whole = Math.round(fineBpm);
      if (Math.abs(whole - fineBpm) < 0.25) {
        const w = comb(whole);
        if (w.score >= fine.score * 0.97) { fine = w; fineBpm = whole; }
      }
      bpm = Math.round(fineBpm * 100) / 100;
      firstBeat = ((aStart + fine.off) % fine.beatFrames) / fps;
    }
  }

  return {
    duration: len / sampleRate,
    fps, amp, lowRatio, overviewAmp, overviewLow, bpm, firstBeat,
  };
}

// ---- Run analysis in a background worker (falls back to the main thread) ----
const MeltAnalyzer = (() => {
  let worker = null;
  let nextId = 1;
  const pending = new Map();

  try {
    const code = `${meltAnalyze.toString()}
      onmessage = (e) => {
        const { id, channels, sampleRate } = e.data;
        try {
          const r = meltAnalyze(channels, sampleRate);
          postMessage({ id, r }, [r.amp.buffer, r.lowRatio.buffer, r.overviewAmp.buffer, r.overviewLow.buffer]);
        } catch (err) { postMessage({ id, error: String(err) }); }
      };`;
    worker = new Worker(URL.createObjectURL(new Blob([code], { type: 'text/javascript' })));
    worker.onmessage = (e) => {
      const p = pending.get(e.data.id);
      if (!p) return;
      pending.delete(e.data.id);
      e.data.error ? p.reject(new Error(e.data.error)) : p.resolve(e.data.r);
    };
    worker.onerror = () => { worker = null; };
  } catch (_) {
    worker = null;
  }

  function analyze(channels, sampleRate) {
    if (!worker) return Promise.resolve(meltAnalyze(channels, sampleRate));
    const id = nextId++;
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject });
      worker.postMessage({ id, channels, sampleRate }, channels.map((c) => c.buffer));
    });
  }

  return { analyze };
})();
