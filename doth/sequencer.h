class Sequencer {
  bool isPlaying;
  bool isRecording;
  uint8_t len;
  // 128 bytes
  uint8_t mem[128] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

 public:
  void Init() {
    isPlaying = false;
    isRecording = false;
    len = 0;
  }

  void Reset() { len = 0; }

  void Load(uint8_t save_data_[FLASH_PAGE_SIZE]) {
    for (uint8_t i = 0; i < 128; i++) {
      mem[i] = save_data_[i + 100];
    }
    len = save_data_[98];
    // Record() could push len past the end of mem, and Save() persists it, so a
    // page written by older firmware can restore an out of range length. Start
    // empty rather than indexing past mem.
    if (len > sizeof(mem)) {
      len = 0;
    }
    if (save_data_[99] == 1) {
      isPlaying = true;
    }
  }

  void Save(uint8_t save_data_[FLASH_PAGE_SIZE]) {
    if (isPlaying && len > 0) {
      save_data_[99] = 1;
    } else {
      save_data_[99] = 0;
    }
    save_data_[98] = len;
    for (uint8_t i = 0; i < 128; i++) {
      save_data_[i + 100] = mem[i];
    }
  }

  // len is a uint8_t and mem is 128 bytes, so an unbounded recording runs off
  // the end of the object and into whatever follows it in memory. Stop at the
  // end of the buffer instead.
  void Record(uint8_t v) {
    if (isRecording && len < sizeof(mem)) {
      mem[len] = v;
      len++;
    }
  }

  bool IsPlaying() { return isPlaying && len > 0; }
  bool IsRecording() { return isRecording; }

  void SetRecording(bool on) {
    isRecording = on;
    if (on == true) {
      isPlaying = false;
    }
  }

  void SetPlaying(bool on) {
    isRecording = false;
    isPlaying = on;
  }

  uint8_t Last() {
    if (len > 0) {
      return mem[len - 1];
    }
    return 255;
  }

  uint8_t Next(uint32_t beat) {
    if (isPlaying && len > 0) {
      return mem[beat % len];
    } else {
      return 0;
    }
  }
  uint8_t NextI(uint32_t beat) { return beat % len; }
};
