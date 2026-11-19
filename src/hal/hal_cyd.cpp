#ifndef TT_HOST_BUILD

#include "hal_cyd.h"
#include "vm/tt_world.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_bt.h>

#include <algorithm>
#include <cstring>

namespace tt {

namespace {

static uint8_t xptTransfer8(int mosi, int miso, int clk, uint8_t out) {
  uint8_t in = 0;
  for (int bit = 7; bit >= 0; bit--) {
    digitalWrite(mosi, (out & (1u << bit)) ? HIGH : LOW);
    digitalWrite(clk, HIGH);
    in = static_cast<uint8_t>((in << 1) | (digitalRead(miso) ? 1u : 0u));
    digitalWrite(clk, LOW);
  }
  return in;
}

static uint16_t xptRead12(int mosi, int miso, int clk, uint8_t cmd) {
  xptTransfer8(mosi, miso, clk, cmd);
  uint16_t hi = xptTransfer8(mosi, miso, clk, 0x00);
  uint16_t lo = xptTransfer8(mosi, miso, clk, 0x00);
  return static_cast<uint16_t>(((hi << 8) | lo) >> 3) & 0x0FFFu;
}

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) std::swap(a, b);
  if (b > c) std::swap(b, c);
  if (a > b) std::swap(a, b);
  return b;
}

static std::string stripMountPrefix(const char* path, const char* mount) {
  size_t mlen = std::strlen(mount);
  if (std::strncmp(path, mount, mlen) == 0) {
    const char* rest = path + mlen;
    if (*rest == '\0') return "/";
    if (*rest == '/') return std::string(rest);
  }
  return std::string(path);
}

static void ensureSdParentDirs(const std::string& sdRelPath) {
  size_t pos = 1;
  while (true) {
    size_t slash = sdRelPath.find('/', pos);
    if (slash == std::string::npos) break;
    std::string dir = sdRelPath.substr(0, slash);
    if (!dir.empty() && !SD.exists(dir.c_str())) {
      SD.mkdir(dir.c_str());
    }
    pos = slash + 1;
  }
}

}  // namespace

bool HalCyd::init() {
  // Reclaim Bluetooth controller RAM (~64 KB DRAM; Wi-Fi is never initialized)
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

  Serial.begin(115200);

  // Active-low RGB LED off
  pinMode(kPinLedR, OUTPUT); digitalWrite(kPinLedR, HIGH);
  pinMode(kPinLedG, OUTPUT); digitalWrite(kPinLedG, HIGH);
  pinMode(kPinLedB, OUTPUT); digitalWrite(kPinLedB, HIGH);

  // Backlight PWM on GPIO 21 (compatible with both Arduino ESP32 v2.x and v3.x)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(kPinBacklight, 5000, 8);
  ledcWrite(kPinBacklight, 255);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(kPinBacklight, 0);
  ledcWrite(0, 255);
#endif

  // ILI9341 320x240 Landscape on HSPI
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(Color::kDarkBg);

  // Bit-banged XPT2046 Touch + NVS calibration
  initTouchPins();
  loadCalibrationNvs();

  // Immutable Flash filesystem (/flash)
  flashOk = LittleFS.begin(true, "/flash", 8, "spiffs");

  // Mutable MicroSD card on hardware VSPI (/sd)
  sdSpi.begin(kPinSdSck, kPinSdMiso, kPinSdMosi, kPinSdCs);
  sdOk = SD.begin(kPinSdCs, sdSpi, 16000000u);
  if (sdOk) {
    if (!SD.exists("/worlds")) SD.mkdir("/worlds");
    if (!SD.exists("/docs"))   SD.mkdir("/docs");
  }

  resetClipRect();
  return true;
}

void HalCyd::initTouchPins() {
  pinMode(kPinTouchIrq,  INPUT);
  pinMode(kPinTouchMiso, INPUT);
  pinMode(kPinTouchMosi, OUTPUT); digitalWrite(kPinTouchMosi, LOW);
  pinMode(kPinTouchClk,  OUTPUT); digitalWrite(kPinTouchClk,  LOW);
  pinMode(kPinTouchCs,   OUTPUT); digitalWrite(kPinTouchCs,   HIGH);
}

