# Melt Crate: by Meltdown Empire

A sampler instrument for Akai MPC standalone (MPC OS 3.10.1+) in the spirit of the early-90s 16-bit sampling
keyboards behind a lot of classic hip-hop. Play your own WAVs: chop loops across pads, pitch them down, loop,
reverse, filter, and give them grit. Original design and code. It is not a circuit-accurate copy of any
product and contains no factory sounds from one.

## Your sounds

Put them in `Content/Samples/` (in the project before building, or in the plugin folder on the MPC drive),
then reload the plugin.

- **A WAV file** = one sound. The root note and loop come from the file's sampler (smpl) chunk, the one the MPC,
  Logic, Audacity and most editors write. Without one, the root is C3 (note 60) and there is no loop.
- **A folder of WAVs** = one multisampled instrument. Name each file with its note, e.g. `Keys C3.wav`,
  `Bass_F#1.wav` or `pad 48.wav` (C3 = 60), or give it a smpl chunk. Key ranges split halfway between notes.
- Mono or stereo; 8/16/24/32-bit; any sample rate. Up to 16M frames per sound (about 6 minutes of stereo).
- Only the selected sound is in memory. Picking another loads it in the background.
- **Presets keep your sound.** They change only the engine settings.

Demo sounds included (all original, made for this plugin): **Melt Break 90** (a 2-bar drum break to chop),
**Melt Pad Loop C3** (a seamless looped pad) and **Tine Keys** (a 5-note multisample).

## Chopping (Slices)

Set **Slices** to 2-64. The selected sample is cut into equal slices on consecutive notes, starting at
**Slice Key** (default 36, pad A01 in the MPC's default layout). 16 slices of a 2-bar loop = one 8th note per
pad. Slices play at the original pitch (Transpose still works, for the pitched-down sound).
- **Trigger One Shot** plays the whole slice from one tap.
- **Gate** plays only while you hold the pad.
- Hitting the same pad again cuts the previous hit.

## Pages

| Tab | Controls |
|---|---|
| SAMPLE | Sound, Loop Mode (One Shot, Loop, Ping-Pong, Reverse, Loop+Rel), Trigger, Slices, Slice Key, Start, Vel>Start, Transpose, Fine, Key Track, Voice Mode (Poly/Mono), Glide, Bend Range, Volume, Pan |
| FILTER | Filter (Off, LP 24, LP 12, Band, HP 12, HP 24), Cutoff, Reso, Filt Env, Key Follow, Vel>Filter, filter ADSR |
| AMP/MOD | Amp ADSR, Vel>Amp, Pitch Env + Decay, LFO Rate/Wave/Delay, LFO>Pitch/Filter/Amp |
| COLOR/FX | Character, Bits, Drive, Chorus, Delay, Reverb |

- **Vel>Start:** softer hits start later in the sample, a classic sampler trick for expressive drums and stabs.
- **Loop+Rel:** loops while held, then plays on to the end of the sample when you let go.

## Character

| Mode | What it does |
|---|---|
| Clean | Cubic interpolation, full rate, no colouring |
| Vintage | Linear interpolation, 32 kHz engine hold, gentle 15 kHz smoothing |
| Crunch | No interpolation (pitched-down samples get gritty), 26 kHz hold, 12-bit, 11 kHz smoothing |
| Dusty | No interpolation, 20 kHz hold, 12-bit, darker 7.5 kHz smoothing |

**Bits** (6-16) and **Drive** add more on top.

## Presets

Keys Vintage, Chop 16 Pads, Chop 8 Gate, Loop Crunch, Dusty Filtered, Pitched Down Dirt, Reverse Swell,
Pad Loop Wide, Mono Bass Glide, Clean Hi-Fi.

`Melt-Crate-demo.wav` has five sections:
1. The break chopped across 16 pads, Clean.
2. The same chops at Crunch, 3 semitones down.
3. Dusty, 5 semitones down, filtered.
4. Tine Keys chords.
5. The pad loop held.

## Tested (desktop build)

- SDK host test: PASS (19/19).
- Multisample: each note plays from the right file at the right pitch (within 0.2 Hz).
- A slice output matches its part of the source exactly (correlation 1.000, sample-accurate start) and stops
  cleanly at the slice end. Reverse matches the reversed source exactly.
- A looped pad held for 8 s has no clicks at the loop points.
- Memory and thread checkers: 20,000 blocks of random settings, presets, notes and **sound switching while
  loading**, with broken and truncated WAVs in the folder. No errors, no NaN.
- CPU: 16 voices with LP 24 filters and effects take about 2 % of a desktop core.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Crate"`
2. Copy `plugins/Melt Crate/build/Meltdown Empire - VST - Melt Crate/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track
   and pick Melt Crate.

## Sampling and copyright

Melt Crate plays whatever you give it. Samples from other people's records need clearing before you release
or license the music, which matters for sync. Your own recordings and cleared or royalty-free samples are
safe to use.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT). "ASR-10" and "Ensoniq" are trademarks of
their owners; Melt Crate is not affiliated with them.
