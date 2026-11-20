# Silk Opto — by Meltdown Empire

An optical-style tube compressor for Akai MPC standalone (MPC OS 3.10.1+), inspired by the
smooth, musical behaviour of classic opto tube compressors. Original name, design and code.

## Controls

| Control | What it does |
|---|---|
| **Threshold** (−40–0 dB) | Where compression starts. Wide 10 dB soft knee, so it eases in. |
| **Ratio** (1.5:1–10:1) | How hard it compresses above the threshold. |
| **Gain** (0–30 dB) | Make-up gain after compression. |
| **Timing** | **Fixed**: the opto character. Fast attack, a release that lets go of half the compression in ~150 ms, then slows down, and slows down more the longer and harder it has been working. **Manual**: your Attack and Release. **Fix+Man**: a blend of both. |
| **Attack / Release** | Used in Manual and Fix+Man. |
| **Tube** (0–100 %) | Tube output colour: 2nd and 3rd harmonics for warmth and density. |
| **SC HPF** | Side-chain high-pass (Off / 60 / 100 / 150 Hz): stops kick and bass from pumping the whole mix. |
| **Mix** | Parallel ("New York") compression: blend with the dry signal. |
| **Link** | On: both channels compress together (stable stereo image). Off: independent. |
| **GR** | Shows the current gain reduction (moves by itself; turning it does nothing). |

Presets: Gentle Glue, Vocal Smooth, Bass Level, Drum Bus Squash, Mix Bus Silk, Lo-Fi Pump.

## Measured (desktop, 44.1 kHz)

- Threshold −18 dB, 4:1: 0 dB in → −13.7 dB out (ideal −13.5).
- Fixed timing release (90 %): 1.3 s after a short hit, 3.8 s after 4 s of heavy compression.
- Fully transparent when not compressing with Tube at 0.
- No latency. About 0.1 % of a desktop CPU core per instance.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Silk Opto"`
2. Copy `plugins/Silk Opto/build/Meltdown Empire - VST - Silk Opto/` into `VST2 Plugins` on a
   USB drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Load Silk Opto as an insert effect.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