bool HalCyd::readRawTouch(uint16_t& rawX, uint16_t& rawY) {
  if (digitalRead(kPinTouchIrq) == HIGH) return false;

  digitalWrite(kPinTouchCs, LOW);
  uint16_t z1 = xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0xB1);
  uint16_t z2 = xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0xC1);
  int32_t pressure = static_cast<int32_t>(z1) + 4095 - static_cast<int32_t>(z2);

  if (pressure < 350) {
    // Re-enable PENIRQ before releasing CS
    xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0xD0);
    digitalWrite(kPinTouchCs, HIGH);
    return false;
  }

  uint16_t xs[3], ys[3];
  for (int i = 0; i < 3; i++) {
    xs[i] = xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0xD1);
    ys[i] = xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0x91);
  }
  // Power-down ADC & re-arm PENIRQ on GPIO 36
  xptRead12(kPinTouchMosi, kPinTouchMiso, kPinTouchClk, 0xD0);
  digitalWrite(kPinTouchCs, HIGH);

  rawX = median3(xs[0], xs[1], xs[2]);
  rawY = median3(ys[0], ys[1], ys[2]);
  return (rawX > 50 && rawX < 4040 && rawY > 50 && rawY < 4040);
}

void HalCyd::loadCalibrationNvs() {
  Preferences prefs;
  if (prefs.begin("cyd_cal", true)) {
    if (prefs.getBool("valid", false)) {
      calSwapXY = prefs.getBool("swapXY", true);
      calU1     = prefs.getFloat("u1", 650.0f);
      calU2     = prefs.getFloat("u2", 3350.0f);
      calV1     = prefs.getFloat("v1", 980.0f);
      calV2     = prefs.getFloat("v2", 3280.0f);
    }
    prefs.end();
  }
}

void HalCyd::saveCalibrationNvs() {
  Preferences prefs;
  if (prefs.begin("cyd_cal", false)) {
    prefs.putBool("valid",  true);
    prefs.putBool("swapXY", calSwapXY);
    prefs.putFloat("u1",    calU1);
    prefs.putFloat("u2",    calU2);
    prefs.putFloat("v1",    calV1);
    prefs.putFloat("v2",    calV2);
    prefs.end();
  }
}

bool HalCyd::pollTouch(int16_t& outX, int16_t& outY) {
  uint16_t rx = 0, ry = 0;
  if (!readRawTouch(rx, ry)) return false;

  float u = calSwapXY ? static_cast<float>(ry) : static_cast<float>(rx);
  float v = calSwapXY ? static_cast<float>(rx) : static_cast<float>(ry);

  float du = (calU2 - calU1);
  float dv = (calV2 - calV1);
  if (std::fabs(du) < 1.0f) du = 1.0f;
  if (std::fabs(dv) < 1.0f) dv = 1.0f;

  float fx = kCalX1 + (u - calU1) * (kCalX2 - kCalX1) / du;
  float fy = kCalY1 + (v - calV1) * (kCalY2 - kCalY1) / dv;

  int32_t ix = static_cast<int32_t>(fx + 0.5f);
  int32_t iy = static_cast<int32_t>(fy + 0.5f);
  if (ix < 0) ix = 0;
  if (ix >= kScreenW) ix = kScreenW - 1;
  if (iy < 0) iy = 0;
  if (iy >= kScreenH) iy = kScreenH - 1;

  outX = static_cast<int16_t>(ix);
  outY = static_cast<int16_t>(iy);
  return true;
}

