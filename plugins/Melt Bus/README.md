# Melt Bus: by Meltdown Empire

Console bus colour for Akai MPC standalone (MPC OS 3.10.1+). It's an original model in the spirit of the classic
British transformer-coupled, class-A console summing amps: warmth, weight and glue, from a touch of colour up to
real drive. Put it on your drum bus, music bus or master.

It is not a copy of any product's circuit, and the name "Neve" is not used. That's a trademark of AMS Neve.

## Controls

**BUS tab**

| Control | Default | What it does |
|---|---|---|
| **Drive** | +4 dB | How hard you hit the console. Low settings add a little 2nd-harmonic warmth, higher ones bring in 3rd harmonics and round off peaks |
| **Tone** | Warm | **Flat**; **Warm** (+1.2 dB at 40 Hz, softer top); **Bright** (+1.4 dB at 12 kHz, slightly leaner lows) |
| **Output** | 0 dB | Trim, ±12 dB |
| **Mix** | 100 % | Parallel blend. Dry and wet are sample-aligned, so there's no phasing |

**CONSOLE tab**

| Control | Default | What it does |
|---|---|---|
| **Auto Gain** | On | Turning Drive changes the colour, not the volume. It's tuned on a full mix around −16 dBFS RMS |
| **Bleed** | 30 % | Console crosstalk: a little of each side leaks into the other, mostly mids and highs (up to −35 dB) |
| **Noise** | Off | A faint console hiss (about −90 dBFS) |

## Inside

```
in × Drive → 2× oversampling →
   INPUT TRANSFORMER: the lows (below ~100 Hz) saturate first, like an iron core
   CLASS-A AMP: a gently biased soft clipper; 2nd harmonic first, 3rd as you push
→ back to 1× → DC block → Tone → Bleed → Auto Gain → Mix → Output
```

## Measured (desktop build)

Harmonics of a 1 kHz sine at −12 dBFS, Tone Flat:

| Drive | 2nd | 3rd | 5th |
|---|---|---|---|
| 0 dB | −40 dB | −46 dB | −90 dB |
| +4 dB (default) | −37 dB | −38 dB | −75 dB |
| +10 dB | −32 dB | −27 dB | −52 dB |
| +18 dB | −31 dB | −16 dB | −29 dB |

On a beat peaking at −2 dBFS, Auto Gain keeps loudness within 0.2 dB from Drive 0 to +24. Peaks come down from
−3.3 to −7.9 dBFS as you push, which is the glue.

- Flat is flat to 18 kHz (0.0 dB).
- Aliasing on a 7 kHz tone driven hard is 38 dB down.
- Mix is sample-aligned with the dry signal.
- SDK host test: PASS (20/20).
- Memory checker: 20,000 blocks of random settings with full-scale noise in, no errors.
- CPU: about 1.5 % of a desktop core.
- Latency: 31 samples (0.7 ms), from the oversampling filters. It's reported to the host.

Bugs found and fixed while testing:
- Mix combed against the dry signal because the wet path was half a sample late.
- The top end rolled off 1.1 dB at 18 kHz.
- No drive was already too coloured: the 2nd harmonic was at −30 dB, now −40.
- Auto Gain lost up to 10 dB at full drive.

`Melt-Bus-demo.wav` plays a beat four times, at the same loudness:
1. Dry.
2. Drive +4, Warm.
3. Drive +10, Warm.
4. Drive +16, Bright.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Bus"`
2. Copy `plugins/Melt Bus/build/Meltdown Empire - VST - Melt Bus/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Bus as an insert on
   a submix, a program or the master.

Tip: put a copy on each bus at a low Drive (+2 to +4) and one on the master. It's the "everything through the desk"
sound, the same idea as running a mix through a console.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
