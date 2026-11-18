#pragma once

#ifndef TT_HOST_BUILD

#include "display_hal.h"

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

namespace tt {

class HalCyd : public DisplayHal {
 public:
  // CYD Hardware Pinout (ESP32-2432S028R)
  static constexpr int kPinTouchIrq  = 36;
  static constexpr int kPinTouchMosi = 32;
  static constexpr int kPinTouchMiso = 39;
  static constexpr int kPinTouchClk  = 25;
  static constexpr int kPinTouchCs   = 33;

  static constexpr int kPinSdSck     = 18;
  static constexpr int kPinSdMiso    = 19;
  static constexpr int kPinSdMosi    = 23;
  static constexpr int kPinSdCs      = 5;

  static constexpr int kPinBacklight = 21;
  static constexpr int kPinLedR      = 4;
  static constexpr int kPinLedG      = 16;
  static constexpr int kPinLedB      = 17;

  // Calibration reference points (shared with `yak-cyd` NVS `"cyd_cal"`)
  static constexpr float kCalX1 = 40.0f;
  static constexpr float kCalX2 = 280.0f;
  static constexpr float kCalY1 = 50.0f;
  static constexpr float kCalY2 = 200.0f;

  TFT_eSPI tft;
  SPIClass sdSpi{VSPI};

  bool  flashOk = false;
  bool  sdOk    = false;

  // Touch calibration state
  bool  calSwapXY = true;
  float calU1     = 650.0f;
  float calU2     = 3350.0f;
  float calV1     = 980.0f;
  float calV2     = 3280.0f;

  std::string serialLineBuf;

  HalCyd() = default;
  ~HalCyd() override = default;

  bool init() override;

  void drawPixel(int16_t x, int16_t y, uint16_t color565) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) override;
  void drawHLine(int16_t x, int16_t y, int16_t w, uint16_t color565) override;
  void drawVLine(int16_t x, int16_t y, int16_t h, uint16_t color565) override;
  void drawChar(int16_t x, int16_t y, char c, uint16_t fg565,
                uint16_t bg565 = 0, bool transparentBg = true, uint8_t scale = 1) override;
  void blitBmp8(int16_t dstX, int16_t dstY, int16_t dstW, int16_t dstH,
                const uint8_t* pixels, uint16_t srcW, uint16_t srcH,
                const uint16_t* palette565 = nullptr) override;

  bool pollTouch(int16_t& outX, int16_t& outY) override;
  void runTouchCalibration() override;
  uint32_t millis() const override { return ::millis(); }
  bool isSdMounted() const override { return sdOk; }
  bool isFlashMounted() const override { return flashOk; }
  void setRgbLed(bool r, bool g, bool b) override;
  void setBacklight(uint8_t level) override;

  bool pollSerialLine(std::string& outLine) override;
  void serialPrint(const char* s) override;

  void attachToVm(VM& vm) override;

 private:
  void initTouchPins();
  bool readRawTouch(uint16_t& rawX, uint16_t& rawY);
  void loadCalibrationNvs();
  void saveCalibrationNvs();
};

}  // namespace tt

#endif  // !TT_HOST_BUILD
