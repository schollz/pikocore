class LED {
  uint8_t gpio;
  uint8_t state;
  uint8_t dim;
  uint8_t dim_i;

 public:
  void Init(uint8_t gpio_) {
    gpio = gpio_;
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
    state = 0;
    dim_i = 0;
    gpio_put(gpio, 0);
  }

  uint8_t Val() { return dim; }

  void Update() {
    dim_i++;
    if (dim_i < dim) {
      gpio_put(gpio, 1);
    } else {
      gpio_put(gpio, 0);
    }
  }

  void Set(bool on) {
    if (on) {
      gpio_put(gpio, 1);
    } else {
      gpio_put(gpio, 0);
    }
  }

  void SetDim(uint16_t dim_) {
    if (dim_ < 255) {
      dim = dim_;
    } else {
      dim = 255;
    }
  }
};
