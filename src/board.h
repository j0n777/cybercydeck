#pragma once
// ESP32-2432S028 "Cheap Yellow Display", ST7789 variant (CYD2USB pinout):
// LCD on HSPI (SCLK 14, MOSI 13, MISO 12, DC 2, CS 15), backlight GPIO 21,
// XPT2046 touch on its own pins (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36).
// Source: witnessmenow/ESP32-Cheap-Yellow-Display DisplayConfig/CYD2USB/User_Setup.h
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;

  void setup(bool invert, bool rgbOrder) {
    {
      auto c = bus.config();
      c.spi_host = HSPI_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
      bus.config(c);
      panel.setBus(&bus);
    }
    {
      auto c = panel.config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.readable = true;
      c.invert = invert;
      c.rgb_order = rgbOrder;   // false = BGR (what this board uses)
      c.dlen_16bit = false;
      c.bus_shared = false;
      panel.config(c);
    }
    {
      auto c = light.config();
      c.pin_bl = 21;
      c.invert = false;
      c.freq = 12000;
      c.pwm_channel = 7;
      light.config(c);
      panel.setLight(&light);
    }
    setPanel(&panel);
  }
};

// Board extras
#define PIN_LED_R 4      // RGB LED, active low
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_LDR 34       // light sensor (input only)
#define PIN_SPEAKER 26   // speaker connector via amplifier
// XPT2046 touch (read by touch.cpp with its own SPI)
#define TP_CLK 25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS 33
#define TP_IRQ 36
