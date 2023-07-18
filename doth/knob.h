class Knob {
  uint8_t input;
  uint16_t val[2];  // 0 = current value, 1 = last value
  uint16_t val_max;
  uint16_t alpha;
  uint16_t startup;
  bool changed;

 public:
  void Init(uint8_t input_, uint16_t alpha_) {
    input = input_;  // 0, 1, or 2
    alpha = alpha_;
    adc_gpio_init(input + 26);  // GPIO 26, 27, or 28
    // val_max = 4095 - ((4095 * alpha) >> 10);
    val_max = 4095;
    startup = 800;
  }

  void Reset() { startup = 800; }

  uint16_t Value() { return val[1]; }
  uint16_t ValueMax() { return val_max; }
  void Read() {
    adc_select_input(input);
    uint16_t adc = 4095 - adc_read();
    val[0] = adc;
    // val[0] = ((alpha * adc) + (1024 - alpha) * val[0]) >> 10;
    changed = abs(val[0] - val[1]) > 100;
    if (changed) {
      val[1] = val[0];
    }
  }
  bool Changed() {
    // prevent reading on startup
    if (startup > 0) {
      startup--;
      return false;
    }
    return changed;
  }
};
