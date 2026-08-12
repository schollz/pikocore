class TriggerOut {
  uint8_t gpio;
  uint32_t pulse_width_us;
  uint32_t deadline_us;
  bool high;

 public:
  void Init(uint8_t gpio_, uint16_t milliseconds, uint16_t loop_hz) {
    (void)loop_hz;
    gpio = gpio_;
    pulse_width_us = static_cast<uint32_t>(milliseconds) * 1000u;
    deadline_us = 0;
    high = false;

    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
    gpio_pull_down(gpio);
  }

  void Update() {
    if (high && static_cast<int32_t>(time_us_32() - deadline_us) >= 0) {
      gpio_put(gpio, 0);
      high = false;
    }
  }

  void Trigger() {
    deadline_us = time_us_32() + pulse_width_us;
    gpio_put(gpio, 1);
    high = true;
  }
};
