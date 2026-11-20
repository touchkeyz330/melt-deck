# Font Box — by Meltdown Empire

A SoundFont 2 (`.sf2`) player for Akai MPC standalone (MPC OS 3.10.1+).

## Adding SoundFonts

Put `.sf2` files in `Content/SoundFonts/`, either in this project before you build, or straight
into the plugin folder on your MPC drive (`VST2 Plugins/Meltdown Empire - VST - Font Box/Content/SoundFonts/`).
The File list on the touch screen is fixed when you build, so rebuild after adding files if you
want them to show by name in the menu. A file that cannot be read is skipped.

Included: `MeltdownTest.sf2`, a tiny original test font (synthesized waveforms) with 5 presets:
Saw Multi, Saw Up Octave, Velocity Layers, Stereo Pad and Test Kit (bank 128).
Only use SoundFonts whose license allows it. Many free ones are fine for music but not for
redistribution.

## Controls

| Control | What it does |
|---|---|
| **File** | Which SoundFont. Loading happens in the background; notes playing at that moment stop. |
| **Preset** | Which preset in the file, shown as `bank:program Name`, sorted by bank and program. |
| **Transpose / Fine** | ±24 semitones / ±50 cents. |
| **Velocity** | 100 % = the standard SoundFont velocity curve; 0 % = every note at full level. |
| **Bend Range** | Pitch-bend range in semitones. |
| **Brightness** | Moves every zone's filter up or down (up to ±5 octaves). Negative darkens any preset. |
| **Resonance** | Adds filter resonance. |
| **Attack / Release** | Scale the preset's own envelope times (−100 % = 10× shorter, +100 % = 10× longer). |
| **Bits** | 16 = clean, 12 = vintage sampler grit, 8 = lo-fi. |
| **Volume** | Output level. |

Presets (engine settings only; pick File and Preset after): Init, Darker, Long Tails, Snappy, 12-Bit.

## What is supported

Presets, instruments and global zones combined per the SoundFont 2.04 spec; key and velocity
ranges (layers and splits); root key, coarse/fine tune, sample pitch correction, scale tuning,
fixed key/velocity; loop modes (no loop, continuous, loop while held then play out); sample
offsets; volume envelope (delay, attack, hold, decay, sustain, release); attenuation; pan;
static low-pass filter and Q; exclusive classes (hi-hat choke); sustain pedal; pitch bend.
32 voices, cubic interpolation.

Not supported (yet): modulators other than the standard velocity curve, the modulation
envelope and LFOs (vibrato/tremolo/filter sweeps stored in the font), 24-bit samples, chorus
and reverb sends.

## Memory

The selected file's samples are loaded into RAM (16-bit). Files over 512 MB are refused (they
show "(too big)" and play a simple tone). Large General MIDI fonts (30–150 MB) are fine;
switching files briefly needs memory for both while the new one loads.

## Tested (desktop, 44.1 kHz)

Pitch within ±3 cents across zone splits; layers, preset-level tuning, stereo pan, envelopes
and hi-hat choke verified on the test font; 4,000 corrupted files parsed without a crash; rapid
file/preset switching checked with memory and thread checkers. About 0.2 % desktop CPU for a
3-note chord.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Font Box"`
2. Copy `plugins/Font Box/build/Meltdown Empire - VST - Font Box/` into `VST2 Plugins` on a USB
   drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Make a Plugin track and choose Font Box.

## License

Engine code and MeltdownTest.sf2: MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
SoundFont files you add keep their own licenses.
