// c++ include
#include <stdio.h>
#include <stdlib.h>

#include <cmath>

// pico files
#include "hardware/adc.h"  // adc_read
#include "hardware/clocks.h"
#include "hardware/flash.h"  // flash memory
#include "hardware/irq.h"    // interrupts
#include "hardware/pwm.h"    // pwm
#include "hardware/sync.h"   // wait for interrupt
#include "pico/binary_info.h"
#include "pico/stdlib.h"  // stdlib
#include "pico/multicore.h"
#include "tusb.h"

#include "PikoAudioBank.h"
#include "ClockSync.h"
#include "PikoRuntime.h"
#include "PikoSampleManager.h"
#include "SpscQueue.h"
// pikocore files
#include "doth/button.h"
#include "doth/delay.h"
#include "doth/easing.h"
#include "doth/filter.h"
#include "doth/knob.h"
#include "doth/led.h"
#include "doth/ledarray.h"
#include "doth/onewiremidi.h"
#include "doth/sequencer.h"
#include "doth/trigger_out.h"

// constants
#define CLOCK_RATE 248000
#define SAMPLE_RATE 24000
#define BPM_SAMPLED 165
#define SAMPLES_PER_BEAT 4364
#define NUM_RETRIGS 19
#define raw_val piko_raw_val
#define raw_len piko_raw_len
#define raw_beats piko_raw_beats
#define NUM_BUTTONS 8
#define NUM_KNOBS 3
#define NUM_LEDS 8
#define DISTORTION_MAX 30
#define VOLUME_REDUCE_MAX 30
#define HEAD_SHIFT 10  // crossfade time in samples (2^HEAD_SHIFT)
#define AUDIO_PIN 20   // audio out
#ifdef PICO_DEFAULT_LED_PIN
#define LED_PIN PICO_DEFAULT_LED_PIN
#endif
#define CLOCK_PIN 22  // clock in pin
#define TRIGO_PIN 21  // trigger out pin
#define MAIN_LOOP_HZ 4
#define MAIN_LOOP_DELAY 50

#if WS2812_ENABLED == 1
#include "doth/WS2812.hpp"
#endif
/*
 * GLOBAL VARIABLES
 */

// flash
// https://github.com/raspberrypi/pico-examples/blob/master/flash/program/flash_program.c
// https://kevinboone.me/picoflash.html
#define SAVE_VOLUME 0  // needs two bytes
#define SAVE_BPM 2     // needs two bytes
#define SAVE_FILTER 4  // needs one byte
#define SAVE_SAMPLE 5  // needs one byte
#define SAVE_GATE 6    // needs two bytes
#define SAVE_PROB_DIRECTION 8
#define SAVE_PROB_RETRIG 9
#define SAVE_PROB_JUMP 10
#define SAVE_PROB_GATE 11
#define SAVE_PROB_TUNNEL 12
#define SAVE_CLOCK_INPUT_MODE 13
#define SAVE_PULSE_PPQN 14
#define CLOCK_INPUT_CLOCK 0
#define CLOCK_INPUT_MIDI 1
#define MIDI_NOTES_AVAILABLE_TOTAL 28
static constexpr uint32_t kKnobMax = 4095u;
static constexpr uint32_t kStretchQ8One = 256u;
static constexpr uint32_t kStretchQ8Bypass =
    (11u * kStretchQ8One + 5u) / 10u;
static constexpr uint32_t kStretchQ8Max = 10u * kStretchQ8One;
static constexpr uint32_t kGrainLengthSamples = 2048u;
static constexpr uint32_t kGrainHopSamples = 1024u;
static constexpr uint32_t kGrainHopShift = 10u;
static constexpr uint64_t kTimestretchPhaseIncQ32 = 1ull << 32u;
static constexpr uint16_t kPwmWrap = 2047u;
static constexpr uint16_t kPwmLevelScale = (kPwmWrap + 1u) / 256u;
uint8_t midi_notes_available[MIDI_NOTES_AVAILABLE_TOTAL] = {
    36, 38, 40, 41, 43, 45, 47, 48, 50, 52, 53, 55, 57, 59,
    60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83};
uint8_t midi_notes_set[8] = {36, 38, 40, 41, 43, 45, 47, 48};

const uint8_t *flash_target_contents =
    (const uint8_t *)(XIP_BASE + PIKO_SETTINGS_FLASH_OFFSET);

// inputs
Button input_button[NUM_BUTTONS];
Knob input_knob[NUM_KNOBS];

// outputs
LEDArray ledarray;

TriggerOut output_trigger;

// audio tracking
uint8_t audio_now = 0;
uint64_t playback_phase_q32 = 0;
uint64_t playback_increment_q32 = 1;
uint64_t playback_effective_increment_q32 = 1;
uint32_t pwm_carrier_hz = 1;
uint32_t playback_target_bpm_x100 = 16500;
bool do_mute = false;
uint8_t do_mute_debounce = 0;

// sample tracking
uint16_t sample = 0;
uint16_t sample_beats = 8;
uint16_t sample_source_bpm = BPM_SAMPLED;
uint32_t sample_frames_per_slice = SAMPLES_PER_BEAT;
uint16_t sample_change = 0;
uint16_t sample_add = 0;
uint16_t sample_set = 0;
uint32_t phase_sample[] = {0, 0};
uint32_t phase_retrig = 0;
bool phase_head = 0;
uint32_t phase_xfade = 0;

// beat tracking
uint16_t select_beat = 0;
uint16_t select_beat_freeze = 0;
bool direction[] = {1, 1};  // 0 = reverse, 1 = forward
bool base_direction = 1;    // 0 = reverse, 1 == forward
uint8_t volume_mod = 0;

struct TimestretchGrain {
  uint64_t start_phase_q32;
  uint64_t phase_inc_q32;
  uint16_t age;
};

// volume/distortion/filter/bitcrush/stretch
uint8_t distortion = 0;
uint8_t volume_reduce = 0;
uint8_t filter_fc = LPF_MAX + 10;
uint8_t hpf_fc = 0;
uint8_t filter_q = 0;
uint8_t bitcrush = 0;
uint32_t stretch_q8 = kStretchQ8One;
uint32_t timestretch_applied_q8 = kStretchQ8One;
uint64_t timestretch_phase_q32 = 0;
uint64_t timestretch_source_inc_q32 = kTimestretchPhaseIncQ32;
TimestretchGrain timestretch_grains[2] = {
    {0, kTimestretchPhaseIncQ32, 0},
    {0, kTimestretchPhaseIncQ32, kGrainHopSamples}};
uint8_t timestretch_audio_now = 128;
bool timestretch_active = false;
bool timestretch_grains_initialized = false;
bool do_lock_clock = false;

// beat tracking (beat = eighth-note)
uint16_t bpm_set = 79;
uint16_t internal_bpm_set = 165;
uint32_t beat_num_total = 0;
bool beat_onset = false;
bool beat_led = 0;
bool btn_reset = 0;
bool soft_sync = 0;
bool resume_transport_phase = false;

// probabilities
uint8_t probability_jump = 0;
uint8_t probability_direction = 0;
uint8_t probability_retrig = 0;
uint8_t probability_gate = 0;
uint8_t probability_tunnel = 0;  // jumps between samples

// retriggering / fx
bool fx_retrig = false;
bool btn_retrig = 0;
uint8_t retrig_sel = 4;
uint8_t retrig_count = 0;
uint8_t retrig_max = 2;
uint8_t retrig_filter = 0;
uint8_t retrig_filter_change = 0;
int8_t retrig_pitch_change = 0;
uint8_t retrig_volume_reduce = 0;
uint8_t button_on = NUM_BUTTONS;
uint8_t button_on2 = 3;
uint8_t button_filter = 0;
bool button_filter_on = false;
uint8_t retrig_volume_reduce_change = 0;
bool retrig_pitch_up = false;
bool retrig_pitch_down = false;

// bpm configuring
bool flag_half_time = 0;  // specifies quarter note or not

// noise gate
uint16_t noise_gate_val;
uint16_t noise_gate_thresh;
uint16_t noise_gate_thresh_use;
uint8_t noise_gate_fade = 0;

// buttons
bool button_trigger[8] = {false, false, false, false,
                          false, false, false, false};

// sequencer
Sequencer sequencer;

// midi handler
Onewiremidi *onewiremidi;
int8_t midi_button1 = -1;
int8_t midi_button2 = -1;
volatile bool clock_input_ittybittymidi = false;
volatile uint8_t pulse_ppqn = 2;

struct MidiByteEvent {
  uint8_t byte;
  uint32_t timestamp_us;
};

piko::ClockSync clock_sync;
SpscQueue<piko::ClockEvent, 32> clock_event_queue;
SpscQueue<MidiByteEvent, 64> midi_byte_queue;
uint32_t core1_stack[2048] __attribute__((aligned(8)));

inline void set_audio_pwm_level(uint8_t level) {
  pwm_set_gpio_level(AUDIO_PIN,
                      static_cast<uint16_t>(level) * kPwmLevelScale);
}

