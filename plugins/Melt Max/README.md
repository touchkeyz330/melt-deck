# Melt Max: by Meltdown Empire

A look-ahead brickwall mastering limiter for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of the classic
"maximizer" plugins. Put it last on the master: pull **Threshold** down and the beat gets louder, while nothing ever
passes the **Ceiling**. That includes peaks *between* samples, the ones that clip converters, streaming encoders and
MP3s. Original design and code. It isn't affiliated with any other company's limiter.

## Controls

**MAX tab**

| Control | Default | What it does |
|---|---|---|
| **Threshold** | −3 dB | How much louder: −6 dB gives up to 6 dB more level, limited at the ceiling |
| **Ceiling** | −0.3 dB | The highest level that can come out. −0.3 to −1.0 is common for streaming |
| **Release** | 80 ms | How fast the limiter lets go (when Auto Rel is off) |
| **Auto Rel** | On | Program-dependent release: quick recovery from single hits, slow under steady limiting, so no pumping on dense mixes and no dulled drums |
| **Reduction** | meter | Gain reduction right now (display only) |

**OUTPUT tab**

| Control | Default | What it does |
|---|---|---|
| **True Peak** | On | Detects peaks between samples (4× interpolation with a 0.5 dB safety margin) |
| **Dither** | Off | **24-bit**, **16-bit** (TPDF), or **16 Shaped** (TPDF with the noise moved up above ~10 kHz, where you hear it least). Use it only on the final bounce, at the bit depth you export |

## How it works

```
in × (−Threshold) → peak detector (true peak) → needed gain = Ceiling ÷ peak
→ lowest needed gain over the 1.5 ms look-ahead → smoothed over the same 1.5 ms
   (the gain glides down and arrives exactly at the peak: no overs, no clicks)
→ release (manual or Auto) → same gain on L and R → safety clamp at the Ceiling → dither
```

Latency: 1.5 ms of look-ahead plus 11 samples (77 samples at 44.1 kHz). It's reported to the host.

## Measured (desktop build)

Test beat: a deliberately harsh drum loop plus pad, peaking at −1 dBFS but **+1.0 dBTP** between samples.

| Threshold | Sample peak | True peak (16× check) | Louder by |
|---|---|---|---|
| −3 dB | −0.80 dBFS | **−0.64 dBTP** | +2.6 dB |
| −6 dB | −0.72 dBFS | **−0.64 dBTP** | +4.3 dB |
| −10 dB | −0.72 dBFS | **−0.64 dBTP** | +5.6 dB |
| −15 dB | −0.72 dBFS | **−0.64 dBTP** | +6.6 dB |

- With True Peak off, sample peaks stay exactly at −0.30 but true peaks reach +3.1 dBTP. That's why it's on by
  default.
- Below the threshold the output is bit-identical to the input, only delayed.
- A steady 1 kHz tone pushed 6 dB into the limiter: distortion is −130 dB (Auto) and −169 dB (80 ms). The gain
  stays smooth.
- 16-bit dither: every output sample lands exactly on the 16-bit grid.
  - TPDF noise is −96 dBFS and flat.
  - Shaped puts 25 dB less noise in the 0–4 kHz range than at 14–20 kHz.
- SDK host test: PASS (20/20). Memory checker: 20,000 blocks of random settings with full-scale noise in, no errors,
  and never above the ceiling.
- CPU: about 0.5 % of a desktop core.

Bugs found and fixed while testing:
- True-peak overs of up to +1.6 dBTP. The first interpolator was too soft on the very top end, and a peak between
  two samples only turned down the earlier one.
- The dither clamp knocked samples off the 16-bit grid.
- The meter broke project recall.

`Melt-Max-demo.wav` plays the same quiet, unmastered loop three times:
1. As is (−21.7 dBFS RMS).
2. Threshold −6 (−15.7 dBFS RMS).
3. Threshold −10 (−12.4 dBFS RMS).

## Tips

- Keep it last on the master, after Melt Bus or your EQ and compression.
- Watch **Reduction**. Around −2 to −4 dB on the loudest hits keeps punch; constant −6 or more starts to flatten
  the drums.
- For streaming, a Ceiling of −1.0 dB leaves room for lossy encoding.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Max"`
2. Copy `plugins/Melt Max/build/Meltdown Empire - VST - Melt Max/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Max as the last
   insert on the master.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
