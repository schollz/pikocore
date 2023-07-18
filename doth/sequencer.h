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

  void Record(uint8_t v) {
    if (isRecording) {
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