void HalCyd::runTouchCalibration() {
  resetClipRect();
  auto waitTapRaw = [&](int16_t tx, int16_t ty, const char* label, uint16_t& outRx, uint16_t& outRy) {
    clear(Color::kBlack);
    drawText(36, 16, "TOUCH CALIBRATION", Color::kAccent, 0, true, 2);
    drawText(40, 38, label, Color::kWhite);
    drawHLine(tx - 10, ty, 21, Color::kCyan);
    drawVLine(tx, ty - 10, 21, Color::kCyan);
    drawCircle(tx, ty, 6, Color::kAccent);

    // Wait for touch release first
    uint16_t dx = 0, dy = 0;
    while (readRawTouch(dx, dy)) delay(15);
    delay(60);

    // Average 8 steady samples
    uint32_t sx = 0, sy = 0;
    int got = 0;
    while (got < 8) {
      if (readRawTouch(dx, dy)) {
        sx += dx;
        sy += dy;
        got++;
      }
      delay(12);
    }
    outRx = static_cast<uint16_t>(sx / 8u);
    outRy = static_cast<uint16_t>(sy / 8u);
    while (readRawTouch(dx, dy)) delay(15);
  };

  uint16_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0;
  waitTapRaw(static_cast<int16_t>(kCalX1), static_cast<int16_t>(kCalY1), "Tap top-left crosshair (1/3)", rx0, ry0);
  waitTapRaw(static_cast<int16_t>(kCalX2), static_cast<int16_t>(kCalY1), "Tap top-right crosshair (2/3)", rx1, ry1);
  waitTapRaw(static_cast<int16_t>(kCalX2), static_cast<int16_t>(kCalY2), "Tap bottom-right crosshair (3/3)", rx2, ry2);

  int dRxHoriz = std::abs(static_cast<int>(rx1) - static_cast<int>(rx0));
  int dRyHoriz = std::abs(static_cast<int>(ry1) - static_cast<int>(ry0));
  calSwapXY = (dRyHoriz > dRxHoriz);

  if (calSwapXY) {
    calU1 = static_cast<float>(ry0);
    calU2 = static_cast<float>(ry1);
    calV1 = static_cast<float>(rx1);
    calV2 = static_cast<float>(rx2);
  } else {
    calU1 = static_cast<float>(rx0);
    calU2 = static_cast<float>(rx1);
    calV1 = static_cast<float>(ry1);
    calV2 = static_cast<float>(ry2);
  }
  saveCalibrationNvs();
}

void HalCyd::drawPixel(int16_t x, int16_t y, uint16_t color565) {
  if (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1) return;
  tft.drawPixel(x, y, color565);
}

void HalCyd::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) {
  if (w <= 0 || h <= 0) return;
  int16_t x0 = (x < clipX0) ? clipX0 : x;
  int16_t y0 = (y < clipY0) ? clipY0 : y;
  int16_t x1 = (x + w > clipX1) ? clipX1 : static_cast<int16_t>(x + w);
  int16_t y1 = (y + h > clipY1) ? clipY1 : static_cast<int16_t>(y + h);
  if (x0 >= x1 || y0 >= y1) return;
  tft.fillRect(x0, y0, x1 - x0, y1 - y0, color565);
}

void HalCyd::drawHLine(int16_t x, int16_t y, int16_t w, uint16_t color565) {
  if (w <= 0 || y < clipY0 || y >= clipY1) return;
  int16_t x0 = (x < clipX0) ? clipX0 : x;
  int16_t x1 = (x + w > clipX1) ? clipX1 : static_cast<int16_t>(x + w);
  if (x0 < x1) tft.drawFastHLine(x0, y, x1 - x0, color565);
}

void HalCyd::drawVLine(int16_t x, int16_t y, int16_t h, uint16_t color565) {
  if (h <= 0 || x < clipX0 || x >= clipX1) return;
  int16_t y0 = (y < clipY0) ? clipY0 : y;
  int16_t y1 = (y + h > clipY1) ? clipY1 : static_cast<int16_t>(y + h);
  if (y0 < y1) tft.drawFastVLine(x, y0, y1 - y0, color565);
}

