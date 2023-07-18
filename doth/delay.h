class Delay {
  uint8_t delayline[16000];
  uint16_t pos;
  uint16_t posmax;
  uint8_t feedback;

 public:
  void Init() {
    posmax = 16000;
    // initialize the delay line
    for (uint8_t i = 0; i < 16000; i++) {
      delayline[i] = 128;
    }
  }

  void SetTime(uint16_t posmax_) {
    posmax = posmax_;
    if (pos > posmax) {
      pos = 0;
    }
  }

  void SetFeedback(uint8_t feedback_) { feedback = feedback_; }

  uint8_t Update(uint8_t audio_now_) {
    // add delayed audio
    int16_t audio_now = (audio_now_ + (delayline[pos] - 128));
    if (audio_now > 255) {
      audio_now = 255;
    } else if (audio_now < 0) {
      audio_now = 0;
    }

    // determine next delay
    int16_t next_delay = (audio_now - 128) * feedback / 100 + 128;
    if (next_delay > 255) {
      next_delay = 255;
    } else if (next_delay < 0) {
      next_delay = 0;
    }
    delayline[pos] = next_delay;
    pos++;
    if (pos > posmax) {
      pos = 0;
    }
    return audio_now;
  }
};
