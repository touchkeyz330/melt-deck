# Melt Stutter: by Meltdown Empire

A tempo-synced stutter / beat-repeat effect for Akai MPC standalone (MPC OS 3.10.1+). Hit **Stutter** and it grabs a
slice on the beat and repeats it: rolls, glitches, tape-stop dives, reverse chops and trance-gate chops. Original
design and code.

## Playing it

- **Stutter** is the button. Put it on a Q-Link (or record it as automation) and flip it while the beat plays.
  The repeat starts on the next grid point (**Quantize**, default 1/16), so it's always on time, even if your
  hand isn't.
- **Auto** fires the stutter for you. On every beat it rolls the dice (**Chance**) and stutters for **Auto Len**.
- Tempo and grid follow the MPC's transport. When the MPC is stopped it uses a free 90 BPM grid.

## Controls

**STUTTER tab**

| Control | Default | What it does |
|---|---|---|
| Stutter | Off | On = repeat |
| Rate | 1/16 | Slice length: 1/2 to 1/64, plus triplets (1/4T to 1/32T) |
| Gate | 100 % | Plays only the first part of each repeat, for choppy gated stutters |
| Roll | Off | **Faster**: each repeat is 25 % shorter, for the snare-roll build. **Slower**: each one is longer |
| Pitch | 0 st | Each repeat drops or rises by this much: −1 gives a tape-stop dive, +1 a riser |
| Fade | 0 % | Each repeat is this much quieter than the last |
| Reverse | Off | Repeats after the first play backwards |
| Mix | 100 % | Blend with the dry signal |

**AUTO tab**

| Control | Default | What it does |
|---|---|---|
| Auto | Off | Random stutters |
| Chance | 25 % | Odds per beat |
| Auto Len | 1/4 | How long each auto stutter lasts |
| Quantize | 1/16 | When a stutter may start: Off (immediately), 1/16, 1/8, 1/4, Bar |

## Tested (desktop build, 90 BPM drum break)

- Switched on off the grid, the repeat starts exactly on the next 1/16 (sample 51450).
- Every repeat is identical to the grabbed slice (correlation 1.000).
- Each repeat has 1.5 ms edges and the switch crossfades in 4 ms, so there are no clicks.
- 50 ms after switching off, the output is bit-identical to the input again.
- Roll Faster: repeats of 7350, 5512, 4134, 3101 … samples.
- Gate 50 %: the second half of each repeat is silent (−131 dBFS).
- Pitch −12: the 2nd repeat is exactly the slice at half speed. Reverse: exactly the slice backwards (correlation
  1.000 both).
- Auto 50 %: fired in 19 of 24 beats (each stutter lasts a beat, so they often run into each other).
- SDK host test: PASS (20/20). Memory checker: 30,000 blocks of random settings with full-scale input, no errors.
- CPU: about 0.06 % of a desktop core. Latency: none.

`Melt-Stutter-demo.wav` plays a 2-bar break five times, stuttering the second half of bar 2 each time:
1. 1/16.
2. 1/16 with Roll Faster.
3. Pitch −1 per repeat with Fade.
4. 1/8 Reverse with Gate.
5. 1/64 with Gate (buzz).

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Stutter"`
2. Copy `plugins/Melt Stutter/build/Meltdown Empire - VST - Melt Stutter/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support. Then add Melt Stutter as an
   insert on a drum track, a program or the master, and assign **Stutter** to a Q-Link.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
