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
#include "pico/stdlib.h"     // stdlib

// pikocore files
#include "doth/audio2h.h"
#include "doth/button.h"
#include "doth/delay.h"
#include "doth/easing.h"
#include "doth/filter.h"
#include "doth/flash_target_offset.h"
#include "doth/knob.h"
#include "doth/led.h"
#include "doth/ledarray.h"
#include "doth/onewiremidi.h"
#include "doth/runningavg.h"
#include "doth/sequencer.h"
#include "doth/trigger_out.h"

// constants
#define CLOCK_RATE 264000
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

const uint8_t *flash_target_contents =
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

// inputs
Button input_button[NUM_BUTTONS];
Knob input_knob[NUM_KNOBS];

// outputs
LEDArray ledarray;

TriggerOut output_trigger;

// audio tracking
uint8_t audio_now = 0;
uint8_t audio_clk = 0;
uint8_t audio_clk_thresh = 48;
bool do_mute = false;
uint8_t do_mute_debounce = 0;

// sample tracking
uint16_t sample = 0;
uint16_t sample_beats = 8;
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

// volume/distortion/filter/bitcrush
uint8_t distortion = 0;
uint8_t volume_reduce = 0;
uint8_t filter_fc = LPF_MAX + 10;
uint8_t hpf_fc = 0;
uint8_t filter_q = 0;
uint8_t bitcrush = 0;
uint8_t stretch_change = 0;

// beat tracking (beat = eighth-note)
uint32_t beat_counter = 0;
uint16_t bpm_set = 79;
uint32_t beat_thresh = 21120000;
uint32_t beat_num = 0;
uint32_t beat_num_total = 0;
bool beat_onset = false;
bool beat_led = 0;
bool btn_reset = 0;
bool soft_sync = 0;
bool is_syncing = false;
bool do_sync_play = false;

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
uint8_t syncing_clicks = 0;

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

#if MIDI_IN_ENABLED == 1
// midi handler
Onewiremidi *onewiremidi;
#endif
int8_t midi_button1 = -1;
int8_t midi_button2 = -1;

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

