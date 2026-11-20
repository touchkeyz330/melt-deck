# Melt Flip — by Meltdown Empire

A song chopper for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of DJ-style sampling
plugins: load a song, it finds the **tempo, bars, key and hits**, and lays **16 chops across your
pads**. Change the **key** or **match your project tempo** without the chipmunk effect.
Original design and code.

## Getting songs in

Put WAV, MP3 or FLAC files in `Content/Songs/`, either in this project before you build, or straight
into the plugin folder on your MPC drive (`VST2 Plugins/Meltdown Empire - VST - Melt Flip/Content/Songs/`).
The Song menu is fixed when you build, so rebuild after adding songs to see them by name.
Songs longer than 8 minutes are cut at 8 minutes (memory).
Included: `Meltdown Demo 92bpm Amin.mp3`, an original Meltdown Empire beat to try it with.

## Pages

**FLIP**

| Control | What it does |
|---|---|
| **Song** | Pick a song. Loading + analysis takes about a second; the **Status** display shows progress. |
| **Chop** | **Hits**: the 16 strongest hits in the 8 bars from Start Bar. **Bars**: one bar per pad. **Beats**: one beat per pad (4 bars). **16ths**: one sixteenth per pad (1 bar, great for breaks). |
| **Start Bar** | Where in the song the chops start. Turn it to scroll through the song. |
| **Key Shift** | ±12 semitones, tempo unchanged (time-stretch). |
| **Tempo Sync** | On: the song is stretched to your MPC's tempo, key unchanged. |
| **BPM Fix** | If the detected tempo is half or double what you hear (common with trap/half-time), set x2 or /2. |
| **Song BPM / Song Key / Status** | What Melt Flip detected (after Key Shift and Sync), and what it is doing ("Rendering 40%", "Ready"). |

Key Shift and Tempo Sync re-render the song in the background (a few seconds for a full song);
the pads keep working meanwhile and switch over when it's ready.

**PADS**

| Control | What it does |
|---|---|
| **Play Mode** | One Shot (plays the whole chop), Gate (stops when you let go), Loop (loops while held). |
| **Choke** | Mono: a new pad cuts the previous one (classic chop style). Poly: pads ring over each other. |
| **Length** | Shortens every chop (5–100 %). |
| **Pitch** | Classic MPC re-pitch: ±24 semitones by speed (faster = higher). |
| **Attack / Release** | Fades on each chop. |
| **Filter** | DJ filter: left = low-pass, right = high-pass, centre = off. |
| **Keys Mode / Key Pad** | Keys Mode on: every note plays the Key Pad chop chromatically (C3 = original pitch). |

Pads: notes 36–51 (C1–D#2) are pads 1–16; other notes wrap around.

Presets: Chop Hits, Bar Loops (Bars + Loop + Tempo Sync), Beat Flip, Break Slicer (16ths, poly), Keys Mode.

## How it works

- **Analysis** (decimated to ~11 kHz): spectral-flux onset envelope; tempo from autocorrelation
  scored across metrical levels (16ths, 8ths, beat, half bar, bar); beat phase by comb filtering;
  bar start from kick energy plus chord changes; key from overtone-aware note salience matched
  against major/minor key profiles; hits by adaptive peak picking, refined to the sample.
- **Time-stretch**: phase vocoder (2048-point, 75 % overlap) with identity phase locking,
  transient phase reset and stereo-linked phases, plus a 16-tap windowed-sinc resampler for the
  pitch. Key and tempo change independently.

## Measured (desktop, on original test songs with known answers)

- Tempo: 92 / 124 / 78 / 140 BPM songs detected as 92.0 / 123.9 / 78.0 / 140.0.
- Key: A minor, E major, F# minor, G minor all correct; bar starts within 16 ms.
- Bar chops match the song sample-accurately (correlation 1.000, bar length exact).
- Key Shift +2: A minor → B minor at the same tempo; Tempo Sync 92 → 100 BPM: bars last exactly
  2.400 s; pitch accuracy of the stretcher on test tones: 0.0 cents.
- Rendering runs 50–85× faster than real time on a desktop core (a 4-minute song in ~3–5 s;
  expect a few times slower on the MPC). Playback CPU: about 0.1 % of a core.
- Tested with memory and thread checkers while switching songs, keys and tempo constantly,
  including broken and fake files.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Flip"`
2. Copy `plugins/Melt Flip/build/Meltdown Empire - VST - Melt Flip/` into `VST2 Plugins` on a
   USB drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Make a Plugin track and choose Melt Flip.

## Sampling and copyright

Melt Flip is a tool; the songs are not part of it. Sampling a commercial recording in a release
generally needs clearance from the owners of the master and the song. Only share song files you
have the right to share.

## License

Melt Flip code and the demo song: MIT, © Meltdown Empire.
`src/third_party/dr_wav.h`, `dr_mp3.h`, `dr_flac.h`: dr_libs by David Reid, public domain or
MIT-0 (your choice). Built with the inMusic MPC Plugin SDK (MIT).
