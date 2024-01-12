// MIDI code adapted from Jacob Vosmaer
// MIT License
//
// Copyright (c) 2020 andrewikenberry
// Copyright (c) 2023 Jacob Vosmaer
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdint.h>

#include "onewiremidi.pio.h"

enum {
  MIDI_NOTE_OFF = 0x80,
  MIDI_NOTE_ON = 0x90,
  MIDI_AFTERTOUCH = 0xa0,
  MIDI_CONTROL_CHANGE = 0xb0,
  MIDI_PROGRAM_CHANGE = 0xc0,
  MIDI_CHANNEL_PRESSURE = 0xd0,
  MIDI_PITCH_BEND = 0xe0,
  MIDI_SYSEX = 0xf0,
  MIDI_SYSEX_END = 0xf7,
  MIDI_TIMING_CLOCK = 0xf8,
  MIDI_ACTIVE_SENSE = 0xfe,
  MIDI_START = 0xfa,
  MIDI_CONTINUE = 0xfb,
  MIDI_STOP = 0xfc,
};

typedef void (*callback_int_int)(uint8_t, uint8_t);
typedef void (*callback_int)(uint8_t);
typedef void (*callback_void)();

typedef struct Onewiremidi {
  PIO pio;
  unsigned char sm;
  uint8_t status;
  uint8_t previous;
  uint32_t last_time;
  callback_int_int midi_note_on;
  callback_int midi_note_off;
  callback_void midi_start;
  callback_void midi_continue;
  callback_void midi_stop;
  callback_void midi_timing;
} Onewiremidi;

Onewiremidi *Onewiremidi_new(PIO pio, unsigned char sm, const uint pin,
                             callback_int_int midi_note_on,
                             callback_int midi_note_off,
                             callback_void midi_start,
                             callback_void midi_continue,
                             callback_void midi_stop,
                             callback_void midi_timing) {
  Onewiremidi *self = (Onewiremidi *)malloc(sizeof(Onewiremidi));
  self->pio = pio;
  self->sm = sm;
  self->status = 0;
  self->previous = 0;
  self->midi_note_on = midi_note_on;
  self->midi_note_off = midi_note_off;
  self->midi_start = midi_start;
  self->midi_continue = midi_continue;
  self->midi_stop = midi_stop;
  self->midi_timing = midi_timing;

  uint offset = pio_add_program(pio, &midi_rx_program);
  pio_sm_config c = midi_rx_program_get_default_config(offset);
  sm_config_set_in_pins(&c, pin);
  pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
  sm_config_set_set_pins(&c, pin, 1);
  sm_config_set_in_shift(&c, 0, 0, 0);  // Corrected the shift setup
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_clkdiv(pio, sm,
                    (float)clock_get_hz(clk_sys) / 1000000.0f);  // 1 us/cycle
  pio_sm_set_enabled(pio, sm, true);
  return self;
}

uint8_t Onewiremidi_reverse_uint8_t(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

typedef struct midi_message {
  uint8_t status;
  uint8_t data[2];
} midi_message;

void Onewiremidi_receive(Onewiremidi *self) {
  if (pio_sm_is_rx_fifo_empty(self->pio, self->sm)) {
    return;
  }
  uint32_t t = time_us_32();
  if (t - self->last_time > 1000) {
    self->status = 0;
    self->previous = 0;
  }
  self->last_time = t;
  uint8_t b = Onewiremidi_reverse_uint8_t(pio_sm_get(self->pio, self->sm));
  b = ~b;

  enum { DATA0_PRESENT = 0x80 };
  midi_message msg = {0};

  if (b >= 0xf8) {
    msg.status = b;
    if (msg.status == MIDI_TIMING_CLOCK && self->midi_timing != NULL) {
      self->midi_timing();
    } else if (msg.status == MIDI_START && self->midi_start != NULL) {
      self->midi_start();
    } else if (b == MIDI_CONTINUE && self->midi_continue != NULL) {
      self->midi_continue();
    } else if (b == MIDI_STOP && self->midi_stop != NULL) {
      self->midi_stop();
    }
  } else if (b >= 0xf4) {
    msg.status = b;
    self->status = 0;
  } else if (b >= 0x80) {
    self->status = b;
    self->previous = b == 0xf0 ? b : 0;
  } else if ((self->status >= 0xc0 && self->status < 0xe0) ||
             (self->status >= 0xf0 && self->status != 0xf2)) {
    msg.status = self->status;
    msg.data[0] = b;
    msg.data[1] = self->previous;
    self->previous = 0;
  } else if (self->status && self->previous & DATA0_PRESENT) {
    msg.status = self->status;
    msg.data[0] = self->previous ^ DATA0_PRESENT;
    msg.data[1] = b;
    self->previous = 0;
    if (msg.status == MIDI_NOTE_ON) {
      if (self->midi_note_on != NULL) {
        self->midi_note_on(msg.data[0], msg.data[1]);
      }
    } else if (msg.status == MIDI_NOTE_OFF) {
      if (self->midi_note_off != NULL) {
        self->midi_note_off(msg.data[0]);
      }
    }
  } else {
    self->previous = b | DATA0_PRESENT;
  }

  return;
}
