// Adapted from code released under the BSD-3-Clause license
// supplied by Raspberry Pi (Trading) Ltd.
// Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
// and code released under the GNU GPL-3 license
// supplied by ForsakenNGS
// Copyright (c) 2022 ForsakenNGS
#ifndef WS2812_H
#define WS2812_H

#include "pico/types.h"
#include "hardware/pio.h"

class WS2812 {
    public:
        enum DataByte {
            NONE=0,
            RED=1,
            GREEN=2,
            BLUE=3,
            WHITE=4
        };
        enum DataFormat {
            FORMAT_RGB=0,
            FORMAT_GRB=1,
            FORMAT_WRGB=2
        };

        WS2812(uint pin, uint length, PIO pio, uint sm);
        WS2812(uint pin, uint length, PIO pio, uint sm, DataFormat format);
        WS2812(uint pin, uint length, PIO pio, uint sm, DataByte b1, DataByte b2, DataByte b3);
        WS2812(uint pin, uint length, PIO pio, uint sm, DataByte b1, DataByte b2, DataByte b3, DataByte b4);
        ~WS2812();

        static uint32_t RGB(uint8_t red, uint8_t green, uint8_t blue) {
            return (uint32_t)(blue) << 16 | (uint32_t)(green) << 8 | (uint32_t)(red);
        };

        static uint32_t RGBW(uint8_t red, uint8_t green, uint8_t blue, uint8_t white) {
            return (uint32_t)(white) << 24 | (uint32_t)(blue) << 16 | (uint32_t)(green) << 8 | (uint32_t)(red);
        }

        void setPixelColor(uint index, uint32_t color);
        void setPixelColor(uint index, uint8_t red, uint8_t green, uint8_t blue);
        void setPixelColor(uint index, uint8_t red, uint8_t green, uint8_t blue, uint8_t white);
        void fill(uint32_t color);
        void fill(uint32_t color, uint first);
        void fill(uint32_t color, uint first, uint count);
        void show();

    private:
        uint pin;
        uint length;
        PIO pio;
        uint sm;
        DataByte bytes[4];
        uint32_t *data;

        void initialize(uint pin, uint length, PIO pio, uint sm, DataByte b1, DataByte b2, DataByte b3, DataByte b4);
        uint32_t convertData(uint32_t rgbw);

};

#endif