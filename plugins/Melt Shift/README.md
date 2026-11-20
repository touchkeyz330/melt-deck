# Melt Shift: by Meltdown Empire

A one-knob pitch shifter for Akai MPC standalone (MPC OS 3.10.1+). Put it on a sample, a loop, a vocal or a whole
beat and turn one knob:

```
  −12 st ◄──────── 0 ────────► +12 st
   lower        original        higher        (the tempo never changes)
```

- Snaps to whole semitones by default, so every position is in key. The screen shows `+7 st`, `-5 st` and so on.
- **Centre:** original pitch. The audio passes through untouched, only delayed (see Latency).
- The knob is smoothed, so moves glide instead of clicking.

The **PITCH** tab has only the knob. Assign it to a Q-Link, or automate it.

## SETUP tab (optional)

| Control | Default | What it does |
|---|---|---|
| Snap | On | Off gives a continuous knob for slides and detune |
| Mode | Smooth | **Smooth** is for melodies, chords, vocals, bass and full mixes. **Punchy** is tighter on drums and plucks; use it for beats, not chords |
| Mix | 100 % | Blend with the dry signal for harmonies (e.g. +7 at 50 %) |

## How it works (original code)

The input is time-stretched by the pitch ratio with a phase vocoder, then resampled back to its original length,
which moves the pitch.
- **Phase locking:** every band keeps its phase relation to its spectral peak.
- **Stereo:** phases are linked across channels, so the image holds.
- **Hits:** phases restart at real hits, so attacks stay sharp. Steady notes are never mistaken for hits.
- **Frame size:** Smooth uses 2048-point frames, Punchy 1024.
- **Timing:** Smooth, Punchy and the dry path share one fixed delay. Turning the knob, switching modes or using Mix
  never moves anything in time.

## Latency

4608 samples (104 ms at 44.1 kHz). This is reported to the MPC as the plugin's delay. If the MPC compensates for
plugin delay on your setup, everything stays in time. If you hear the track late, shift it earlier by that amount,
or use Melt Shift where the delay doesn't matter: resampling a loop, pads, vocals, risers, a part you bounce.
The delay is this long so a full octave down still sounds smooth.

## Tested (desktop build)

| Test | Result |
|---|---|
| 440 Hz tone at −12/−5/+7/+12 st | 220.04 / 329.73 / 659.12 / 879.50 Hz, level within 0.1 dB, artefacts 75-77 dB down (both modes) |
| Major chord (with harmonics) on A4 or A5, shifted +5, −7, +12 | artefacts 36-47 dB below the notes (Smooth) |
| Major chord low on A3, shifted up | notes correct, artefacts −13 dB (close low voicings are the hard case for any shifter) |
| Drum bursts, −12 to +12 st | arrive within about 1 ms of the reported delay |
| Fast knob moves on a break | no NaN, no clicks; peaks kept under full scale |

- SDK host test: PASS (20/20). Memory checker: 20,000 blocks of random settings and knob moves, no errors.
- CPU: about 2 % of a desktop core with both engines running.

Getting there took three engine designs. Two earlier ones failed these tests (chords drifted off pitch, a grain
design was 6 % off on pure tones), and the stretch-and-resample design is the one that passed. Along the way two bugs
were fixed: a delay jump when the knob moved, and steady tones being mistaken for drum hits.

`Melt-Shift-demo.wav`:
1. A break, original.
2. The break at −4 st (Punchy).
3. The break at +3 st (Punchy).
4. Fast knob moves.
5. A slow, continuous slide on a pad (Snap off).

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Shift"`
2. Copy `plugins/Melt Shift/build/Meltdown Empire - VST - Melt Shift/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Shift as an insert
   effect.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
