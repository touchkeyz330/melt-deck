# Melt Rack: by Meltdown Empire

A 4-tone rack synth for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of the classic 90s
ROM racks. Original design and code. It uses no samples or names from any other instrument.

## How a patch works

Every patch stacks up to **4 tones**. Each tone is a complete synth:

```
WAVE ─► pitch (Coarse, Fine, P Env, LFO vibrato, glide, bend, mod wheel)
     ─► FILTER (Off / LPF / BPF / HPF / Peak) + its own ADSR, Key Follow, LFO
     ─► AMP + its own ADSR, Velocity, LFO tremolo, Pan + LFO auto-pan
```

- **Key Lo / Key Hi** per tone let you build splits and layers (see *Bass + Keys Split*).
- **Struct 1-2 / 3-4 = Ring**: tone 2 (or 4) is ring-modulated by tone 1 (or 3), which gives bells and metallic sounds.
- **Key Mode Mono** gives legato with last-note priority: hold a note, play another, release it and you fall back to the first. **Portamento** glides between notes (in Poly too).
- **Mod wheel** adds vibrato (amount set by **Wheel Vib**). The sustain pedal and pitch bend (**Bend Range**) also work.
- **Effects:** EFX (Overdrive, Phaser, Delay, Enhancer), then stereo Chorus, then Reverb.
- 16 voices, each with up to 4 tones.

## Waves

33 built-in waves made from maths: Sine, Triangle, Saw, Square, pulses, organs, Vox Ah/Ee/Oo,
Glass, Bell Tone, Choir, FM Bright/Hollow/Metal, sync, fold, Pluck, Metal, **Noise**, and more.

**Add your own:** put WAVs in `Content/Waves/` (in the project before building, or in the plugin folder on
the MPC drive), then reload the plugin.
- A single cycle (up to 4096 points, no loop) becomes a wave you can play anywhere on the keyboard.
- A longer file becomes a sample. The **root note** and **loop** are read from the WAV's sampler (smpl)
  chunk, the one the MPC, Logic and most editors write. Without one, the root is C3 (60) and the
  sample plays once.
- Only use sounds you own or have the rights to.

## Patches (all original)

Rack Strings · Glass Heaven · Tine Keys · Funk Whistle (mono sine lead with glide and delayed vibrato) ·
Sub Rumble · Fretless Glide · Pizz Section · Rack Brass · Big Stab · Choir Air · Square Lead ·
Organ Drive · Analog Sweep · Ring Bell · Bass + Keys Split · Init Tone

`Melt-Rack-demo.wav` (sent with this zip) plays 14 of them in order.

## Screen pages

| Tab | What's there |
|---|---|
| PATCH | Volume, Key Mode, Portamento, Bend, Wheel Vib, Structures, tone On switches and Levels |
| TONE 1-4 | On, Wave, Level, Pan, Coarse, Fine, Key Lo/Hi, Velocity, Pitch Env, Filter type, Cutoff, Reso, Key Follow, Filter Env amount |
| FX | EFX type/amount/rate/mix, Chorus, Reverb |
| T1-T4 ENV | Filter ADSR, Amp ADSR, LFO rate/shape/fade and its depth to pitch/filter/amp/pan |

The MPC has 6 function keys. Tabs 7-10 (the ENV pages) haven't been tried on hardware yet.
If they're hard to reach, tell me and I'll fold them into fewer pages.

## Tested (desktop build)

- SDK host test: PASS (19/19, no NaN, no denormals).
- Pitch: tables and samples land on the right note (A4 = 440.1 Hz in the analysis). A looped user
  sample at 48 kHz holds its loop past the end of the file. A one-shot stops. A broken WAV is skipped.
- Mono legato: no retrigger, glide about 140 ms, falls back to the held note on release.
- Memory checker: 30,000 blocks of random parameters, patch changes, notes, bends, mod wheel, sustain
  and odd block sizes, with test WAVs present: no errors, no NaN.
- CPU: the heaviest patch (4 tones) with 16 voices takes about 3.5% of one desktop core. The MPC
  will need more.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Rack"`
2. Copy `plugins/Melt Rack/build/Meltdown Empire - VST - Melt Rack/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track
   and pick Melt Rack.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
"JV-1080" and "Roland" are trademarks of Roland Corporation. Melt Rack is not affiliated with Roland and
does not include its sounds.
