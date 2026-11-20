# Melt-Tune — by Meltdown Empire

Real-time vocal pitch correction for Akai MPC standalone (MPC OS 3.10.1+). From subtle
polish to the hard, robotic effect. Original design and code.

## Controls

| Control | What it does |
|---|---|
| **Key / Scale** | The notes the voice is pulled to. Scales: Chromatic (every note), Major, Minor, Harm Minor, Pent Major, Pent Minor, Blues, Dorian. |
| **Retune** (0–400 ms) | How fast a note is pulled in. **0 = hard tune** (instant, robotic, flattens vibrato). 20–40 ms = modern pop polish. 80 ms and up = natural, keeps scoops and vibrato. |
| **Amount** | How much of the correction is applied (100 % = all the way to the note). |
| **Transpose** | Shifts the whole voice ±12 semitones (set Amount to 0 for a pure octave/harmony shift). |
| **Voice** | Low (deep male, 60–500 Hz), Mid (most voices, 80–900 Hz), High (high female/child, 150–1500 Hz). Pick the range that fits the singer for the most stable tracking. |
| **Pitch** | Shows the note being sung and how far off it is (e.g. `A3 -18c`). Display only. |
| **Mix** | Blend with the dry voice. Keep at 100 % for normal use (below that you hear doubling/phasing). |
| **Output** | −24 to +12 dB. |

Presets: Pop Polish, Natural, Hard Tune, Hard Tune Minor (A minor), Octave Down, Octave Up.

Tips: put Melt-Tune first on the vocal track, before reverb and delay. Set the Key and Scale
of the song: in Chromatic mode a note sung between two semitones can snap either way.
Breaths and "s" sounds pass through untouched.

## How it works

A YIN pitch detector (at a quarter of the sample rate, every ~6 ms) finds the sung pitch and
projects it 15 ms ahead so fast movements are caught in time; the nearest allowed note is
chosen with a little hysteresis so it does not flicker; and a pitch-synchronous overlap-add
(PSOLA) shifter moves the voice there while keeping its natural tone.

## Measured (synthetic vowels, desktop, 44.1 kHz)

- Lands within about 1 cent of the target note from E2 to C5.
- Retune 0 ms reaches the note in ~10 ms; 25 ms in ~40 ms; 80 ms in ~105 ms.
- Processing artifacts 36–52 dB below the voice. Hard Tune reduces a ±40-cent vibrato to
  about ±18 cents (wider vibrato flips between notes: the classic hard-tune warble).
- Latency follows the voice: about two sung periods + 1 ms (≈ 6–25 ms). Not reported to the
  MPC, so for tight timing record dry and tune afterwards.
- About 0.8 % of a desktop CPU core.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt-Tune"`
2. Copy `plugins/Melt-Tune/build/Meltdown Empire - VST - Melt-Tune/` into `VST2 Plugins` on a
   USB drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Load Melt-Tune as an insert effect on your vocal track.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
