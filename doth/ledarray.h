class LEDArray {
  LED led[8];
  uint8_t vals[8];
  uint16_t do_leds;

 public:
  void Init() {
    for (uint8_t i = 0; i < 8; i++) {
      led[i].Init(i + 12);
      vals[i] = 0;
    }
    do_leds = 0;
  }

  bool Continue() {
    do_leds++;
    if (do_leds % 1000 == 0) {
      return true;
    }
    return false;
  }

  void LedSet(uint8_t i, uint8_t v) { led[i].Set(v); }

  void LedUpdate(uint8_t i) { led[i].Update(); }

  void Update() {
    for (uint8_t i = 0; i < 8; i++) {
      if (vals[i] != led[i].Val()) {
        led[i].SetDim(vals[i]);
      }
      led[i].Update();
    }
  }

  void Clear() {
    for (uint8_t i = 0; i < 8; i++) {
      vals[i] = 0;
    }
  }

  void On(uint8_t j) {
    for (uint8_t i = 0; i < 8; i++) {
      led[i].Set(i == j);
    }
  }

  // sets between 0 and 1000
  void Set(uint8_t i, uint16_t v) {
    v = v * 255 / 1000;
    if (v != vals[i]) {
      vals[i] = v;
    }
  }

  // sets between 0 and 1000
  void Add(uint8_t i, uint16_t v) {
    v = v * 255 / 1000;
    if (v != vals[i]) {
      if (vals[i] + v > 255) {
        vals[i] = 255;
      } else {
        vals[i] = v;
      }
    }
  }

  void SetBinary(uint8_t v) {
    uint8_t j = 0;
    for (uint8_t i = 128; i > 0; i = i / 2) {
      vals[j] = 255 * (v & i);
      j++;
    }
  }

  // SetAll sets between 0 and 1000
  void SetAll(uint16_t v) {
    v = v * 2040 / 1000;  // sets between 0 and 2040 to divide between 8 leds
    for (uint8_t i = 0; i < 8; i++) {
      if (v > 255) {
        vals[i] = 255;
        v -= 255;
      } else if (v > 0) {
        vals[i] = v;
        v = 0;
      } else {
        vals[i] = 0;
      }
    }
  }
};
