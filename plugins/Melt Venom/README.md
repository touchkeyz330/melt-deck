# Melt Venom: by Meltdown Empire

A virtual-analog powerhouse synth for Akai MPC standalone (MPC OS 3.10.1+), in the spirit of the big German VA
synths of the 2000s: huge detuned leads, nasty basses, acid, plucks, pads and arps.

It's an original design. No code, sounds, wavetables, presets or names are taken from any product. "Virus" and
"Access" are trademarks of their owners, and Melt Venom isn't affiliated with them.

## Sound engine

| Section | What's there |
|---|---|
| **OSC 1 / OSC 2** | Saw, Pulse (PW), Tri, Sine, **Hyper** (1–9 detuned saws; Density + Detune) and **Wave** (32 band-limited waves; Table morphs smoothly between neighbours). Osc 2 adds Semi, Fine, hard **Sync**, **FM** (osc 1 bends osc 2's phase) and **Env>Osc2** (the filter envelope sweeps osc 2's pitch, the classic sync scream) |
| **MIX** | Osc balance, Sub (square or triangle, one octave down), Noise, Ring (osc 1 × osc 2) |
| **UNISON** | 1–4 copies per note, detuned (up to ±35 cents) and spread across the stereo field |
| **FILTER 1** | **Ladder 24** (zero-delay-feedback 4-pole that saturates and self-oscillates at full resonance), LP 12, BP, HP |
| **FILTER 2** | Off, LP, HP, BP or Notch at an offset (±48 st) from filter 1, in **Serial** or **Parallel** |
| **ENV** | Filter ADSR (also drives Env>Osc2) and amp ADSR, plus velocity to filter and to amp |
| **LFO 1** | To pitch, PW, wave Table and cutoff. **LFO 2**: to cutoff, pan and amp. Free (Hz) or synced (1/16 to 4 bars). Tri, Sine, Saw, Square, S&H |
| **KEYS** | Poly, Mono (retrigger) or Legato; Glide; Bend; Voices (1–12). The mod wheel adds LFO 1 vibrato |
| **ARP** | Up, Down, Up-Down, Random, Played; 1–3 octaves; 1/4 to 1/32 including triplets; Gate. Its clock starts on your first key, at the MPC's tempo |
| **FX** | Distortion → stereo Chorus → tempo-synced ping-pong Delay → Reverb |

The saws and pulses are anti-aliased with polyBLEP, and Wave mode uses mip-mapped tables.

## Presets (all original)

Hyper Lead · Venom Bass · Acid Line · Trance Pluck · Super Pad · Sync Scream · FM Bell · Wobble Sub · Ring Metal ·
Arp Pluck 16ths · Arp Trance Gate · Morph Pad · Noise Sweep FX · Square Lead · Reese Bass · Init

`Melt-Venom-demo.wav` plays 12 of them.

## Tabs (6, one per function key)

**OSC**, **MIX**, **FILTER**, **ENV**, **LFO**, **ARP/FX**.

## Tested (desktop build)

- **Pitch:** Init plays A4 at 440.08 Hz.
- **Aliasing** (energy between the harmonics) on a plain saw: −40 dB at A4 and −33 dB at C7. That's normal for
  polyBLEP: very high leads get a little digital sparkle.
- **Arp** at 120 BPM, holding C-E-G:
  - Up gives C E G C E G…
  - Up-Down gives C E G E C E G E…
  - Up over 2 octaves gives C E G C' E' G'…
  - Gate 50 % lets each note release halfway through its step.
- **Ladder:** self-oscillates at 100 % resonance. Key tracking moves its pitch with the note.
- **Presets:** every one plays with no NaN; levels are balanced.
- **CPU, desktop:** 0.5 % for a mono lead and 3 % for the heaviest case (Super Pad: 9+9 saws × 3 unison, 6 voices
  stacking up). The MPC's processor is slower. If it stutters, lower **Voices**, **Unison** or **Density**.
- **SDK host test:** PASS (19/19).
- **Memory checker:** 20,000 blocks of random settings, presets, notes, bends and sustain. No errors.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Venom"`
2. Copy `plugins/Melt Venom/build/Meltdown Empire - VST - Melt Venom/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then make a Plugin track and pick
   Melt Venom.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