void param_set_bpm(uint16_t bpm, uint16_t &bpm_set_, uint32_t &beat_thresh_,
                   uint8_t &audio_clk_thresh_) {
  if (bpm > 360) {
    return;
  }
  // set default bpm
  bpm_set_ = bpm;
  // double bpm_fudge = ((double)bpm);
  // calibrated
  double bpm_fudge = ((double)bpm) * 1.054234344 - 1.420118655;
  // beat_thresh_ = round(((SAMPLE_RATE * 960.0) << flag_half_time) /
  // bpm_fudge);
  beat_thresh_ = round(((SAMPLE_RATE * 960.0)) / bpm_fudge);
  audio_clk_thresh = round(CLOCK_RATE * BPM_SAMPLED / 250.0 /
                           (SAMPLE_RATE / 1000.0) / bpm_fudge);
#ifdef DEBUG_BPM
  printf("new bpm: %d\n", bpm_set_);
  printf("new bpm fudge: %2.3f\n", bpm_fudge);
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

/*
 * PWM INTERRUPT LOGIC (main audio thread)
 */
void pwm_interrupt_handler() {
  pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));

  if ((!do_sync_play && is_syncing) || do_mute) {
    pwm_set_gpio_level(AUDIO_PIN, 128);
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

  // clocking when to change beats
  beat_counter++;
  if ((!is_syncing && beat_counter >= beat_thresh) || btn_reset || soft_sync) {
#ifdef DEBUG_CLOCK
    if (soft_sync) {
      printf("softsync; beat_counter: %d, beat_thresh: %d\n", beat_counter,
             beat_thresh);
    }
#endif
    soft_sync = false;
    beat_num++;
    beat_num_total++;
    beat_counter = 0;
    beat_onset = true;
    beat_led = 1 - beat_led;
    noise_gate_val = 0;
    if (btn_reset) {
      beat_led = 1;
      beat_num = 0;
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
    if (!fx_retrig) {
#ifdef DEBUG_PWM
      printf("[%d bpm / %d thresh / beat_num: %d] ", bpm_set, beat_thresh,
             beat_num);
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
        audio_clk = audio_clk_thresh - 1;
        phase_retrig = (retrigs[retrig_sel] << flag_half_time) - 1;
      }
    }
  }

  // disable beat interrupts during fx
  if (fx_retrig) {
    beat_onset = false;
  }

  // clocking when to change samples
  audio_clk++;
  if (audio_clk == (audio_clk_thresh + retrig_pitch_change + stretch_change) ||
      beat_onset) {
    audio_clk = 0;
    // beat onset causes next sample
    if (beat_onset && fx_retrig == false) {
      bool do_switch_heads = true;

      if (probability_tunnel > 0) {
        if (randint(0, 255) < probability_tunnel) {
          sample_add = randint(0, NUM_SAMPLES);
        } else {
          sample_add = 0;
        }
      } else {
        sample_add = 0;
      }
      if (sample_set != sample_change) {
        sample_set = sample_change;
      }
      sample = (sample_set + sample_add) % NUM_SAMPLES;
      sample_beats = raw_beats(sample);

      beat_onset = false;
      select_beat++;
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
          noise_gate_thresh_use = SAMPLES_PER_BEAT * randint(800, 1000) / 1000;
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
             retrigs[retrig_sel] << flag_half_time);
#endif
      if (do_switch_heads) {
        phase_head = 1 - phase_head;  // switch heads
        phase_xfade = 1 << HEAD_SHIFT;
      }
      phase_sample[phase_head] =
          select_beat * (SAMPLES_PER_BEAT << flag_half_time);

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
      if (noise_gate_val<10 & noise_gate_fade> 0) {
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

      if (phase_retrig % (retrigs[retrig_sel] << flag_half_time) == 0) {
        retrig_count++;
        if (retrig_filter > 0) {
          retrig_filter--;
        }
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
        } else if (retrig_pitch_down) {
          retrig_pitch_change--;
        }
        if (retrig_count >= retrig_max) {
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
        }
#ifdef DEBUG_PWM
        printf(
            "[retrig %d/%d] select_beat:%d for %d samples, beat_counter: %d, "
            "\n\tphase_sample[phase_head]: %d%%%d==0\n",
            retrig_count, retrig_max, select_beat,
            retrigs[retrig_sel] << flag_half_time, beat_counter,
            phase_sample[phase_head], (retrigs[retrig_sel] << flag_half_time));
#endif
        // setup
        phase_head = 1 - phase_head;  // switch heads
        phase_xfade = 1 << HEAD_SHIFT;
        phase_sample[phase_head] =
            select_beat * (SAMPLES_PER_BEAT << flag_half_time);
        phase_retrig = 0;
      }
    }

    // determine sample
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
  }

  pwm_set_gpio_level(AUDIO_PIN, audio_now);
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
  // reset syncing
  is_syncing = false;
  syncing_clicks = 0;
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
}

#if MIDI_IN_ENABLED == 1
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
uint32_t midi_last_time = 0;
uint32_t midi_delta_sum = 0;
uint32_t midi_delta_count = 0;
#define MIDI_DELTA_COUNT_MAX 32
uint32_t midi_timing_count = 0;
const uint8_t midi_timing_modulus = 24;

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

void midi_start() {
  do_start_everything();
  soft_sync = false;
  btn_reset = false;
  midi_timing_count = 24 * MIDI_RESET_EVERY_BEAT - 1;
}
void midi_continue() {
  do_start_everything();
  soft_sync = false;
  btn_reset = false;
  midi_timing_count = 24 * MIDI_RESET_EVERY_BEAT - 1;
}
void midi_stop() {
  do_stop_everything();
  soft_sync = false;
  btn_reset = false;
  midi_timing_count = 24 * MIDI_RESET_EVERY_BEAT - 1;
}
void midi_timing() {
  midi_timing_count++;
  if (midi_timing_count % (24 * MIDI_RESET_EVERY_BEAT) == 0) {
    btn_reset = true;
#ifdef DEBUG_MIDI
    printf("midi resetting");
#endif
  } else if (midi_timing_count %
                 (midi_timing_modulus / MIDI_CLOCK_MULTIPLIER) ==
             0) {
    soft_sync = true;
  }
  uint32_t now_time = time_us_32();
  if (midi_last_time > 0) {
    midi_delta_sum += now_time - midi_last_time;
    midi_delta_count++;
    if (midi_delta_count == MIDI_DELTA_COUNT_MAX) {
      uint32_t bpm_input =
          (int)round(1250000.0 * MIDI_CLOCK_MULTIPLIER * MIDI_DELTA_COUNT_MAX /
                     (float)(midi_delta_sum));
#ifdef DEBUG_MIDI
      printf("midi bpm\t%d\n", bpm_input);
#endif
      if (bpm_input - 7 != bpm_set) {
        // #ifdef DEBUG_CLOCK
        //         printf("%d, %d\n", clock_sync_ms, bpm_input);
        // #endif
        // REDUCE THE BPM INPUT TO ELIMINATE OVERSTEPPING
        param_set_bpm(bpm_input - 7, bpm_set, beat_thresh, audio_clk_thresh);
      }
      midi_delta_count = 0;
      midi_delta_sum = 0;
    }
  }
  midi_last_time = now_time;
}
#endif