void HalCyd::drawChar(int16_t x, int16_t y, char c, uint16_t fg565,
                      uint16_t bg565, bool transparentBg, uint8_t scale) {
  if (scale == 0) scale = 1;
  const int16_t cw = kFontW * scale;
  const int16_t ch = kFontH * scale;
  if (x + cw <= clipX0 || x >= clipX1 || y + ch <= clipY0 || y >= clipY1) return;

  // Fast batched SPI write so each character uses a single CS transaction!
  tft.startWrite();
  if (!transparentBg) {
    int16_t rx0 = (x < clipX0) ? clipX0 : x;
    int16_t ry0 = (y < clipY0) ? clipY0 : y;
    int16_t rx1 = (x + cw > clipX1) ? clipX1 : static_cast<int16_t>(x + cw);
    int16_t ry1 = (y + ch > clipY1) ? clipY1 : static_cast<int16_t>(y + ch);
    if (rx0 < rx1 && ry0 < ry1) tft.fillRect(rx0, ry0, rx1 - rx0, ry1 - ry0, bg565);
  }
  const uint8_t* cols = glyph5x7(c);
  for (int col = 0; col < 5; col++) {
    uint8_t bits = cols[col];
    if (bits == 0) continue;
    for (int row = 0; row < 7; row++) {
      if (bits & (1u << row)) {
        if (scale == 1) {
          int16_t px = static_cast<int16_t>(x + col);
          int16_t py = static_cast<int16_t>(y + row);
          if (px >= clipX0 && px < clipX1 && py >= clipY0 && py < clipY1) {
            tft.drawPixel(px, py, fg565);
          }
        } else {
          int16_t rx0 = static_cast<int16_t>(x + col * scale);
          int16_t ry0 = static_cast<int16_t>(y + row * scale);
          int16_t rx1 = static_cast<int16_t>(rx0 + scale);
          int16_t ry1 = static_cast<int16_t>(ry0 + scale);
          if (rx0 < clipX0) rx0 = clipX0;
          if (ry0 < clipY0) ry0 = clipY0;
          if (rx1 > clipX1) rx1 = clipX1;
          if (ry1 > clipY1) ry1 = clipY1;
          if (rx0 < rx1 && ry0 < ry1) tft.fillRect(rx0, ry0, rx1 - rx0, ry1 - ry0, fg565);
        }
      }
    }
  }
  tft.endWrite();
}

void HalCyd::blitBmp8(int16_t dstX, int16_t dstY, int16_t dstW, int16_t dstH,
                      const uint8_t* pixels, uint16_t srcW, uint16_t srcH,
                      const uint16_t* palette565) {
  if (!pixels || srcW == 0 || srcH == 0 || dstW <= 0 || dstH <= 0) return;
  int16_t yStart = (dstY < clipY0) ? clipY0 : dstY;
  int16_t yEnd   = (dstY + dstH > clipY1) ? clipY1 : static_cast<int16_t>(dstY + dstH);
  int16_t xStart = (dstX < clipX0) ? clipX0 : dstX;
  int16_t xEnd   = (dstX + dstW > clipX1) ? clipX1 : static_cast<int16_t>(dstX + dstW);
  if (yStart >= yEnd || xStart >= xEnd) return;

  uint16_t lineBuf[kScreenW];
  const int16_t spanW = xEnd - xStart;

  tft.startWrite();
  for (int16_t py = yStart; py < yEnd; py++) {
    uint32_t sy = static_cast<uint32_t>(py - dstY) * srcH / static_cast<uint32_t>(dstH);
    const uint8_t* srcRow = pixels + sy * srcW;
    for (int16_t i = 0; i < spanW; i++) {
      uint32_t sx = static_cast<uint32_t>((xStart + i) - dstX) * srcW / static_cast<uint32_t>(dstW);
      uint8_t idx = srcRow[sx];
      lineBuf[i] = palette565 ? palette565[idx] : defaultPalette565(idx);
    }
    tft.pushImage(xStart, py, spanW, 1, lineBuf);
  }
  tft.endWrite();
}

void HalCyd::setRgbLed(bool r, bool g, bool b) {
  digitalWrite(kPinLedR, r ? LOW : HIGH);
  digitalWrite(kPinLedG, g ? LOW : HIGH);
  digitalWrite(kPinLedB, b ? LOW : HIGH);
}

void HalCyd::setBacklight(uint8_t level) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(kPinBacklight, level);
#else
  ledcWrite(0, level);
#endif
}

bool HalCyd::pollSerialLine(std::string& outLine) {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      outLine = serialLineBuf;
      serialLineBuf.clear();
      return true;
    }
    if (serialLineBuf.size() < 2048) {
      serialLineBuf.push_back(c);
    }
  }
  return false;
}