/*
 * HELPER FUNCTIONS
 * knobs / clock in can set these inputs
 *
 */

void param_set_break(uint16_t knob_val, uint8_t &filter_fc_,
                     uint8_t &distortion_, uint8_t &probability_jump_,
                     uint8_t &probability_retrig_, uint8_t &probability_gate_,
                     uint8_t &probability_direction_,
                     uint8_t &probability_tunnel_,
                     uint8_t save_data_[FLASH_PAGE_SIZE]) {
  if (knob_val < 50) {
    // turn it all off
    distortion_ = 0;
    probability_jump_ = 0;
    probability_retrig_ = 0;
    probability_gate_ = 0;
    probability_direction_ = 0;
    probability_tunnel_ = 0;
  } else {
    distortion_ = ease_distortion(knob_val) * DISTORTION_MAX / 255;
    probability_jump_ = ease_probability_jump(knob_val);
    probability_retrig_ = ease_probability_retrig(knob_val);
    probability_gate_ = ease_probability_gate(knob_val);
    probability_direction_ = ease_probability_direction(knob_val);
    probability_tunnel_ = ease_probability_tunnel(knob_val);
  }
  save_data_[SAVE_PROB_JUMP] = probability_jump_;
  save_data_[SAVE_PROB_DIRECTION] = probability_direction_;
  save_data_[SAVE_PROB_RETRIG] = probability_jump_;
  save_data_[SAVE_PROB_GATE] = probability_direction_;
  save_data_[SAVE_PROB_TUNNEL] = probability_tunnel_;
  save_data_[SAVE_VOLUME] =
      (uint8_t)((distortion_ * 1095 / DISTORTION_MAX + 3000) >> 8);
  save_data_[SAVE_VOLUME + 1] =
      (uint8_t)(distortion_ * 1095 / DISTORTION_MAX + 3000);
}

void update_playback_rate() {
  playback_target_bpm_x100 = clock_sync.targetBpmX100();
  playback_increment_q32 = piko::ClockSync::playbackIncrementQ32(
      pwm_carrier_hz, playback_target_bpm_x100, sample_source_bpm);
  if (playback_increment_q32 == 0) playback_increment_q32 = 1;

  // Retrigger pitch historically adjusted the number of carrier periods per
  // source frame. Preserve that behavior while keeping the base rate
  // fractional instead of rounding it to a whole PWM period.
  const uint64_t period_q16 = (1ull << 48u) / playback_increment_q32;
  int64_t adjusted_period_q16 =
      static_cast<int64_t>(period_q16) +
      (static_cast<int64_t>(retrig_pitch_change) << 16u);
  if (adjusted_period_q16 < (1 << 16u)) adjusted_period_q16 = 1 << 16u;
  playback_effective_increment_q32 =
      (1ull << 48u) / static_cast<uint64_t>(adjusted_period_q16);
  if (playback_effective_increment_q32 == 0) {
    playback_effective_increment_q32 = 1;
  }
}

void param_set_bpm(uint16_t bpm) {
  if (bpm < 30 || bpm > 360) return;
  const uint32_t interrupts = save_and_disable_interrupts();
  internal_bpm_set = bpm;
  clock_sync.setInternalBpmX100(static_cast<uint32_t>(bpm) * 100u);
  bpm_set = static_cast<uint16_t>((clock_sync.targetBpmX100() + 50u) / 100u);
  update_playback_rate();
  restore_interrupts(interrupts);
#ifdef DEBUG_BPM
  printf("new bpm: %d\n", bpm_set);
#endif
}

void param_set_volume(uint16_t knobval, uint8_t &distortion_,
                      uint8_t &volume_reduce_) {
  if (knobval < 2000) {
    distortion_ = 0;
    volume_reduce_ = (2000 - knobval) * (VOLUME_REDUCE_MAX + 3) / 2000;
  } else if (knobval > 3000) {
    volume_reduce_ = 0;
    distortion = (knobval - 3000) * DISTORTION_MAX / (4095 - 3000);
  } else {
    volume_reduce_ = 0;
    distortion = 0;
  }
}

// randint returns value
int randint(int min, int max) {
  int MaxValue = max - min;
  int random_value =
      (int)((1.0 + MaxValue) * rand() /
            (RAND_MAX + 1.0));  // Scale rand()'s return value against RAND_MAX
                                // using doubles instead of a pure modulus to
                                // have a more distributed result.
  return (random_value + min);
}

uint32_t retrig_len(uint8_t index) {
  static const uint16_t retrig_q8[NUM_RETRIGS] = {
      1024, 939, 768, 683, 640, 512, 384, 341, 256, 192,
      171,  128, 96,  85,  64,  48,  32,  24,  16};
  if (index >= NUM_RETRIGS) {
    index = NUM_RETRIGS - 1;
  }
  uint32_t value = (sample_frames_per_slice * retrig_q8[index] + 128u) >> 8u;
  return value > 0 ? value : 1u;
}

uint16_t clamp_gate_thresh(uint32_t value) {
  return value > 0xffffu ? 0xffffu : (uint16_t)value;
}

uint16_t gate_default_thresh() {
  return clamp_gate_thresh(sample_frames_per_slice * 4u);
}

uint16_t gate_scaled_thresh(uint16_t knob_value, uint16_t knob_max) {
  if (knob_max == 0) {
    return 1;
  }
  uint32_t scaled_q1000 = (uint32_t)knob_value * 1000u / knob_max;
  return clamp_gate_thresh(sample_frames_per_slice * scaled_q1000 / 1000u);
}

void refresh_sample_timing(uint16_t sample_index) {
  sample_beats = raw_beats(sample_index);
  if (sample_beats == 0) {
    sample_beats = 1;
  }
  sample_frames_per_slice = raw_len(sample_index) / sample_beats;
  if (sample_frames_per_slice == 0) {
    sample_frames_per_slice = 1;
  }
  sample_source_bpm = piko_audio_sample(sample_index).source_bpm;
  if (sample_source_bpm == 0) {
    sample_source_bpm = BPM_SAMPLED;
  }
  update_playback_rate();
}

uint32_t stretch_from_knob_q8(uint16_t knob) {
  const uint64_t eased_knob = static_cast<uint64_t>(knob) * knob * knob;
  const uint64_t eased_max =
      static_cast<uint64_t>(kKnobMax) * kKnobMax * kKnobMax;
  const uint64_t range = kStretchQ8Max - kStretchQ8One;
  return static_cast<uint32_t>(
      kStretchQ8One + ((eased_knob * range) + (eased_max / 2u)) / eased_max);
}

void set_timestretch_knob(uint16_t knob) {
  stretch_q8 = stretch_from_knob_q8(knob);
}

uint64_t wrap_stretch_phase(uint64_t phase_q32, uint32_t frame_count) {
  if (frame_count == 0) {
    return 0;
  }
  const uint64_t loop_len_q32 = static_cast<uint64_t>(frame_count) << 32u;
  while (phase_q32 >= loop_len_q32) {
    phase_q32 -= loop_len_q32;
  }
  return phase_q32;
}

uint64_t subtract_stretch_phase(uint64_t phase_q32, uint64_t amount_q32,
                                uint32_t frame_count) {
  if (frame_count == 0) {
    return 0;
  }
  const uint64_t loop_len_q32 = static_cast<uint64_t>(frame_count) << 32u;
  while (amount_q32 >= loop_len_q32) {
    amount_q32 -= loop_len_q32;
  }
  if (phase_q32 >= amount_q32) {
    return phase_q32 - amount_q32;
  }
  return loop_len_q32 - (amount_q32 - phase_q32);
}

uint32_t grain_window(uint16_t age) {
  if (age >= kGrainLengthSamples) {
    return 0;
  }
  if (age <= kGrainHopSamples) {
    return age;
  }
  return kGrainLengthSamples - age;
}

int16_t read_interpolated_stretch_sample(uint16_t sample_index,
                                         uint64_t phase_q32) {
  const uint32_t frame_count = raw_len(sample_index);
  phase_q32 = wrap_stretch_phase(phase_q32, frame_count);
  const uint32_t frame = static_cast<uint32_t>(phase_q32 >> 32u);
  const uint32_t next_frame = frame + 1u < frame_count ? frame + 1u : 0u;
  const uint32_t frac = static_cast<uint32_t>(phase_q32);

  const int16_t a = static_cast<int16_t>(raw_val(sample_index, frame)) - 128;
  const int16_t b =
      static_cast<int16_t>(raw_val(sample_index, next_frame)) - 128;
  const int32_t diff = static_cast<int32_t>(b) - static_cast<int32_t>(a);
  return static_cast<int16_t>(
      static_cast<int32_t>(a) +
      static_cast<int32_t>((static_cast<int64_t>(diff) * frac) >> 32u));
}

void invalidate_timestretch_grains() {
  timestretch_grains_initialized = false;
}

