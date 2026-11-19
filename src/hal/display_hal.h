#pragma once

#include "tt_fonts.h"
#include "vm/tt_vm.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tt {

static constexpr int16_t kScreenW = 320;
static constexpr int16_t kScreenH = 240;
static constexpr uint8_t kMinBrightness = 16;

// Standard RGB565 Color Constants
namespace Color {
  static constexpr uint16_t kBlack      = 0x0000;
  static constexpr uint16_t kWhite      = 0xFFFF;
  static constexpr uint16_t kDarkBg     = 0x1082;  // Deep slate #101010
  static constexpr uint16_t kWinBg      = 0x18E3;  // Panel dark #181C18
  static constexpr uint16_t kBarBg      = 0x2124;  // Top bar #202420
  static constexpr uint16_t kTitleFocus = 0x1B53;  // Active window title blue-teal
  static constexpr uint16_t kTitleBlur  = 0x39E7;  // Inactive window title gray
  static constexpr uint16_t kBorder     = 0x6B4D;  // Mid gray border
  static constexpr uint16_t kAccent     = 0xFFE0;  // CYD Yellow accent
  static constexpr uint16_t kCyan       = 0x07FF;
  static constexpr uint16_t kGreen      = 0x07E0;
  static constexpr uint16_t kRed        = 0xF800;
  static constexpr uint16_t kOrange     = 0xFD20;
  static constexpr uint16_t kMagenta    = 0xF81F;
  static constexpr uint16_t kMenuBg     = 0x2945;
  static constexpr uint16_t kMenuSel    = 0x0451;
  static constexpr uint16_t kKeyBg      = 0x3186;
  static constexpr uint16_t kKeySpec    = 0x4A49;
}

static inline constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint16_t>(r & 0xF8u) << 8) |
         (static_cast<uint16_t>(g & 0xFCu) << 3) |
         (static_cast<uint16_t>(b) >> 3);
}

// Convert TerseTalk 3-digit RGB `000`..`999` (e.g. 900=red, 090=green, 990=yellow) to RGB565
static inline uint16_t rgb999To565(int32_t code) {
  if (code < 0) code = 0;
  if (code > 999) code = 999;
  int r9 = (code / 100) % 10;
  int g9 = (code / 10)  % 10;
  int b9 = code % 10;
  uint8_t r = static_cast<uint8_t>((r9 * 255 + 4) / 9);
  uint8_t g = static_cast<uint8_t>((g9 * 255 + 4) / 9);
  uint8_t b = static_cast<uint8_t>((b9 * 255 + 4) / 9);
  return rgb565(r, g, b);
}

