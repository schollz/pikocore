# pikocore sample studio

The sample studio is a local web app for managing your pikocore's audio library and building custom firmware — no command line required.

Run it from the repo root:

```
make server
```

This opens your browser at `http://localhost:8765`. To stop it, run `make kill-server`. To restart, run `make restart-server`.

## Why does loading samples require flashing firmware?

pikocore has no SD card or file system. Audio samples are compiled directly into flash memory as part of the firmware binary. This means swapping your sample library = building and flashing a new `.uf2`. The upside is zero latency, zero file management on the device, and no moving parts.

The `.uf2` you download contains both your audio and all firmware logic in a single file. Flashing it **completely replaces** everything currently on the device.

## Step-by-step

### 1. Add your audio files

Drag and drop audio files onto the drop zone, or click to browse. Files are listed with their size so you can check before building.

**Supported formats:** WAV · FLAC · MP3 · AIF · OGG · up to 254 files

#### Filename conventions

The converter reads metadata directly from filenames — no tagging required.

| Convention | Example | Effect |
|---|---|---|
| `_bpmXXX` | `amen_bpm170.wav` | Sets the loop's original BPM. Audio is time-stretched to match the build's target BPM. |
| `_beatsN` | `amen_beats16_bpm170.wav` | Sets the beat count. More beats = finer button slicing granularity. |
| No BPM in name | `myloop.wav` | pikocore tries to auto-detect. Works best for 100–200 BPM loops. |
| Numbered prefix | `01_kick.wav`, `02_snare.wav` | Controls sample slot order (files load alphabetically). |

### 2. Configure build options

| Option | Default | Notes |
|---|---|---|
| Pico flash size | 16 mb | Match your hardware. Most pikocore builds use 16 mb. |
| Sample rate | 31000 Hz | Lower if the build fails or the device won't load. Max 31 kHz. |
| RGB LED | enabled | Disable if your build doesn't have a WS2812 LED. |
| Sync input | clock | Switch to `midi` if you're using an [itty bitty midi](https://ittybittymidi.com) module. |
| PCB V2 layout | no | Set to `yes` if your board has Function A and B knobs swapped. |

### 3. Build

Click **Build firmware**. The live log streams each build step:

1. **Convert audio** — audio files are resampled, time-stretched, and converted to raw 8-bit mono
2. **Codegen** — filter and easing lookup tables are generated for your sample rate
3. **CMake** — the build system is configured
4. **Compile** — the firmware is compiled with your audio baked in
5. **Ready** — `.uf2` is available to download

A typical build takes 10–30 seconds depending on your machine and how many samples you added.

### 4. Flash to device

After a successful build, a download card appears with the `.uf2` and step-by-step flash instructions:

1. Hold **BOOTSEL** on the Pico and plug it in via USB while holding it
2. Release BOOTSEL — a drive called **RPI-RP2** appears on your desktop
3. Drag `pikocore.uf2` onto the **RPI-RP2** drive
4. The drive disappears and pikocore reboots automatically

> ⚠️ Flashing erases the existing firmware and samples on the device. There is no undo.

## Git badge

The header shows the current **branch** and **commit** of your local repo. This is what gets compiled — always check this before building, especially if you've been making changes or pulling updates.

## Related docs

- [MIDI output and CC mapping](midi.md)