void initialize_timestretch_grains() {
  const uint32_t frame_count = raw_len(sample);
  timestretch_phase_q32 = wrap_stretch_phase(timestretch_phase_q32, frame_count);
  const uint64_t previous_grain_offset =
      static_cast<uint64_t>(kGrainHopSamples) * kTimestretchPhaseIncQ32;
  timestretch_grains[0] = {timestretch_phase_q32, kTimestretchPhaseIncQ32, 0};
  timestretch_grains[1] = {
      subtract_stretch_phase(timestretch_phase_q32, previous_grain_offset,
                             frame_count),
      kTimestretchPhaseIncQ32, static_cast<uint16_t>(kGrainHopSamples)};
  timestretch_grains_initialized = true;
}

void advance_timestretch_phase_by(uint64_t increment_q32) {
  timestretch_phase_q32 =
      wrap_stretch_phase(timestretch_phase_q32 + increment_q32, raw_len(sample));
}

void accumulate_timestretch_grain(const TimestretchGrain &grain,
                                  int32_t &mixed) {
  const uint32_t weight = grain_window(grain.age);
  if (weight == 0) {
    return;
  }

  const uint64_t grain_phase =
      grain.start_phase_q32 +
      (static_cast<uint64_t>(grain.age) * grain.phase_inc_q32);
  mixed += static_cast<int32_t>(read_interpolated_stretch_sample(sample,
                                                                  grain_phase)) *
           static_cast<int32_t>(weight);
}

void advance_timestretch_grain(TimestretchGrain &grain) {
  ++grain.age;
  if (grain.age < kGrainLengthSamples) {
    return;
  }

  grain.start_phase_q32 = timestretch_phase_q32;
  grain.phase_inc_q32 = kTimestretchPhaseIncQ32;
  grain.age = 0;
}

uint8_t render_stretched_sample() {
  if (!timestretch_grains_initialized) {
    initialize_timestretch_grains();
  }

  int32_t mixed = 0;
  accumulate_timestretch_grain(timestretch_grains[0], mixed);
  accumulate_timestretch_grain(timestretch_grains[1], mixed);

  advance_timestretch_phase_by(timestretch_source_inc_q32);
  advance_timestretch_grain(timestretch_grains[0]);
  advance_timestretch_grain(timestretch_grains[1]);

  const int32_t centered = mixed >> kGrainHopShift;
  const int32_t output = centered + 128;
  if (output < 0) {
    return 0;
  }
  if (output > 255) {
    return 255;
  }
  return static_cast<uint8_t>(output);
}

void reset_retrig_fx() {
  retrig_filter = 0;
  retrig_pitch_up = false;
  retrig_pitch_down = false;
  retrig_pitch_change = 0;
  retrig_volume_reduce = 0;
  retrig_volume_reduce_change = 0;
  button_filter_on = false;
  fx_retrig = false;
  btn_retrig = false;
  update_playback_rate();
}

void sync_phase_sample_from_timestretch() {
  const uint32_t frame_count = raw_len(sample);
  timestretch_phase_q32 = wrap_stretch_phase(timestretch_phase_q32, frame_count);
  const uint32_t frame = static_cast<uint32_t>(timestretch_phase_q32 >> 32u);
  phase_sample[phase_head] = frame;
  phase_sample[1 - phase_head] = frame;
  phase_xfade = 0;
}

void update_timestretch_state() {
  const uint32_t target_stretch_q8 = stretch_q8;
  if (target_stretch_q8 < kStretchQ8Bypass) {
    if (timestretch_active) {
      sync_phase_sample_from_timestretch();
      invalidate_timestretch_grains();
    }
    timestretch_active = false;
    timestretch_applied_q8 = kStretchQ8One;
    timestretch_source_inc_q32 = kTimestretchPhaseIncQ32;
    return;
  }

  if (!timestretch_active) {
    timestretch_phase_q32 =
        static_cast<uint64_t>(phase_sample[phase_head] % raw_len(sample)) << 32u;
    timestretch_audio_now = raw_val(sample, phase_sample[phase_head]);
    invalidate_timestretch_grains();
    reset_retrig_fx();
  }

  timestretch_active = true;
  if (target_stretch_q8 != timestretch_applied_q8) {
    timestretch_applied_q8 = target_stretch_q8;
    timestretch_source_inc_q32 =
        (kTimestretchPhaseIncQ32 << 8u) / target_stretch_q8;
  }
}

void sync_timestretch_sample_selection() {
  const uint32_t sample_count = piko_audio_sample_count();
  if (sample_count == 0 || sample_set == sample_change) {
    return;
  }

  sample_set = sample_change % sample_count;
  sample_change = sample_set;
  sample = sample_set;
  sample_add = 0;
  refresh_sample_timing(sample);
  timestretch_phase_q32 =
      wrap_stretch_phase(timestretch_phase_q32, raw_len(sample));
  invalidate_timestretch_grains();
}

void do_stop_everything();
void do_start_everything();

void restart_loop_from_beginning() {
  reset_retrig_fx();
  beat_num_total = 0;
  select_beat = 0;
  select_beat_freeze = 0;
  phase_sample[0] = 0;
  phase_sample[1] = 0;
  phase_head = 0;
  phase_xfade = 0;
  phase_retrig = 0;
  playback_phase_q32 = 0;
  timestretch_phase_q32 = 0;
  timestretch_audio_now = 128;
  invalidate_timestretch_grains();
  resume_transport_phase = false;
  btn_reset = true;
}

void clock_gpio_irq_handler(uint gpio, uint32_t events) {
  if (gpio == CLOCK_PIN && (events & GPIO_IRQ_EDGE_FALL) != 0 &&
      !clock_input_ittybittymidi) {
    clock_event_queue.push(
        {piko::ClockEventType::Pulse, time_us_32()});
  }
}

void midi_pio_irq_handler() {
  while (!pio_sm_is_rx_fifo_empty(pio1, 0)) {
    const uint32_t timestamp_us = time_us_32();
    const uint8_t byte = Onewiremidi_decode(pio_sm_get(pio1, 0));
    piko::ClockEventType type{};
    bool is_clock_event = true;
    switch (byte) {
      case MIDI_TIMING_CLOCK:
        type = piko::ClockEventType::MidiClock;
        break;
      case MIDI_START:
        type = piko::ClockEventType::MidiStart;
        break;
      case MIDI_CONTINUE:
        type = piko::ClockEventType::MidiContinue;
        break;
      case MIDI_STOP:
        type = piko::ClockEventType::MidiStop;
        break;
      default:
        is_clock_event = false;
        break;
    }
    if (is_clock_event) {
      clock_event_queue.push({type, timestamp_us});
    } else {
      midi_byte_queue.push({byte, timestamp_us});
    }
  }
}

