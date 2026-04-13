# MIDI on pikocore

pikocore connects over USB and exposes itself as a class-compliant MIDI device — no drivers needed on Mac, Windows, or Linux.

## MIDI Out

### Notes

pikocore sends **Note On / Note Off** messages on channel 1 each time a beat plays. Each of the 8 buttons maps to a note:

- A note fires when a beat is triggered during normal playback.
- During retriggering (fx mode), notes fire with a velocity that scales with retrig position (`0–120`).

The default note set is:

| Button | Default note |
|--------|-------------|
| 1 | 36 (C2) |
| 2 | 38 (D2) |
| 3 | 40 (E2) |
| 4 | 41 (F2) |
| 5 | 43 (G2) |
| 6 | 45 (A2) |
| 7 | 47 (B2) |
| 8 | 48 (C3) |

#### Remapping notes

You can remap the note for any button by holding that button down and turning **Knob 0** (the selector knob). The note is selected from a chromatic scale spanning C2–B5.

### Control Changes (CC)

Every time you turn **Knob A** or **Knob B**, pikocore sends a MIDI CC message reflecting the current knob value (scaled 0–127). The CC number depends on which **selector position** (0–7) Knob 0 is set to:

| Selector | Parameter A | Knob A CC | Parameter B | Knob B CC |
|----------|-------------|-----------|-------------|-----------|
| 0 | Sample | CC 14 | Break | CC 22 |
| 1 | Filter | CC 15 | Stretch | CC 23 |
| 2 | Gate threshold | CC 16 | Gate probability | CC 24 |
| 3 | Jump probability | CC 17 | Retrig probability | CC 25 |
| 4 | Tunnel probability | CC 18 | Reverse probability | CC 26 |
| 5 | Sequencer record | CC 19 | Sequencer play | CC 27 |
| 6 | Save | CC 20 | Load | CC 28 |
| 7 | Volume | CC 21 | Tempo | CC 29 |

CCs 14–29 are in the MIDI "undefined" range and will not conflict with standard MIDI CCs.

## MIDI In

pikocore also supports MIDI clock and note input via a hardware one-wire MIDI interface (requires [itty bitty midi](https://ittybittymidi.com)). Enable it by setting `MIDI_IN_ENABLED=1` in `target_compile_definitions.cmake`. See the [customization section](../README.md#customization) for details.
