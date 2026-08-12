#include "PikoRuntime.h"

#include "SpscQueue.h"
#include "pico/stdlib.h"
#include "tusb.h"

namespace {

struct UsbMidiEvent {
  uint8_t note;
  uint8_t velocity;
};

SpscQueue<PikoRequest, 8> request_queue;
SpscQueue<UsbMidiEvent, 32> usb_midi_queue;
volatile uint32_t next_request_id = 1;
volatile uint32_t acknowledged_request_id = 0;
volatile bool acknowledged_request_ok = false;
volatile bool usb_midi_ready = false;

volatile uint32_t snapshot_sequence = 0;
PikoClockSnapshot clock_snapshot{};
int16_t usb_last_note = -1;

bool submitRequest(PikoRequestType type, uint8_t value) {
  const uint32_t id = next_request_id++;
  if (!request_queue.push({id, type, value})) return false;
  const absolute_time_t deadline = make_timeout_time_ms(2000);
  while (acknowledged_request_id != id && !time_reached(deadline)) {
    tud_task();
    piko_runtime_service_usb_midi();
    sleep_us(100);
  }
  return acknowledged_request_id == id && acknowledged_request_ok;
}

}  // namespace

bool piko_request_clock_mode(bool midi) {
  return submitRequest(PikoRequestType::SetClockMode, midi ? 1 : 0);
}

bool piko_request_pulse_ppqn(uint8_t ppqn) {
  return submitRequest(PikoRequestType::SetPulsePpqn, ppqn);
}

bool piko_request_stop_playback() {
  return submitRequest(PikoRequestType::StopPlayback, 0);
}

bool piko_request_start_playback() {
  return submitRequest(PikoRequestType::StartPlayback, 0);
}

bool piko_runtime_pop_request(PikoRequest* request) {
  return request != nullptr && request_queue.pop(*request);
}

void piko_runtime_ack_request(uint32_t id, bool ok) {
  acknowledged_request_ok = ok;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  acknowledged_request_id = id;
}

void piko_usb_midi_note_on(uint8_t note, uint8_t velocity) {
  if (!usb_midi_ready) return;
  usb_midi_queue.push({note, velocity});
}

void piko_runtime_service_usb_midi() {
#if CFG_TUD_MIDI
  usb_midi_ready = tud_mounted() && !tud_suspended();
  if (!usb_midi_ready) {
    usb_midi_queue.clear();
    usb_last_note = -1;
    return;
  }
  UsbMidiEvent event{};
  while (usb_midi_queue.pop(event)) {
    uint8_t message[3];
    if (usb_last_note >= 0) {
      message[0] = 0x80;
      message[1] = static_cast<uint8_t>(usb_last_note);
      message[2] = 0;
      tud_midi_n_stream_write(0, 0, message, sizeof(message));
    }
    message[0] = 0x90;
    message[1] = event.note;
    message[2] = event.velocity;
    tud_midi_n_stream_write(0, 0, message, sizeof(message));
    usb_last_note = event.note;
  }
#endif
}

uint32_t piko_usb_midi_queue_drops() { return usb_midi_queue.drops(); }

void piko_publish_clock_snapshot(const PikoClockSnapshot& snapshot) {
  ++snapshot_sequence;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  clock_snapshot = snapshot;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  ++snapshot_sequence;
}

bool piko_read_clock_snapshot(PikoClockSnapshot* snapshot) {
  if (snapshot == nullptr) return false;
  for (uint8_t attempt = 0; attempt < 8; ++attempt) {
    const uint32_t before = snapshot_sequence;
    if (before & 1u) continue;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *snapshot = clock_snapshot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint32_t after = snapshot_sequence;
    if (before == after && !(after & 1u)) return true;
  }
  return false;
}