void configure_clock_capture(bool midi) {
  const uint32_t interrupts = save_and_disable_interrupts();
  gpio_set_irq_enabled(CLOCK_PIN, GPIO_IRQ_EDGE_FALL, false);
  pio_set_irq0_source_enabled(pio1, pis_sm0_rx_fifo_not_empty, false);
  Onewiremidi_set_enabled(onewiremidi, false);
  clock_event_queue.clear();
  midi_byte_queue.clear();
  gpio_acknowledge_irq(CLOCK_PIN, GPIO_IRQ_EDGE_FALL);

  clock_input_ittybittymidi = midi;
  if (midi) {
    pio_gpio_init(pio1, CLOCK_PIN);
    pio_sm_set_consecutive_pindirs(pio1, 0, CLOCK_PIN, 1, false);
    Onewiremidi_set_enabled(onewiremidi, true);
    pio_set_irq0_source_enabled(pio1, pis_sm0_rx_fifo_not_empty, true);
    clock_sync.setSource(piko::ClockSource::Midi, pulse_ppqn, time_us_32());
  } else {
    gpio_set_function(CLOCK_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(CLOCK_PIN, GPIO_IN);
    gpio_pull_down(CLOCK_PIN);
    gpio_set_irq_enabled(CLOCK_PIN, GPIO_IRQ_EDGE_FALL, true);
    clock_sync.setSource(piko::ClockSource::Pulse, pulse_ppqn, time_us_32());
  }
  update_playback_rate();
  restore_interrupts(interrupts);
}

bool service_clock_transport(uint32_t& now_us) {
  piko::ClockEvent event{};
  while (clock_event_queue.pop(event)) {
    // Keep the carrier's cached time at least as new as the latest timestamp.
    // GPIO capture can preempt PWM after its queue check but before this drain.
    now_us = event.timestamp_us;
    if (event.type == piko::ClockEventType::MidiStart ||
        event.type == piko::ClockEventType::MidiContinue) {
      do_start_everything();
      soft_sync = false;
      btn_reset = false;
    } else if (event.type == piko::ClockEventType::MidiStop) {
      do_stop_everything();
      soft_sync = false;
      btn_reset = false;
    }
    clock_sync.process(event);
    if (clock_sync.consumeLoopRestart()) {
      restart_loop_from_beginning();
    }
  }
  const uint32_t target = clock_sync.targetBpmX100();
  if (target != playback_target_bpm_x100) {
    bpm_set = static_cast<uint16_t>((target + 50u) / 100u);
    update_playback_rate();
  }
  return clock_sync.advanceCarrier(now_us);
}

/*
 * PWM INTERRUPT LOGIC (main audio thread)
 */
void pwm_interrupt_handler() {
  pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));

  static uint16_t timing_check_divider = 0;
  static uint32_t cached_now_us = 0;
  if (++timing_check_divider >= 1024u || !clock_event_queue.empty()) {
    timing_check_divider = 0;
    cached_now_us = time_us_32();
  }
  const bool transport_beat = service_clock_transport(cached_now_us);

  // Match the legacy external-clock pause: after two missing expected pulses,
  // hold the current sample position and mute until capture resumes. Clock
  // processing remains first so the returning landmark restarts immediately.
  if (clock_sync.transportPaused()) {
    set_audio_pwm_level(128);
    return;
  }

  if (piko_audio_bank_mutating() || piko_audio_sample_count() == 0) {
    if (transport_beat) {
      ++beat_num_total;
      beat_onset = true;
      resume_transport_phase = true;
    }
    set_audio_pwm_level(128);
    return;
  }

  if (do_mute) {
    if (transport_beat) {
      ++beat_num_total;
      beat_onset = true;
      resume_transport_phase = true;
    }
    set_audio_pwm_level(128);
    return;
    // bool do_manual_hit = false;
    // if (do_mute) {
    //   if (input_button[1].ChangedHigh(true) ||
    //       input_button[2].ChangedHigh(true) ||
    //       input_button[5].ChangedHigh(true) ||
    //       input_button[6].ChangedHigh(true)) {
    //     soft_sync = true;
    //     do_manual_hit = true;
    //     do_mute_debounce = 0;
    //   }
    // }
    // if (!do_manual_hit) {
    // pwm_set_gpio_level(AUDIO_PIN, 128);
    // return;
    // }
  }

  if (resume_transport_phase && beat_onset && sample_beats > 0) {
    select_beat =
        (beat_num_total == 0 ? 0 : beat_num_total - 1u) % sample_beats;
    resume_transport_phase = false;
  }

  // All clock sources meet here as one eighth-note transport event.
  if (transport_beat || btn_reset || soft_sync) {
#ifdef DEBUG_CLOCK
    if (soft_sync) {
      printf("softsync\n");
    }
#endif
    soft_sync = false;
    beat_num_total++;
    beat_onset = true;
    beat_led = 1 - beat_led;
    noise_gate_val = 0;
    if (btn_reset) {
      beat_led = 1;
      beat_num_total = 0;
    }
    gpio_put(LED_PIN, beat_led);
    output_trigger.Trigger();

    if (do_mute_debounce > 0) {
      do_mute_debounce--;
    }

    // check button 1
    if (button_on < NUM_BUTTONS) {
      if (!input_button[button_on].On()) {
        // button is off
        button_on = NUM_BUTTONS;
        button_on2 = NUM_BUTTONS;
        select_beat_freeze = 0;
        button_filter_on = false;
        // hm
        retrig_volume_reduce = 0;
        retrig_volume_reduce_change = 0;  // reset

        if (btn_reset) {
          retrig_count = retrig_max;
        }
      }
    } else if (do_mute_debounce == 0) {
      for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        if (input_button[i].On()) {
          if (button_on >= NUM_BUTTONS) {
            select_beat_freeze = (select_beat / NUM_BUTTONS) * NUM_BUTTONS;
          }
          button_on = i;

// select new beat
#ifdef DEBUG_BUTTONS
          printf("%d on\n", button_on);
#endif
          break;
        }
      }
    }

    // check button 2
    if (button_on2 < NUM_BUTTONS) {
      if (!input_button[button_on2].On()) {
        button_on2 = NUM_BUTTONS;
        button_filter_on = false;
      }
    }
    if (!timestretch_active) {
      if (!btn_retrig) {
        // check button 2
        if (button_on < NUM_BUTTONS && do_mute_debounce == 0) {
          // 1st button pressed, check for second button
          for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
            if (i == button_on) {
              continue;
            }
            if (input_button[i].On()) {
#ifdef DEBUG_BUTTONS
              printf("%d + %d\n", button_on, i);
#endif
              btn_retrig = true;
              button_on2 = i;
            }
          }
        } else {
          if (randint(0, 254) < probability_retrig) {
            btn_retrig = true;
          }
        }
      } else if (btn_retrig) {
        // turn off retrig if one of the buttons is released
        if (button_on == NUM_BUTTONS || button_on2 == NUM_BUTTONS) {
          retrig_count = retrig_max;
        }
      }
    } else {
      reset_retrig_fx();
    }
    if (!fx_retrig) {
#ifdef DEBUG_PWM
      printf("[%d bpm / beat_num: %d] ", bpm_set, beat_num_total);
#endif

      // check for fx
      if (btn_retrig && !fx_retrig) {
#ifdef DEBUG_PWM
        printf("\n");
#endif
        fx_retrig = true;
        uint8_t r1 = randint(0, 100);
        uint8_t r2 = randint(0, 100);
        uint8_t r3 = randint(0, 100);
        uint8_t r4 = randint(0, 100);
        retrig_count = 0;
        // retrig_sel = randint(0, 11);
        // if (retrig_sel == 4) {
        //   retrig_sel = 5;
        // }
        if (button_on2 >= NUM_BUTTONS) {
          retrig_sel = randint(2, 16);
        } else {
          switch (button_on2) {
            case 0:
              retrig_sel = randint(0, 2);
              break;
            case 1:
              retrig_sel = randint(2, 4);
              break;
            case 2:
              retrig_sel = randint(4, 6);
              break;
            case 3:
              retrig_sel = randint(6, 8);
              break;
            case 4:
              retrig_sel = randint(8, 10);
              break;
            case 5:
              retrig_sel = randint(10, 12);
              break;
            case 6:
              retrig_sel = randint(12, 14);
              break;
            case 7:
              retrig_sel = randint(14, 16);
              break;
          }
        }
        retrig_max = randint(3, 16);
        if (retrig_sel < 6) {
          retrig_max = retrig_max / 2;
        } else if (retrig_sel > 11) {
          retrig_max = retrig_max * 2;
        }
        if (r1 <= 15) {
          retrig_pitch_up = true;
        } else if (r2 <= 15) {
          retrig_pitch_down = true;
        }
        if (r3 < 30) {
          retrig_filter = retrig_max;
          retrig_filter_change = (LPF_MAX - 10) / retrig_max;
        }
        if (r4 < 20 && retrig_sel > 6) {
          retrig_volume_reduce = retrig_max;
          if (retrig_volume_reduce > 5) {
            retrig_volume_reduce = 5;
          }
          retrig_volume_reduce_change = 1;  // volume increases
          if (randint(1, 100) < 30) {
            // delay fx
            retrig_volume_reduce_change = 2;  // volume decreases
            retrig_volume_reduce = 1;
          }
        }
        playback_phase_q32 =
            (1ull << 32u) - playback_effective_increment_q32;
        phase_retrig = (retrig_len(retrig_sel) << flag_half_time) - 1;
      }
    }
  }

  // disable beat interrupts during fx
  if (fx_retrig && !timestretch_active) {
    beat_onset = false;
  }

  // Fractional source-frame scheduling. At unity this is exactly 24 kHz on
  // average even though 24 kHz is not an integer divisor of the PWM carrier.
  playback_phase_q32 += playback_effective_increment_q32;
  const bool audio_tick = playback_phase_q32 >= (1ull << 32u);
  if (audio_tick) playback_phase_q32 -= 1ull << 32u;
  if (!audio_tick && !beat_onset) {
    set_audio_pwm_level(audio_now);
    return;
  }

  update_timestretch_state();

  if (audio_tick || beat_onset) {
    if (timestretch_active) {
      if (audio_tick) {
        sync_timestretch_sample_selection();
        noise_gate_val++;
        if (noise_gate_val < 10 & noise_gate_fade > 0) {
          noise_gate_fade--;
        } else if (noise_gate_val > noise_gate_thresh_use) {
          if (noise_gate_val % 100 == 0 && noise_gate_fade < 8) {
            noise_gate_fade++;
          }
        }
        timestretch_audio_now = render_stretched_sample();
        sync_phase_sample_from_timestretch();
      }
      if (beat_onset) {
        beat_onset = false;
        btn_reset = 0;
      }
    } else {
      // beat onset causes next sample
      if (beat_onset && fx_retrig == false) {
        bool do_switch_heads = true;

        if (probability_tunnel > 0) {
          if (randint(0, 255) < probability_tunnel) {
            sample_add = randint(0, piko_audio_sample_count() - 1);
          } else {
            sample_add = 0;
          }
        } else {
          sample_add = 0;
        }
        if (sample_set != sample_change) {
          sample_set = sample_change;
        }
        sample = (sample_set + sample_add) % piko_audio_sample_count();
        refresh_sample_timing(sample);

        beat_onset = false;
        if (do_lock_clock) {
          select_beat = beat_num_total % sample_beats;
        } else {
          select_beat++;
        }
        if (flag_half_time) {
          select_beat++;
          if (select_beat % 2 > 0)
            select_beat++;  // make sure for halftime mode its only on the even
                            // beats
        }

        if (select_beat < 0) {
          do_switch_heads = false;
          select_beat = sample_beats - 1;
        }
        if (select_beat >= sample_beats) {
          do_switch_heads = false;
          select_beat = 0;
        }

        // random jumps
        if (probability_jump > 0) {
          if (randint(0, 255) < probability_jump) {
            select_beat = randint(0, sample_beats - 1);
          }
        }

        // random gate
        if (probability_gate > 0) {
          if (randint(0, 255) < probability_gate) {
            noise_gate_thresh_use =
                sample_frames_per_slice * randint(800, 1000) / 1000;
          } else {
            noise_gate_thresh_use = noise_gate_thresh;
          }
        } else {
          noise_gate_thresh_use = noise_gate_thresh;
        }

        // reset
        if (btn_reset) {
          btn_reset = 0;
          select_beat = 0;
        }
        // printf("button_on: %d\n", button_on);
        // printf("button_on2: %d\n", button_on2);
        // printf("select_beat: %d\n", select_beat);

        // get beat from sequencer
        if (sequencer.IsPlaying()) {
          select_beat = sequencer.Next(beat_num_total);
#ifdef DEBUG_SEQUENCER
          printf("sequencer: [%d] %d\n", sequencer.NextI(beat_num_total),
                 select_beat);
#endif
        }

        // hold button down to play that beat
        if (button_on < NUM_BUTTONS) {
          select_beat = (button_on + select_beat_freeze) % sample_beats;
          // record the current beat
          sequencer.Record(select_beat);
        }
#ifdef DEBUG_PWM
        printf("select_beat:%d for %d samples\n", select_beat,
               retrig_len(retrig_sel) << flag_half_time);
#endif
        piko_usb_midi_note_on(midi_notes_set[(select_beat % 8)], 127);

        if (do_switch_heads) {
          phase_head = 1 - phase_head;  // switch heads
          phase_xfade = 1 << HEAD_SHIFT;
        }
        phase_sample[phase_head] =
            select_beat * (sample_frames_per_slice << flag_half_time);

        // random direction for the new head
        if (probability_direction > 0) {
          uint8_t r1 = randint(0, 255);
          if (direction[phase_head] == base_direction) {
            if (r1 < probability_direction) {
              direction[phase_head] = 1 - base_direction;
            }
          } else {
            if (r1 > probability_direction) {
              direction[phase_head] = base_direction;
            }
          }
        } else {
          direction[phase_head] = base_direction;
        }
      } else {
        // update the sample
        noise_gate_val++;
        if (noise_gate_val < 10 & noise_gate_fade > 0) {
          // noise gate fade in
          noise_gate_fade--;
        } else if (noise_gate_val > noise_gate_thresh_use) {
          // noise gate fade out
          if (noise_gate_val % 100 == 0) {
            if (noise_gate_fade < 8) {
              noise_gate_fade++;
            }
          }
        }
        for (uint8_t i = 0; i < 2; i++) {
          if (direction[i]) {
            phase_sample[i]++;
            if (phase_sample[i] == raw_len(sample) - 1) {
              phase_sample[i] = 0;
            }
          } else {
            if (phase_sample[i] == 0) {
              phase_sample[i] = raw_len(sample) - 2;
            } else {
              phase_sample[i]--;
            }
          }
        }
      }

      if (fx_retrig) {
        phase_retrig++;

        // prevent noise gating?
        noise_gate_fade = 0;
        noise_gate_val = 0;

        if (phase_retrig % (retrig_len(retrig_sel) << flag_half_time) == 0) {
          retrig_count++;
          if (retrig_filter > 0) {
            retrig_filter--;
          }

          piko_usb_midi_note_on(midi_notes_set[(select_beat % 8)],
                                120 * retrig_count / retrig_max);

          // printf("retrig_volume_reduce_change: %d\n",
          //        retrig_volume_reduce_change);
          // printf("retrig_volume_reduce: %d\n", retrig_volume_reduce);

          if (retrig_volume_reduce_change == 1 && retrig_volume_reduce > 0) {
            if (retrig_sel > 11) {
              if (retrig_count % 2 == 0) {
                retrig_volume_reduce--;
              }
            } else {
              retrig_volume_reduce--;
            }
          } else if (retrig_volume_reduce_change == 2 &&
                     retrig_volume_reduce < 8 && retrig_count % 2 == 0) {
            if (retrig_sel > 11) {
              if (retrig_count % 4 == 0) {
                retrig_volume_reduce++;
              }
            } else {
              retrig_volume_reduce++;
            }
          }
          if (retrig_pitch_up) {
            retrig_pitch_change++;
            update_playback_rate();
          } else if (retrig_pitch_down) {
            retrig_pitch_change--;
            update_playback_rate();
          }
          if (retrig_count >= retrig_max) {
            reset_retrig_fx();
          }
#ifdef DEBUG_PWM
          printf(
              "[retrig %d/%d] select_beat:%d for %d samples, "
              "\n\tphase_sample[phase_head]: %d%%%d==0\n",
              retrig_count, retrig_max, select_beat,
              retrig_len(retrig_sel) << flag_half_time,
              phase_sample[phase_head],
              (retrig_len(retrig_sel) << flag_half_time));
#endif
          // setup
          phase_head = 1 - phase_head;  // switch heads
          phase_xfade = 1 << HEAD_SHIFT;
          phase_sample[phase_head] =
              select_beat * (sample_frames_per_slice << flag_half_time);
          phase_retrig = 0;
        }
      }
    }
  }

  // determine sample
  if (timestretch_active) {
    audio_now = timestretch_audio_now;
  } else {
    if (phase_xfade == 0) {
      audio_now = raw_val(sample, phase_sample[phase_head]);
    } else {
      phase_xfade--;

      // new head
      uint32_t u = (uint32_t)raw_val(sample, phase_sample[phase_head]);
      u = u * ((1 << HEAD_SHIFT) - phase_xfade);  // fade it in

      // old head
      uint32_t v = (uint32_t)raw_val(sample, phase_sample[1 - phase_head]);
      v = v * phase_xfade;  // fade it out

      // combine
      u = (u + v) >> HEAD_SHIFT;

      // set to audio now
      audio_now = (uint8_t)u;
      // if (phase_xfade == 1 << HEAD_SHIFT - 1) {
      //   // printf("\nphase_head: %d; audio_now=%d\n", phase_head, u);
      // }
    }
  }

    // <volume>
    if (volume_reduce >= VOLUME_REDUCE_MAX) audio_now = 128;
    if (audio_now != 128) {
      // distortion / wave-folding
      if (distortion > 0) {
        if (audio_now > 128) {
          if (audio_now < (255 - distortion)) {
            audio_now += distortion;
          } else {
            audio_now = 255 - distortion;
          }
          audio_now = 128 + ((audio_now - 128) / ((distortion >> 4) + 1));
        } else {
          if (audio_now > distortion) {
            audio_now -= distortion;
          } else {
            audio_now = distortion - audio_now;
          }
          audio_now = 128 - ((128 - audio_now) / ((distortion >> 4) + 1));
        }
      }
      // reduce volume
      if (volume_reduce > 0) {
        if (audio_now > 128) {
          audio_now = audio_now - (volume_reduce);
          if (audio_now < 128) audio_now = 128;
        } else {
          audio_now = audio_now + (volume_reduce);
          if (audio_now > 128) audio_now = 128;
        }
      }
      if ((volume_mod + retrig_volume_reduce + noise_gate_fade) > 0 &&
          audio_now != 128) {
        if (audio_now > 128) {
          audio_now = ((audio_now - 128) >>
                       (volume_mod + retrig_volume_reduce + noise_gate_fade)) +
                      128;
        } else {
          audio_now =
              128 - ((128 - audio_now) >>
                     (volume_mod + retrig_volume_reduce + noise_gate_fade));
        }
      }
    }  // </volume>

    // <bitcrush>
    // if (bitcrush > 0) {
    //   if (audio_now > 128) {
    //     audio_now = 128 + (((audio_now - 128) >> bitcrush) << bitcrush);
    //   } else if (audio_now < 128) {
    //     audio_now = 128 - (((128 - audio_now) >> bitcrush) << bitcrush);
    //   }
    // }
    // </bitcrush>

    // <filter>
    if ((filter_fc - (retrig_filter * retrig_filter_change) - button_filter) <=
        LPF_MAX) {
      audio_now = (uint8_t)filter_lpf(
          (int64_t)audio_now,
          (filter_fc - (retrig_filter * retrig_filter_change) - button_filter),
          filter_q);
      // } else {
      // audio_now = (uint8_t)filter_lpf((int64_t)audio_now, LPF_MAX, filter_q);
    }
    // </filter>

    // <delay>
    // audio_now = delay.Update(audio_now);
    // </delay>

    // <dither>
    // audio_now = ditherer.Update(audio_now);
    // </dither>
  set_audio_pwm_level(audio_now);
}

