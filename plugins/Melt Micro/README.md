# Melt Micro: by Meltdown Empire

A compact, 4-voice analog-modelling synth for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of the small
analog-modelling keyboards of the early 2000s.

It's an original design. No code, waves, presets or names are taken from any product. "microKORG" and "KORG" are
trademarks of KORG Inc., and Melt Micro isn't affiliated with them.

## Sound engine

| Section | What's there |
|---|---|
| **OSC 1** | Saw, Square, Tri, Sine, **Vox**, **Digital**, **Cross**, Noise. **Ctrl 1** shapes the wave (see below). **Ctrl 2** sets how much LFO 1 moves Ctrl 1 |
| **OSC 2** | Saw, Square, Tri; Semi, Tune; **Osc Mod**: Off, Ring, Sync, Ring+Sync |
| **MIXER** | Osc 1, Osc 2, Noise levels (0–127) |
| **FILTER** | 24 LPF (saturating ladder that self-oscillates), 12 LPF, 12 BPF, 12 HPF; Cutoff, Resonance, EG1 Int, Key Track |
| **AMP** | EG 2, Level, Pan, Distortion |
| **EG 1 / EG 2** | ADSR (EG 1 goes to the filter, EG 2 to the amp) |
| **LFO 1** | Saw, Square, Tri, S&H. **LFO 2**: Saw, Square (+), Sine, S&H. Free (Hz) or synced to tempo |
| **VIRTUAL PATCH** | 4 slots. Sources: EG1, EG2, LFO1, LFO2, Velocity, Key Track, Mod Wheel. Destinations: Pitch, Osc2 Tune, Osc1 Ctrl1, Noise, Cutoff, Amp, Pan, LFO2 Rate. Amounts −63 to +63 |
| **VOICE** | Poly, Mono, Legato; Portamento; Bend; **Voices** 1–8 (default 4, like the classic) |
| **ARP** | Up, Down, **Alt 1** (up-down, ends once), **Alt 2** (up-down, ends twice), Random, **Trigger** (the whole chord each step); range 1–4 octaves; 1/4 to 1/32 including triplets; Gate |
| **FX** | Mod FX (**Cho/Flg**: low Depth flanges, high Depth choruses; **Ensemble**: three-voice; **Phaser**: 6 stages), then Delay (**Stereo** or **Cross** ping-pong, tempo-synced), then EQ (low and high shelf, ±12 dB) |

What **Ctrl 1** does for each osc 1 wave:

| Wave | Ctrl 1 |
|---|---|
| Saw | Waveform: blends in a saw an octave up (brighter, thinner) |
| Square | Pulse width |
| Tri | Wavefold |
| Sine | Brightness |
| Vox | Vowel morph: Oo → Ah → Ee → Choir |
| Digital | Picks one of 32 digital waves |
| Cross | How hard osc 2 modulates the sine |

Saws and squares are anti-aliased with polyBLEP; Vox and Digital use mip-mapped tables.

## Presets (all original)

Vintage Lead · Brassy Saws · Vox Choir · Digital Keys · Cross Bell · Ring Bass · Sync Lead · Phaser Pad ·
Ensemble Strings · Trigger Arp · Alt Arp Pluck · Wobble Square · Noise Snare Hit · Init

`Melt-Micro-demo.wav` plays 12 of them.

## Tabs (5)

**OSC**, **FLT/AMP**, **LFO**, **PATCH**, **ARP/FX**.

## The vocoder

The classic keyboard's vocoder needs a microphone in *and* keys to play. On the MPC, instrument plugins get no audio
input and effect plugins get no MIDI, so a vocoder can't live inside this synth. It would have to be a separate
effect with its own built-in chord (Key + Chord type) that you put on a vocal track. That's a possible next step.

## Tested (desktop build)

- **Pitch:** A4 = 440.08 Hz.
- **4-voice limit:** hold 6 notes and the 4 newest sound; the oldest two are taken over.
- **Arp at 120 BPM:** Alt 1 on C-E-G plays C E G E C E G E; Trigger plays the whole chord every 1/16 at an even level.
- **Presets:** all 14 play with no NaN.
- **SDK host test:** PASS (19/19). Memory checker: 20,000 blocks of random settings, presets and notes, no errors.
- **CPU:** about 0.6 % of a desktop core with 8 voices piling up.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Micro"`
2. Copy `plugins/Melt Micro/build/Meltdown Empire - VST - Melt Micro/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track and pick
   Melt Micro.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
