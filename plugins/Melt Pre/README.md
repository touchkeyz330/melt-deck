# Melt Pre: by Meltdown Empire

A British-style, two-stage class-A mic preamp for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of the classic
transformer-coupled "red knob" console pres. Use it on vocals, drums, bass, keys and samples for warmth, weight and
grit. Original model and code. It isn't a circuit copy of any product and doesn't use any trademarked names.

(Forge EQ also has a drive section. Melt Pre is the dedicated preamp: stepped gain, two stages, Impedance and
calibrated Auto Trim.)

## Controls (PRE tab)

| Control | Default | What it does |
|---|---|---|
| **Gain** | +10 dB | Stepped in 5 dB like the real thing (0 to +30). Low steps add clean warmth; +20 brings in the second stage; +25/+30 is thick and gritty |
| **Fine** | 0 dB | ±5 dB between steps |
| **Impedance** | Lo | **Lo**: thicker lows, softer top (+1.0 dB at 40 Hz, −1.3 dB at 12 kHz), iron saturates sooner. **Hi**: more open and crisp (+1.3 dB at 12 kHz), a touch leaner |
| **High Pass** | Off | 50 / 80 / 160 / 300 Hz, 18 dB/oct |
| **Phase** | Off | Polarity flip |
| **Output** | 0 dB | Trim, −24 to +12 dB |
| **Auto Trim** | On | Takes back the gain you add *and* the loudness the saturation shaves off, so the Gain knob changes colour, not volume |
| **Mix** | 100 % | Parallel blend (sample-aligned with the dry signal) |

## Inside

```
in → phase → Gain → 2× oversampling:
   INPUT TRANSFORMER (lows saturate first; Impedance sets how soon)
   STAGE 1, class A (2nd harmonic leads)
   STAGE 2, output amp (gets pushed past +20 dB: thicker, 3rd harmonic)
→ back to 1× → DC block → output transformer / Impedance tone → High Pass → Auto Trim → Output → Mix
```

## Measured (desktop build)

Harmonics of a 1 kHz sine at −18 dBFS (a typical recorded level), Impedance Lo:

| Gain | 2nd | 3rd | 5th |
|---|---|---|---|
| 0 | −45 dB | −71 dB | −138 dB |
| +10 (default) | −35 dB | −51 dB | −100 dB |
| +20 | −26 dB | −32 dB | −61 dB |
| +30 | −24 dB | −19 dB | −36 dB |

- Auto Trim keeps a beat at the same loudness from Gain 0 to +30 (within 0.1 dB). The constant +0.5 dB on that bass-
  heavy beat is the Lo Impedance tone, on purpose.
- High Pass is −2 to −3 dB at the set frequency, −17 to −18 dB an octave below and −33 to −36 dB two octaves below
  (18 dB/oct). 1 kHz is untouched.
- Phase flips polarity. Mix at 50 % stays aligned with the dry signal (correlation 0.997).
- SDK host test: PASS (20/20). Memory checker: 20,000 blocks of random settings with full-scale input, no errors.
- CPU: about 1.2 % of a desktop core. Latency: 31 samples (0.7 ms), reported to the host.

Problems found and fixed while testing:
- It was too coloured at low gain: 2nd harmonic −36 dB at Gain 0. Now −45 dB, thanks to proper internal headroom.
- Auto Trim lost up to 14 dB at +30. It's now calibrated per gain step.

`Melt-Pre-demo.wav` plays a beat and some keys four times:
1. Dry.
2. Gain +15, Lo.
3. Gain +25, Lo.
4. Gain +30, Hi.

All four are at the same loudness, thanks to Auto Trim.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Pre"`
2. Copy `plugins/Melt Pre/build/Meltdown Empire - VST - Melt Pre/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Pre as the first
   insert on a track.

Chain idea: **Melt Pre** (first insert) → **Forge EQ** → **Silk Opto** on the track. Then **Melt Bus** on the bus and
**Melt Max** last on the master.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
