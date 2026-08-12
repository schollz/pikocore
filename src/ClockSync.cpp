#include "ClockSync.h"

#include <limits.h>

namespace piko {
namespace {

constexpr uint64_t kPhaseOne = 1ull << 32u;
constexpr uint32_t kMinEdgeSpacingUs = 2000u;
constexpr uint32_t kMinBpmX100 = 3000u;
constexpr uint32_t kMaxBpmX100 = 36000u;
constexpr uint32_t kLongHoldoverRestartUs = 1000000u;

uint32_t abs32(int32_t value) {
  if (value >= 0) return static_cast<uint32_t>(value);
  if (value == INT32_MIN) return 0x80000000u;
  return static_cast<uint32_t>(-value);
}

uint32_t roundedDivide(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  return static_cast<uint32_t>((numerator + denominator / 2u) / denominator);
}

}  // namespace

ClockSync::ClockSync(uint32_t carrier_hz) { setCarrierHz(carrier_hz); }

bool ClockSync::validPulsePpqn(uint8_t ppqn) {
  return ppqn == 1 || ppqn == 2 || ppqn == 4;
}

void ClockSync::setCarrierHz(uint32_t carrier_hz) {
  carrier_hz_ = carrier_hz == 0 ? 1 : carrier_hz;
  updateTransportIncrement();
}

void ClockSync::setInternalBpmX100(uint32_t bpm_x100) {
  if (bpm_x100 < kMinBpmX100 || bpm_x100 > kMaxBpmX100) return;
  internal_bpm_x100_ = bpm_x100;
  if (source_ == ClockSource::Internal || !have_edge_) {
    target_bpm_x100_ = bpm_x100;
    if (source_ == ClockSource::Internal) measured_bpm_x100_ = bpm_x100;
    updateTransportIncrement();
  }
}

void ClockSync::setSource(ClockSource source, uint8_t pulse_ppqn,
                          uint32_t now_us) {
  source_ = source;
  if (validPulsePpqn(pulse_ppqn)) pulse_ppqn_ = pulse_ppqn;
  midi_running_ = true;
  loop_restart_pending_ = false;
  resetAcquisition(now_us);
  target_bpm_x100_ = internal_bpm_x100_;
  measured_bpm_x100_ = source == ClockSource::Internal ? internal_bpm_x100_ : 0;
  state_ = source == ClockSource::Internal ? ClockState::Locked
                                           : ClockState::Unlocked;
  updateTransportIncrement();
}

void ClockSync::setPulsePpqn(uint8_t pulse_ppqn, uint32_t now_us) {
  if (!validPulsePpqn(pulse_ppqn)) return;
  pulse_ppqn_ = pulse_ppqn;
  loop_restart_pending_ = false;
  if (source_ == ClockSource::Pulse) {
    resetAcquisition(now_us);
    state_ = ClockState::Unlocked;
  }
}

void ClockSync::resetAcquisition(uint32_t now_us) {
  have_edge_ = false;
  first_edge_us_ = now_us;
  last_edge_us_ = now_us;
  pulse_ordinal_ = 0;
  consecutive_valid_intervals_ = 0;
  interval_history_count_ = 0;
  interval_history_pos_ = 0;
  filtered_quarter_us_ = 0;
  tempo_candidate_us_ = 0;
  have_tempo_candidate_ = false;
  jitter_us_ = 0;
  phase_error_us_ = 0;
  slew_increment_q32_ = 0;
  slew_ticks_remaining_ = 0;
}

void ClockSync::process(const ClockEvent& event) {
  switch (event.type) {
    case ClockEventType::Pulse:
      if (source_ == ClockSource::Pulse) {
        processLandmark(event.timestamp_us, pulse_ppqn_);
      }
      break;
    case ClockEventType::MidiClock:
      if (source_ == ClockSource::Midi && midi_running_) {
        processLandmark(event.timestamp_us, 24);
      }
      break;
    case ClockEventType::MidiStart:
    case ClockEventType::MidiContinue:
      if (source_ == ClockSource::Midi) {
        if (state_ == ClockState::Holdover &&
            event.timestamp_us - holdover_started_us_ >=
                kLongHoldoverRestartUs) {
          loop_restart_pending_ = true;
        }
        midi_running_ = true;
        resetAcquisition(event.timestamp_us);
        state_ = ClockState::Unlocked;
      }
      break;
    case ClockEventType::MidiStop:
      if (source_ == ClockSource::Midi) {
        midi_running_ = false;
        state_ = ClockState::Unlocked;
        pending_beats_ = 0;
        loop_restart_pending_ = false;
      }
      break;
  }
}

void ClockSync::processLandmark(uint32_t timestamp_us, uint8_t ppqn) {
  bool returning_from_holdover = false;
  if (state_ == ClockState::Holdover) {
    if (timestamp_us - holdover_started_us_ >= kLongHoldoverRestartUs) {
      loop_restart_pending_ = true;
    }
    // The first returning landmark re-anchors phase. Tempo is deliberately
    // retained until fresh intervals establish the returning clock.
    have_edge_ = false;
    consecutive_valid_intervals_ = 0;
    interval_history_count_ = 0;
    interval_history_pos_ = 0;
    have_tempo_candidate_ = false;
    returning_from_holdover = true;
  }
  if (!have_edge_) {
    acceptFirstLandmark(timestamp_us);
    alignPhase(timestamp_us, ppqn, !returning_from_holdover);
    return;
  }

  const uint32_t elapsed = timestamp_us - last_edge_us_;
  if (elapsed < kMinEdgeSpacingUs) {
    ++rejected_events_;
    return;
  }

  uint8_t multiplier = 1;
  uint32_t normalized = elapsed;
  const uint32_t expected = expectedPulseUs(ppqn);
  bool expected_match = expected == 0 || intervalAgrees(elapsed, expected, 15);
  if (!expected_match && expected != 0) {
    for (uint8_t candidate = 2; candidate <= 4; ++candidate) {
      if (intervalAgrees(elapsed, expected * candidate, 15)) {
        multiplier = candidate;
        normalized = elapsed / candidate;
        expected_match = true;
        break;
      }
    }
  }

  if (!intervalBpmValid(normalized, ppqn)) {
    ++rejected_events_;
    have_tempo_candidate_ = false;
    return;
  }

  if (!expected_match && expected != 0) {
    if (have_tempo_candidate_ &&
        intervalAgrees(normalized, tempo_candidate_us_, 5)) {
      // Two agreeing off-tempo intervals are a real change. The first one was
      // retained as a candidate; the second starts a clean acquisition.
      interval_history_count_ = 0;
      interval_history_pos_ = 0;
      filtered_quarter_us_ = 0;
      consecutive_valid_intervals_ = 1;
      state_ = ClockState::Acquiring;
      updateFilteredInterval((normalized + tempo_candidate_us_) / 2u, ppqn,
                             true);
      have_tempo_candidate_ = false;
      ++accepted_events_;
      ++pulse_ordinal_;
      last_edge_us_ = timestamp_us;
      alignPhase(timestamp_us, ppqn, false);
      return;
    }
    tempo_candidate_us_ = normalized;
    have_tempo_candidate_ = true;
    // Retain the candidate as the reference for the next interval. It is not
    // yet allowed to alter tempo or phase.
    last_edge_us_ = timestamp_us;
    ++pulse_ordinal_;
    ++rejected_events_;
    return;
  }

  have_tempo_candidate_ = false;
  acceptInterval(timestamp_us, normalized, multiplier, ppqn);
}

void ClockSync::acceptFirstLandmark(uint32_t timestamp_us) {
  have_edge_ = true;
  first_edge_us_ = timestamp_us;
  last_edge_us_ = timestamp_us;
  pulse_ordinal_ = 0;
  consecutive_valid_intervals_ = 0;
  state_ = ClockState::Acquiring;
  ++accepted_events_;
}

void ClockSync::acceptInterval(uint32_t timestamp_us, uint32_t interval_us,
                               uint8_t multiplier, uint8_t ppqn) {
  ++accepted_events_;
  if (multiplier > 1) missed_events_ += multiplier - 1u;
  pulse_ordinal_ += multiplier;
  last_edge_us_ = timestamp_us;
  updateFilteredInterval(interval_us, ppqn, false);
  if (consecutive_valid_intervals_ < 255) ++consecutive_valid_intervals_;
  state_ = consecutive_valid_intervals_ >= 2 ? ClockState::Locked
                                             : ClockState::Acquiring;
  alignPhase(timestamp_us, ppqn, false);
}

void ClockSync::updateFilteredInterval(uint32_t interval_us, uint8_t ppqn,
                                       bool reset_filter) {
  const uint32_t quarter_us = interval_us * ppqn;
  interval_history_[interval_history_pos_] = quarter_us;
  interval_history_pos_ = (interval_history_pos_ + 1u) % 5u;
  if (interval_history_count_ < 5) ++interval_history_count_;
  const uint32_t median = medianInterval();
  if (reset_filter || filtered_quarter_us_ == 0) {
    filtered_quarter_us_ = median;
  } else {
    const int32_t delta = static_cast<int32_t>(median - filtered_quarter_us_);
    filtered_quarter_us_ = static_cast<uint32_t>(
        static_cast<int32_t>(filtered_quarter_us_) + delta / 4);
  }
  const uint32_t deviation = interval_us > expectedPulseUs(ppqn)
                                 ? interval_us - expectedPulseUs(ppqn)
                                 : expectedPulseUs(ppqn) - interval_us;
  jitter_us_ = static_cast<uint32_t>(
      static_cast<int32_t>(jitter_us_) +
      (static_cast<int32_t>(deviation) - static_cast<int32_t>(jitter_us_)) /
          4);
  measured_bpm_x100_ =
      roundedDivide(6000000000ull, filtered_quarter_us_);
  target_bpm_x100_ = measured_bpm_x100_;
  updateTransportIncrement();
}

uint32_t ClockSync::medianInterval() const {
  if (interval_history_count_ == 0) return 0;
  uint32_t values[5]{};
  for (uint8_t i = 0; i < interval_history_count_; ++i) {
    values[i] = interval_history_[i];
  }
  for (uint8_t i = 1; i < interval_history_count_; ++i) {
    const uint32_t value = values[i];
    uint8_t j = i;
    while (j > 0 && values[j - 1] > value) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
  return values[interval_history_count_ / 2u];
}

void ClockSync::alignPhase(uint32_t, uint8_t ppqn, bool first) {
  const uint32_t pulses_per_eighth = ppqn / 2u;
  uint32_t desired_phase = 0;
  bool eighth_landmark = true;
  if (pulses_per_eighth > 0) {
    const uint32_t within = pulse_ordinal_ % pulses_per_eighth;
    desired_phase = static_cast<uint32_t>(
        (static_cast<uint64_t>(within) * kPhaseOne) / pulses_per_eighth);
    eighth_landmark = within == 0;
  }

  const int32_t phase_error =
      static_cast<int32_t>(static_cast<uint32_t>(transport_phase_q32_) -
                           desired_phase);
  phase_error_us_ = static_cast<int32_t>(
      (static_cast<int64_t>(phase_error) * eighthPeriodUs()) /
      static_cast<int64_t>(kPhaseOne));
  const uint32_t absolute_error = abs32(phase_error_us_);
  if (absolute_error > max_phase_error_us_) max_phase_error_us_ = absolute_error;

  const uint32_t expected = expectedPulseUs(ppqn);
  const bool snap = first || expected == 0 || absolute_error > expected / 10u;
  if (eighth_landmark) {
    const uint32_t quarter_beat_ticks =
        static_cast<uint32_t>((static_cast<uint64_t>(carrier_hz_) *
                               eighthPeriodUs()) /
                              4000000ull);
    if (first || carriers_since_beat_ > quarter_beat_ticks) {
      ++pending_beats_;
      suppress_next_wrap_ = !snap;
    }
    carriers_since_beat_ = 0;
  }
  if (snap) {
    transport_phase_q32_ = desired_phase;
    slew_increment_q32_ = 0;
    slew_ticks_remaining_ = 0;
    return;
  }

  const uint64_t slew_us = static_cast<uint64_t>(expected) * 2u;
  const uint32_t ticks = static_cast<uint32_t>(
      (slew_us * carrier_hz_ + 999999u) / 1000000u);
  if (ticks > 0) {
    slew_ticks_remaining_ = ticks;
    slew_increment_q32_ = -static_cast<int64_t>(phase_error) /
                          static_cast<int64_t>(ticks);
  }
}

void ClockSync::updateTransportIncrement() {
  const uint64_t numerator =
      static_cast<uint64_t>(target_bpm_x100_) * 2u * kPhaseOne;
  const uint64_t denominator =
      static_cast<uint64_t>(60u * 100u) * carrier_hz_;
  transport_increment_q32_ =
      denominator == 0 ? 1 : (numerator + denominator / 2u) / denominator;
  if (transport_increment_q32_ == 0) transport_increment_q32_ = 1;
}

bool ClockSync::advanceCarrier(uint32_t now_us) {
  updateHoldover(now_us);
  if (source_ == ClockSource::Midi && !midi_running_) return false;
  if (state_ == ClockState::Holdover) return false;
  if (pending_beats_ > 0) {
    --pending_beats_;
    return true;
  }
  if (source_ != ClockSource::Internal && have_edge_ &&
      filtered_quarter_us_ == 0) {
    ++carriers_since_beat_;
    return false;
  }

  int64_t increment = static_cast<int64_t>(transport_increment_q32_);
  if (slew_ticks_remaining_ > 0) {
    increment += slew_increment_q32_;
    --slew_ticks_remaining_;
    if (slew_ticks_remaining_ == 0) slew_increment_q32_ = 0;
  }
  if (increment < 1) increment = 1;
  transport_phase_q32_ += static_cast<uint64_t>(increment);
  ++carriers_since_beat_;
  if (transport_phase_q32_ >= kPhaseOne) {
    transport_phase_q32_ -= kPhaseOne;
    if (suppress_next_wrap_) {
      suppress_next_wrap_ = false;
      return false;
    }
    carriers_since_beat_ = 0;
    return true;
  }
  return false;
}

void ClockSync::updateHoldover(uint32_t now_us) {
  if ((source_ != ClockSource::Pulse && source_ != ClockSource::Midi) ||
      !have_edge_ || state_ == ClockState::Unlocked ||
      state_ == ClockState::Holdover) {
    return;
  }
  const uint8_t ppqn = source_ == ClockSource::Midi ? 24 : pulse_ppqn_;
  const uint32_t expected = expectedPulseUs(ppqn);
  const uint32_t elapsed = now_us - last_edge_us_;
  // A higher-priority capture ISR can publish an edge between a caller's time
  // sample and queue drain. Treat an apparent gap over half the uint32 range as
  // a slightly stale `now`, while preserving ordinary time_us_32 wraparound.
  if (expected > 0 && elapsed < 0x80000000u && elapsed >= expected * 2u) {
    state_ = ClockState::Holdover;
    holdover_started_us_ = now_us;
  }
}

bool ClockSync::consumeLoopRestart() {
  const bool restart = loop_restart_pending_;
  loop_restart_pending_ = false;
  return restart;
}

uint32_t ClockSync::expectedPulseUs(uint8_t ppqn) const {
  if (ppqn == 0 || filtered_quarter_us_ == 0) return 0;
  return filtered_quarter_us_ / ppqn;
}

uint32_t ClockSync::eighthPeriodUs() const {
  if (target_bpm_x100_ == 0) return 0;
  return roundedDivide(3000000000ull, target_bpm_x100_);
}

bool ClockSync::intervalBpmValid(uint32_t interval_us, uint8_t ppqn) const {
  if (interval_us == 0 || ppqn == 0) return false;
  const uint32_t bpm_x100 =
      roundedDivide(6000000000ull, static_cast<uint64_t>(interval_us) * ppqn);
  return bpm_x100 >= kMinBpmX100 && bpm_x100 <= kMaxBpmX100;
}

bool ClockSync::intervalAgrees(uint32_t a, uint32_t b,
                               uint32_t percent) const {
  if (a == 0 || b == 0) return false;
  const uint32_t delta = a > b ? a - b : b - a;
  return static_cast<uint64_t>(delta) * 100u <=
         static_cast<uint64_t>(b) * percent;
}

ClockDiagnostics ClockSync::diagnostics() const {
  return {source_,
          state_,
          measured_bpm_x100_,
          target_bpm_x100_,
          jitter_us_,
          phase_error_us_,
          max_phase_error_us_,
          last_edge_us_,
          pulse_ppqn_,
          accepted_events_,
          rejected_events_,
          missed_events_};
}

uint64_t ClockSync::playbackIncrementQ32(uint32_t carrier_hz,
                                         uint32_t target_bpm_x100,
                                         uint32_t source_bpm) {
  if (carrier_hz == 0 || source_bpm == 0) return 0;
  const uint64_t numerator = 24000ull * target_bpm_x100 * kPhaseOne;
  const uint64_t denominator =
      static_cast<uint64_t>(source_bpm) * 100u * carrier_hz;
  return (numerator + denominator / 2u) / denominator;
}

const char* clockSourceName(ClockSource source) {
  switch (source) {
    case ClockSource::Internal:
      return "INTERNAL";
    case ClockSource::Pulse:
      return "PULSE";
    case ClockSource::Midi:
      return "MIDI";
  }
  return "INTERNAL";
}

const char* clockStateName(ClockState state) {
  switch (state) {
    case ClockState::Unlocked:
      return "UNLOCKED";
    case ClockState::Acquiring:
      return "ACQUIRING";
    case ClockState::Locked:
      return "LOCKED";
    case ClockState::Holdover:
      return "HOLDOVER";
  }
  return "UNLOCKED";
}

}  // namespace piko
