# Alpaca source (build NDLR11)

These are the files Alpaca changes or adds on top of Polyend's Tracker Community Edition 1.0.0.
Everything else comes from Tracker CE unchanged.

## Build

1. Get Tracker CE from https://github.com/polyend/tracker-ce and check out the tag `v1.0.0`.
2. Copy the `core` folder from here over the `core` folder of that checkout.
3. From the folder with the top-level `Makefile`:

   ```bash
   make mini TOOLCHAIN=arm-none-eabi- BUILD_TAG=ndlr11 MINI_POSTFIX=CE_1.0.0_NDLR11
   ```

4. Copy the `.ptf` from `build/` to `/Firmware` on the Mini's SD card and run Config > Firmware update.

Tracker CE's own README lists the ARM toolchain it needs. Later Tracker CE versions changed the
display code, so these files are for `v1.0.0`.

## Where things are

| Path | What it is |
| --- | --- |
| `core/interface/ndlrPanel/` | The panel: clock, chords, Pad, Drone, motifs, editors, Chord Sequencer |
| `core/libs/NewDisplay/ndlrPanelControl.*` | Draws the panel, its sub screens and the editors |
| `core/source/NdlrRecorder.*` | The REC button: records the stereo mix to `/Recordings` |
| `core/source/NdlrCapture.*` | Capture: writes the parts into the pattern, with the Chord FX for chords |
| `core/source/mtSequencer.*`, `mtMidi.*` | The Tracker clock for the panel, and notes to the four synths |
| `core/synth/Synth.*` | Voice sharing between the four synths |
| `core/interface/samplePlayback/`, `core/libs/Audio/play_memory*` | Loop setting for sample playback |
| `core/interface/sampleRecorder/` | Master Mix as a Sample Recorder source |
| the other files | Menu entries, the start screen and the settings that connect the above |

## What the panel does

- Eight dials, seven chord buttons, ALL, 1/2, Menu, Shift and four part buttons, moved between with
  the dpad. Pad on Synth 1, Drone on Synth 2, Motif 1 on Synth 3, Motif 2 on Synth 4.
- A sub screen at the top of the right-hand column that follows the dial in use: Pad, Drone,
  Tempo-Key-Mode with the notes of the key on a keyboard, and Motif.
- Forty patterns and forty rhythms. Slots 1-20 are presets and 21-40 are the user's. A pattern is
  sixteen values counted along the chord, the scale or the chromatic scale. A rhythm is thirty-two
  beats, each a velocity, a rest or a tie.
- Editor mode (Shift + Menu): the Pattern Editor on dial 5, the Rhythm Editor on dial 6 and the
  Chord Sequencer on dial 1. Shift + press dial 1 loads; Shift + press dial 5 saves.
- Quantised launches (next beat or next bar), the Tracker's clock or its own, accent patterns, and
  tap tempo on the panel's SHIFT.

## Menu mode: MIDI out and capture

Press MENU to enter Menu mode. The dials then do this:

| Dial | Label | Turn | Press |
| --- | --- | --- | --- |
| 1-4 | PAD OUT, DRN OUT, M1 OUT, M2 OUT | The part's own synth (Synth 1-4) or MIDI channel 1-16 | - |
| 5 | CAPTURE | Parts to capture: all, one, both motifs, or Pad + Drone | Arm or stop the capture |
| 6 | CAP TRK | The first track written (default 9) | Arm or stop |
| 7 | PAD TRKS | Tracks for the Pad, 1-8 (default 2) | Arm or stop |
| 8 | STRUM | Keep a strummed Pad's timing, or ignore the strum | Arm or stop |

- **MIDI out.** A part set to a MIDI channel goes out through the Tracker's MIDI out, so set
  Config > MIDI > Notes out to USB, Jack or both. The Tracker's middle C setting applies. A part on
  MIDI does not use or limit the internal synth's voices. Changing a part's output ends its notes
  first.
- **Capture.** Arm it with the Tracker playing the pattern, or arm it and press Play. It starts at
  the next start of the pattern at which a captured part is playing, and writes one pass of the
  pattern. Then it stops the captured parts so the pattern plays them, and the step can be undone.
- Tracks are given out in order from CAP TRK: the Pad gets PAD TRKS tracks, the Drone two, each
  motif one. The tracks are cleared first. Each note uses the part's instrument: its synth, or the
  MIDI instrument of its channel.
- A chord that is one of the Tracker's Chord FX shapes (root position, up to four notes within
  fourteen semitones) goes on one track as its lowest note with the Chord FX. Other voicings are
  split: track by track, the lowest note left with the biggest shape built on it. Notes that do not
  fit on the part's tracks are left out, and the toast says how many.
- A strum kept goes one note per Pad track with micromove. A strum ignored is written as one chord.
- Notes between steps are written with micromove (twelve NDLR pulses to a step). A note-off goes
  on the step the note ends in, at least one step after it starts.
- Capture stops early if playback stops or restarts, or the Tracker moves to another pattern.

## Limits

- User patterns, rhythms and sequences are kept in RAM and are lost at power-off.
- The Tonic modes, the Mod Matrix, LFO and Settings menus, and the MIDI Controller and Perlin editors
  are not in this build. Menu mode holds the MIDI out and capture settings instead.
- The MIDI out and capture settings are kept until power-off.
- Capture writes one pattern, the one playing, up to 128 steps.

## For anyone changing the panel

- The whole panel must fit in 8172 bytes of display list per frame (see `cDisplay::step3`). It uses
  about 6300 at most. Text is the expensive part: one command per glyph.
- While the Motif sub screen or an editor is showing, the panel redraws on every motif step to move
  the playhead. `kShowPlayhead` near the top of `fNdlrPanel.cpp` turns that off.

- `tests/ndlrCapture/run.sh` tests the capture writer on a computer, with stubs for the Tracker:
  `TRACKER_CE=/path/to/tracker-ce firmware/source/tests/ndlrCapture/run.sh`.

## Licence

The same as Tracker CE: see `LICENSE` in this folder. Free to use and share, not to sell.
Fan-made and unofficial. Not made, sold or endorsed by Conductive Labs or Polyend.
