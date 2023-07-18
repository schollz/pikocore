class Button {
  uint8_t gpio;
  bool val[2];
  bool rising;
  bool falling;
  bool changed;
  uint16_t debounce;
  uint16_t debounce_time;

 public:
  void Init(uint8_t gpio_, uint16_t debounce_time_) {
    gpio = gpio_;
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
    debounce_time = debounce_time_;
    val[1] = (bool)(1 - gpio_get(gpio));
  }

  bool On() { return val[0]; }

  void Set(bool v) {
    val[0] = v;
    rising = val[0] > val[1];
    falling = val[0] < val[1];
    changed = rising || falling;
    if (changed) debounce = debounce_time;
    val[1] = val[0];
  }

  void Read() {
    if (debounce == 0) {
      Set((bool)(1 - gpio_get(gpio)));
    } else {
      changed = false;
      rising = false;
      falling = false;
      debounce--;
    }
  }

  bool Changed(bool reset) {
    // can only be read once
    if (changed && reset) {
      changed = false;
      return true;
    }
    return changed;
  }
  bool ChangedHigh(bool reset) {
    // can only be read once
    if (changed && reset) {
      changed = false;
      return true && val[1];
    }
    return changed && val[1];
  }
  bool Rising() { return rising; }
  bool Falling() { return falling; }
};
