class TriggerOut {
  uint8_t gpio;
  uint16_t trig_out_count;
  uint16_t trig_out_max;
  bool high;

 public:
  void Init(uint8_t gpio_, uint16_t milliseconds, uint16_t loop_hz) {
    gpio = gpio_;
    trig_out_count = 0;
    trig_out_max = loop_hz * milliseconds;
    high = false;

    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
    gpio_pull_down(gpio);
  }

  void Update() {
    if (trig_out_count > 0) {
      trig_out_count--;
      if (trig_out_count == 0) {
        gpio_put(gpio, 0);
        high = false;
      } else if (!high) {
        gpio_put(gpio, 1);
        high = true;
      }
    }
  }

  void Trigger() {
    trig_out_count = trig_out_max;
    gpio_put(gpio, 1);
    high = true;
  }
};
