'use strict';
/* Melt Deck — MIDI controller support with MIDI Learn.
   1. Click MIDI → START MIDI LEARN
   2. Click any button / knob / fader in Melt Deck
   3. Press or move the control on your hardware — done, it's linked.
   Mappings are saved and come back next time you open the app. */

const MeltMidi = (() => {
  const STORE_KEY = 'meltdeck.midi.mappings.v1';
  const targets = new Map();   // targetId -> { id, label, type, el, press, release, set }
  let mappings = {};           // midiKey -> targetId
  let learning = false;
  let learnTarget = null;
  let access = null;
  let shiftHeld = false;

  /* ---------- saved mappings ---------- */
  try { mappings = JSON.parse(localStorage.getItem(STORE_KEY) || '{}') || {}; } catch (_) { mappings = {}; }
  const save = () => {
    try { localStorage.setItem(STORE_KEY, JSON.stringify(mappings)); } catch (_) { /* storage unavailable */ }
    refreshMappedMarks();
  };

  /* ---------- what can be controlled ---------- */
  function addButton(id, label, el, press, release) {
    if (!el) return;
    el.dataset.midi = id;
    targets.set(id, { id, label, type: 'button', el, press, release });
  }
  function addRange(id, label, el, set) {
    if (!el) return;
    el.dataset.midi = id;
    targets.set(id, { id, label, type: 'range', el, set });
  }
  const knobRange = (knob) => (v) => knob.set(knob.min + v * (knob.max - knob.min));
  const sliderRange = (input, after) => (v) => {
    const min = +input.min, max = +input.max;
    input.value = Math.round(min + v * (max - min));
    input.dispatchEvent(new Event('input'));
    if (after) after();
  };

  function buildTargets() {
    for (const d of [decks.A, decks.B]) {
      const p = d.id, el = d.el, ch = $(`#ch${p}`);
      const name = `Deck ${p}`;
      addButton(`${p}.play`, `${name} Play`, d.ui.play, () => d.togglePlay());
      addButton(`${p}.cue`, `${name} Cue`, d.ui.cue, () => d.cueDown(), () => d.cueUp());
      addButton(`${p}.sync`, `${name} Sync`, d.ui.sync, () => d.sync());
      d.ui.pads.forEach((pad, i) => addButton(`${p}.hc${i + 1}`, `${name} Hot cue ${i + 1}`, pad, () => d.hotcue(i, shiftHeld)));
      d.ui.loops.forEach((b) => addButton(`${p}.loop${b.dataset.loop}`, `${name} ${b.dataset.loop}-beat loop`, b, () => d.toggleLoop(+b.dataset.loop)));
      addButton(`${p}.tap`, `${name} Tap BPM`, d.ui.tap, () => d.tap());
      addButton(`${p}.half`, `${name} BPM ÷2`, $('[data-act="half"]', el), () => d.scaleBpm(0.5));
      addButton(`${p}.double`, `${name} BPM ×2`, $('[data-act="double"]', el), () => d.scaleBpm(2));
      addButton(`${p}.load`, `${name} Load selected track`, $('[data-act="load"]', el), () => loadSelected(d));
      for (const dir of [-1, 1]) {
        addButton(`${p}.nudge${dir < 0 ? 'Down' : 'Up'}`, `${name} Nudge ${dir < 0 ? 'slower' : 'faster'}`,
          $(`[data-nudge="${dir}"]`, el),
          () => { d.nudge = 1 + dir * 0.04; d.applyRate(); },
          () => { d.nudge = 1; d.applyRate(); });
      }
      addButton(`${p}.keylock`, `${name} Key lock`, d.ui.keylock, () => d.ui.keylock.click());
      addButton(`${p}.range`, `${name} Tempo range`, d.ui.range, () => d.ui.range.click());
      addRange(`${p}.tempo`, `${name} Tempo`, d.ui.tempo, (v) => d.setTempo((v * 2 - 1) * d.range));
      const kn = { trim: 'Trim', hi: 'High EQ', mid: 'Mid EQ', lo: 'Low EQ', filter: 'Filter' };
      for (const [k, label] of Object.entries(kn)) addRange(`${p}.${k}`, `${name} ${label}`, d.knobs[k].el, knobRange(d.knobs[k]));
      addRange(`${p}.volume`, `${name} Volume fader`, $('.chfader', ch), sliderRange($('.chfader', ch)));
    }
    addRange('crossfader', 'Crossfader', xfader, sliderRange(xfader));
    addRange('master', 'Master volume', masterKnob.el, knobRange(masterKnob));
    addButton('curve', 'Crossfader curve', $('#curveBtn'), () => $('#curveBtn').click());
    addButton('rec', 'Record mix', $('#recBtn'), () => recorder.toggle());
    addButton('libUp', 'Library: previous track', $('#libUp'), () => libraryStep(-1));
    addButton('libDown', 'Library: next track', $('#libDown'), () => libraryStep(1));
  }

  /* ---------- library helpers (also used by the arrow keys) ---------- */
  function libraryStep(dir) {
    const rows = $$('#libBody tr');
    if (!rows.length) return;
    let i = rows.findIndex((r) => +r.dataset.id === selectedId);
    i = i < 0 ? (dir > 0 ? 0 : rows.length - 1) : clamp(i + dir, 0, rows.length - 1);
    selectedId = +rows[i].dataset.id;
    renderLibrary();
    const sel = $('#libBody tr.sel');
    if (sel) sel.scrollIntoView({ block: 'nearest' });
  }
  function loadSelected(deck) {
    const t = library.find((x) => x.id === selectedId);
    if (!t) { toast('Select a track in the library first'); return; }
    deck.load(t);
  }

  /* ---------- incoming MIDI ---------- */
  // Turns raw MIDI bytes into { key, kind, value (0..1), pressed }.
  function parse(data) {
    const [status, d1 = 0, d2 = 0] = data;
    const type = status & 0xf0, chan = (status & 0x0f) + 1;
    if (type === 0x90 || type === 0x80) {
      const on = type === 0x90 && d2 > 0;
      return { key: `note:${chan}:${d1}`, kind: 'note', value: d2 / 127, pressed: on, desc: `Note ${d1} (ch ${chan}) ${on ? 'on' : 'off'}` };
    }
    if (type === 0xb0) return { key: `cc:${chan}:${d1}`, kind: 'cc', value: d2 / 127, pressed: d2 >= 64, desc: `CC ${d1} (ch ${chan}) = ${d2}` };
    if (type === 0xe0) {
      const v = ((d2 << 7) | d1) / 16383;
      return { key: `pb:${chan}`, kind: 'pb', value: v, pressed: v > 0.5, desc: `Pitch bend (ch ${chan}) = ${Math.round(v * 100)}%` };
    }
    return null; // clock, aftertouch etc. are ignored
  }

  const lastPressed = new Map(); // midiKey -> was pressed (for CC buttons)

  function handle(data) {
    const m = parse(data);
    if (!m) return;
    $('#midiMonitor').textContent = m.desc;
    flashLed();

    if (learning) {
      if (m.kind === 'note' && !m.pressed) return; // wait for the press
      if (!learnTarget) { toast('Click a control in Melt Deck first, then move it on your controller'); return; }
      const t = targets.get(learnTarget);
      if (t.type === 'range' && m.kind === 'note') { toast(`Use a knob or fader for "${t.label}" (pads/keys work for buttons)`); return; }
      mappings[m.key] = t.id;
      save();
      toast(`Linked: ${t.label}  ←  ${m.desc.replace(/ (on|off)$| = .*$/, '')}`);
      setLearnTarget(null);
      return;
    }

    const id = mappings[m.key];
    const t = id && targets.get(id);
    if (!t) return;
    resumeAudio();
    if (t.type === 'range') { t.set(m.value); return; }
    // Buttons: react to press / release edges.
    const was = lastPressed.get(m.key) || false;
    lastPressed.set(m.key, m.pressed);
    if (m.pressed && !was) t.press();
    else if (!m.pressed && was && t.release) t.release();
  }

  /* ---------- devices ---------- */
  function refreshDevices() {
    const list = $('#midiDevices');
    const inputs = access ? [...access.inputs.values()] : [];
    inputs.forEach((inp) => { inp.onmidimessage = (e) => handle(e.data); });
    $('#midiCount').textContent = inputs.length;
    $('#midiBtn').classList.toggle('has-devices', inputs.length > 0);
    list.innerHTML = inputs.length
      ? inputs.map((i) => `<li><span class="dot ${i.state === 'connected' ? 'on' : ''}"></span>${escapeHtml(i.name || 'MIDI device')}</li>`).join('')
      : '<li class="none">No MIDI devices found. Plug in your controller (or a USB-MIDI cable) — it appears here automatically.</li>';
  }

  async function connect() {
    if (!navigator.requestMIDIAccess) {
      $('#midiDevices').innerHTML = '<li class="none">MIDI is not available on this computer.</li>';
      return;
    }
    try {
      access = await navigator.requestMIDIAccess({ sysex: false });
      access.onstatechange = refreshDevices;
      refreshDevices();
    } catch (err) {
      console.error(err);
      $('#midiDevices').innerHTML = '<li class="none">Melt Deck was not allowed to use MIDI.</li>';
    }
  }

  let ledTimer = null;
  function flashLed() {
    const b = $('#midiBtn');
    b.classList.add('activity');
    clearTimeout(ledTimer);
    ledTimer = setTimeout(() => b.classList.remove('activity'), 80);
  }

  /* ---------- learn mode UI ---------- */
  function setLearning(on) {
    learning = on;
    document.body.classList.toggle('midi-learn', on);
    $('#learnBtn').textContent = on ? 'STOP MIDI LEARN' : 'START MIDI LEARN';
    $('#learnBtn').classList.toggle('on', on);
    $('#learnBar').hidden = !on;
    if (on) $('#midiPanel').hidden = true;
    setLearnTarget(null);
  }
  function setLearnTarget(id) {
    $$('.midi-target').forEach((e) => e.classList.remove('midi-target'));
    learnTarget = id;
    const t = id && targets.get(id);
    if (t) t.el.classList.add('midi-target');
    $('#learnStatus').textContent = t
      ? `Now move or press the control on your hardware for: ${t.label}`
      : 'Click any button, knob or fader in Melt Deck…';
  }
  function refreshMappedMarks() {
    const mapped = new Set(Object.values(mappings));
    targets.forEach((t) => t.el.classList.toggle('midi-mapped', mapped.has(t.id)));
    const n = Object.keys(mappings).length;
    $('#mapCount').textContent = `${n} control${n === 1 ? '' : 's'} linked`;
  }
  function unmapTarget(id) {
    let removed = 0;
    for (const [k, v] of Object.entries(mappings)) if (v === id) { delete mappings[k]; removed++; }
    save();
    const t = targets.get(id);
    toast(removed ? `Unlinked: ${t.label}` : `${t.label} wasn't linked`);
  }

  // While learning, clicks pick a control instead of using it.
  const swallow = (e) => {
    if (!learning) return;
    if (e.target.closest('#learnBar')) return;
    const el = e.target.closest('[data-midi]');
    e.preventDefault();
    e.stopPropagation();
    if (e.type === 'pointerdown' && el) {
      if (e.button === 2) unmapTarget(el.dataset.midi);
      else setLearnTarget(el.dataset.midi);
    }
  };
  ['pointerdown', 'mousedown', 'click', 'dblclick', 'pointerup', 'wheel'].forEach((ev) =>
    document.addEventListener(ev, swallow, { capture: true, passive: false }));
  document.addEventListener('contextmenu', (e) => { if (learning) e.preventDefault(); }, { capture: true });

  /* ---------- wiring ---------- */
  function init() {
    buildTargets();
    refreshMappedMarks();

    const panel = $('#midiPanel');
    $('#midiBtn').addEventListener('click', (e) => { e.stopPropagation(); panel.hidden = !panel.hidden; });
    document.addEventListener('click', (e) => { if (!panel.hidden && !e.target.closest('.midi-wrap')) panel.hidden = true; });
    $('#learnBtn').addEventListener('click', () => setLearning(!learning));
    $('#learnDone').addEventListener('click', () => setLearning(false));
    $('#clearMaps').addEventListener('click', () => {
      if (!Object.keys(mappings).length) return;
      if (confirmClear()) { mappings = {}; save(); toast('All MIDI links cleared'); }
    });

    window.addEventListener('keydown', (e) => {
      if (e.key === 'Shift') shiftHeld = true;
      if (e.key === 'Escape' && learning) setLearning(false);
      if (e.target.matches('input')) return;
      if (e.code === 'ArrowUp' || e.code === 'ArrowDown') { e.preventDefault(); libraryStep(e.code === 'ArrowUp' ? -1 : 1); }
    });
    window.addEventListener('keyup', (e) => { if (e.key === 'Shift') shiftHeld = false; });

    connect();
  }

  // Two clicks within 3 seconds to clear (avoids a pop-up dialog).
  let clearArmed = 0;
  function confirmClear() {
    const now = Date.now();
    if (now - clearArmed < 3000) { clearArmed = 0; $('#clearMaps').textContent = 'Clear all'; return true; }
    clearArmed = now;
    $('#clearMaps').textContent = 'Click again to clear';
    setTimeout(() => { $('#clearMaps').textContent = 'Clear all'; }, 3000);
    return false;
  }

  init();
  return { handle, targets, get mappings() { return mappings; } };
})();
