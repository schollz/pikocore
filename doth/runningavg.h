class RunningAverage {
  uint16_t vals[32];
  bool hasnum[32];
  uint8_t len;
  uint8_t i;
  uint16_t current;

 public:
  void Init(uint8_t len_) {
    len = len_;
    i = 0;
  }

  void Update(uint16_t v) {
    if (i >= len) {
      i = 0;
    }
    vals[i] = v;
    hasnum[i] = true;
    double count = 0;
    double total = 0;
    for (uint8_t j = 0; j < len; j++) {
      if (hasnum[j]) {
        total += ((double)vals[j]);
        count++;
      }
    }
    total = round(total / count);
    i++;
    current = ((uint16_t)total);
  }

  uint16_t Value() { return current; }
};
