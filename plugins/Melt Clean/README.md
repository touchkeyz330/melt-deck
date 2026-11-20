# Melt Clean: by Meltdown Empire

Melt Clean is a one-fader noise suppressor for the MPC. Push the **Clean** fader up and steady
background noise goes down while the vocal or instrument stays put. It handles hiss, room tone,
fan or AC noise, mic-pre noise, and the noise bed of old samples, tape or vinyl. There is nothing
to set up: no "learn" button, no threshold. It is an original design in the spirit of one-fader
dialogue noise suppressors. "Waves" and "NS1" are trademarks of their owners and are named here
only for comparison.

## How to use it

1. Put Melt Clean on a vocal, a sample, an audio track or a bus.
2. Give it about **3 seconds** of audio. It learns the noise floor by itself from the quietest
   moments in each frequency band, and it keeps learning, so it follows the noise if it changes.
3. Raise **Clean** until the noise is gone, then back off a little if the sound gets watery or swirly.
   - About 25 % is gentle, good for vocals.
   - About 50–75 % suits noisy recordings.
   - 100 % is maximum.

The **Noise Removed** meter shows how much the plugin is taking out, in dB.

| Clean | What it does |
|---|---|
| 0 % | Exact bypass, bit-for-bit, delayed to stay in sync |
| 25 % | Light: about 7 dB less noise |
| 50 % | Medium: about 14 dB less noise |
| 75 % | Heavy: about 20 dB less noise |
| 100 % | Max: about 24–26 dB less noise |

## How it works

- **Spectral analysis:** the sound is split into 513 frequency bands. Each band gets a gain:
  bands that sit at the noise floor are turned down, and bands with signal in them pass.
- **Smoothing:** the gains are smoothed across neighbouring bands and over time (fast to
  open, gentle to close), so you don't get "twinkly" musical-noise artifacts.
- **Tone guard:** noise flickers from moment to moment, but a held note is steady, so steady
  bands are left alone. Sustained pads, 808s, bass and chords are never mistaken for noise.
  - In testing, an 8-second held chord lost at most 0.4 dB at 100 %, while the hiss between its notes dropped 26 dB.
  - The trade-off: steady **hum** (50/60 Hz and its harmonics) counts as a tone, so it is kept.
    Use an EQ notch for hum.
- **Stereo link:** both channels share one gain, so the stereo image never moves.
- **Latency:** 1024 samples (about 23 ms) at 44.1/48 kHz, and 2048 at 88.2/96 kHz. The plugin
  reports this to the MPC so the track stays in time.
  - Use it on recorded audio, samples and mixdown.
  - Don't monitor a live mic through it while you track.

## Presets

Off, Light (vocals), Medium, Heavy (noisy sample), Max.

## Tested (desktop build)

- host_sim: PASS (20 passed, 0 warnings) at 44.1 kHz and 96 kHz.
- At 0 % the output matched the delayed input exactly (max error 1.6e-7, which is float rounding).
- **Synthetic vocal with hiss and hum** (vocal 26 dB above the noise). The noise drop is measured in the gaps between phrases:

  | Clean | Hiss removed | Vocal level change |
  |---|---|---|
  | 25 % | 7.3 dB | under 0.1 dB |
  | 50 % | 14.0 dB | under 0.1 dB |
  | 75 % | 20.5 dB | under 0.1 dB |
  | 100 % | 26.5 dB | under 0.1 dB |

- **Steady tones** (30 Hz to 1 kHz) on their own: left untouched by the tone guard.
- ASan/UBSan stress (3,000 random blocks and parameter changes) was clean, with no NaN or Inf.
- CPU was about 1 % of one desktop core.

**Limits:**
- It is built for steady noise. Clicks, crackle, wind gusts or a dog barking are not steady noise, and it won't remove them.
- On very noisy material at 100 %, expect some "underwater" colour. Back the fader off.

## Install

1. In the MPC Plugin SDK folder, run `python tools/build.py "plugins/Melt Clean"`.
2. Copy `plugins/Melt Clean/build/Meltdown Empire - VST - Melt Clean/` into `VST2 Plugins` on a USB drive or SD card.
3. On the MPC, turn on Preferences → Enhancements (Beta) → External Plugin Support.
4. Add Melt Clean as an insert effect and assign **Clean** to a Q-Link.

## License

MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
