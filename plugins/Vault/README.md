# Vault — by Meltdown Empire

A multisample "rompler" instrument for Akai MPC standalone (MPC OS 3.10.1 or later).
Vault plays banks of WAV samples across the keyboard with loop points, tuning, a resonant
filter with its own envelope, an amp envelope, an LFO and a bit-depth crusher.

**The engine is open source (MIT). Sounds are not included.** Vault plays whatever WAVs you
put in `Content/Banks/`, described by `Content/vault.map`. Only share sounds you have the
right to share.

## Controls

| Page | Controls |
|---|---|
| SOUND | **Bank**, **Sound** (shows the sound's name), Tune (±24 st), Fine (±50 ct), Volume, Velocity (sensitivity), Start (sample start offset), Bits (16 = clean, 12 = SP-style grit, 8 = lo-fi), Loops (On/Off), Bend Range |
| FILTER | Cutoff, Resonance, Mode (LP/BP/HP), Drive, Env Amount, Key Track, filter ADSR |
| AMP | Attack, Decay, Sustain, Release, Volume, Velocity |
| LFO | Shape, Sync, Rate or tempo Division, LFO>Pitch, LFO>Filter, Wheel Vib (mod wheel vibrato) |

Built-in programs: Init, Soft Pad, Pluck, 12-Bit Grit, Lo-Fi 8-Bit (these change the engine
settings; pick the Bank and Sound afterwards).

How sounds behave:
- **Instruments** (names ending Multi, Pad, Loop, Wave, Hit) are spread across the keyboard
  and transposed from their nearest sample.
- **Kits** (names ending Kit) put one sample per key starting at C1 (note 36), play at
  their original pitch, and play to the end even when you let go (one-shot), like an MPC
  drum program. Keys past the last sample are silent.
- Changing Bank or Sound loads the new sound in the background (usually a fraction of a
  second) and stops notes that were playing.

## Install on the MPC

1. Build (from the SDK folder): `python tools/build.py plugins/Vault`
2. Copy the folder `plugins/Vault/build/Meltdown Empire - VST - Vault/` into a folder named
   exactly `VST2 Plugins` at the top of a USB drive or SD card (or the MPC's internal drive).
3. On the MPC (MPC OS 3.10.1+): Preferences → Enhancements (Beta) → turn on
   **Enable Enhancements** and **External Plugin Support**. Restart if asked.
4. Make a Plugin track and pick **Vault** under External Plugins.

## Adding or changing sounds

- `python plugins/Vault/tools/setup_sounds.py <zip or folder>` copies bank WAVs into
  `Content/Banks/`.
- `python plugins/Vault/tools/vault_mapper.py` (needs `pip install numpy`) rebuilds
  `Content/vault.map`: it detects each sample's pitch, groups neighbouring samples into
  instruments and unpitched one-shots into kits, and reads loop points from the WAVs.
- The map is plain text. Rename a sound by editing its `I` line; fix a key range on its
  `Z` line (file, root note, low key, high key, loop start, loop end). Rebuild after edits.
- The Bank list is baked into the touch screen at build time: rebuild after adding banks.

## Notes and limits

- Auto-mapping is a best guess (the original preset key maps were not in the WAVs): a few
  sounds may be split in two or merged with a neighbour. Fix them in `vault.map`.
- One velocity layer per zone; mono samples summed to both outputs.
- 16 voices, cubic interpolation. The filter is skipped when it would not change the sound,
  to save CPU. Measured on desktop: well under 1% CPU per block; check on the MPC with
  several instances before relying on it in a big project.
- Memory: only the selected sound is loaded (typically under 1 MB, at most a few MB).
- `uid` (MCb9) and parameter ids must never change once projects use the plugin.

## License

Engine code: MIT, © Meltdown Empire. Built with the inMusic MPC Plugin SDK (MIT).
Sample content is not part of this project and keeps its own license.