void print_buf(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    printf("%02x", buf[i]);
    if (i % 24 == 23)
      printf("\n");
    else
      printf(" ");
  }
}

void do_stop_everything() { do_mute = true; }
void do_start_everything() {
  do_mute_debounce = 8;
  button_on = NUM_BUTTONS;
  button_on2 = NUM_BUTTONS;
  btn_reset = true;
  // reset everything
  // reset retrig stuff
  retrig_filter = 0;
  retrig_pitch_up = false;
  retrig_pitch_down = false;
  retrig_pitch_change = 0;
  retrig_volume_reduce = 0;
  retrig_volume_reduce_change = 0;
  button_filter_on = false;
  fx_retrig = false;
  btn_retrig = false;
  do_mute = false;
  update_playback_rate();
}

uint32_t current_time() { return to_ms_since_boot(get_absolute_time()); }

uint16_t *sort_int32_t(uint32_t array[], int n) {
  // C
  // uint16_t *indexes = malloc(sizeof(uint16_t) * n);
  // C++
  uint16_t *indexes = new uint16_t[n];
  for (int i = 0; i < n; i++) {
    indexes[i] = i;
  }
  // Sort the auxiliary array based on the values in the original array.
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (array[indexes[i]] > array[indexes[j]]) {
        // Swap the elements at indexes[i] and indexes[j].
        int temp = indexes[i];
        indexes[i] = indexes[j];
        indexes[j] = temp;
      }
    }
  }
  return indexes;
}

