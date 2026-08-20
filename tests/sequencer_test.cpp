#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// sequencer.h is a bare header meant to be included after the firmware's flash
// constants; 256 matches FLASH_PAGE_SIZE on the RP2040.
#define FLASH_PAGE_SIZE 256
#include "sequencer.h"

namespace {

constexpr int kGuard = 64;
constexpr uint8_t kGuardByte = 0xA5;

// mem is the last member of Sequencer, so a run off its end lands in whatever
// follows the object. This puts something recognisable there.
struct Harness {
  Sequencer seq;
  uint8_t guard[kGuard];

  Harness() {
    memset(guard, kGuardByte, sizeof(guard));
    seq.Init();
  }

  void assertGuardIntact() const {
    for (int i = 0; i < kGuard; i++) {
      assert(guard[i] == kGuardByte);
    }
  }
};

// Recording longer than the buffer must stop, not keep writing.
void testRecordStopsAtBufferEnd() {
  Harness h;
  h.seq.SetRecording(true);
  for (int i = 0; i < 400; i++) {
    h.seq.Record((uint8_t)(i % 8));
  }
  h.assertGuardIntact();

  // Everything written is still readable, and playback stays inside it.
  h.seq.SetPlaying(true);
  for (uint32_t beat = 0; beat < 1000; beat++) {
    assert(h.seq.Next(beat) < 8);
  }
}

// The unbounded Record() could leave len anywhere up to 255, and Save() writes
// it to flash, so units running the old firmware have out of range lengths
// sitting in their settings page. Loading one must not index past mem.
void testLoadRejectsOversizedLength() {
  const uint8_t bad_lengths[] = {129, 200, 255};
  for (uint8_t bad : bad_lengths) {
    Harness h;
    uint8_t save_data[FLASH_PAGE_SIZE];
    memset(save_data, 0, sizeof(save_data));
    save_data[98] = bad;
    save_data[99] = 1;  // saved mid-playback
    h.seq.Load(save_data);
    h.assertGuardIntact();
    // The length was rejected, so there is nothing to play whatever byte 99 said.
    assert(!h.seq.IsPlaying());
    assert(h.seq.Last() == 255);
    for (uint32_t beat = 0; beat < 512; beat++) {
      assert(h.seq.Next(beat) == 0);
    }
  }
}

// A length that was genuinely saved must survive the round trip untouched.
void testSaveLoadRoundTrip() {
  Harness h;
  h.seq.SetRecording(true);
  for (uint8_t i = 0; i < 5; i++) {
    h.seq.Record(i);
  }
  h.seq.SetPlaying(true);

  uint8_t save_data[FLASH_PAGE_SIZE];
  memset(save_data, 0, sizeof(save_data));
  h.seq.Save(save_data);

  Harness loaded;
  loaded.seq.Load(save_data);
  assert(loaded.seq.IsPlaying());
  assert(loaded.seq.Last() == 4);
  for (uint32_t beat = 0; beat < 5; beat++) {
    assert(loaded.seq.Next(beat) == beat);
  }
  loaded.assertGuardIntact();
}

}  // namespace

int main(void) {
  testRecordStopsAtBufferEnd();
  testLoadRejectsOversizedLength();
  testSaveLoadRoundTrip();
  printf("sequencer_test: all assertions passed\n");
  return 0;
}
