# Melt FM: by Meltdown Empire

A 4-operator FM synth for Akai MPC standalone (MPC OS 3.10.1+), for the glassy, punchy digital sounds of
80s FM: electric pianos, bells, slap-style basses, brass, mallets and clavs. Original design and code.

## How FM works here

There are four **operators**. Each one is an oscillator with its own envelope. An operator is either:
- a **carrier**, which you hear. Its Level sets the volume.
- a **modulator**, which bends the pitch of another operator many times a second. Its Level sets the
  brightness, and its Ratio sets the character: whole numbers sound musical, odd fractions sound like bells.

The **Algorithm** sets who modulates whom (`>` means "modulates", `|` separates parallel stacks):

```
1 4>3>2>1     one long stack: brightest and most complex
2 (3+4)>2>1   two modulators feed op 2
3 3>2>1,4>1
4 4>3>1,2>1
5 2>1 | 4>3   two 2-op stacks, the classic electric piano layout
6 4>1,2,3     one modulator feeding three carriers
7 4>3 |1|2
8 1|2|3|4     four sine carriers: an additive organ
```

**Feedback** (0-7) makes op 4 modulate itself: soft saw-like at 3-5, noisy at 7.

## Per operator (OP 1-4 pages)

- **Ratio:** 0.5, 1, 2 … 31 times the note.
- **Fine:** +0-99 %. 3 with Fine 50 gives a ratio of 4.5.
- **Detune:** ±20 cents.
- **Level:** 0-99, about 0.75 dB per step.
- **Wave:** Sine, Half, Abs or Square.
- **Envelope:** Attack, Decay, Sustain, Release.
- **Velocity:** on a modulator, harder hits sound brighter.
- **Key Scale:** makes the operator quieter (or, on a modulator, darker) higher up the keyboard.

## PATCH and FX pages

**PATCH:**
- Algorithm, Feedback, Volume.
- Key Mode Poly or Mono (legato, last-note priority), Portamento, Bend Range.
- Wheel Vib: how much vibrato the mod wheel adds.
- LFO: Rate, Wave, and Delay (a fade-in). It sends to Pitch, Bright (the modulator levels, for "talking"
  wobbles) and Amp.
- An optional low-pass filter (Cutoff and Reso; it is off at the top setting).

**FX:** Chorus, Delay, Reverb.

There are 6 tabs, one per function key.

## Patches (all original)

Tine Piano · Solid Bass · Deep Sine Bass · Bell Tower · Brass Section · Marimba Wood · Drawbar FM · Glass Pad ·
Pluck Harp · Metal Lead · Toy Keys · Synth Strings · Wobble Talk · Clav Funk · Kalimba Drop · Init FM

`Melt-FM-demo.wav` plays 15 of them.

## Tested (desktop build)

- SDK host test: PASS (19/19).
- Init FM plays a clean sine: A4 = 440.08 Hz, with the 2nd harmonic 122 dB down.
- A 2-operator test at modulation index π matches the textbook Bessel sideband levels.
- Memory checker: 30,000 blocks of random parameters, patch changes, notes, bends, mod wheel, sustain
  and odd block sizes. No errors, no NaN. This test found one bug, which is fixed: a wave-table read
  could go one step past the end.
- CPU: Glass Pad with 16 voices takes about 2 % of a desktop core.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt FM"`
2. Copy `plugins/Melt FM/build/Meltdown Empire - VST - Melt FM/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track
   and pick Melt FM.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT). It contains no patches, samples or names from
any other FM instrument.