#define MIDI_MAX_NOTES 128
#define MIDI_MAX_TIME_ON 10000  // 10 seconds

uint32_t note_hit[MIDI_MAX_NOTES];
bool note_on[MIDI_MAX_NOTES];

void midi_note_off(uint8_t note) {
#ifdef DEBUG_MIDI
  printf("note_off: %d\n", note);
#endif
#if MIDI_NOTE_KEY == 1
  input_button[note % NUM_BUTTONS].Set(false);
  if (midi_button2 > -1) {
    midi_button2 = -1;
  } else {
    midi_button1 = -1;
  }
#endif
}

void midi_note_on(uint8_t note, uint8_t velocity) {
#ifdef DEBUG_MIDI
  printf("note_on: %d\n", note);
#endif
#if MIDI_NOTE_KEY == 1
  if (midi_button1 > -1) {
    midi_button2 = note % NUM_BUTTONS;
  } else {
    midi_button1 = note % NUM_BUTTONS;
  }
  input_button[note % NUM_BUTTONS].Set(true);
#endif
}

bool piko_clock_input_ittybittymidi() {
  return clock_input_ittybittymidi;
}

uint8_t piko_pulse_ppqn() { return pulse_ppqn; }

int main(void) {
  // TinyUSB is initialized and serviced exclusively by core 1.
  set_sys_clock_khz(CLOCK_RATE, true);
  piko_audio_bank_init();
  stdio_init_all();
  multicore_launch_core1_with_stack(piko_sample_manager_core, core1_stack,
                                    sizeof(core1_stack));

  // sleep needed to make sure it can start on battery
  // not sure why
  sleep_ms(100);

  // initialize leds
  ledarray.Init();

  // initialize clocking and PWM interrupts
  // overclock at a multiple of sampling rate
  gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
  int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);
  pwm_clear_irq(audio_pin_slice);
  irq_set_priority(PWM_IRQ_WRAP, 0x40);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
  pwm_set_irq_enabled(audio_pin_slice, false);
  irq_set_enabled(PWM_IRQ_WRAP, false);
  pwm_config config = pwm_get_default_config();
  pwm_config_set_clkdiv(&config, 1.0f);
  pwm_config_set_wrap(&config, kPwmWrap);
  pwm_init(audio_pin_slice, &config, true);
  pwm_set_gpio_level(AUDIO_PIN, 0);
  pwm_carrier_hz = clock_get_hz(clk_sys) / (kPwmWrap + 1u);
  clock_sync.setCarrierHz(pwm_carrier_hz);

  if (piko_audio_sample_count() > 0) refresh_sample_timing(0);
  param_set_bpm(BPM_SAMPLED);

  // setup gpio pins
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  gpio_init(CLOCK_PIN);
  gpio_set_dir(CLOCK_PIN, GPIO_IN);
  gpio_pull_down(CLOCK_PIN);
  gpio_init(23);
  gpio_pull_up(23);
  gpio_set_dir(23, GPIO_OUT);
  gpio_put(23, 0);

  // initialize buttons
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    input_button[i].Init(i + 4, 10);  // GPIO 4 through 11 are buttons
  }

  // initialize knobs
  adc_init();
  for (uint8_t i = 0; i < NUM_KNOBS; i++) {
    input_knob[i].Init(i, 50);
  }

  // initialize sequencer
  sequencer.Init();

  // initialize save data
  uint8_t save_data[FLASH_PAGE_SIZE];
  for (uint32_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
    save_data[i] = 0;
  }
  // // save defaults that aren't defaulted to 0
  save_data[SAVE_VOLUME] = (uint8_t)(2500 >> 8);
  save_data[SAVE_VOLUME + 1] = (uint8_t)2500;
  save_data[SAVE_BPM] = (uint8_t)(165 >> 8);
  save_data[SAVE_BPM + 1] = (uint8_t)165;
  noise_gate_thresh = gate_default_thresh();
  noise_gate_thresh_use = noise_gate_thresh;
  save_data[SAVE_GATE] = (uint8_t)(noise_gate_thresh >> 8);
  save_data[SAVE_GATE + 1] = (uint8_t)noise_gate_thresh;
  save_data[SAVE_CLOCK_INPUT_MODE] = CLOCK_INPUT_CLOCK;
  save_data[SAVE_PULSE_PPQN] = 2;

  // initializer trigger
  output_trigger.Init(TRIGO_PIN, 10, MAIN_LOOP_HZ);

  // initialize control loop variables
  uint32_t clock_ms = 0;
  uint16_t alpha0 = 500;
  uint8_t selector_knob = 0;
  uint16_t ledarray_sel = 0;
  uint32_t ledarray_sel_debounce = 0;
  uint16_t ledarray_save = 0;
  uint16_t ledarray_load = 0;
  uint32_t ledarray_binary_debounce = 0;
  uint8_t ledarray_binary = 0;

  // debouncing
  uint16_t debounce_sample = 0;
  uint16_t debounce_lock_clock = 0;
  uint32_t debounce_saving = 0;
  uint32_t debounce_led_save = 0;
  uint8_t debounce_led_sequencer = 0;
  uint8_t debounce_led_load = 0;
  uint8_t last_button_on = NUM_BUTTONS;
  bool has_saved = false;
  bool do_load = false;
  bool first_time = false;
  bool has_loaded = false;
  auto save_settings = [&]() {
    save_data[FLASH_PAGE_SIZE - 1] = 0x01;
    save_data[FLASH_PAGE_SIZE - 2] = 0x02;
    save_data[FLASH_PAGE_SIZE - 3] = 0x03;
    save_data[FLASH_PAGE_SIZE - 4] = 0x04;
    sequencer.Save(save_data);
#ifdef DEBUG_SAVE
    print_buf(save_data, FLASH_PAGE_SIZE);
#endif
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(PIKO_SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PIKO_SETTINGS_FLASH_OFFSET, save_data, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
  };

  // initialize one wire midi
  onewiremidi =
      Onewiremidi_new(pio1, 0, CLOCK_PIN, midi_note_on, midi_note_off,
                      nullptr, nullptr, nullptr, nullptr);
  irq_set_exclusive_handler(PIO1_IRQ_0, midi_pio_irq_handler);
  irq_set_priority(PIO1_IRQ_0, 0x00);
  irq_set_enabled(PIO1_IRQ_0, true);
  gpio_set_irq_enabled_with_callback(CLOCK_PIN, GPIO_IRQ_EDGE_FALL, false,
                                     clock_gpio_irq_handler);
  irq_set_priority(IO_IRQ_BANK0, 0x00);
  irq_set_enabled(IO_IRQ_BANK0, true);
  configure_clock_capture(false);

// LED
#if WS2812_ENABLED == 1
  WS2812 ledStrip(
      23,    // Data line is connected to pin 0. (GP0)
      1,     // Strip is 6 LEDs long.
      pio0,  // Use PIO 0 for creating the state machine.
      0,  // Index of the state machine that will be created for controlling the
          // LED strip You can have 4 state machines per PIO-Block up to 8
          // overall. See Chapter 3 in:
          // https://datasheets.raspberrypi.org/rp2040/rp2040-datasheet.pdf
      WS2812::FORMAT_GRB
  );
#endif

  piko_sample_manager_set_ready();
  pwm_clear_irq(audio_pin_slice);
  pwm_set_irq_enabled(audio_pin_slice, true);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  // control loop
  while (1) {
    __wfi();  // Wait for Interrupt
    clock_ms++;

    MidiByteEvent midi_byte{};
    while (midi_byte_queue.pop(midi_byte)) {
      Onewiremidi_receive_byte(onewiremidi, midi_byte.byte,
                               midi_byte.timestamp_us);
    }
#if WS2812_ENABLED == 1
    if (clock_ms % 200 == 0) {
      const uint8_t knob_a_led =
          input_knob[1].Value() * 120 / input_knob[1].ValueMax();
      const uint8_t knob_b_led =
          input_knob[2].Value() * 120 / input_knob[2].ValueMax();
      if (debounce_led_save > 0) debounce_led_save--;
      if (debounce_lock_clock > 0) debounce_lock_clock--;
      if (sequencer.IsPlaying() && debounce_led_sequencer > 0) {
        debounce_led_sequencer--;
      }
      if (debounce_led_load > 0) debounce_led_load--;
      ledStrip.fill(WS2812::RGB(knob_a_led, 0, knob_b_led));
      ledStrip.show();
    }
#endif

    if (debounce_sample > 0) {
      debounce_sample--;
    }
    PikoRequest request{};
    while (piko_runtime_pop_request(&request)) {
      bool ok = true;
      switch (request.type) {
        case PikoRequestType::SetClockMode:
          if (request.value > 1) {
            ok = false;
          } else {
            configure_clock_capture(request.value == CLOCK_INPUT_MIDI);
            save_data[SAVE_CLOCK_INPUT_MODE] = request.value;
            save_settings();
          }
          break;
        case PikoRequestType::SetPulsePpqn:
          if (!piko::ClockSync::validPulsePpqn(request.value)) {
            ok = false;
          } else {
            pulse_ppqn = request.value;
            save_data[SAVE_PULSE_PPQN] = pulse_ppqn;
            const uint32_t interrupts = save_and_disable_interrupts();
            clock_sync.setPulsePpqn(pulse_ppqn, time_us_32());
            restore_interrupts(interrupts);
            save_settings();
          }
          break;
        case PikoRequestType::StopPlayback:
          do_stop_everything();
          break;
        case PikoRequestType::StartPlayback:
          do_start_everything();
          break;
      }
      piko_runtime_ack_request(request.id, ok);
    }

    if (clock_ms % 1000u == 0) {
      const uint32_t interrupts = save_and_disable_interrupts();
      const piko::ClockDiagnostics clock_diagnostics = clock_sync.diagnostics();
      restore_interrupts(interrupts);
      piko_publish_clock_snapshot({
          clock_diagnostics, clock_event_queue.drops(),
          midi_byte_queue.drops() + piko_usb_midi_queue_drops()});
    }
    // flash works
    if (debounce_saving > 0 && clock_ms > 64000) {
      debounce_saving--;
      if (debounce_saving == 0) {
#ifdef DEBUG_SAVE
        printf("\nsaving:\n");
#endif
        save_settings();
#ifdef DEBUG_SAVE
        printf("saved!\n");
#endif
      }
    }
    if (clock_ms == 100) {
      do_load = true;
      first_time = true;
    }
    if (do_load) {
      do_load = false;
      ledarray_load = 16000;
      debounce_saving = 0;
#ifdef DEBUG_SAVE
      printf("\n\n\nPICO_FLASH_SIZE_BYTES: \t%d\n", PICO_FLASH_SIZE_BYTES);
      printf("PIKO_SETTINGS_FLASH_OFFSET: \t%d\n", PIKO_SETTINGS_FLASH_OFFSET);
      printf("FLASH_PAGE_SIZE: \t%d\n", FLASH_PAGE_SIZE);
      printf("FLASH_SECTOR_SIZE: \t%d\n", FLASH_SECTOR_SIZE);
      printf("XIP_BASE: \t%d\n", XIP_BASE);
      printf("save data:\n");
      print_buf(flash_target_contents, FLASH_PAGE_SIZE);
      printf("\nloading saved data: \n");
#endif
      if (flash_target_contents[FLASH_PAGE_SIZE - 1] == 0x01 &&
          flash_target_contents[FLASH_PAGE_SIZE - 2] == 0x02 &&
          flash_target_contents[FLASH_PAGE_SIZE - 3] == 0x03 &&
          flash_target_contents[FLASH_PAGE_SIZE - 4] == 0x04) {
        for (uint i = 0; i < FLASH_PAGE_SIZE; i++) {
          save_data[i] = flash_target_contents[i];
        }
        param_set_volume((uint16_t)(save_data[SAVE_VOLUME] << 8) +
                             save_data[SAVE_VOLUME + 1],
                         distortion, volume_reduce);
        // filter_fc = flash_target_contents[SAVE_FILTER];
        sample_change = save_data[SAVE_SAMPLE];
        if (piko_audio_sample_count() > 0) {
          sample_change %= piko_audio_sample_count();
          sample = sample_change;
          refresh_sample_timing(sample);
        } else {
          sample_change = 0;
          sample = 0;
        }
        param_set_bpm((uint16_t)(save_data[SAVE_BPM] << 8) +
                      save_data[SAVE_BPM + 1]);
        noise_gate_thresh =
            (uint16_t)(save_data[SAVE_GATE] << 8) + save_data[SAVE_GATE + 1];
        probability_direction = save_data[SAVE_PROB_DIRECTION];
        probability_jump = save_data[SAVE_PROB_JUMP];
        probability_retrig = save_data[SAVE_PROB_RETRIG];
        probability_gate = save_data[SAVE_PROB_GATE];
        probability_tunnel = save_data[SAVE_PROB_TUNNEL];
        clock_input_ittybittymidi =
            save_data[SAVE_CLOCK_INPUT_MODE] == CLOCK_INPUT_MIDI;
        save_data[SAVE_CLOCK_INPUT_MODE] =
            clock_input_ittybittymidi ? CLOCK_INPUT_MIDI : CLOCK_INPUT_CLOCK;
        pulse_ppqn = piko::ClockSync::validPulsePpqn(
                         save_data[SAVE_PULSE_PPQN])
                         ? save_data[SAVE_PULSE_PPQN]
                         : 2;
        save_data[SAVE_PULSE_PPQN] = pulse_ppqn;
        configure_clock_capture(clock_input_ittybittymidi);
        sequencer.Load(save_data);
#ifdef DEBUG_SAVE
        printf("volume_reduce: %d\n", volume_reduce);
        printf("distortion: %d\n", distortion);
        printf("bpm_set: %d\n", bpm_set);
        printf("filter_fc: %d\n", filter_fc);
        printf("sample_change: %d\n", sample_change);
        printf("noise_gate_thresh: %d\n", noise_gate_thresh);
        printf("probability_direction: %d\n", probability_direction);
        printf("probability_jump: %d\n", probability_jump);
        printf("probability_retrig: %d\n", probability_retrig);
        printf("probability_gate: %d\n", probability_gate);
#endif
      }
    }

    if (clock_ms % 16 == 0) {  // 250 Hz
      // read gpio inputs
      for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        if (midi_button1 != i && midi_button2 != i) {
          input_button[i].Read();
        }
        // if (input_button[i].ChangedHigh(false)) {
        //   MidiOut_on(midiout, midi_notes[(i % 8)], 127);
        // }
        if (input_button[1].ChangedHigh(true) ||
            input_button[2].ChangedHigh(true) ||
            input_button[5].ChangedHigh(true) ||
            input_button[6].ChangedHigh(true)) {
          if (input_button[1].On() && input_button[2].On() &&
              input_button[5].On() && input_button[6].On()) {
            debounce_lock_clock = 80;
            do_lock_clock = !do_lock_clock;
          }
        }
        if (input_button[0].ChangedHigh(true) ||
            input_button[1].ChangedHigh(true) ||
            input_button[6].ChangedHigh(true) ||
            input_button[7].ChangedHigh(true)) {
          if (input_button[0].On() && input_button[1].On() &&
              input_button[6].On() && input_button[7].On()) {
            // reset fx
            param_set_break(0, filter_fc, distortion, probability_jump,
                            probability_retrig, probability_gate,
                            probability_direction, probability_tunnel,
                            save_data);
          }
        }
        if (input_button[0].ChangedHigh(true) ||
            input_button[3].ChangedHigh(true) ||
            input_button[4].ChangedHigh(true) ||
            input_button[7].ChangedHigh(true)) {
          // button combo
          if (input_button[0].On() && input_button[3].On() &&
              input_button[4].On() && input_button[7].On()) {
            if (do_mute) {
              do_start_everything();
            } else {
              do_stop_everything();
            }
            // printf("switching do mute: %d\n", do_mute);
          }
        }
#ifdef DEBUG_BUTTONS
        if (input_button[i].Changed(false)) {
          printf("[%6d] %d: %d", clock_ms, i, input_button[i].On());
          if (input_button[i].Rising()) {
            printf("rising");
          }
          if (input_button[i].Falling()) {
            printf("falling");
          }
          printf("\n");
        }
#endif
      }

      // adc reading
      if (!btn_retrig) {
        for (uint8_t i = 0; i < NUM_KNOBS; i++) {
          input_knob[i].Read();

          // set the current midi note if a button is held down
          if (input_knob[i].Changed() && i == 0) {
            // midi_notes_set
            for (uint8_t j = 0; j < NUM_BUTTONS; j++) {
              if (input_button[j].On()) {
                midi_notes_set[j] =
                    midi_notes_available[input_knob[i].Value() *
                                         MIDI_NOTES_AVAILABLE_TOTAL / 4095];
              }
            }
          }

          if (input_knob[i].Changed() || first_time) {
            if (i == 0) {
              uint8_t selector_knob_before = selector_knob;
              selector_knob =
                  (input_knob[i].Value() * 8 / input_knob[i].ValueMax());
#ifdef DEBUG_KNOB
              printf("%d: %d; \n", i, input_knob[i].Value());
#endif
              if (selector_knob != selector_knob_before) {
                has_saved = false;
                for (uint8_t j = 1; j < NUM_KNOBS; j++) {
                  input_knob[j].Reset();  // prevent spurious changes when
                                          // changing selection
                }
              }
              ledarray_sel = selector_knob;
              ledarray_sel_debounce = 16000;
              sequencer.SetRecording(false);
              if (first_time) {
                first_time = false;
                break;
              }
            } else if (i == 1) {
              ledarray_sel_debounce = 0;
#ifdef DEBUG_KNOB
              printf("%d: %d; \n", i, input_knob[i].Value());
#endif
              /////////////
              // KNOB A //
              ////////////
              switch (selector_knob) {
                case 0:
                  // sample
                  if (debounce_sample == 0 && piko_audio_sample_count() > 0) {
                    uint16_t sample_count = piko_audio_sample_count();
                    sample_change = input_knob[i].Value() * sample_count /
                                    input_knob[i].ValueMax();
                    if (sample_change >= sample_count) {
                      sample_change = sample_count - 1;
                    }
                    debounce_sample = 500;
                    save_data[SAVE_SAMPLE] = sample_change;
                  }
                  break;
                case 1:
                  filter_fc = input_knob[i].Value() * (LPF_MAX + 10) /
                              input_knob[i].ValueMax();
                  break;
                case 2:
                  // gate
                  if (input_knob[i].Value() > 3700) {
                    noise_gate_thresh = gate_default_thresh();
                  } else {
                    noise_gate_thresh =
                        gate_scaled_thresh(input_knob[i].Value(),
                                           input_knob[i].ValueMax());
                  }
                  save_data[SAVE_GATE] = (uint8_t)(noise_gate_thresh >> 8);
                  save_data[SAVE_GATE + 1] = (uint8_t)noise_gate_thresh;
                  break;
                case 3:
                  // jump probability
                  if (input_knob[i].Value() < 200) {
                    probability_jump = 0;
                  } else {
                    probability_jump = (input_knob[i].Value() * 254 /
                                        input_knob[i].ValueMax());
                  }
                  save_data[SAVE_PROB_JUMP] = probability_jump;
                  break;
                case 4:
                  // tunnel probability
                  if (input_knob[i].Value() < 200) {
                    probability_tunnel = 0;
                  } else {
                    probability_tunnel = (input_knob[i].Value() * 254 /
                                          input_knob[i].ValueMax());
                  }
                  save_data[SAVE_PROB_TUNNEL] = probability_tunnel;
                  break;
                case 5:
                  // sequencer rec
                  if (input_knob[i].Value() > 3500) {
                    sequencer.SetRecording(true);
                  } else {
                    sequencer.SetRecording(false);
                    if (input_knob[i].Value() < 1000) {
                      sequencer.Reset();
                    }
                  }
                  break;
                case 6:
                  // save
                  if (input_knob[i].Value() > 2040) {
                    if (!has_saved) {
                      has_saved = true;
                      debounce_saving = 32000;
                      debounce_led_save = 255;
                    }
                  } else {
                    has_saved = false;
                  }
                  break;
                case 7:
                  // volume
                  save_data[SAVE_VOLUME] =
                      (uint8_t)(input_knob[i].Value() >> 8);
                  save_data[SAVE_VOLUME + 1] = (uint8_t)input_knob[i].Value();
                  param_set_volume(input_knob[i].Value(), distortion,
                                   volume_reduce);
#ifdef DEBUG_KNOB
                  printf("%d: %d; \n", i, input_knob[i].Value());
#endif
                  break;
                default:
                  break;
              }
            } else if (i == 2) {
              ledarray_sel_debounce = 0;
#ifdef DEBUG_KNOB
              printf("%d: %d; \n", i, input_knob[i].Value());
              printf("[%d] %d: %d; \n", selector_knob, i,
                     input_knob[i].Value());
#endif

              switch (selector_knob) {
                case 0:
                  param_set_break(input_knob[i].Value(), filter_fc, distortion,
                                  probability_jump, probability_retrig,
                                  probability_gate, probability_direction,
                                  probability_tunnel, save_data);
                  break;
                case 1:
                  // stretch
                  set_timestretch_knob(input_knob[i].Value());
                  break;
                case 2:
                  // gate probability
                  if (input_knob[i].Value() < 200) {
                    probability_gate = 0;
                  } else {
                    probability_gate = (input_knob[i].Value() * 254 /
                                        input_knob[i].ValueMax());
                  }
                  save_data[SAVE_PROB_GATE] = probability_direction;
                  break;
                case 3:
                  // retrig probability
                  if (input_knob[i].Value() < 200) {
                    probability_retrig = 0;
                  } else {
                    probability_retrig = (input_knob[i].Value() * 254 /
                                          input_knob[i].ValueMax());
                  }
                  save_data[SAVE_PROB_RETRIG] = probability_jump;
                  break;
                case 4:
                  // reverse probability
                  if (input_knob[i].Value() < 200) {
                    probability_direction = 0;
                  } else {
                    probability_direction = (input_knob[i].Value() * 254 /
                                             input_knob[i].ValueMax());
                  }
                  save_data[SAVE_PROB_DIRECTION] = probability_direction;
                  break;
                case 5:
                  // sequencer on
                  sequencer.SetPlaying(input_knob[i].Value() > 2200);
                  if (sequencer.IsPlaying()) {
                    debounce_led_sequencer = 255;
                  }
                  break;
                case 6:
                  // load
                  if (input_knob[i].Value() > 4000) {
                    if (!has_loaded) {
                      do_load = true;
                      debounce_led_load = 255;
                    }
                    has_loaded = true;
                  } else {
                    has_loaded = false;
                  }

                  break;
                case 7:
                  // tempo
                  {
                    uint16_t bpm_set_new =
                        round((double)input_knob[i].Value() * 255.0 / 4095 / 5) *
                            5 +
                        50;
                    ledarray_binary_debounce = 48000;
                    ledarray_binary = bpm_set_new - 50;
                    if (bpm_set_new > 360) bpm_set_new = 360;
                    if (bpm_set_new != internal_bpm_set) {
#ifdef DEBUG_KNOB
                      printf("%d: %d; \n", i, input_knob[i].Value());
#endif
                      save_data[SAVE_BPM] = (uint8_t)(bpm_set_new >> 8);
                      save_data[SAVE_BPM + 1] = (uint8_t)bpm_set_new;

                      param_set_bpm(bpm_set_new);
                    }
                  }
                  break;

                default:
                  break;
              }
            }
          }
        }
      }
      // adc reading end
    }

    // trig out
    output_trigger.Update();

    if (ledarray_binary_debounce > 0) {
      ledarray_binary_debounce--;
      ledarray.SetBinary(ledarray_binary);
    } else if (sequencer.IsRecording()) {
      ledarray.Clear();
      if (sequencer.Last() < 255) {
        ledarray.Set(sequencer.Last() % NUM_BUTTONS, 10000);
      }
    } else {
      ledarray.Clear();
      if (ledarray_sel_debounce > 0) {
        ledarray_sel_debounce--;
        ledarray.Set(ledarray_sel, 950);
      } else {
        if (button_on < NUM_BUTTONS) {
          ledarray.Add(button_on, 250);
          if (button_on2 < NUM_BUTTONS) {
            ledarray.Add(button_on2, 250);
          }
        } else {
          ledarray.Add(select_beat % NUM_BUTTONS, 250);
        }
      }
    }
    ledarray.Update();

    // sleep this thread
    sleep_us(MAIN_LOOP_DELAY);
  }
}
