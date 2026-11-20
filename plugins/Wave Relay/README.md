# Wave Relay — by Meltdown Empire

A wave-sequencing synthesizer for Akai MPC standalone (MPC OS 3.10.1+). Every note walks
through a chain of up to 8 single-cycle waves, each step with its own wave, pitch and level,
crossfading from one to the next. Evolving pads, rhythmic textures, vocal morphs and arps.
Original design and code; all 32 built-in waves are generated from maths.

## Pages

| Page | Controls |
|---|---|
| **SEQ** | Steps (1–8), Order (Forward / PingPong / Random / Once), Crossfade (0 % = hard steps, 100 % = constant morphing), Key Sync, Sync + Step Div (follows the MPC's tempo) or Step Time (ms), Volume |
| **WAVES** | The wave for each of the 8 steps |
| **STEPS** | Pitch (±24 semitones) and Level for each step |
| **DRONE** | A steady second wave under the sequence: Drone Wave, Oct, Det(une), Mix; Vector sweeps the mix toward or away from the drone over Vec Time after each note; Bend Range; Chorus |
| **FILTER** | Cutoff, Resonance, Env Amount, Key Track, filter ADSR |
| **AMP** | ADSR, Velocity, Volume |

**Key Sync On**: each note starts the sequence from step 1. **Off**: one clock for all notes;
while the MPC plays it locks to the song position, so the steps land on the grid.

Presets: Relay Pad, Vox Morph, Glass Steps, Pulse Arp, Evolving Drone, Digital Bells,
Sync Bass, Choir Sweep.

## Waves

32 built in: Sine, Triangle, Saw, Square, Pulse 25, Pulse 10, Organ, Soft Organ, Hollow,
Bright Saw, Vox Ah, Vox Ee, Vox Oo, Glass, Bell Tone, Choir, FM Bright, FM Hollow, FM Metal,
Sync 2.5, Sync 4.3, Fold, Digital, Clip Sine, Half Sine, Ramp Sq, Comb Saw, Odd Saw, Stairs,
Buzz, Pluck, Metal.

Add your own in `Content/Waves/` (see the README there): single-cycle WAVs or 2048-point
wavetables. Every wave is band-limited, so high notes stay clean.

## Measured (desktop, 44.1 kHz)

Step pitches exact; steps land on the 1/8 grid at 120 BPM; aliasing on a saw at the top of the
keyboard is about 108 dB down; 12 voices; about 0.4 % of a desktop CPU core for a 4-note chord.
Checked with a memory checker under random notes and settings.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Wave Relay"`
2. Copy `plugins/Wave Relay/build/Meltdown Empire - VST - Wave Relay/` into `VST2 Plugins` on a
   USB drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin
   Support. Make a Plugin track and choose Wave Relay.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