// Convert HSV (h in degrees [0..360), s in [0..1], v in [0..1]) to RGB565
static inline uint16_t hsvTo565(float hDeg, float s, float v) {
  if (s < 0.0f) s = 0.0f;
  if (s > 1.0f) s = 1.0f;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  hDeg = std::fmod(hDeg, 360.0f);
  if (hDeg < 0.0f) hDeg += 360.0f;

  float c = v * s;
  float x = c * (1.0f - std::fabs(std::fmod(hDeg / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r1 = 0, g1 = 0, b1 = 0;
  if      (hDeg <  60.0f) { r1 = c; g1 = x; b1 = 0; }
  else if (hDeg < 120.0f) { r1 = x; g1 = c; b1 = 0; }
  else if (hDeg < 180.0f) { r1 = 0; g1 = c; b1 = x; }
  else if (hDeg < 240.0f) { r1 = 0; g1 = x; b1 = c; }
  else if (hDeg < 300.0f) { r1 = x; g1 = 0; b1 = c; }
  else                    { r1 = c; g1 = 0; b1 = x; }

  uint8_t r = static_cast<uint8_t>((r1 + m) * 255.0f + 0.5f);
  uint8_t g = static_cast<uint8_t>((g1 + m) * 255.0f + 0.5f);
  uint8_t b = static_cast<uint8_t>((b1 + m) * 255.0f + 0.5f);
  return rgb565(r, g, b);
}

// Rich 256-entry default palette for 8-bit Bmp buffers:
//   0      = Black
//   1      = White
//   2..15  = Bright primary/secondary colors
//   16..223= Smooth 208-step HSV spectrum (great for Mandelbrot / heatmaps!)
//   224..255= 32-step grayscale ramp
static inline uint16_t defaultPalette565(uint8_t idx) {
  static const uint16_t kBase16[16] = {
    0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
    0xFD20, 0x87F0, 0x045F, 0xFC10, 0x8410, 0xC618, 0x4208, 0xA145,
  };
  if (idx < 16) return kBase16[idx];
  if (idx < 224) {
    float t = static_cast<float>(idx - 16) / 208.0f;
    float h = std::fmod(t * 360.0f * 2.0f + 220.0f, 360.0f);
    float v = 0.35f + 0.65f * std::sin(t * 3.14159265f * 0.9f + 0.1f);
    return hsvTo565(h, 0.92f, v > 1.0f ? 1.0f : v);
  }
  uint8_t g = static_cast<uint8_t>(((idx - 224) * 255) / 31);
  return rgb565(g, g, g);
}

// ---------------------------------------------------------------------------
// Hardware Abstraction Layer Interface (Implemented by HalCyd & HalMock)
// ---------------------------------------------------------------------------
class DisplayHal {
 public:
  int16_t clipX0 = 0;
  int16_t clipY0 = 0;
  int16_t clipX1 = kScreenW;
  int16_t clipY1 = kScreenH;

  uint8_t activeFontSize = 7;    // Selectable pixel font size: 7, 8, 9, or 10
  uint8_t backlightLevel = 255;  // PWM brightness: 16..255

  int16_t fontSizePx() const { return static_cast<int16_t>(activeFontSize); }
  int16_t fontW() const      { return static_cast<int16_t>(activeFontSize - 1); } // 6, 7, 8, 9
  int16_t fontH() const      { return static_cast<int16_t>(activeFontSize + 1); } // 8, 9, 10, 11
  int16_t lineH(int16_t pad = 2) const { return static_cast<int16_t>(fontH() + pad); }

  virtual void setFontSize(uint8_t sz) {
    if (sz < 7) sz = 7;
    if (sz > 10) sz = 10;
    activeFontSize = sz;
  }
  uint8_t getBacklight() const { return backlightLevel; }
  virtual void saveDisplayConfig() {}

  virtual ~DisplayHal() = default;

  virtual bool init() = 0;
  virtual void present() {}  // Flush framebuffer if buffered

  // Clipping rectangle in screen coordinates [x .. x+w, y .. y+h)
  virtual void setClipRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t x1 = x + w;
    int16_t y1 = y + h;
    clipX0 = (x < 0) ? 0 : (x > kScreenW ? kScreenW : x);
    clipY0 = (y < 0) ? 0 : (y > kScreenH ? kScreenH : y);
    clipX1 = (x1 < 0) ? 0 : (x1 > kScreenW ? kScreenW : x1);
    clipY1 = (y1 < 0) ? 0 : (y1 > kScreenH ? kScreenH : y1);
  }

  virtual void resetClipRect() {
    setClipRect(0, 0, kScreenW, kScreenH);
  }

  // Core 2D Graphics Primitives (all honor clipX0..clipX1, clipY0..clipY1)
  virtual void clear(uint16_t color565) {
    fillRect(clipX0, clipY0, clipX1 - clipX0, clipY1 - clipY0, color565);
  }

  virtual void drawPixel(int16_t x, int16_t y, uint16_t color565) = 0;
  virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) = 0;

  virtual void drawHLine(int16_t x, int16_t y, int16_t w, uint16_t color565) {
    if (w > 0) fillRect(x, y, w, 1, color565);
  }
  virtual void drawVLine(int16_t x, int16_t y, int16_t h, uint16_t color565) {
    if (h > 0) fillRect(x, y, 1, h, color565);
  }

  virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) {
    if (w <= 0 || h <= 0) return;
    drawHLine(x, y, w, color565);
    drawHLine(x, y + h - 1, w, color565);
    drawVLine(x, y, h, color565);
    drawVLine(x + w - 1, y, h, color565);
  }

  virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color565, uint8_t thickness = 1);
  virtual void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color565);
  virtual void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color565);
  virtual void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color565);
  virtual void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color565);
  virtual void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color565);
  virtual void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color565);

  virtual void drawChar(int16_t x, int16_t y, char c, uint16_t fg565,
                        uint16_t bg565 = 0, bool transparentBg = true, uint8_t scale = 1);
  virtual void drawText(int16_t x, int16_t y, const char* s, uint16_t fg565,
                        uint16_t bg565 = 0, bool transparentBg = true, uint8_t scale = 1);
  virtual void drawTextClipped(int16_t x, int16_t y, int16_t maxW, const char* s,
                               uint16_t fg565, uint16_t bg565 = 0, bool transparentBg = true, uint8_t scale = 1);

  virtual void blitBmp8(int16_t dstX, int16_t dstY, int16_t dstW, int16_t dstH,
                        const uint8_t* pixels, uint16_t srcW, uint16_t srcH,
                        const uint16_t* palette565 = nullptr);

  // `yak-cyd` Corner Dropdown Wedge Triangle:
  // Draws a solid box (boxX, boxY, boxSize, boxSize) containing a crisp
  // downward-pointing right-isosceles triangle indicating a dropdown menu.
  void drawMenuWedge(int16_t boxX, int16_t boxY, int16_t boxSize,
                     uint16_t bg565 = Color::kBlack, uint16_t fg565 = Color::kAccent) {
    fillRect(boxX, boxY, boxSize, boxSize, bg565);
    int16_t pad = 2;
    int16_t xL  = boxX + pad;
    int16_t xR  = boxX + boxSize - 1 - pad;
    int16_t xM  = boxX + boxSize / 2;
    int16_t yT  = boxY + pad + 2;
    int16_t yB  = yT + (xR - xL) / 2;
    fillTriangle(xL, yT, xR, yT, xM, yB, fg565);
  }

  // Touch & Peripherals
  virtual bool pollTouch(int16_t& outX, int16_t& outY) = 0;
  virtual void runTouchCalibration() {}
  virtual uint32_t millis() const = 0;
  virtual bool isSdMounted() const { return true; }
  virtual bool isFlashMounted() const { return true; }
  virtual void setRgbLed(bool r, bool g, bool b) { (void)r; (void)g; (void)b; }
  virtual void setBacklight(uint8_t level) {
    if (level < kMinBrightness) level = kMinBrightness;
    backlightLevel = level;
  }

  // Non-blocking Serial I/O
  virtual bool pollSerialLine(std::string& outLine) { (void)outLine; return false; }
  virtual void serialPrint(const char* s) { (void)s; }

  // Install VFS & timing hooks onto a VM instance
  virtual void attachToVm(VM& vm) = 0;
};

