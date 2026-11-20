# Forge EQ — by Meltdown Empire

A console-style preamp and EQ for Akai MPC standalone (MPC OS 3.10.1+), inspired by the
broad, musical sound of classic British console channels. Original name, design and code.

## Controls

| Control | What it does |
|---|---|
| **Drive** (0–30 dB) | Pushes the preamp. Low settings add warmth, high settings add audible grit. |
| **Iron** (0–100 %) | Transformer character: lows saturate first and even harmonics are added for weight. |
| **Auto Gain** | On (default): loudness stays matched as you change Drive, so you hear tone, not volume. Off: high Drive squashes peaks and gets quieter, like a real overdriven preamp. |
| **High Pass** | Off / 50 / 80 / 160 / 300 Hz, 18 dB per octave. |
| **Low Freq / Low Gain** | Shelf at 35 / 60 / 110 / 220 Hz, ±16 dB, with a slight bump for punch. |
| **Mid Freq / Mid Gain** | Bell at 360 Hz / 700 Hz / 1.6k / 3.2k / 4.8k / 7.2k, ±18 dB. Gets narrower the harder you push it. |
| **Mid Q** | Wide (classic broad bell) or Tight (more surgical). |
| **High Freq / High Gain** | Shelf at 10k / 12k / 16k ("Air"), ±16 dB. |
| **EQ** | EQ in/out (the preamp stays on). |
| **Mix** | Blend with the dry signal for parallel saturation. |
| **Output** | −24 to +12 dB trim. |

Presets: Flat Warm, Kick Weight, Snare Crack, Vocal Forward, Bass DI Grit, Drum Bus Heat.

## Measured (desktop, 44.1 kHz)

- Flat within ±0.15 dB from 35 Hz to 18 kHz (−0.7 dB at 20 kHz).
- Drive runs at 2× oversampling; aliasing at heavy drive on a 1 kHz tone is about −73 dB.
  Extreme drive on very bright sounds still adds some aliasing grit.
- Latency 31 samples (0.7 ms), reported to the MPC; the dry signal for Mix is aligned.
- About 0.6 % of a desktop CPU core per instance. Check several instances on the MPC.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Forge EQ"`
2. Copy `plugins/Forge EQ/build/Meltdown Empire - VST - Forge EQ/` into `VST2 Plugins` on a
   USB drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Load Forge EQ as an insert effect.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
