#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

// This board's panel answers the ID query with 81 81 B3, which LovyanGFX's autodetect does not
// know (it only accepts ILI9341 / ST7789), so the panel is configured by hand: ST7789 driver,
// RGB order as LovyanGFX defines it (see config.h), no inversion. Touch is an XPT2046 on its own pins (software SPI).
class CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  CYD() {
    {
      auto c = _bus.config();
      c.spi_host    = SPI2_HOST;
      c.spi_mode    = 0;
      c.freq_write  = 40000000;
      c.freq_read   = 16000000;
      c.spi_3wire   = false;
      c.use_lock    = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk    = 14;
      c.pin_mosi    = 13;
      c.pin_miso    = 12;
      c.pin_dc      = 2;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs          = 15;
      c.pin_rst         = -1;
      c.pin_busy        = -1;
      c.panel_width     = 240;
      c.panel_height    = 320;
      c.offset_x        = 0;
      c.offset_y        = 0;
      c.offset_rotation = 0;
      c.readable        = false;
      c.invert          = TFT_INVERT;
      c.rgb_order       = TFT_RGB_ORDER;
      c.dlen_16bit      = false;
      c.bus_shared      = false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl      = 21;
      c.invert      = false;
      c.freq        = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {  // pin numbers and raw ranges are LovyanGFX's own Sunton 2432S028 values; the real mapping comes from calibration
      auto c = _touch.config();
      c.x_min      = 300;
      c.x_max      = 3900;
      c.y_min      = 3700;
      c.y_max      = 200;
      c.pin_int    = -1;
      c.bus_shared = false;
      c.spi_host   = -1;  // -1 = software SPI
      c.pin_sclk   = 25;
      c.pin_mosi   = 32;
      c.pin_miso   = 39;
      c.pin_cs     = 33;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
