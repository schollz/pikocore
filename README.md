# pikocore


pikocore is a hackable, open-source, lo-fi music mangler based on the Raspberry Pi Pico.

read more here: https://pikocore.com


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
make audio
```

The audio is taken from the `audio2h/demo` folder. You can edit the `Makefile` to choose a different folder.

### build

To build just run

```
make
```

If you are using a 2mb pico, then you should do `make build2` (the default is 16mb).

Then upload the `build/pikocore.uf2` to your pico.

## dev

You can open a minicom terminal by running `make debug` after switching on `DEBUG_X` flags in `main.cpp`.

Easing functions generated with: https://editor.p5js.org/schollz/sketches/l5F_ZWjZM

