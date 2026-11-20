# Melt MS: by Meltdown Empire

Melt MS is a semi-modular monosynth for the MPC. It is built around a pair of aggressive
high-pass and low-pass filters that scream and self-oscillate, plus a virtual patch panel. It
is an original design in the spirit of the late-70s Japanese semi-modular monos. No code,
samples, presets or panel artwork come from any product. "Korg" and "MS-20" are trademarks of
their owners and are used here only to describe compatibility.

## Sound engine

- **VCO 1:** Triangle, Saw, Pulse (PW 5–95 %) or Noise. Scale is 32' / 16' / 8' / 4'.
- **VCO 2:** Saw, Square, Narrow pulse or Ring (VCO 1 × VCO 2). It has its own scale and a ±12 semitone Pitch control.
- **Pitch modulation:** Freq Mod from the MG and from EG 1 (bipolar), portamento, master tune and bend range.
- **Filters:** HP into LP. Both are 12 dB zero-delay-feedback Sallen-Key-style filters with a saturating feedback loop.
  - Above about 93 % Peak each filter self-oscillates as a clean sine that tracks the cutoff.
  - Each filter has Cutoff, Peak, MG Mod and EG 2 Mod. Kbd Track moves both cutoffs.
  - Each gate "pings" the filters. At high Peak they ring, which gives the Filter Kick preset.
- **MG (LFO):** the shape morphs from falling ramp through triangle to rising saw. It also has a square output that clocks the sample & hold.
- **EG 1:** Delay, Attack, Release. **EG 2:** Hold, Attack, Decay, Sustain, Release. EG 2 drives the VCA.
- **Key modes:** Mono (last-note priority, retrigger), Legato (no retrigger, with glide) and Poly 4.
- **Output:** Drive and Volume.

## PATCH tab: the plugin's "CV jacks"

The plugin has four virtual patch cables. Each has **From**, **To** and **Amt** (±100 %).

**From (sources):**

| Group | Sources |
|---|---|
| Modulators | MG Tri, MG Square, EG1, EG2 Rev, Noise, S&H |
| Audio rate | VCO2 Out (filter FM and growl) |
| Played on the MPC | Kbd CV, Velocity, Wheel/Strip (touch strip or mod wheel, CC1), Pressure (pad aftertouch, channel or poly), Pitch Bend |

**To (destinations):** Total Freq, VCO2 Freq, HP Cutoff, LP Cutoff, VCA, PW, MG Freq, S&H Clock.

The default patch has Cable 1 set to Wheel/Strip → LP Cutoff, so the MPC touch strip opens the
filter. Assign the touch strip to CC1 (mod wheel) in the track's settings.

Noise and VCO2 Out are audio-rate sources, so they work as true filter FM. Patching one to Total Freq gives audio-rate pitch FM.

## Connecting a real MS-20 to the MPC Live III

The MPC Live III has 8 CV/Gate outputs (four stereo 3.5 mm jacks). The MPC's own **CV tracks**
and the **CV Playgroup** drive these jacks; a plugin can't. The SDK discards a plugin's MIDI
output, and the audio outputs aren't DC-coupled. So the plugin is for sound, and the hardware is
patched like this:

| Your synth | Pitch | Gate | How to hook it up |
|---|---|---|---|
| **MS-20 mini** | MIDI IN | MIDI IN | Easiest. Connect MPC MIDI OUT (or USB host) to the MS-20 mini's MIDI IN and use a MIDI track. Then use the MPC's CV outs for extra modulation (see below). |
| **MS-20M** (desktop kit) | **VCO1+2 CV IN (V/Oct)** | **TRIG IN** (V-trig, +5–15 V) | Works directly. Use an MPC CV track: one CV out to the V/Oct input and one gate out to TRIG IN. Use a TRS→TS split cable if the jack is stereo. |
| **Original MS-20 (1978)** | Hz/V | S-trig | Needs a converter. The MPC sends V/Oct and positive gates, but the MS-20 expects Hz/V (double the voltage = one octave up) and a negative S-trig (short to ground). Use a V/Oct→Hz/V converter with a V-trig→S-trig adapter, or a MIDI-to-CV box with Hz/V and S-trig outputs. |

**Tips:**
- **Tune the scale.** On the CV track, play C1, C2 and C3 and adjust the MS-20's tuning so the octaves line up. Analog VCOs drift, so let the synth warm up for about 10 minutes first.
- **Extra modulation from the CV Playgroup.** MPC LFOs and envelopes can go into the MS-20's patch inputs, such as the Total, cutoff and VCA/initial gain inputs. That gives you tempo-synced modulation the MS-20 can't make on its own.
- **Recording the MS-20.** Bring its output back into the MPC's inputs and record it to an audio track.
- **Check the patch-panel voltage ranges** in your MS-20 manual before patching, and keep the MPC CV range modest at first.

## Presets (all original)

Fat Bass, Screaming Lead, Filter FM Growl, S&H Bleeps, Wind Noise, Siren, Squelch Acid, Ring Clang,
MS Brass (Poly 4 with a delayed vibrato), Filter Kick (a self-oscillating filter swept by EG 2),
Pressure Pad Lead (pad pressure opens the filter and widens the pulse), and Init.

## Tabs (4)

| Tab | Controls |
|---|---|
| VCO | VCO 1 and 2, portamento, Freq Mod, tune, key mode, bend, kbd track |
| FILTERS | HP and LP Cutoff, Peak, MG and EG 2 amounts, drive, volume |
| MG / EG | MG wave and rate; EG 1 Delay, Attack, Release; EG 2 Hold, Attack, Decay, Sustain, Release |
| PATCH | Cables 1–4: From, To, Amt |

## Tested (desktop build)

- host_sim: PASS (19 passed, 0 warnings).
- Pitch: A4 measured 440.0 Hz. Mono last-note priority returns to the held note.
- Self-oscillation:
  - LP at 100 % Peak with no oscillators sustains a sine at 998 Hz for a 1000 Hz cutoff.
  - HP behaves the same.
  - At 90 % Peak the filter rings and dies away, as intended.
- Filter Kick sweeps from about 170 Hz down to 51 Hz.
- Filter FM (VCO2 → LP) adds about +12 dB of energy at 2–6 kHz and +23 dB above 6 kHz compared with the cable off.
- S&H steps to a new random pitch on each MG cycle.
- ASan/UBSan stress (3,000 random blocks of parameters, programs, MIDI, bend, wheel and sustain) was clean, with no NaN or Inf.
- CPU was under 1 % of one desktop core.

## Install

1. In the MPC Plugin SDK folder, run `python tools/build.py "plugins/Melt MS"`.
2. Copy `plugins/Melt MS/build/Meltdown Empire - VST - Melt MS/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support.
4. Make a Plugin track, pick Melt MS, and set the touch strip to CC1.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