// ---------------------------------------------------------------------------
// Default Software Geometry & Font Rasterizers (used by HalMock & fallback)
// ---------------------------------------------------------------------------
inline void DisplayHal::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color565, uint8_t thickness) {
  if (y0 == y1 && thickness <= 1) {
    if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
    drawHLine(x0, y0, x1 - x0 + 1, color565);
    return;
  }
  if (x0 == x1 && thickness <= 1) {
    if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
    drawVLine(x0, y0, y1 - y0 + 1, color565);
    return;
  }

  int dx  = std::abs(static_cast<int>(x1) - static_cast<int>(x0));
  int sx  = (x0 < x1) ? 1 : -1;
  int dy  = -std::abs(static_cast<int>(y1) - static_cast<int>(y0));
  int sy  = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  int cx  = x0;
  int cy  = y0;
  int half = (thickness > 1) ? (thickness / 2) : 0;

  while (true) {
    if (thickness <= 1) {
      drawPixel(static_cast<int16_t>(cx), static_cast<int16_t>(cy), color565);
    } else {
      fillRect(static_cast<int16_t>(cx - half), static_cast<int16_t>(cy - half),
               thickness, thickness, color565);
    }
    if (cx == x1 && cy == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; cx += sx; }
    if (e2 <= dx) { err += dx; cy += sy; }
  }
}

inline void DisplayHal::drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color565) {
  if (r <= 0) {
    if (r == 0) drawPixel(cx, cy, color565);
    return;
  }
  int16_t f     = 1 - r;
  int16_t ddF_x = 1;
  int16_t ddF_y = -2 * r;
  int16_t x     = 0;
  int16_t y     = r;

  drawPixel(cx, cy + r, color565);
  drawPixel(cx, cy - r, color565);
  drawPixel(cx + r, cy, color565);
  drawPixel(cx - r, cy, color565);

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;
    drawPixel(cx + x, cy + y, color565);
    drawPixel(cx - x, cy + y, color565);
    drawPixel(cx + x, cy - y, color565);
    drawPixel(cx - x, cy - y, color565);
    drawPixel(cx + y, cy + x, color565);
    drawPixel(cx - y, cy + x, color565);
    drawPixel(cx + y, cy - x, color565);
    drawPixel(cx - y, cy - x, color565);
  }
}

