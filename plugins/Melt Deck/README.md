# Melt Deck — by Meltdown Empire

A two-deck DJ mixer for Akai MPC standalone (MPC OS 3.10.1+). Two decks, key-lock pitch, sync,
cues, hot cues, loops, 3-band kill EQ, filter, echo and a crossfader, played from the MPC's pads,
Q-links and screen. Original design and code.

## Getting tracks in

Put WAV, MP3 or FLAC files in `Content/Songs/`, either in this project before you build, or straight
into the plugin folder on the MPC drive (`VST2 Plugins/Meltdown Empire - VST - Melt Deck/Content/Songs/`).
The Track menus are fixed when you build: rebuild after adding tracks to see them by name.
Tracks up to 10 minutes. Included: `Meltdown Demo 92bpm Amin.mp3` (original).

Pick a track with **A Track** / **B Track**. It loads in the background (a second or two), then
the deck shows its **BPM**, **Key** and **time remaining**, and parks on the first beat.

## The pads

Bank A = deck A, Bank B = deck B. Same layout on both (pad 1 is bottom-left):

```
 13 Nudge -   | 14 Nudge +   | 15 Echo throw | 16 SHIFT
  9 Jump -4   | 10 Jump +4   | 11 Loop /2    | 12 Loop x2
  5 Hot cue 1 |  6 Hot cue 2 |  7 Hot cue 3  |  8 Hot cue 4
  1 PLAY      |  2 CUE       |  3 SYNC       |  4 LOOP
```

- **Play** starts/pauses (5 ms fades, no clicks).
- **Cue** (CDJ style): while playing, jumps back to the cue point and stops. While stopped
  somewhere else, sets the cue there (snapped to the beat). While stopped on the cue point,
  plays as long as you hold it.
- **Sync** matches this deck's tempo to the other deck (picks half/double time if that is
  closer, widens the pitch range if needed) and lines up the beats; it then stays locked.
- **Loop** sets an auto-loop of the **Loop** length (1/4 to 32 beats) from the current beat;
  press again to exit. **Loop /2** and **x2** change the length while it loops.
- **Hot cues**: an empty pad stores the current position (on the beat); a stored pad jumps
  there and plays. Hold **Shift** and tap a hot cue to delete it.
- **Jump -4 / +4** move 4 beats. **Nudge -/+** slow down/speed up by 4 % while held, to
  beat-match by ear. **Echo throw** sends the deck into the echo while held.

## Screen pages

| Page | Controls |
|---|---|
| **MIX** | A/B Volume, Crossfader, Master, A/B Filter, A/B Echo, both BPMs and times remaining, X Curve (Smooth or Cut for scratching) |
| **DECK A / DECK B** | Track, Play, Pitch fader, Key Lock, Low/Mid/High, Range (8/16/50 %), Status, Key, Loop length, Volume |

- **Key Lock** on: the pitch fader changes tempo only (key stays). Off: vinyl style (key moves
  with speed; the Key display follows).
- **EQ**: Linkwitz-Riley isolator at 250 Hz / 2.5 kHz. Turn fully left to **kill** a band; right
  boosts up to +6 dB.
- **Filter**: left = low-pass, right = high-pass, centre = off.
- **Echo**: beat-synced (3/4 beat) with feedback.
- Tracks are **auto-gained** to a similar loudness when loaded.

## Measured (desktop, 44.1 kHz)

- Key lock: a 440 Hz tone stays at 440.00 Hz with the pitch at ±8 % (0.0 cents); the house test
  track goes 124 → 134 BPM and stays in E major. Key lock off: exactly +8 % pitch, like vinyl.
- Sync: same track on both decks, one started late at +5 %: after Sync the decks sit 2 ms apart
  with **no drift** over 25 s.
- Hot cues land exactly on the beat and start at full level (no fade-in, no click); a 1-beat
  loop repeats exactly every beat (484.26 ms at 123.9 BPM).
- EQ kills: −31 dB low, −28 dB mid, −36 dB high; flat within measurement noise at 0.
- CPU: both decks playing with key lock: about 2 % of a desktop core (expect more on the MPC).
- Checked with memory and thread checkers while loading tracks, mashing pads and moving every
  control, including broken and fake files.

## Limits

- One stereo output: an MPC plugin has no separate headphone/cue output, so there is no pre-listen.
  Use the crossfader, EQ and filter to bring tracks in.
- Tempo detection can land on half or double time (e.g. 70 vs 140); Sync allows for this.
- Pad presses are handled at the start of each audio block (a few ms), which is fine for DJ
  cueing but not sample-accurate like the drum pads of a program.

## Install

1. In the MPC Plugin SDK folder: `python tools/build.py "plugins/Melt Deck"`
2. Copy `plugins/Melt Deck/build/Meltdown Empire - VST - Melt Deck/` into `VST2 Plugins` on a USB
   drive or SD card.
3. On the MPC: Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin Support.
   Make a Plugin track and choose Melt Deck. Pads in Bank A play deck A, Bank B deck B.

## Music and copyright

Melt Deck is a tool; the tracks are not part of it. Playing commercial music in public, streaming
a DJ set or releasing a mix can need licences (venue PRO licences, platform rules, clearances).

## License

Code and demo track: MIT, © Meltdown Empire. dr_libs decoders (David Reid): public domain / MIT-0.
Built with the inMusic MPC Plugin SDK (MIT).