void HalCyd::serialPrint(const char* s) {
  if (s) Serial.print(s);
}

void HalCyd::attachToVm(VM& vm) {
  vm.host.user     = this;
  vm.host.millisFn = [](void* u) -> uint32_t {
    return static_cast<HalCyd*>(u)->millis();
  };
  vm.host.vfsRead = [](void* u, const char* path, std::string& out) -> bool {
    auto* self = static_cast<HalCyd*>(u);
    if (!path) return false;
    if (std::strncmp(path, "/flash", 6) == 0) {
      std::string rel = stripMountPrefix(path, "/flash");
      if (self->flashOk) {
        fs::File f = LittleFS.open(rel.c_str(), "r");
        if (f) {
          size_t sz = f.size();
          out.resize(sz);
          if (sz > 0) f.readBytes(&out[0], sz);
          f.close();
          return true;
        }
      }
      if (rel == "/prelude.tt" && kEmbeddedPreludeLen > 0) {
        out.assign(kEmbeddedPrelude, kEmbeddedPreludeLen);
        return true;
      }
      return false;
    }
    if (!self->sdOk) return false;
    std::string rel = stripMountPrefix(path, "/sd");
    fs::File f = SD.open(rel.c_str(), FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    out.resize(sz);
    if (sz > 0) f.readBytes(&out[0], sz);
    f.close();
    return true;
  };

  vm.host.vfsWrite = [](void* u, const char* path, const char* data, size_t len, bool append) -> bool {
    auto* self = static_cast<HalCyd*>(u);
    if (!path || std::strncmp(path, "/flash", 6) == 0) return false;
    if (!self->sdOk) return false;
    std::string rel = stripMountPrefix(path, "/sd");
    ensureSdParentDirs(rel);
    fs::File f = SD.open(rel.c_str(), append ? FILE_APPEND : FILE_WRITE);
    if (!f) return false;
    size_t wr = (len > 0 && data) ? f.write(reinterpret_cast<const uint8_t*>(data), len) : 0;
    f.close();
    return wr == len;
  };

  vm.host.vfsDelete = [](void* u, const char* path) -> bool {
    auto* self = static_cast<HalCyd*>(u);
    if (!path || std::strncmp(path, "/flash", 6) == 0 || !self->sdOk) return false;
    std::string rel = stripMountPrefix(path, "/sd");
    return SD.remove(rel.c_str());
  };

  vm.host.vfsList = [](void* u, const char* dir, std::vector<std::pair<std::string, uint32_t>>& out) -> bool {
    auto* self = static_cast<HalCyd*>(u);
    out.clear();
    const char* d = (dir && dir[0]) ? dir : "/sd";
    if (std::strncmp(d, "/flash", 6) == 0) {
      if (!self->flashOk) {
        if (kEmbeddedPreludeLen > 0) out.emplace_back("prelude.tt", static_cast<uint32_t>(kEmbeddedPreludeLen));
        return true;
      }
      std::string rel = stripMountPrefix(d, "/flash");
      fs::File root = LittleFS.open(rel.c_str());
      if (!root || !root.isDirectory()) return false;
      fs::File file = root.openNextFile();
      while (file) {
        const char* nm = file.name();
        const char* slash = std::strrchr(nm, '/');
        out.emplace_back(slash ? (slash + 1) : nm, static_cast<uint32_t>(file.size()));
        file = root.openNextFile();
      }
      return true;
    }
    if (!self->sdOk) return false;
    std::string rel = stripMountPrefix(d, "/sd");
    fs::File root = SD.open(rel.c_str());
    if (!root || !root.isDirectory()) return false;
    fs::File file = root.openNextFile();
    while (file) {
      const char* nm = file.name();
      const char* slash = std::strrchr(nm, '/');
      out.emplace_back(slash ? (slash + 1) : nm, static_cast<uint32_t>(file.size()));
      file = root.openNextFile();
    }
    return true;
  };
}

}  // namespace tt

#endif  // !TT_HOST_BUILD
