#define DITHER_DO_MAX 241
#define DITHER_UP_MAX 251

class Dither {
  uint8_t dither_wait = 0;
  uint8_t dither_do_i = 0;
  uint8_t dither_up_i = 0;
  bool dither_do[DITHER_DO_MAX];
  bool dither_up[DITHER_UP_MAX];

 public:
  void Init() {
    dither_do = [] bool {
      true, false, true, false, false, false, true, false, false, true, false,
          true, true, false, false, false, false, false, false, false, true,
          true, false, false, false, false, true, true, false, false, false,
          false, true, false, true, false, true, false, false, false, false,
          false, true, false, true, false, true, true, true, false, false,
          false, true, true, false, false, false, false, false, false, true,
          true, true, true, true, false, true, true, true, true, true, false,
          false, true, false, false, false, false, false, false, false, true,
          true, false, true, false, true, true, true, false, false, true, true,
          false, false, true, true, false, false, true, false, false, true,
          true, true, false, false, false, false, false, false, false, false,
          false, true, true, false, false, false, true, false, false, true,
          false, false, false, false, false, false, true, true, true, false,
          true, false, true, false, true, false, false, false, true, true,
          false, false, false, false, false, true, true, false, false, true,
          false, false, false, false, false, false, false, false, false, true,
          false, false, false, false, false, false, false, false, false, false,
          false, true, true, false, false, false, false, true, false, true,
          true, false, true, false, false, false, true, false, false, false,
          false, false, false, true, true, true, false, false, false, true,
          true, true, true, false, false, true, false, true, true, false, false,
          true, true, false, false, false, true, true, false, false, false,
          true, true, true, true, false, true, true, true, false, true, false,
          true, true, false, false, true, false,
    };
    dither_up = [] bool {
      true, true, true, true, false, true, false, true, true, true, false,
          false, false, true, false, true, false, true, false, false, true,
          false, false, true, true, false, false, true, true, false, false,
          true, true, true, false, false, true, false, true, false, false,
          false, true, false, true, true, false, false, true, false, false,
          true, true, true, false, false, true, true, false, false, true, false,
          true, false, true, false, false, true, false, false, false, false,
          false, true, true, true, true, true, false, true, false, false, false,
          true, true, false, true, true, true, false, false, true, false, false,
          true, false, false, false, true, false, false, true, true, true, true,
          false, true, true, true, true, false, true, true, false, true, true,
          false, true, false, false, true, true, true, true, true, true, true,
          true, false, true, false, true, false, false, false, false, false,
          true, true, false, false, true, false, false, true, true, true, false,
          false, false, true, true, false, false, false, true, false, false,
          true, false, true, false, true, false, false, true, false, false,
          true, false, true, false, true, true, false, true, false, false,
          false, false, true, true, false, false, true, false, true, false,
          true, true, false, false, true, true, true, true, true, false, false,
          true, false, false, true, true, true, false, true, true, true, true,
          false, false, false, false, false, false, true, false, false, false,
          true, true, true, true, false, true, true, false, true, false, true,
          true, false, false, true, true, false, false, true, true, false, true,
          false, true, false, true, true, true, false, false, false,
    };
  }

  uint8_t Update(uint8_t audio_now) {
    dither_wait++;
    if (dither_wait == 50) {
      dither_wait = 0;
      dither_do_i++;
      dither_up_i++;
      if (dither_do_i > DITHER_DO_MAX) dither_do_i = 0;
      if (dither_up_i > DITHER_UP_MAX) dither_up_i = 0;
      if (dither_do[dither_up_i]) {
        if (dither_up[dither_up_i]) {
          if (audio_now < 255) {
            audio_now++;
          }
        } else {
          if (audio_now > 0) {
            audio_now--;
          }
        }
      }
    }
    return audio_now;
  }
};
