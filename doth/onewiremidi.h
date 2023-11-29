
#include "onewiremidi.pio.h"

#define MIDI_NOTE_ON_MIN 0x90
#define MIDI_NOTE_ON_MAX 0x9F
#define MIDI_NOTE_OFF_MIN 0x80
#define MIDI_NOTE_OFF_MAX 0x8F
#define MIDI_TIMING_CLOCK 0xF8
#define MIDI_ACTIVE_SENSE 0xFE
#define MIDI_START 0xFA
#define MIDI_CONTINUE 0xFB
#define MIDI_STOP 0xFC

typedef void (*callback_int_int)(uint8_t, uint8_t);
typedef void (*callback_int)(uint8_t);
typedef void (*callback_void)();

typedef struct Onewiremidi {
  PIO pio;
  unsigned char sm;
  uint8_t rbs[3];
  uint8_t rbi;
  bool ready;
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
  Onewiremidi *om = (Onewiremidi *)malloc(sizeof(Onewiremidi));
  om->pio = pio;
  om->sm = sm;
  om->rbi = 0;
  for (uint8_t i = 0; i < 3; i++) {
    om->rbs[i] = 0;
  }
  om->midi_note_on = midi_note_on;
  om->midi_note_off = midi_note_off;
  om->midi_start = midi_start;
  om->midi_continue = midi_continue;
  om->midi_stop = midi_stop;
  om->midi_timing = midi_timing;

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
  return om;
}

uint8_t Onewiremidi_reverse_uint8_t(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

void Onewiremidi_receive(Onewiremidi *om) {
  if (pio_sm_is_rx_fifo_empty(om->pio, om->sm)) {
    return;
  }
  uint32_t t = time_us_32();
  if (t - om->last_time > 1000) {
    om->rbi = 0;
  }
  om->last_time = t;
  uint8_t value = Onewiremidi_reverse_uint8_t(pio_sm_get(om->pio, om->sm));
  om->rbs[om->rbi] = ~value;
  // printf("%d: %x\n", om->rbi, om->rbs[om->rbi]);

  if (om->rbs[om->rbi] == MIDI_START) {
    if (om->midi_start != NULL) om->midi_start();
    om->rbi = 0;
  } else if (om->rbs[om->rbi] == MIDI_CONTINUE) {
    if (om->midi_continue != NULL) om->midi_continue();
    om->rbi = 0;
  } else if (om->rbs[om->rbi] == MIDI_STOP) {
    if (om->midi_stop != NULL) om->midi_stop();
    om->rbi = 0;
  } else if (om->rbs[om->rbi] == MIDI_ACTIVE_SENSE) {
    om->rbi = 0;
  } else if (om->rbs[om->rbi] == MIDI_TIMING_CLOCK) {
    if (om->midi_timing != NULL) om->midi_timing();
    om->rbi = 0;
  } else if (om->rbi < 2 && (om->rbs[0] >= MIDI_NOTE_OFF_MIN ||
                             om->rbs[0] <= MIDI_NOTE_ON_MAX)) {
    // iterater for not on/off
    om->rbi++;
  } else if (om->rbi == 2) {
    om->rbi = 0;
    // printf("%02X %02X %02X\n", om->rbs[0], om->rbs[1], om->rbs[2]);
    if (om->rbs[0] >= MIDI_NOTE_ON_MIN && om->rbs[0] <= MIDI_NOTE_ON_MAX) {
      if (om->midi_note_on != NULL) {
        om->midi_note_on(om->rbs[1], om->rbs[2]);
      }
    } else if (om->rbs[0] >= MIDI_NOTE_OFF_MIN &&
               om->rbs[0] <= MIDI_NOTE_OFF_MAX) {
      if (om->midi_note_off != NULL) {
        om->midi_note_off(om->rbs[1]);
      }
    } else {
#ifdef DEBUG_MIDI
      printf("%02X %02X %02X\n", om->rbs[0], om->rbs[1], om->rbs[2]);
#endif
    }
  } else {
#ifdef DEBUG_MIDI
    printf("??? %d: %x\n", om->rbi, om->rbs[om->rbi]);
#endif
    om->rbi = 0;
  }
  return;
}
