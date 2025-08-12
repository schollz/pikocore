# pikocore

[![build](https://github.com/schollz/pikocore/actions/workflows/build.yml/badge.svg)](https://github.com/schollz/pikocore/actions/workflows/build.yml)

pikocore is a hackable, open-source, lo-fi music mangler based on the Raspberry Pi Pico.

![img](https://user-images.githubusercontent.com/6550035/276962341-4c0065e4-f0cf-4315-9de2-e26aa2ebe1e7.jpg)

read more here: https://pikocore.com


## diy

- [Website](https://pikocore.com)
- [Schematic](https://infinitedigits.co/img/pikocore_schematic.png)
- [Bom](https://infinitedigits.co/wares/pikocore/#bom)
- [Source code](https://github.com/schollz/pikocore)
- [Firmware](https://infinitedigits.co/wares/pikocore/#firmware)
- [Instructions for uploading firmware](https://infinitedigits.co/wares/pikocore/#update) 
- [Video demonstration](https://www.youtube.com/watch?v=mKPq1Chm9Tg)
- [Video DIY guide](https://www.youtube.com/watch?v=VG0q74ASlLQ)


## usage

### prerequisites

First install Go and then install pre-reqs:

```
make prereqs
```

This will install `clang-format`, `cmake`, the pico toolchain, `gcc`, `python`, and other useful packages.

### create audio

You can use the default audio by just running

```
SAMPLE_RATE=31000 make audio
```

The audio is taken from the `audio2h/demo` folder. You can edit the `Makefile` to choose a different folder. The max sample rate is 31khz, but if that doesn't work, try reducing it.

### build

To build just run

```
make
```

If you are using a 2mb pico, then you should do `make build2` (the default is 16mb).

Then upload the `build/pikocore.uf2` to your pico.

### customization

If you want to turn off the LED, change `WS2812_ENABLED=1` to `WS2812_ENABLED=0` in the `target_compile_definitions.cmake` file.

If you want to use MIDI instead of clock in (requires [itty bitty midi](https://ittybittymidi.com)) then set `MIDI_IN_ENABLED=1` in the `target_compile_definitions.cmake` file.

## dev

You can open a minicom terminal by running `make debug` after switching on `DEBUG_X` flags in `main.cpp`.

Easing functions generated with: https://editor.p5js.org/schollz/sketches/l5F_ZWjZM

