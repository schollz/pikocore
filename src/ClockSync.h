#pragma once

#include <stddef.h>
#include <stdint.h>

namespace piko {

enum class ClockSource : uint8_t { Internal = 0, Pulse = 1, Midi = 2 };
enum class ClockState : uint8_t {
  Unlocked = 0,
  Acquiring = 1,
  Locked = 2,
  Holdover = 3,
};
enum class ClockEventType : uint8_t {
  Pulse = 0,
  MidiClock = 1,
  MidiStart = 2,
  MidiContinue = 3,
  MidiStop = 4,
};

struct ClockEvent {
  ClockEventType type;
  uint32_t timestamp_us;
};

struct ClockDiagnostics {
  ClockSource source;
  ClockState state;
  uint32_t measured_bpm_x100;
  uint32_t target_bpm_x100;
  uint32_t jitter_us;
  int32_t phase_error_us;
  uint32_t max_phase_error_us;
  uint32_t last_edge_us;
  uint8_t pulse_ppqn;
  uint32_t accepted_events;
  uint32_t rejected_events;
  uint32_t missed_events;
};

class ClockSync {
 public:
  explicit ClockSync(uint32_t carrier_hz = 1);

  void setCarrierHz(uint32_t carrier_hz);
  void setInternalBpmX100(uint32_t bpm_x100);
  void setSource(ClockSource source, uint8_t pulse_ppqn, uint32_t now_us);
  void setPulsePpqn(uint8_t pulse_ppqn, uint32_t now_us);
  void process(const ClockEvent& event);

  // Called once per PWM carrier IRQ. Returns true for one unified eighth-note
  // event. External-clock holdover pauses until a pulse or MIDI transport start
  // arrives; after one second it also requests a loop restart on that event.
  // Callers may continue invoking this while sample-bank audio is muted.
  bool advanceCarrier(uint32_t now_us);

  ClockDiagnostics diagnostics() const;
  uint64_t transportPhaseQ32() const { return transport_phase_q32_; }
  uint32_t carrierHz() const { return carrier_hz_; }
  uint32_t targetBpmX100() const { return target_bpm_x100_; }
  bool midiRunning() const { return midi_running_; }
  bool transportPaused() const { return state_ == ClockState::Holdover; }
  bool consumeLoopRestart();

  static bool validPulsePpqn(uint8_t ppqn);
  static uint64_t playbackIncrementQ32(uint32_t carrier_hz,
                                       uint32_t target_bpm_x100,
                                       uint32_t source_bpm);

 private:
  void resetAcquisition(uint32_t now_us);
  void processLandmark(uint32_t timestamp_us, uint8_t ppqn);
  void acceptFirstLandmark(uint32_t timestamp_us);
  void acceptInterval(uint32_t timestamp_us, uint32_t interval_us,
                      uint8_t multiplier, uint8_t ppqn);
  void alignPhase(uint32_t timestamp_us, uint8_t ppqn, bool first);
  void updateFilteredInterval(uint32_t interval_us, uint8_t ppqn,
                              bool reset_filter);
  void updateTransportIncrement();
  void updateHoldover(uint32_t now_us);
  uint32_t expectedPulseUs(uint8_t ppqn) const;
  uint32_t eighthPeriodUs() const;
  uint32_t medianInterval() const;
  bool intervalBpmValid(uint32_t interval_us, uint8_t ppqn) const;
  bool intervalAgrees(uint32_t a, uint32_t b, uint32_t percent) const;

  uint32_t carrier_hz_ = 1;
  ClockSource source_ = ClockSource::Internal;
  ClockState state_ = ClockState::Unlocked;
  uint8_t pulse_ppqn_ = 2;
  bool midi_running_ = true;

  uint32_t internal_bpm_x100_ = 16500;
  uint32_t measured_bpm_x100_ = 0;
  uint32_t target_bpm_x100_ = 16500;
  uint32_t filtered_quarter_us_ = 0;
  uint64_t transport_increment_q32_ = 0;
  uint64_t transport_phase_q32_ = 0;
  int64_t slew_increment_q32_ = 0;
  uint32_t slew_ticks_remaining_ = 0;
  uint32_t pending_beats_ = 0;
  uint32_t carriers_since_beat_ = 0;
  bool suppress_next_wrap_ = false;

  bool have_edge_ = false;
  uint32_t first_edge_us_ = 0;
  uint32_t last_edge_us_ = 0;
  uint32_t holdover_started_us_ = 0;
  uint32_t pulse_ordinal_ = 0;
  uint8_t consecutive_valid_intervals_ = 0;
  uint32_t interval_history_[5]{};
  uint8_t interval_history_count_ = 0;
  uint8_t interval_history_pos_ = 0;
  uint32_t tempo_candidate_us_ = 0;
  bool have_tempo_candidate_ = false;

  uint32_t jitter_us_ = 0;
  int32_t phase_error_us_ = 0;
  uint32_t max_phase_error_us_ = 0;
  uint32_t accepted_events_ = 0;
  uint32_t rejected_events_ = 0;
  uint32_t missed_events_ = 0;
  bool loop_restart_pending_ = false;
};

const char* clockSourceName(ClockSource source);
const char* clockStateName(ClockState state);

}  // namespace piko
