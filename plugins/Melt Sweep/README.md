# Melt Sweep: by Meltdown Empire

A one-knob DJ filter for Akai MPC standalone (MPC OS 3.10.1+). Put it on a track, a program or the master and turn
one knob:

```
   LP ◄──────────── OFF ────────────► HP
 darker, muffled    centre    thinner, no bass
```

- **Centre:** off. The plugin outputs your audio unchanged, sample for sample.
- **Left:** low-pass, 20 kHz down to 40 Hz. All the way left is only a deep rumble.
- **Right:** high-pass, 20 Hz up to 12 kHz. All the way right is only the sizzle.
- Resonance rises as you turn, so sweeps sing.
- The screen shows the cutoff: `LP 1.2k`, `OFF`, `HP 350`.
- The knob is smoothed, so fast twists and automation never click or zipper.

The **SWEEP** tab has only the knob. Assign it to a Q-Link, or record automation.

## SETUP tab (optional)

| Control | Default | What it does |
|---|---|---|
| Resonance | 65 % | How much the filter rings near the cutoff: about +4 dB at the default, about +20 dB (a whistle) at 100 % |
| Slope | 24 dB | 24 dB/oct is the steep club-mixer sound; 12 dB is gentler |
| Drive | 0 % | Warm saturation into the filter |
| Mix | 100 % | Blend with the dry signal |

## Tested (desktop build, white noise in, response measured)

| Knob | 60 Hz | 250 Hz | 1 kHz | 4 kHz | 12 kHz |
|---|---|---|---|---|---|
| −50 (LP) | 0.0 | +0.7 | +2.0 | −50 | −98 |
| 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 (exact bypass) |
| +50 (HP) | −67 | −19 | +1.9 | +0.1 | 0.0 |

- The pass band stays at 0 dB, so there's no volume drop when the filter kicks in. The resonant peak is +4.4 dB
  at the default setting.
- Fast twists through the whole range, including straight through the centre, show no clicks on a pure tone. Both
  filter chains run all the time, so they're already settled when you turn into them.
- SDK host test: PASS (20/20). Memory checker: 30,000 blocks of random settings, no errors.
- CPU: under 1 % of a desktop core.

`Melt-Sweep-demo.wav`: a beat swept slowly all the way down, back, all the way up, and back.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Sweep"`
2. Copy `plugins/Melt Sweep/build/Meltdown Empire - VST - Melt Sweep/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Sweep as an insert
   effect.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