int main(void) {
  stdio_init_all();

  // sleep needed to make sure it can start on battery
  // not sure why
  sleep_ms(100);

  // initialize bpm
  param_set_bpm(BPM_SAMPLED, bpm_set, beat_thresh, audio_clk_thresh);

  // initialize leds
  ledarray.Init();

  // initialize clocking and PWM interrupts
  // overclock at a multiple of sampling rate
  set_sys_clock_khz(CLOCK_RATE, true);
  gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
  int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);
  pwm_clear_irq(audio_pin_slice);
  pwm_set_irq_enabled(audio_pin_slice, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler);
  irq_set_enabled(PWM_IRQ_WRAP, true);
  pwm_config config = pwm_get_default_config();
  /*
   * clock fires 176 Mhz
   * with wrap set to 250
   * at clock division of 1
   * = 704 kHz
   * so 22 kHz sampled audio needs a new sample
   * every 32 pulses
   */
  pwm_config_set_clkdiv(&config, 1.0f);
  pwm_config_set_wrap(&config, 250);
  pwm_init(audio_pin_slice, &config, true);
  pwm_set_gpio_level(AUDIO_PIN, 0);

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
  // // save defaults that aren't defaulted to 0
  save_data[SAVE_VOLUME] = (uint8_t)(2500 >> 8);
  save_data[SAVE_VOLUME + 1] = (uint8_t)2500;
  save_data[SAVE_BPM] = (uint8_t)(165 >> 8);
  save_data[SAVE_BPM + 1] = (uint8_t)165;
  noise_gate_thresh = SAMPLES_PER_BEAT * 4;
  noise_gate_thresh_use = noise_gate_thresh;
  save_data[SAVE_GATE] = (uint8_t)(noise_gate_thresh >> 8);
  save_data[SAVE_GATE + 1] = (uint8_t)noise_gate_thresh;

  // initializer trigger
  output_trigger.Init(TRIGO_PIN, 10, MAIN_LOOP_HZ);

  // initialize control loop variables
  uint32_t clock_ms = 0;
  uint32_t bpm_input = 165;
  uint32_t clock_hits = 0;
  uint32_t clock_sync_ms = 0;
  uint16_t alpha0 = 500;
  uint8_t selector_knob = 0;
  uint16_t ledarray_bar = 0;
  uint32_t ledarray_bar_debounce = 0;
  uint16_t ledarray_sel = 0;
  uint32_t ledarray_sel_debounce = 0;
  uint16_t ledarray_save = 0;
  uint16_t ledarray_load = 0;
  uint32_t ledarray_binary_debounce = 0;
  uint8_t ledarray_binary = 0;

  // debouncing
  uint16_t debounce_sample = 0;
  uint32_t debounce_saving = 0;
  uint32_t debounce_led_save = 0;
  uint8_t debounce_led_sequencer = 0;
  uint8_t debounce_led_load = 0;
  uint8_t clock_pin_last = 0;
  uint8_t last_button_on = NUM_BUTTONS;
  bool has_saved = false;
  bool do_load = false;
  bool first_time = false;
  bool has_loaded = false;
  RunningAverage ra;
  ra.Init(5);

#if MIDI_IN_ENABLED == 1

  // initialize one wire midi
  onewiremidi =
      Onewiremidi_new(pio1, 0, CLOCK_PIN, midi_note_on, midi_note_off,
                      midi_start, midi_continue, midi_stop, midi_timing);
#endif

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
      WS2812::FORMAT_GRB  // Pixel format used by the LED strip
  );
