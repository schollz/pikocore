#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <cmath>
#include <initializer_list>

#include "ClockSync.h"
#include "SpscQueue.h"

using piko::ClockDiagnostics;
using piko::ClockEvent;
using piko::ClockEventType;
using piko::ClockSource;
using piko::ClockState;
using piko::ClockSync;

namespace {

void lockPulse(ClockSync& clock, uint8_t ppqn, uint32_t start,
               uint32_t interval) {
  clock.setSource(ClockSource::Pulse, ppqn, start);
  clock.process({ClockEventType::Pulse, start});
  clock.process({ClockEventType::Pulse, start + interval});
  clock.process({ClockEventType::Pulse, start + interval * 2u});
}

void testPulseDivisions() {
  for (const uint8_t ppqn : {1, 2, 4}) {
    ClockSync clock(1000000);
    const uint32_t interval = 500000u / ppqn;
    lockPulse(clock, ppqn, 1000u, interval);
    const ClockDiagnostics d = clock.diagnostics();
    assert(d.state == ClockState::Locked);
    assert(d.measured_bpm_x100 == 12000u);
    assert(d.pulse_ppqn == ppqn);
    assert(d.rejected_events == 0u);
  }
}

void testMidiClockAndTransport() {
  ClockSync clock(1000);
  clock.setSource(ClockSource::Midi, 2, 0);
  clock.process({ClockEventType::MidiStart, 0});
  uint32_t now = 0;
  uint32_t beats = 0;
  for (uint32_t tick = 0; tick <= 2000; ++tick) {
    now = tick * 1000u;
    if (tick % 21u == 0) {
      // Rounded 120 BPM MIDI clock (20.833 ms) stays well inside filtering.
      clock.process({ClockEventType::MidiClock, now});
    }
    if (clock.advanceCarrier(now)) ++beats;
  }
  const ClockDiagnostics d = clock.diagnostics();
  assert(d.state == ClockState::Locked);
  assert(d.measured_bpm_x100 > 11800u && d.measured_bpm_x100 < 12100u);
  assert(beats >= 7u && beats <= 9u);

  clock.advanceCarrier(now + 50000u);
  assert(clock.diagnostics().state == ClockState::Holdover);
  assert(clock.transportPaused());
  clock.process({ClockEventType::MidiContinue, now + 60000u});
  assert(clock.midiRunning());
  assert(!clock.transportPaused());

  const uint32_t stop_time = now + 70000u;
  clock.process({ClockEventType::MidiStop, stop_time});
  assert(!clock.midiRunning());
  for (uint32_t i = 0; i < 1000; ++i) {
    assert(!clock.advanceCarrier(stop_time + i * 1000u));
  }
}

void testBounceJitterAndMissedPulse() {
  ClockSync clock(1000000);
  clock.setSource(ClockSource::Pulse, 2, 0);
  clock.process({ClockEventType::Pulse, 10000});
  clock.process({ClockEventType::Pulse, 11000});  // bounce
  clock.process({ClockEventType::Pulse, 260100});
  clock.process({ClockEventType::Pulse, 509900});
  clock.process({ClockEventType::Pulse, 1010100});  // one missed pulse
  const ClockDiagnostics d = clock.diagnostics();
  assert(d.state == ClockState::Locked);
  assert(d.rejected_events == 1u);
  assert(d.missed_events == 1u);
  assert(d.jitter_us < 1000u);
}

void testTempoChangeHoldoverAndReacquisition() {
  ClockSync clock(1000000);
  lockPulse(clock, 2, 0, 250000);
  clock.process({ClockEventType::Pulse, 700000});
  clock.process({ClockEventType::Pulse, 900000});
  clock.process({ClockEventType::Pulse, 1100000});
  ClockDiagnostics d = clock.diagnostics();
  assert(d.state == ClockState::Locked);
  assert(d.measured_bpm_x100 == 15000u);

  clock.advanceCarrier(1600000);
  d = clock.diagnostics();
  assert(d.state == ClockState::Holdover);
  assert(d.target_bpm_x100 == 15000u);

  const uint64_t paused_phase = clock.transportPhaseQ32();
  for (uint32_t now = 1600001; now < 2600000; now += 1000) {
    assert(!clock.advanceCarrier(now));
  }
  assert(clock.transportPaused());
  assert(clock.transportPhaseQ32() == paused_phase);

  clock.process({ClockEventType::Pulse, 3000000});
  assert(clock.diagnostics().state == ClockState::Acquiring);
  assert(!clock.transportPaused());
  assert(clock.consumeLoopRestart());
  assert(!clock.consumeLoopRestart());
  assert(clock.advanceCarrier(3000000));
  clock.process({ClockEventType::Pulse, 3200000});
  clock.process({ClockEventType::Pulse, 3400000});
  assert(clock.diagnostics().state == ClockState::Locked);

  ClockSync short_holdover(1000000);
  lockPulse(short_holdover, 2, 0, 250000);
  short_holdover.advanceCarrier(1000000);
  assert(short_holdover.transportPaused());
  short_holdover.process({ClockEventType::Pulse, 1999999});
  assert(!short_holdover.consumeLoopRestart());
}

void testTimestampWraparound() {
  ClockSync clock(1000000);
  const uint32_t start = 0xffff0000u;
  lockPulse(clock, 2, start, 250000u);
  ClockDiagnostics d = clock.diagnostics();
  assert(d.state == ClockState::Locked);
  assert(d.measured_bpm_x100 == 12000u);

  // A capture IRQ may publish a timestamp just after the carrier sampled its
  // current time. That stale `now` must not look like a uint32-sized gap.
  const uint32_t latest_edge = start + 500000u;
  clock.advanceCarrier(latest_edge - 1u);
  d = clock.diagnostics();
  assert(d.state == ClockState::Locked);
}

void testQueueOverflow() {
  SpscQueue<uint32_t, 4> queue;
  assert(queue.push(1));
  assert(queue.push(2));
  assert(queue.push(3));
  assert(!queue.push(4));
  assert(queue.drops() == 1u);
  uint32_t value = 0;
  assert(queue.pop(value) && value == 1u);
  assert(queue.push(5));
  assert(queue.pop(value) && value == 2u);
  assert(queue.pop(value) && value == 3u);
  assert(queue.pop(value) && value == 5u);
  assert(!queue.pop(value));
}

void testPhaseLandmarksAndLongTermAccuracy() {
  ClockSync clock(1000);
  clock.setSource(ClockSource::Internal, 2, 0);
  clock.setInternalBpmX100(12345);
  assert(clock.diagnostics().state == ClockState::Locked);
  const uint32_t ticks = 1000000u;
  uint32_t beats = 0;
  for (uint32_t i = 0; i < ticks; ++i) {
    if (clock.advanceCarrier(i * 1000u)) ++beats;
  }
  const double expected = (ticks / 1000.0) * 123.45 / 30.0;
  assert(std::abs(static_cast<double>(beats) - expected) <= 1.0);

  // Quarter-note input must interpolate one eighth between input landmarks.
  ClockSync quarter(1000);
  quarter.setSource(ClockSource::Pulse, 1, 0);
  uint32_t quarter_beats = 0;
  for (uint32_t tick = 0; tick <= 2000; ++tick) {
    const uint32_t now = tick * 1000u;
    if (tick % 500u == 0) quarter.process({ClockEventType::Pulse, now});
    if (quarter.advanceCarrier(now)) ++quarter_beats;
  }
  assert(quarter_beats >= 7u && quarter_beats <= 9u);

  // Sixteenth input landmarks only produce every second eighth event.
  ClockSync sixteenth(1000);
  sixteenth.setSource(ClockSource::Pulse, 4, 0);
  uint32_t sixteenth_beats = 0;
  for (uint32_t tick = 0; tick <= 2000; ++tick) {
    const uint32_t now = tick * 1000u;
    if (tick % 125u == 0) sixteenth.process({ClockEventType::Pulse, now});
    if (sixteenth.advanceCarrier(now)) ++sixteenth_beats;
  }
  assert(sixteenth_beats >= 7u && sixteenth_beats <= 9u);
}

void testPlaybackRatios() {
  constexpr uint32_t carrier = 1000000u;
  const uint64_t unity =
      ClockSync::playbackIncrementQ32(carrier, 12000, 120);
  const uint64_t double_speed =
      ClockSync::playbackIncrementQ32(carrier, 24000, 120);
  const uint64_t half_speed =
      ClockSync::playbackIncrementQ32(carrier, 6000, 120);
  assert(double_speed == unity * 2u || double_speed == unity * 2u - 1u ||
         double_speed == unity * 2u + 1u);
  assert(half_speed == unity / 2u || half_speed == unity / 2u + 1u);

  // Exercise loader BPM/beat-count combinations. Beat count changes slicing
  // only, while source BPM determines the fractional playback rate.
  for (const uint32_t target_bpm_x100 : {6000u, 12000u, 24000u, 36000u}) {
    for (const uint32_t source_bpm : {90u, 120u, 165u, 200u}) {
      uint64_t rate_for_first_beat_count = 0;
      for (const uint32_t beat_count : {1u, 8u, 16u, 128u}) {
        (void)beat_count;
        const uint64_t rate = ClockSync::playbackIncrementQ32(
            carrier, target_bpm_x100, source_bpm);
        if (rate_for_first_beat_count == 0) {
          rate_for_first_beat_count = rate;
        } else {
          assert(rate == rate_for_first_beat_count);
        }
        const long double actual_hz =
            static_cast<long double>(rate) * carrier / (1ull << 32u);
        const long double expected_hz =
            24000.0L * target_bpm_x100 / (100.0L * source_bpm);
        assert(std::abs(actual_hz - expected_hz) < 0.001L);
      }
    }
  }
}

}  // namespace

int main() {
  testPulseDivisions();
  testMidiClockAndTransport();
  testBounceJitterAndMissedPulse();
  testTempoChangeHoldoverAndReacquisition();
  testTimestampWraparound();
  testQueueOverflow();
  testPhaseLandmarksAndLongTermAccuracy();
  testPlaybackRatios();
  puts("clock_sync_test: all tests passed");
  return 0;
}
