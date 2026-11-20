# Melt Quad: by Meltdown Empire

Melt Quad is a four-part performance instrument for the MPC. It has four synth parts (A–D) that you
can layer, split across the keyboard, or play from separate MIDI channels. The parts share a
ping-pong delay and a room reverb. It is an original design in the general spirit of four-part
performance instruments. No code, sounds, presets or artwork come from any other product, and all
26 sounds are original.

## The four parts

Each part is a full synth with these settings:

| Group | Settings |
|---|---|
| **Sound** | 26 original sounds. See the list below. |
| **On** and **Channel** | Channel is **Omni** (plays from any channel) or 1–16. |
| **Key Low / Key High** | The key range, for splits. For example, bass below C3 and bells above. |
| **Semi / Fine** | Tuning. |
| **Filter** | LP, BP or HP, with Cutoff, Reso and Env Amt. Velocity and key also open it a little. |
| **Envelope** | Attack, Decay, Sustain, Release. |
| **Mixer** | Level, Pan, Low EQ (200 Hz shelf ±12 dB), High EQ (6 kHz shelf ±12 dB), **Comp** (one knob, with auto make-up) |
| **Sends** | Delay Send (FX1) and Reverb Send (FX2) |

**Ways to set up the parts:**
- **Layer:** leave every part on Omni with the full key range. One pad then plays all the parts at once.
- **Split:** give the parts different key ranges.
- **Multi:** give each part its own channel and drive it from separate MPC tracks.

## Shared effects and master

- **FX1, ping-pong delay:** synced to the MPC tempo (1/16, 1/8T, 1/8, 1/8D, 1/4, 1/4D, 1/2). Each repeat bounces between left and right and gets darker. Controls: Feedback and Return.
- **FX2, room reverb:** Size (0.3–8 s), Damp and Return.
- **Master:** volume and stereo Width (0–200 %).
- **Mod wheel / touch strip:** adds vibrato. Pitch bend is ±2 semitones.

## Sounds (all original)

| Type | Sounds |
|---|---|
| Keys | Velvet Keys, Glass EP, Drawbar Organ, FM Bell, Metal Keys, Kalimba, Pluck |
| Pads and voices | Warm Pad, Silk Strings, Choir Ah, Air Vox, Comb Pad, Noise Air |
| Brass and wind | Soul Brass, Hollow Flute |
| Bass | Sub Bass, Saw Bass, Buzz Bass, Clip 808 (with a pitch drop) |
| Leads | Square Lead, Sync Lead, Odd Lead, Digital Vox, Stairs Arp, Fold Grit, Basic Saw |

The pads and strings use three detuned, stereo-spread oscillators. The rest are two-oscillator sounds.

## Presets

| Preset | What it is |
|---|---|
| Velvet Session | Keys + pad + sub bass on the low keys |
| Bell Split | Saw bass below C3, FM bell above |
| Choir Strings | Choir and strings layered |
| Trap Bells 808 | Kalimba and glass bells above, Clip 808 below |
| Soul Brass Stack | Brass with an organ an octave down |
| Vox Lead | Digital vox lead with a square an octave down |
| Four Layer Pad | All four parts layered |
| Pluck Keys | Pluck with metal keys |
| Init | One basic saw |

## Pages (6, one per function key)

| Page | Controls |
|---|---|
| MAIN | Sound and Level for A–D, delay, reverb, Master, Width |
| PART A–D | On, Channel, Semi, Fine, Key Low/High, Low/High EQ, Filter, Cutoff, Reso, Env Amt, ADSR |
| MIXER | Pan, Delay Send, Reverb Send and Comp for A–D |

## Tested (desktop build)

- host_sim: PASS (19 passed, 0 warnings).
- All 26 sounds were played at A3 and landed on the right pitch (220 Hz and its harmonics).
- The unison pads are truly stereo, and the single-oscillator sounds are mono-centred.
- Splits work:
  - In Bell Split, C5 sounds only part A, and C3 sounds only part B.
  - Turning off the part that owns a key silences that key completely.
- The worst case is an 8-note chord on all four parts of Four Layer Pad: 32 voices, with three oscillators on the pads. It peaked at −5.5 dBFS with no clipping, and rendered about 30 times faster than real time on a desktop.
- ASan/UBSan stress (2,000–3,000 random blocks of parameters, programs, MIDI, bend, wheel and sustain) was clean, with no NaN or Inf.
- If the MPC gets busy with large layered chords, turn off the parts you aren't using.

## Install

1. In the MPC Plugin SDK folder, run `python tools/build.py "plugins/Melt Quad"`.
2. Copy `plugins/Melt Quad/build/Meltdown Empire - VST - Melt Quad/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support.
4. Make a Plugin track and pick Melt Quad.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