inline void DisplayHal::fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color565) {
  if (r <= 0) {
    if (r == 0) drawPixel(cx, cy, color565);
    return;
  }
  drawHLine(cx - r, cy, 2 * r + 1, color565);
  int16_t f     = 1 - r;
  int16_t ddF_x = 1;
  int16_t ddF_y = -2 * r;
  int16_t x     = 0;
  int16_t y     = r;

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }
    x++;
    ddF_x += 2;
    f += ddF_x;
    drawHLine(cx - x, cy + y, 2 * x + 1, color565);
    drawHLine(cx - x, cy - y, 2 * x + 1, color565);
    drawHLine(cx - y, cy + x, 2 * y + 1, color565);
    drawHLine(cx - y, cy - x, 2 * y + 1, color565);
  }
}

inline void DisplayHal::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                      int16_t r, uint16_t color565) {
  if (w <= 0 || h <= 0) return;
  if (r <= 0) { drawRect(x, y, w, h, color565); return; }
  int16_t maxR = ((w < h) ? w : h) / 2;
  if (r > maxR) r = maxR;
  drawHLine(x + r, y, w - 2 * r, color565);
  drawHLine(x + r, y + h - 1, w - 2 * r, color565);
  drawVLine(x, y + r, h - 2 * r, color565);
  drawVLine(x + w - 1, y + r, h - 2 * r, color565);

  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, cx = 0, cy = r;
  while (cx < cy) {
    if (f >= 0) { cy--; ddF_y += 2; f += ddF_y; }
    cx++; ddF_x += 2; f += ddF_x;
    drawPixel(x + r - cx,         y + r - cy,         color565);
    drawPixel(x + r - cy,         y + r - cx,         color565);
    drawPixel(x + w - 1 - r + cx, y + r - cy,         color565);
    drawPixel(x + w - 1 - r + cy, y + r - cx,         color565);
    drawPixel(x + w - 1 - r + cx, y + h - 1 - r + cy, color565);
    drawPixel(x + w - 1 - r + cy, y + h - 1 - r + cx, color565);
    drawPixel(x + r - cx,         y + h - 1 - r + cy, color565);
    drawPixel(x + r - cy,         y + h - 1 - r + cx, color565);
  }
}

inline void DisplayHal::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                      int16_t r, uint16_t color565) {
  if (w <= 0 || h <= 0) return;
  if (r <= 0) { fillRect(x, y, w, h, color565); return; }
  int16_t maxR = ((w < h) ? w : h) / 2;
  if (r > maxR) r = maxR;
  fillRect(x, y + r, w, h - 2 * r, color565);
  fillRect(x + r, y, w - 2 * r, r, color565);
  fillRect(x + r, y + h - r, w - 2 * r, r, color565);

  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, cx = 0, cy = r;
  while (cx < cy) {
    if (f >= 0) { cy--; ddF_y += 2; f += ddF_y; }
    cx++; ddF_x += 2; f += ddF_x;
    drawHLine(x + r - cx, y + r - cy,         w - 2 * r + 2 * cx, color565);
    drawHLine(x + r - cy, y + r - cx,         w - 2 * r + 2 * cy, color565);
    drawHLine(x + r - cx, y + h - 1 - r + cy, w - 2 * r + 2 * cx, color565);
    drawHLine(x + r - cy, y + h - 1 - r + cx, w - 2 * r + 2 * cy, color565);
  }
}

inline void DisplayHal::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     int16_t x2, int16_t y2, uint16_t color565) {
  drawLine(x0, y0, x1, y1, color565);
  drawLine(x1, y1, x2, y2, color565);
  drawLine(x2, y2, x0, y0, color565);
}

inline void DisplayHal::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                     int16_t x2, int16_t y2, uint16_t color565) {
  if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y1 > y2) { int16_t t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

  if (y0 == y2) {
    int16_t minX = x0, maxX = x0;
    if (x1 < minX) minX = x1;
    if (x2 < minX) minX = x2;
    if (x1 > maxX) maxX = x1;
    if (x2 > maxX) maxX = x2;
    drawHLine(minX, y0, maxX - minX + 1, color565);
    return;
  }

  int32_t dx01 = x1 - x0, dy01 = y1 - y0;
  int32_t dx02 = x2 - x0, dy02 = y2 - y0;
  int32_t dx12 = x2 - x1, dy12 = y2 - y1;
  int32_t sa = 0, sb = 0;

  int16_t last = (y1 == y2) ? y1 : static_cast<int16_t>(y1 - 1);
  int16_t y    = y0;
  for (; y <= last; y++) {
    int16_t a = static_cast<int16_t>(x0 + sa / dy01);
    int16_t b = static_cast<int16_t>(x0 + sb / dy02);
    sa += dx01;
    sb += dx02;
    if (a > b) { int16_t t = a; a = b; b = t; }
    drawHLine(a, y, b - a + 1, color565);
  }

  sa = dx12 * (y - y1);
  sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    int16_t a = static_cast<int16_t>(x1 + sa / dy12);
    int16_t b = static_cast<int16_t>(x0 + sb / dy02);
    sa += dx12;
    sb += dx02;
    if (a > b) { int16_t t = a; a = b; b = t; }
    drawHLine(a, y, b - a + 1, color565);
  }
}