#endif

  // control loop
  while (1) {
    __wfi();  // Wait for Interrupt
    clock_ms++;
    clock_sync_ms++;

#if MIDI_IN_ENABLED == 1
    Onewiremidi_receive(onewiremidi);
#endif
#if WS2812_ENABLED == 1
    if (clock_ms % 200 == 0) {
      // leds
      // ledStrip.fill(WS2812::RGB(input_knob[0].Value() * 255 / 4095,
      //                           input_knob[1].Value() * 255 / 4095,
      //                           input_knob[2].Value() * 255 / 4095));
      // ledStrip.show();

      if (debounce_led_save > 0) {
        debounce_led_save--;
        ledStrip.fill(WS2812::RGB(150, 100, 0));
      } else if (do_mute) {
        ledStrip.fill(WS2812::RGB(255, 0, 50));
      } else if (sequencer.IsRecording()) {
        ledStrip.fill(WS2812::RGB(80, 80, 0));
      } else if (sequencer.IsPlaying() && debounce_led_sequencer > 0) {
        debounce_led_sequencer--;
        ledStrip.fill(WS2812::RGB(0, 80, 0));
      } else if (debounce_led_load > 0) {
        debounce_led_load--;
        ledStrip.fill(WS2812::RGB(0, 150, 50));
      } else if (ledarray_bar_debounce == 1) {
        ledStrip.fill(WS2812::RGB(ledarray_bar * 80 / 1000, 0, 0));
      } else if (ledarray_bar_debounce == 2) {
        ledStrip.fill(WS2812::RGB(0, 0, ledarray_bar * 80 / 1000));
      } else if (ledarray_bar_debounce == 3) {
        // volume knob
        if (distortion > 0) {
          ledStrip.fill(WS2812::RGB(distortion * 3,
                                    2 * (DISTORTION_MAX - distortion), 0));
        } else if (volume_reduce > 0) {
          uint8_t vv = (VOLUME_REDUCE_MAX - volume_reduce) * 4;
          if (vv > 200) {
            vv = 0;
          }
          ledStrip.fill(WS2812::RGB(0, vv, vv));
        } else {
          ledStrip.fill(WS2812::RGB(0, 80, 0));
        }
      } else {
        ledStrip.fill(WS2812::RGB(0, 0, 0));
      }
      ledStrip.show();
    }
#endif

    if (debounce_sample > 0) {
      debounce_sample--;
    }
    // flash works
    if (debounce_saving > 0 && clock_ms > 64000) {
      debounce_saving--;
      if (debounce_saving == 0) {
#ifdef DEBUG_SAVE
        printf("\nsaving:\n");
#endif
        save_data[FLASH_PAGE_SIZE - 1] = 0x01;
        save_data[FLASH_PAGE_SIZE - 2] = 0x02;
        save_data[FLASH_PAGE_SIZE - 3] = 0x03;
        save_data[FLASH_PAGE_SIZE - 4] = 0x04;
        sequencer.Save(save_data);
#ifdef DEBUG_SAVE
        print_buf(save_data, FLASH_PAGE_SIZE);
#endif
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_TARGET_OFFSET, save_data, FLASH_PAGE_SIZE);
        restore_interrupts(ints);
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
      printf("FLASH_TARGET_OFFSET: \t%d\n", FLASH_TARGET_OFFSET);
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
        param_set_bpm(
            (uint16_t)(save_data[SAVE_BPM] << 8) + save_data[SAVE_BPM + 1],
            bpm_set, beat_thresh, audio_clk_thresh);
        // filter_fc = flash_target_contents[SAVE_FILTER];
        sample_change = save_data[SAVE_SAMPLE];
        noise_gate_thresh =
            (uint16_t)(save_data[SAVE_GATE] << 8) + save_data[SAVE_GATE + 1];
        probability_direction = save_data[SAVE_PROB_DIRECTION];
        probability_jump = save_data[SAVE_PROB_JUMP];
        probability_retrig = save_data[SAVE_PROB_RETRIG];
        probability_gate = save_data[SAVE_PROB_GATE];
        probability_tunnel = save_data[SAVE_PROB_TUNNEL];
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
        if (input_button[0].ChangedHigh(true) ||
            input_button[3].ChangedHigh(true) ||
            input_button[4].ChangedHigh(true) ||
            input_button[7].ChangedHigh(true)) {
          printf("changed high!\n");
          // button combo
          if (input_button[0].On() && input_button[3].On() &&
              input_button[4].On() && input_button[7].On()) {
            if (do_mute) {
              do_start_everything();
            } else {
              do_stop_everything();
            }
            printf("switching do mute: %d\n", do_mute);
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
              ledarray_bar_debounce = 0;
              sequencer.SetRecording(false);
              if (first_time) {
                first_time = false;
                break;
              }
            } else if (i == 1) {
              ledarray_bar =
                  input_knob[i].Value() * 1000 / input_knob[i].ValueMax();
              ledarray_bar_debounce = 1;
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
                  if (debounce_sample == 0) {
                    sample_change = input_knob[i].Value() * NUM_SAMPLES /
                                    input_knob[i].ValueMax();
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
                    noise_gate_thresh = SAMPLES_PER_BEAT * 4;
                  } else {
                    noise_gate_thresh = SAMPLES_PER_BEAT *
                                        (input_knob[i].Value() * 1000 /
                                         input_knob[i].ValueMax()) /
                                        1000;
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
                  ledarray_bar_debounce = 3;  // special volume knob
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
              ledarray_bar =
                  input_knob[i].Value() * 1000 / input_knob[i].ValueMax();
              ledarray_bar_debounce = 2;
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
                  if (input_knob[i].Value() < 100) {
                    stretch_change = 0;
                  } else {
                    stretch_change = (input_knob[i].Value() * audio_clk_thresh *
                                      2 / input_knob[i].ValueMax());
                  }
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
                  if (input_knob[i].Value() > 4000 && !has_loaded) {
                    do_load = true;
                    debounce_led_load = 255;
                    has_loaded = true;
                  } else {
                    has_loaded = false;
                  }

                  break;
                case 7:
                  // tempo
                  if (clock_sync_ms > 60000) {
                    uint16_t bpm_set_new = round((double)input_knob[i].Value() *
                                                 255.0 / 4095 / 5) *
                                               5 +
                                           50;
                    ledarray_binary_debounce = 48000;
                    ledarray_binary = bpm_set_new - 50;
                    if (bpm_set_new > 360) {
                      bpm_set_new = 360;
                    }
                    if (bpm_set_new != bpm_set) {
#ifdef DEBUG_KNOB
                      printf("%d: %d; \n", i, input_knob[i].Value());
#endif
                      save_data[SAVE_BPM] = (uint8_t)(bpm_set_new >> 8);
                      save_data[SAVE_BPM + 1] = (uint8_t)bpm_set_new;

                      param_set_bpm(bpm_set_new, bpm_set, beat_thresh,
                                    audio_clk_thresh);
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

#if MIDI_IN_ENABLED == 0
    // trigger in
    uint8_t clock_pin = 1 - gpio_get(CLOCK_PIN);
    // code to verify polarity -KEEP
    // if (clock_pin == 1 && clock_pin_last == 0) {
    //   printf("[%d] on\n", clock_sync_ms);
    //   clock_sync_ms = 0;
    // }
    // if (clock_pin == 0 && clock_pin_last == 1) {
    //   printf("[%d] off\n", clock_sync_ms);
    //   clock_sync_ms = 0;
    // }
    if (clock_pin == 1 && clock_pin_last == 0) {
#ifdef DEBUG_CALIBRATE_PO
      // this is used for calibration
      printf("%d\n", clock_sync_ms);
#endif
      if (syncing_clicks < 10) {
        syncing_clicks++;
        is_syncing = true;
      }
      do_sync_play = true;
      // this is from a calibration
      if (clock_sync_ms > 10000) {
        // out of range of the bpm, but will use to reset system
        btn_reset = true;
        clock_hits = 0;
      } else {
        bpm_input = 512508000 / ((935 * clock_sync_ms + 31900));
        ra.Update(bpm_input);
        bpm_input = ra.Value();
        if (bpm_input != bpm_set) {
#ifdef DEBUG_CLOCK
          printf("%d, %d\n", clock_sync_ms, bpm_input);
#endif
          // REDUCE THE BPM INPUT TO ELIMINATE OVERSTEPPING
          param_set_bpm(bpm_input - 7, bpm_set, beat_thresh, audio_clk_thresh);
        }
        clock_hits++;
        soft_sync = true;  // TEST
        if (clock_hits % 16 == 0) {
          soft_sync = true;
        }
      }
      clock_sync_ms = 0;
    }
    if (is_syncing && clock_sync_ms > 10000) {
      do_sync_play = false;
    }
    clock_pin_last = clock_pin;
#endif

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
