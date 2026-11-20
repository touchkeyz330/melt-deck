# Melt Stack: by Meltdown Empire

A 4-oscillator, 4-layer analog-modelling synth for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of today's
layered analog-modelling flagships: thick four-oscillator stacks, layers you stack, rotate or vector-blend, a
per-voice motion sequencer and a physics X/Y "ball" for hands-on modulation.

It's an original design. No code, sounds, presets or names are taken from any product. "multi/poly", "Kaoss" and
"KORG" are trademarks of KORG Inc., and Melt Stack isn't affiliated with them.

## Sound engine

| Section | What's there |
|---|---|
| **4 OSCILLATORS** | Each one is **Classic** (Shape sweeps saw → square → narrow pulse), **Shaper** (a triangle through a wavefolder; Shape = fold) or **Digital** (morphs across 32 band-limited waves), with its own Semi and Level. **Spread** detunes osc 2–4 against osc 1 |
| **MOD** | Noise, **Ring 1×2**, **Sync 2>1**, **X-Mod** (osc 1 bends the phase of osc 2–4) |
| **FILTER 1** | **Ladder 24** (saturating, self-oscillating), **MS 12** (screaming and saturating), BP 12, HP 12. **FILTER 2**: Off, HP, LP or Notch at an offset, in series |
| **ENV** | Filter and amp ADSR, Env Amount, Key Track, velocity |
| **LFO** | To pitch, cutoff, shape, level or pan. The mod wheel adds vibrato |
| **KEYS** | Poly, Mono, Legato; Glide; up to 16 voices |
| **FX** | Chorus, tempo-synced ping-pong delay, reverb |

## Layers

There are four variations of the sound, **A–D**. Each has its own **Semi**, **Tone** (brighter or darker, and more or
less osc shape) and **Level**. **Layer Mode** sets how they play:

- **Single:** A only.
- **Stack:** all four on every note, spread across the stereo field by **Layer Pan**.
- **Rotate:** every key press uses the next layer (A, B, C, D, A …), so repeated notes come out round-robin.
- **Random:** a random layer per key press.

## Motion sequencer (STEPS and SEQ tabs)

A per-voice step sequencer that restarts with every note, so each note in a chord runs its own copy.

- **Pitch 1–8:** a semitone offset per step.
- **Shape 1–8:** −100 to +100 per step, sent to **Shape Dest** (Cutoff, Osc Shape, Level, Pan or Resonance) by **Shape
  Amount**.
- **Seq Rate:** 1/4 to 1/32 including triplets, at the MPC's tempo.
- **Seq Steps:** 1–8.
- **Seq Smooth:** glides from one step to the next.

## Physics ball (KAOSS tab)

A ball on an X/Y surface.

- **Kaoss X / Kaoss Y** pull it toward a point. Put them on two Q-Links and "throw" it.
- **Gravity** pulls it down and **Friction** slows it. Walls bounce it, and friction also absorbs the bounce.
- **Kaoss X Dest / Y Dest** send its position to Cutoff, Shape, Pitch, Resonance or LFO Rate.
- **Vector** blends the four stacked layers by where the ball is: A bottom-left, B bottom-right, C top-left, D
  top-right. Use it with Layer Mode Stack.
- **Ball X / Ball Y** show where it is.

## Presets (all original)

Four Osc Wall · Stack Vector Pad · Rotate Keys · Motion Seq Pluck · Seq Melody Bass · Kaoss Bounce · Shaper Fold
Lead · Sync X-Mod Lead · MS Acid · Ring Bells · Digital Morph Pad · Init

`Melt-Stack-demo.wav` plays 11 of them.

## Tabs (6, one per function key)

**OSC**, **MIX/FX**, **FILT/ENV**, **LAYERS**, **STEPS**, **SEQ/KAOSS**.

## Tested (desktop build)

- **Pitch:** A4 = 440.08 Hz.
- **Layer Rotate:** C3 pressed five times plays 262, 523, 131, 392, 262 Hz (layers A +0, B +12, C −12, D +7, then A
  again).
- **Stack:** one key sounds all four layers.
- **Motion sequencer**, holding C3 at 1/16 and 120 BPM: steps play +0 +0 +12 +0 +7 +0 +12 +3 semitones, exactly the
  Pitch lane.
- **Physics ball:**
  - Gravity 100: it drops, hits the floor, bounces back to about half height, bounces smaller and settles.
  - Friction 90: it glides smoothly to the knob (+45 … +59 toward +60).
  - Friction 0: it overshoots to +98, swings back to +10 and keeps wobbling.
- **SDK host test:** PASS (19/19). Memory checker: 20,000 blocks of random settings, presets and notes, no errors.
- **CPU:** the heaviest case, Stack Vector Pad with 4 layers on an 8-note pile-up (16 voices × 4 oscillators), takes
  about 2.7 % of a desktop core. On the MPC, lower **Voices** if it stutters.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Stack"`
2. Copy `plugins/Melt Stack/build/Meltdown Empire - VST - Melt Stack/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track and pick
   Melt Stack. Assign **Kaoss X** and **Kaoss Y** to two Q-Links.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