inline void DisplayHal::drawChar(int16_t x, int16_t y, char c, uint16_t fg565,
                                 uint16_t bg565, bool transparentBg, uint8_t scale) {
  if (scale == 0) scale = 1;
  const FontSpec spec = getFontSpec(activeFontSize);
  const int16_t cw = static_cast<int16_t>(spec.cellW * scale);
  const int16_t ch = static_cast<int16_t>(spec.cellH * scale);
  if (x + cw <= clipX0 || x >= clipX1 || y + ch <= clipY0 || y >= clipY1) return;

  if (!transparentBg) {
    fillRect(x, y, cw, ch, bg565);
  }
  const uint8_t* rows = getGlyphRows(spec, c);
  for (int row = 0; row < spec.glyphH; row++) {
    uint8_t bits = rows[row];
    if (bits == 0) continue;
    for (int col = 0; col < spec.glyphW; col++) {
      if (bits & (1u << (spec.glyphW - 1 - col))) {
        if (scale == 1) {
          drawPixel(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row), fg565);
        } else {
          fillRect(static_cast<int16_t>(x + col * scale), static_cast<int16_t>(y + row * scale),
                   scale, scale, fg565);
        }
      }
    }
  }
}

inline void DisplayHal::drawText(int16_t x, int16_t y, const char* s, uint16_t fg565,
                                 uint16_t bg565, bool transparentBg, uint8_t scale) {
  if (!s) return;
  if (scale == 0) scale = 1;
  const int16_t fw = static_cast<int16_t>(fontW() * scale);
  const int16_t fh = static_cast<int16_t>(fontH() * scale);
  int16_t cx = x;
  int16_t cy = y;
  for (const char* p = s; *p; p++) {
    if (*p == '\n') {
      cx = x;
      cy = static_cast<int16_t>(cy + fh + 1);
      continue;
    }
    drawChar(cx, cy, *p, fg565, bg565, transparentBg, scale);
    cx = static_cast<int16_t>(cx + fw);
  }
}

inline void DisplayHal::drawTextClipped(int16_t x, int16_t y, int16_t maxW, const char* s,
                                        uint16_t fg565, uint16_t bg565, bool transparentBg, uint8_t scale) {
  if (!s || maxW <= 0) return;
  if (scale == 0) scale = 1;
  const int16_t step = static_cast<int16_t>(fontW() * scale);
  int16_t cx = x;
  for (const char* p = s; *p && *p != '\n'; p++) {
    if (cx + step > x + maxW) break;
    drawChar(cx, y, *p, fg565, bg565, transparentBg, scale);
    cx = static_cast<int16_t>(cx + step);
  }
}

inline void DisplayHal::blitBmp8(int16_t dstX, int16_t dstY, int16_t dstW, int16_t dstH,
                                 const uint8_t* pixels, uint16_t srcW, uint16_t srcH,
                                 const uint16_t* palette565) {
  if (!pixels || srcW == 0 || srcH == 0 || dstW <= 0 || dstH <= 0) return;
  int16_t yStart = (dstY < clipY0) ? clipY0 : dstY;
  int16_t yEnd   = (dstY + dstH > clipY1) ? clipY1 : static_cast<int16_t>(dstY + dstH);
  int16_t xStart = (dstX < clipX0) ? clipX0 : dstX;
  int16_t xEnd   = (dstX + dstW > clipX1) ? clipX1 : static_cast<int16_t>(dstX + dstW);
  if (yStart >= yEnd || xStart >= xEnd) return;

  for (int16_t py = yStart; py < yEnd; py++) {
    uint32_t sy = static_cast<uint32_t>(py - dstY) * srcH / static_cast<uint32_t>(dstH);
    const uint8_t* srcRow = pixels + sy * srcW;
    for (int16_t px = xStart; px < xEnd; px++) {
      uint32_t sx = static_cast<uint32_t>(px - dstX) * srcW / static_cast<uint32_t>(dstW);
      uint8_t idx = srcRow[sx];
      uint16_t col = palette565 ? palette565[idx] : defaultPalette565(idx);
      drawPixel(px, py, col);
    }
  }
}

}  // namespace tt
