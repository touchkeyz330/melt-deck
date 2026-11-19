# Melt Deck

Two-deck DJ software by **Meltdown Empire** for Windows and Mac.

## Download

Open this repo's **Releases** page (right side of the main repo page) and grab the newest build:

| Your computer | File to download |
|---|---|
| Windows | `Melt-Deck-…-win-x64.exe` |
| Mac with Apple chip (M1/M2/M3/M4) | `Melt-Deck-…-mac-arm64.dmg` |
| Older Intel Mac | `Melt-Deck-…-mac-x64.dmg` |

**First time opening it.** The app isn't signed with a paid Apple or Microsoft developer certificate yet, so your computer will warn you once:

- **Windows:** "Windows protected your PC" → click **More info** → **Run anyway**.
- **Mac:** open the `.dmg`, drag Melt Deck into Applications, then right-click the app → **Open** → **Open**. If you don't see an Open button, go to **System Settings → Privacy & Security** and click **Open Anyway**.

## Features

- Two decks with scrolling colour waveforms (bass shows in the deck colour) and a full-track overview
- Automatic BPM and beat-grid detection, with TAP, ÷2 and ×2 to fix it by hand
- **SYNC** matches tempo *and* lines up the beats (handles half-time, like 70 vs 140)
- Tempo slider with ±8 / 16 / 50% range, nudge buttons and **KEY** lock (change speed without changing pitch)
- CUE (hold to preview), 4 hot cues per deck, and 1–16 beat loops that snap to the beat
- Mixer: trim, 3-band EQ (turn fully left to kill), filter (left = low-pass, right = high-pass), channel faders, level meters
- Crossfader with smooth or sharp (scratch) curve
- Master volume with a built-in limiter so the output never clips
- **REC** records your mix to a WAV file
- Library: drag songs in, search, see BPM and length, load onto either deck
- **MIDI control with MIDI Learn**: link any controller, keyboard or MPC to almost every button, knob and fader

## MIDI controllers

1. Plug in your controller. A keyboard that only has a 5-pin MIDI port needs a USB-MIDI interface cable.
2. Click **MIDI** at the top. Your device shows up in the list, and the light flashes green when MIDI comes in.
3. Click **START MIDI LEARN**. Every control you can link gets a gold outline.
4. Click a control in Melt Deck (for example Deck A **PLAY**), then press or move the control on your hardware. It turns green, which means it's linked.
5. Repeat for everything you want, then click **DONE**.

Knobs, faders and pitch wheels work for knobs and faders. Pads, keys and buttons work for buttons. While learning, right-click a control to unlink it. Your links are saved and come back every time you open Melt Deck. The **▲ ▼** buttons by the search box (also linkable) step through the library, and each deck's **LOAD** button, when linked over MIDI, loads the selected track.

### Keyboard

| | Deck A | Deck B |
|---|---|---|
| Play / pause | Q | P |
| Cue | W | O |
| Hot cues 1–4 | 1 2 3 4 | 7 8 9 0 |
| Sync | S | L |

Shift + a hot cue clears it. The left and right arrow keys move the crossfader, and the up and down arrow keys step through the library.

## How the installers get made

Every time code is pushed to `main`, GitHub Actions (`.github/workflows/build.yml`) builds the Windows and Mac installers on GitHub's own machines and posts them as a new Release. You can watch the progress on the **Actions** tab. A build takes about 5–10 minutes.

## Running from source (optional)

You need [Node.js](https://nodejs.org) installed.

```bash
npm install
npm start
```
