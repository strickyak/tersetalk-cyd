#include "tt_pen.h"
#include "tt_scrollbar.h"
#include "tt_wm.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace tt {

namespace {

static DisplayHal*    gHal = nullptr;
static WindowManager* gWm  = nullptr;

// Adapter allowing all DisplayHal geometry/text routines to draw directly into an 8-bit ObjBmp!
class BmpHalAdapter : public DisplayHal {
 public:
  ObjBmp* bmp;
  uint8_t palVal;

  BmpHalAdapter(ObjBmp* b, uint8_t pv) : bmp(b), palVal(pv) {
    clipX0 = 0;
    clipY0 = 0;
    clipX1 = static_cast<int16_t>(b->wid);
    clipY1 = static_cast<int16_t>(b->hei);
    if (gHal) activeFontSize = gHal->activeFontSize;
  }

  bool init() override { return true; }
  bool pollTouch(int16_t&, int16_t&) override { return false; }
  uint32_t millis() const override { return 0; }
  void attachToVm(VM&) override {}

  void drawPixel(int16_t x, int16_t y, uint16_t) override {
    if (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1) return;
    bmp->pixels[y * bmp->wid + x] = palVal;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t) override {
    if (w <= 0 || h <= 0) return;
    int16_t x0 = (x < clipX0) ? clipX0 : x;
    int16_t y0 = (y < clipY0) ? clipY0 : y;
    int16_t x1 = (x + w > clipX1) ? clipX1 : static_cast<int16_t>(x + w);
    int16_t y1 = (y + h > clipY1) ? clipY1 : static_cast<int16_t>(y + h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int16_t py = y0; py < y1; py++) {
      std::memset(bmp->pixels + py * bmp->wid + x0, palVal, static_cast<size_t>(x1 - x0));
    }
  }
};

static ObjPen* reqPen(VM& vm, Oop v) {
  if (isObj(v) && objFromOop(v)->format == OBJ_PEN) {
    return reinterpret_cast<ObjPen*>(objFromOop(v));
  }
  vm.toss("Expected Pen");
}

static ObjBmp* reqBmp(VM& vm, Oop v, const char* ctx) {
  if (isObj(v) && objFromOop(v)->format == OBJ_BMP) {
    return reinterpret_cast<ObjBmp*>(objFromOop(v));
  }
  vm.toss("%s expected Bmp, got <%s>", ctx, vm.toStdString(v).c_str());
}

static int32_t reqNumI(VM& vm, Oop v, const char* ctx) {
  if (isInt(v)) return asInt30(v);
  if (isFloat(v)) return static_cast<int32_t>(std::floor(asFloat(v) + 0.5f));
  vm.toss("%s expected Num, got <%s>", ctx, vm.toStdString(v).c_str());
}

static float reqNumF(VM& vm, Oop v, const char* ctx) {
  if (isInt(v)) return static_cast<float>(asInt30(v));
  if (isFloat(v)) return asFloat(v);
  vm.toss("%s expected Num, got <%s>", ctx, vm.toStdString(v).c_str());
}

static void reqXY(VM& vm, Oop v, float& outX, float& outY, const char* ctx) {
  if (isObj(v) && objFromOop(v)->format == OBJ_LIST) {
    auto* l = reinterpret_cast<ObjList*>(objFromOop(v));
    if (l->len >= 2) {
      outX = reqNumF(vm, l->items[0], ctx);
      outY = reqNumF(vm, l->items[1], ctx);
      return;
    }
  }
  vm.toss("%s expected x @ y pair, got <%s>", ctx, vm.toStdString(v).c_str());
}

static void reqXYInt(VM& vm, Oop v, int16_t& outX, int16_t& outY, const char* ctx) {
  float fx = 0, fy = 0;
  reqXY(vm, v, fx, fy, ctx);
  outX = static_cast<int16_t>(std::floor(fx + 0.5f));
  outY = static_cast<int16_t>(std::floor(fy + 0.5f));
}

// Obtain target DisplayHal + origin offset `(ox, oy)` for `pen`, invoking `fn(DisplayHal& d, int16_t ox, int16_t oy)`
template <typename Fn>
static void withPenTarget(ObjPen* pen, Fn fn) {
  if (isObj(pen->targetBmp) && objFromOop(pen->targetBmp)->format == OBJ_BMP) {
    auto* bmp = reinterpret_cast<ObjBmp*>(objFromOop(pen->targetBmp));
    BmpHalAdapter adapter(bmp, pen->palIdx);
    fn(adapter, 0, 0);
    return;
  }
  if (!gHal) return;
  int16_t savedX0 = gHal->clipX0, savedY0 = gHal->clipY0;
  int16_t savedX1 = gHal->clipX1, savedY1 = gHal->clipY1;
  gHal->setClipRect(pen->vpX, pen->vpY, pen->vpW, pen->vpH);
  fn(*gHal, pen->vpX, pen->vpY);
  gHal->clipX0 = savedX0; gHal->clipY0 = savedY0;
  gHal->clipX1 = savedX1; gHal->clipY1 = savedY1;
}

static int16_t penWid(ObjPen* p) {
  if (isObj(p->targetBmp) && objFromOop(p->targetBmp)->format == OBJ_BMP) {
    return static_cast<int16_t>(reinterpret_cast<ObjBmp*>(objFromOop(p->targetBmp))->wid);
  }
  return p->vpW;
}

static int16_t penHei(ObjPen* p) {
  if (isObj(p->targetBmp) && objFromOop(p->targetBmp)->format == OBJ_BMP) {
    return static_cast<int16_t>(reinterpret_cast<ObjBmp*>(objFromOop(p->targetBmp))->hei);
  }
  return p->vpH;
}

static void worldToLocal(ObjPen* p, float wx, float wy, int16_t& lx, int16_t& ly) {
  float w = static_cast<float>(penWid(p) - 1);
  float h = static_cast<float>(penHei(p) - 1);
  if (!p->hasWorld) {
    lx = static_cast<int16_t>(std::floor(wx + 0.5f));
    ly = static_cast<int16_t>(std::floor(wy + 0.5f));
    return;
  }
  float dx = (p->wx1 - p->wx0);
  float dy = (p->wy1 - p->wy0);
  if (std::fabs(dx) < 1e-6f) dx = 1.0f;
  if (std::fabs(dy) < 1e-6f) dy = 1.0f;
  float px = (wx - p->wx0) / dx * w;
  float py = (1.0f - (wy - p->wy0) / dy) * h;  // +Y points UP!
  if (px < -16000.0f) px = -16000.0f;
  if (px >  16000.0f) px =  16000.0f;
  if (py < -16000.0f) py = -16000.0f;
  if (py >  16000.0f) py =  16000.0f;
  lx = static_cast<int16_t>(std::floor(px + 0.5f));
  ly = static_cast<int16_t>(std::floor(py + 0.5f));
}

// ---------------------------------------------------------------------------
// Pen Primitives
// ---------------------------------------------------------------------------
static Oop primPenWid(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(penWid(reqPen(*(VM*)nullptr, rcvr)));
}
static Oop primPenWidSafe(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(penWid(reqPen(vm, rcvr)));
}
static Oop primPenHeiSafe(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(penHei(reqPen(vm, rcvr)));
}
static Oop primPenSize(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  Oop xy[2] = {makeInt30(penWid(p)), makeInt30(penHei(p))};
  return vm.newListFrom(xy, 2);
}

static Oop primPenClear(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  uint8_t savedIdx = p->palIdx;
  p->palIdx = 0;
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillRect(ox, oy, penWid(p), penHei(p), Color::kBlack);
  });
  p->palIdx = savedIdx;
  return rcvr;
}

static Oop primPenClearCol(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int32_t c999 = reqNumI(vm, args[0], "clear:");
  uint16_t c565 = rgb999To565(c999);
  uint8_t savedIdx = p->palIdx;
  p->palIdx = static_cast<uint8_t>(c999 & 0xFF);
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillRect(ox, oy, penWid(p), penHei(p), c565);
  });
  p->palIdx = savedIdx;
  return rcvr;
}

static Oop primPenColor999(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int32_t c999 = reqNumI(vm, args[0], "color:");
  p->color565 = rgb999To565(c999);
  if (c999 == 0) p->palIdx = 0;
  else if (p->palIdx == 0) p->palIdx = 1;
  return rcvr;
}

static Oop primPenRgb(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int r = reqNumI(vm, args[0], "rgb:g:b:");
  int g = reqNumI(vm, args[1], "rgb:g:b:");
  int b = reqNumI(vm, args[2], "rgb:g:b:");
  p->color565 = rgb565(static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r)),
                       static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g)),
                       static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b)));
  return rcvr;
}

static Oop primPenHue(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float h = reqNumF(vm, args[0], "hue:");
  p->color565 = hsvTo565(h, 1.0f, 1.0f);
  int idx = 16 + (static_cast<int>(std::fmod(std::fabs(h), 360.0f) / 360.0f * 207.0f));
  p->palIdx = static_cast<uint8_t>(idx);
  return rcvr;
}

static Oop primPenHsv(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  p->color565 = hsvTo565(reqNumF(vm, args[0], "hsv:s:v:"),
                         reqNumF(vm, args[1], "hsv:s:v:"),
                         reqNumF(vm, args[2], "hsv:s:v:"));
  return rcvr;
}

static Oop primPenPal(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  uint8_t idx = static_cast<uint8_t>(reqNumI(vm, args[0], "pal:") & 0xFF);
  p->palIdx   = idx;
  p->color565 = defaultPalette565(idx);
  return rcvr;
}

static Oop setPenNamed(VM& vm, Oop rcvr, uint16_t c565, uint8_t idx) {
  ObjPen* p = reqPen(vm, rcvr);
  p->color565 = c565;
  p->palIdx   = idx;
  return rcvr;
}
static Oop primPenBlack(VM& vm, Oop r, const Oop*, uint8_t)   { return setPenNamed(vm, r, Color::kBlack,   0); }
static Oop primPenWhite(VM& vm, Oop r, const Oop*, uint8_t)   { return setPenNamed(vm, r, Color::kWhite,   1); }
static Oop primPenRed(VM& vm, Oop r, const Oop*, uint8_t)     { return setPenNamed(vm, r, Color::kRed,     2); }
static Oop primPenGreen(VM& vm, Oop r, const Oop*, uint8_t)   { return setPenNamed(vm, r, Color::kGreen,   3); }
static Oop primPenBlue(VM& vm, Oop r, const Oop*, uint8_t)    { return setPenNamed(vm, r, 0x001F,          4); }
static Oop primPenYellow(VM& vm, Oop r, const Oop*, uint8_t)  { return setPenNamed(vm, r, Color::kAccent,  5); }
static Oop primPenCyan(VM& vm, Oop r, const Oop*, uint8_t)    { return setPenNamed(vm, r, Color::kCyan,    6); }
static Oop primPenMagenta(VM& vm, Oop r, const Oop*, uint8_t) { return setPenNamed(vm, r, Color::kMagenta, 7); }
static Oop primPenOrange(VM& vm, Oop r, const Oop*, uint8_t)  { return setPenNamed(vm, r, Color::kOrange,  8); }
static Oop primPenGray(VM& vm, Oop r, const Oop*, uint8_t)    { return setPenNamed(vm, r, 0x8410,         12); }

static Oop primPenThick(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int t = reqNumI(vm, args[0], "thick:");
  p->thickness = static_cast<uint8_t>(t < 1 ? 1 : (t > 16 ? 16 : t));
  return rcvr;
}

static Oop primPenScale(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int s = reqNumI(vm, args[0], "scale:");
  p->font = static_cast<uint8_t>(s < 1 ? 1 : (s > 6 ? 6 : s));
  return rcvr;
}

// 2D Screen-space Shapes
static Oop primPenDot(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = 0, y = 0;
  reqXYInt(vm, args[0], x, y, "dot:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    if (p->thickness <= 1) d.drawPixel(ox + x, oy + y, p->color565);
    else d.fillRect(ox + x - p->thickness / 2, oy + y - p->thickness / 2, p->thickness, p->thickness, p->color565);
  });
  return rcvr;
}

static Oop primPenPixXY(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = static_cast<int16_t>(reqNumI(vm, args[0], "pix:y:"));
  int16_t y = static_cast<int16_t>(reqNumI(vm, args[1], "pix:y:"));
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawPixel(ox + x, oy + y, p->color565);
  });
  return rcvr;
}

static Oop primPenLineTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  reqXYInt(vm, args[0], x0, y0, "line:to:");
  reqXYInt(vm, args[1], x1, y1, "line:to:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawLine(ox + x0, oy + y0, ox + x1, oy + y1, p->color565, p->thickness);
  });
  return rcvr;
}

static Oop primPenBoxSize(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = 0, y = 0, w = 0, h = 0;
  reqXYInt(vm, args[0], x, y, "box:size:");
  reqXYInt(vm, args[1], w, h, "box:size:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillRect(ox + x, oy + y, w, h, p->color565);
  });
  return rcvr;
}

static Oop primPenRectSize(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = 0, y = 0, w = 0, h = 0;
  reqXYInt(vm, args[0], x, y, "rect:size:");
  reqXYInt(vm, args[1], w, h, "rect:size:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawRect(ox + x, oy + y, w, h, p->color565);
  });
  return rcvr;
}

static Oop primPenRBox(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = 0, y = 0, w = 0, h = 0;
  reqXYInt(vm, args[0], x, y, "rbox:size:r:");
  reqXYInt(vm, args[1], w, h, "rbox:size:r:");
  int16_t r = static_cast<int16_t>(reqNumI(vm, args[2], "rbox:size:r:"));
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillRoundRect(ox + x, oy + y, w, h, r, p->color565);
  });
  return rcvr;
}

static Oop primPenRRect(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x = 0, y = 0, w = 0, h = 0;
  reqXYInt(vm, args[0], x, y, "rrect:size:r:");
  reqXYInt(vm, args[1], w, h, "rrect:size:r:");
  int16_t r = static_cast<int16_t>(reqNumI(vm, args[2], "rrect:size:r:"));
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawRoundRect(ox + x, oy + y, w, h, r, p->color565);
  });
  return rcvr;
}

static Oop primPenCircR(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t cx = 0, cy = 0;
  reqXYInt(vm, args[0], cx, cy, "circ:r:");
  int16_t r = static_cast<int16_t>(reqNumI(vm, args[1], "circ:r:"));
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawCircle(ox + cx, oy + cy, r, p->color565);
  });
  return rcvr;
}

static Oop primPenDiscR(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t cx = 0, cy = 0;
  reqXYInt(vm, args[0], cx, cy, "disc:r:");
  int16_t r = static_cast<int16_t>(reqNumI(vm, args[1], "disc:r:"));
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillCircle(ox + cx, oy + cy, r, p->color565);
  });
  return rcvr;
}

static Oop primPenTri(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  reqXYInt(vm, args[0], x0, y0, "tri:p2:p3:");
  reqXYInt(vm, args[1], x1, y1, "tri:p2:p3:");
  reqXYInt(vm, args[2], x2, y2, "tri:p2:p3:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawTriangle(ox + x0, oy + y0, ox + x1, oy + y1, ox + x2, oy + y2, p->color565);
  });
  return rcvr;
}

static Oop primPenFTri(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  reqXYInt(vm, args[0], x0, y0, "ftri:p2:p3:");
  reqXYInt(vm, args[1], x1, y1, "ftri:p2:p3:");
  reqXYInt(vm, args[2], x2, y2, "ftri:p2:p3:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.fillTriangle(ox + x0, oy + y0, ox + x1, oy + y1, ox + x2, oy + y2, p->color565);
  });
  return rcvr;
}

static Oop primPenPoly(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  if (!isObj(args[0]) || objFromOop(args[0])->format != OBJ_LIST) {
    vm.toss("poly: expected List of x @ y points");
  }
  auto* l = reinterpret_cast<ObjList*>(objFromOop(args[0]));
  if (l->len < 2) return rcvr;
  int16_t px = 0, py = 0;
  reqXYInt(vm, l->items[0], px, py, "poly:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    for (uint32_t i = 1; i < l->len; i++) {
      int16_t nx = 0, ny = 0;
      reqXYInt(vm, l->items[i], nx, ny, "poly:");
      d.drawLine(ox + px, oy + py, ox + nx, oy + ny, p->color565, p->thickness);
      px = nx;
      py = ny;
    }
  });
  return rcvr;
}

static Oop primPenTextAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  std::string txt = vm.toStdString(args[0]);
  int16_t x = 0, y = 0;
  reqXYInt(vm, args[1], x, y, "text:at:");
  uint8_t sc = p->font ? p->font : 1;
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawText(ox + x, oy + y, txt.c_str(), p->color565, 0, true, sc);
  });
  return rcvr;
}

static Oop primPenAtText(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop swapped[2] = {args[1], args[0]};
  return primPenTextAt(vm, rcvr, swapped, 2);
}

static Oop primPenFontW(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int32_t sc = p->font ? p->font : 1;
  int32_t fw = gHal ? gHal->fontW() : 6;
  return makeInt30(fw * sc);
}

static Oop primPenFontH(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int32_t sc = p->font ? p->font : 1;
  int32_t fh = gHal ? gHal->fontH() : 8;
  return makeInt30(fh * sc);
}

static Oop primPenFontSize(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  (void)reqPen(vm, rcvr);
  return makeInt30(gHal ? gHal->fontSizePx() : 7);
}

static Oop primPenScrollBar(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  int16_t total = 0, vis = 0, bx = 0, by = 0, bw = 0, bh = 0;
  reqXYInt(vm, args[0], total, vis, "scrollBar:at:size:top:");
  reqXYInt(vm, args[1], bx, by, "scrollBar:at:size:top:");
  reqXYInt(vm, args[2], bw, bh, "scrollBar:at:size:top:");
  int32_t top = isInt(args[3]) ? asInt30(args[3])
              : (isFloat(args[3]) ? static_cast<int32_t>(asFloat(args[3])) : 0);
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    ScrollBar::draw(d, static_cast<int16_t>(ox + bx), static_cast<int16_t>(oy + by),
                    bw, bh, total, vis, top);
  });
  return rcvr;
}

static Oop primWinScrollHit(VM& vm, Oop, const Oop* args, uint8_t) {
  if (!gHal) return kOopNil;
  if (!isObj(args[0]) || objFromOop(args[0])->format != OBJ_LIST) return kOopNil;
  auto* evPair = reinterpret_cast<ObjList*>(objFromOop(args[0]));
  if (evPair->len < 2) return kOopNil;
  std::string kind = vm.toStdString(evPair->items[0]);
  int16_t tx = 0, ty = 0, bx = 0, by = 0, bw = 0, bh = 0, total = 0, vis = 0;
  reqXYInt(vm, evPair->items[1], tx, ty, "scrollHit:");
  reqXYInt(vm, args[1], bx, by, "scrollHit:");
  reqXYInt(vm, args[2], bw, bh, "scrollHit:");
  reqXYInt(vm, args[3], total, vis, "scrollHit:");
  int top = isInt(args[4]) ? asInt30(args[4])
          : (isFloat(args[4]) ? static_cast<int>(asFloat(args[4])) : 0);

  static bool sAppThumbDrag = false;
  if (ScrollBar::handleTouch(*gHal, kind.c_str(), tx, ty,
                             bx, by, bw, bh, total, vis, top, sAppThumbDrag)) {
    return makeInt30(top);
  }
  return kOopNil;
}

static Oop primSysFontSize(VM&, Oop, const Oop*, uint8_t) {
  return makeInt30(gHal ? gHal->fontSizePx() : 7);
}

static Oop primSysFontSizeSet(VM&, Oop rcvr, const Oop* args, uint8_t) {
  int32_t sz = isInt(args[0]) ? asInt30(args[0])
             : (isFloat(args[0]) ? static_cast<int32_t>(asFloat(args[0])) : 7);
  if (gHal) {
    gHal->setFontSize(static_cast<uint8_t>(sz));
    gHal->saveDisplayConfig();
  }
  if (gWm) gWm->onFontSizeChanged();
  return rcvr;
}

static Oop primSysBrightness(VM&, Oop, const Oop*, uint8_t) {
  return makeInt30(gHal ? gHal->getBacklight() : 255);
}

static Oop primSysBrightnessSet(VM&, Oop rcvr, const Oop* args, uint8_t) {
  int32_t lvl = isInt(args[0]) ? asInt30(args[0])
              : (isFloat(args[0]) ? static_cast<int32_t>(asFloat(args[0])) : 255);
  if (lvl < kMinBrightness) lvl = kMinBrightness;
  if (lvl > 255) lvl = 255;
  if (gHal) {
    gHal->setBacklight(static_cast<uint8_t>(lvl));
    gHal->saveDisplayConfig();
  }
  if (gWm) gWm->invalidateAll();
  return rcvr;
}

static Oop primPenBlitAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  ObjBmp* b = reqBmp(vm, args[0], "blit:at:");
  int16_t x = 0, y = 0;
  reqXYInt(vm, args[1], x, y, "blit:at:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.blitBmp8(ox + x, oy + y, b->wid, b->hei, b->pixels, b->wid, b->hei, b->palette);
  });
  return rcvr;
}

static Oop primPenBlitAtSize(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  ObjBmp* b = reqBmp(vm, args[0], "blit:at:size:");
  int16_t x = 0, y = 0, w = 0, h = 0;
  reqXYInt(vm, args[1], x, y, "blit:at:size:");
  reqXYInt(vm, args[2], w, h, "blit:at:size:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.blitBmp8(ox + x, oy + y, w, h, b->pixels, b->wid, b->hei, b->palette);
  });
  return rcvr;
}

static Oop primPenBlitFull(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  ObjBmp* b = reqBmp(vm, args[0], "blit:");
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.blitBmp8(ox, oy, penWid(p), penHei(p), b->pixels, b->wid, b->hei, b->palette);
  });
  return rcvr;
}

// ---------------------------------------------------------------------------
// Math World Coordinates & Function / Parametric Plotting
// ---------------------------------------------------------------------------
static Oop primPenWorldTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  reqXY(vm, args[0], p->wx0, p->wy0, "world:to:");
  reqXY(vm, args[1], p->wx1, p->wy1, "world:to:");
  p->hasWorld = true;
  return rcvr;
}

static Oop primPenScreenCoords(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  reqPen(vm, rcvr)->hasWorld = false;
  return rcvr;
}

static Oop primPenToScreen(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float wx = 0, wy = 0;
  reqXY(vm, args[0], wx, wy, "toScreen:");
  int16_t lx = 0, ly = 0;
  worldToLocal(p, wx, wy, lx, ly);
  Oop xy[2] = {makeInt30(lx), makeInt30(ly)};
  return vm.newListFrom(xy, 2);
}

static Oop primPenToWorld(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float px = 0, py = 0;
  reqXY(vm, args[0], px, py, "toWorld:");
  if (!p->hasWorld) return args[0];
  float w = static_cast<float>(penWid(p) > 1 ? penWid(p) - 1 : 1);
  float h = static_cast<float>(penHei(p) > 1 ? penHei(p) - 1 : 1);
  float wx = p->wx0 + (px / w) * (p->wx1 - p->wx0);
  float wy = p->wy0 + (1.0f - py / h) * (p->wy1 - p->wy0);
  Oop xy[2] = {vm.makeFloatChecked(wx), vm.makeFloatChecked(wy)};
  return vm.newListFrom(xy, 2);
}

static Oop primPenAxes(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  if (!p->hasWorld) return rcvr;
  int16_t xZero = 0, yZero = 0;
  worldToLocal(p, 0.0f, 0.0f, xZero, yZero);
  int16_t w = penWid(p), h = penHei(p);
  uint16_t axisCol = 0x52AA;  // Subtle steel gray-blue
  uint16_t gridCol = 0x2104;  // Very dim grid lines

  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    // Integer grid lines when span is reasonable
    float spanX = std::fabs(p->wx1 - p->wx0);
    float spanY = std::fabs(p->wy1 - p->wy0);
    float stepX = (spanX > 24.0f) ? std::pow(10.0f, std::floor(std::log10(spanX * 0.25f))) : 1.0f;
    float stepY = (spanY > 24.0f) ? std::pow(10.0f, std::floor(std::log10(spanY * 0.25f))) : 1.0f;
    float minWX = std::min(p->wx0, p->wx1), maxWX = std::max(p->wx0, p->wx1);
    float minWY = std::min(p->wy0, p->wy1), maxWY = std::max(p->wy0, p->wy1);

    for (float gx = std::ceil(minWX / stepX) * stepX; gx <= maxWX; gx += stepX) {
      int16_t lx = 0, ly = 0;
      worldToLocal(p, gx, 0.0f, lx, ly);
      d.drawVLine(ox + lx, oy, h, gridCol);
    }
    for (float gy = std::ceil(minWY / stepY) * stepY; gy <= maxWY; gy += stepY) {
      int16_t lx = 0, ly = 0;
      worldToLocal(p, 0.0f, gy, lx, ly);
      d.drawHLine(ox, oy + ly, w, gridCol);
    }
    if (yZero >= 0 && yZero < h) d.drawHLine(ox, oy + yZero, w, axisCol);
    if (xZero >= 0 && xZero < w) d.drawVLine(ox + xZero, oy, h, axisCol);
  });
  return rcvr;
}

static Oop primPenWDot(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float wx = 0, wy = 0;
  reqXY(vm, args[0], wx, wy, "wdot:");
  int16_t lx = 0, ly = 0;
  worldToLocal(p, wx, wy, lx, ly);
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    if (p->thickness <= 1) d.drawPixel(ox + lx, oy + ly, p->color565);
    else d.fillRect(ox + lx - p->thickness / 2, oy + ly - p->thickness / 2, p->thickness, p->thickness, p->color565);
  });
  return rcvr;
}

static Oop primPenWLineTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;
  reqXY(vm, args[0], wx0, wy0, "wline:to:");
  reqXY(vm, args[1], wx1, wy1, "wline:to:");
  int16_t lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
  worldToLocal(p, wx0, wy0, lx0, ly0);
  worldToLocal(p, wx1, wy1, lx1, ly1);
  withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
    d.drawLine(ox + lx0, oy + ly0, ox + lx1, oy + ly1, p->color565, p->thickness);
  });
  return rcvr;
}

// Fast continuous function plotter: `p plot: FN(x: x sin)`
static Oop primPenPlot(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  Oop blk = args[0];
  if (!isObj(blk) || objFromOop(blk)->format != OBJ_BLK) {
    vm.toss("plot: expected Blk FN(x: ...)");
  }
  TempRootGuard rg(vm, rcvr);
  rg.push(blk);

  int16_t w = penWid(p);
  int16_t h = penHei(p);
  if (w <= 1) return rcvr;

  bool  havePrev = false;
  int16_t prevX = 0, prevY = 0;

  for (int16_t px = 0; px < w; px++) {
    p = reqPen(vm, rcvr);  // Refresh pointer across GC!
    float wx = p->hasWorld
             ? (p->wx0 + (static_cast<float>(px) / static_cast<float>(w - 1)) * (p->wx1 - p->wx0))
             : static_cast<float>(px);
    Oop xArg = vm.makeFloatChecked(wx);
    Oop yRes = kOopNil;
    try {
      yRes = vm.evalBlk(blk, &xArg, 1);
    } catch (const TerseError&) {
      havePrev = false;
      continue;
    }
    if (!isNum(yRes)) {
      havePrev = false;
      continue;
    }
    float wy = toFloatUnchecked(yRes);
    if (!std::isfinite(wy)) {
      havePrev = false;
      continue;
    }
    p = reqPen(vm, rcvr);
    int16_t lx = px, ly = 0;
    worldToLocal(p, wx, wy, lx, ly);
    // Guard against vertical asymptote spikes (e.g. tan(x) or 1/x jumping across screen)
    if (havePrev && std::abs(static_cast<int>(ly) - static_cast<int>(prevY)) < h * 2) {
      withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
        d.drawLine(ox + prevX, oy + prevY, ox + lx, oy + ly, p->color565, p->thickness);
      });
    } else if (ly >= -h && ly <= 2 * h) {
      withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
        d.drawPixel(ox + lx, oy + ly, p->color565);
      });
    }
    prevX    = lx;
    prevY    = ly;
    havePrev = (ly >= -h && ly <= 2 * h);
  }
  return rcvr;
}

// Fast parametric curve plotter: `p param: FN(t: (t cos) @ (t sin)) from: 0 to: Num tau steps: 200`
static Oop primPenParam(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop blk = args[0];
  if (!isObj(blk) || objFromOop(blk)->format != OBJ_BLK) {
    vm.toss("param:from:to:steps: expected Blk FN(t: x @ y)");
  }
  float t0    = reqNumF(vm, args[1], "param:from:to:steps:");
  float t1    = reqNumF(vm, args[2], "param:from:to:steps:");
  int   steps = reqNumI(vm, args[3], "param:from:to:steps:");
  if (steps < 1) steps = 1;
  if (steps > 4000) steps = 4000;

  TempRootGuard rg(vm, rcvr);
  rg.push(blk);

  bool havePrev = false;
  int16_t prevX = 0, prevY = 0;
  for (int i = 0; i <= steps; i++) {
    float t = t0 + (t1 - t0) * (static_cast<float>(i) / static_cast<float>(steps));
    Oop tArg = vm.makeFloatChecked(t);
    Oop pt = vm.evalBlk(blk, &tArg, 1);
    float wx = 0, wy = 0;
    reqXY(vm, pt, wx, wy, "param:");
    if (!std::isfinite(wx) || !std::isfinite(wy)) {
      havePrev = false;
      continue;
    }
    ObjPen* p = reqPen(vm, rcvr);
    int16_t lx = 0, ly = 0;
    worldToLocal(p, wx, wy, lx, ly);
    if (havePrev) {
      withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
        d.drawLine(ox + prevX, oy + prevY, ox + lx, oy + ly, p->color565, p->thickness);
      });
    }
    prevX = lx;
    prevY = ly;
    havePrev = true;
  }
  return rcvr;
}

// ---------------------------------------------------------------------------
// Turtle Graphics on Pen
// ---------------------------------------------------------------------------
static Oop primPenHome(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  p->turtleX   = penWid(p) * 0.5f;
  p->turtleY   = penHei(p) * 0.5f;
  p->turtleDeg = 0.0f;
  p->penDown   = true;
  return rcvr;
}

static Oop primPenGoto(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float nx = 0, ny = 0;
  reqXY(vm, args[0], nx, ny, "goto:");
  if (p->penDown) {
    int16_t x0 = static_cast<int16_t>(std::floor(p->turtleX + 0.5f));
    int16_t y0 = static_cast<int16_t>(std::floor(p->turtleY + 0.5f));
    int16_t x1 = static_cast<int16_t>(std::floor(nx + 0.5f));
    int16_t y1 = static_cast<int16_t>(std::floor(ny + 0.5f));
    withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
      d.drawLine(ox + x0, oy + y0, ox + x1, oy + y1, p->color565, p->thickness);
    });
  }
  p->turtleX = nx;
  p->turtleY = ny;
  return rcvr;
}

static Oop primPenJump(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  reqXY(vm, args[0], p->turtleX, p->turtleY, "jump:");
  return rcvr;
}

static Oop primPenGo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  float dist = reqNumF(vm, args[0], "go:");
  float rad  = p->turtleDeg * 0.0174532925f;
  float nx   = p->turtleX + std::cos(rad) * dist;
  float ny   = p->turtleY - std::sin(rad) * dist;  // +90 deg = UP on screen!
  if (p->penDown) {
    int16_t x0 = static_cast<int16_t>(std::floor(p->turtleX + 0.5f));
    int16_t y0 = static_cast<int16_t>(std::floor(p->turtleY + 0.5f));
    int16_t x1 = static_cast<int16_t>(std::floor(nx + 0.5f));
    int16_t y1 = static_cast<int16_t>(std::floor(ny + 0.5f));
    withPenTarget(p, [&](DisplayHal& d, int16_t ox, int16_t oy) {
      d.drawLine(ox + x0, oy + y0, ox + x1, oy + y1, p->color565, p->thickness);
    });
  }
  p->turtleX = nx;
  p->turtleY = ny;
  return rcvr;
}

static Oop primPenTurn(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  p->turtleDeg += reqNumF(vm, args[0], "turn:");
  return rcvr;
}

static Oop primPenRt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  p->turtleDeg -= reqNumF(vm, args[0], "rt:");
  return rcvr;
}

static Oop primPenHead(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  reqPen(vm, rcvr)->turtleDeg = reqNumF(vm, args[0], "head:");
  return rcvr;
}

static Oop primPenDown(VM& vm, Oop rcvr, const Oop*, uint8_t)  { reqPen(vm, rcvr)->penDown = true;  return rcvr; }
static Oop primPenUp(VM& vm, Oop rcvr, const Oop*, uint8_t)    { reqPen(vm, rcvr)->penDown = false; return rcvr; }
static Oop primPenPos(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjPen* p = reqPen(vm, rcvr);
  Oop xy[2] = {vm.makeFloatChecked(p->turtleX), vm.makeFloatChecked(p->turtleY)};
  return vm.newListFrom(xy, 2);
}
static Oop primPenHeading(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.makeFloatChecked(reqPen(vm, rcvr)->turtleDeg);
}

// ---------------------------------------------------------------------------
// 8-Bit Indexed Pixel Buffer (`Bmp`) Primitives
// ---------------------------------------------------------------------------
static Oop primBmpClsNew(VM& vm, Oop, const Oop* args, uint8_t) {
  int32_t w = reqNumI(vm, args[0], "Bmp wid:hei:");
  int32_t h = reqNumI(vm, args[1], "Bmp wid:hei:");
  if (w <= 0 || h <= 0 || w > 512 || h > 512 || (w * h) > 76800) {
    vm.toss("Bmp wid:hei: invalid dimensions %dx%d", static_cast<int>(w), static_cast<int>(h));
  }
  uint8_t* pix = static_cast<uint8_t*>(vm.allocExternal(static_cast<size_t>(w * h)));
  std::memset(pix, 0, static_cast<size_t>(w * h));
  auto* b = reinterpret_cast<ObjBmp*>(vm.allocObj(CLS_BMP, OBJ_BMP, 0, sizeof(ObjBmp)));
  b->wid     = static_cast<uint16_t>(w);
  b->hei     = static_cast<uint16_t>(h);
  b->pixels  = pix;
  b->palette = nullptr;
  return oopFromObj(&b->hdr);
}

static Oop primBmpWid(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(reqBmp(vm, rcvr, "wid")->wid);
}
static Oop primBmpHei(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(reqBmp(vm, rcvr, "hei")->hei);
}
static Oop primBmpClear(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "clear");
  if (b->pixels) std::memset(b->pixels, 0, static_cast<size_t>(b->wid) * b->hei);
  return rcvr;
}
static Oop primBmpClearIdx(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "clear:");
  uint8_t v = static_cast<uint8_t>(reqNumI(vm, args[0], "clear:") & 0xFF);
  if (b->pixels) std::memset(b->pixels, v, static_cast<size_t>(b->wid) * b->hei);
  return rcvr;
}

static Oop primBmpAtXY(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "at:y:");
  int32_t x = reqNumI(vm, args[0], "at:y:");
  int32_t y = reqNumI(vm, args[1], "at:y:");
  if (x < 0 || x >= b->wid || y < 0 || y >= b->hei) return makeInt30(0);
  return makeInt30(b->pixels[y * b->wid + x]);
}

static Oop primBmpWrapAtXY(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "wrap:y:");
  if (b->wid == 0 || b->hei == 0) return makeInt30(0);
  int32_t x = reqNumI(vm, args[0], "wrap:y:") % b->wid;
  int32_t y = reqNumI(vm, args[1], "wrap:y:") % b->hei;
  if (x < 0) x += b->wid;
  if (y < 0) y += b->hei;
  return makeInt30(b->pixels[y * b->wid + x]);
}

static Oop primBmpAtXYPut(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "at:y:put:");
  int32_t x = reqNumI(vm, args[0], "at:y:put:");
  int32_t y = reqNumI(vm, args[1], "at:y:put:");
  uint8_t v = static_cast<uint8_t>(reqNumI(vm, args[2], "at:y:put:") & 0xFF);
  if (x >= 0 && x < b->wid && y >= 0 && y < b->hei) {
    b->pixels[y * b->wid + x] = v;
  }
  return rcvr;
}

static Oop primBmpAtPt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  int16_t x = 0, y = 0;
  reqXYInt(vm, args[0], x, y, "Bmp at:");
  Oop xy[2] = {makeInt30(x), makeInt30(y)};
  return primBmpAtXY(vm, rcvr, xy, 2);
}

static Oop primBmpAtPtPut(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  int16_t x = 0, y = 0;
  reqXYInt(vm, args[0], x, y, "Bmp at:put:");
  Oop xyp[3] = {makeInt30(x), makeInt30(y), args[1]};
  return primBmpAtXYPut(vm, rcvr, xyp, 3);
}

static void ensureCustomPalette(VM& vm, ObjBmp* b) {
  if (!b->palette) {
    b->palette = static_cast<uint16_t*>(vm.allocExternal(256 * sizeof(uint16_t)));
    for (int i = 0; i < 256; i++) b->palette[i] = defaultPalette565(static_cast<uint8_t>(i));
  }
}

static Oop primBmpPalRgb999(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "pal:color:");
  ensureCustomPalette(vm, b);
  uint8_t idx = static_cast<uint8_t>(reqNumI(vm, args[0], "pal:color:") & 0xFF);
  b->palette[idx] = rgb999To565(reqNumI(vm, args[1], "pal:color:"));
  return rcvr;
}

static Oop primBmpPen(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "pen");
  uint16_t w = b->wid, h = b->hei;
  TempRootGuard rg(vm, rcvr);
  auto* p = reinterpret_cast<ObjPen*>(vm.allocObj(CLS_PEN, OBJ_PEN, 0, sizeof(ObjPen)));
  p->vpX       = 0;
  p->vpY       = 0;
  p->vpW       = static_cast<int16_t>(w);
  p->vpH       = static_cast<int16_t>(h);
  p->targetBmp = rcvr;
  p->color565  = Color::kWhite;
  p->palIdx    = 1;
  p->thickness = 1;
  p->font      = 1;
  p->penDown   = true;
  p->hasWorld  = false;
  p->turtleX   = w * 0.5f;
  p->turtleY   = h * 0.5f;
  p->turtleDeg = 0.0f;
  return oopFromObj(&p->hdr);
}

// Fast Mandelbrot escape-time kernel on Bmp (`bmp mandel: cx @ cy span: sx @ sy max: maxIter`)
static Oop primBmpMandel(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "mandel:span:max:");
  float cx = 0, cy = 0, sx = 0, sy = 0;
  reqXY(vm, args[0], cx, cy, "mandel:span:max:");
  reqXY(vm, args[1], sx, sy, "mandel:span:max:");
  int maxIter = reqNumI(vm, args[2], "mandel:span:max:");
  if (maxIter < 4) maxIter = 4;
  if (maxIter > 512) maxIter = 512;

  const uint16_t w = b->wid, h = b->hei;
  if (w == 0 || h == 0) return rcvr;
  float x0 = cx - sx * 0.5f;
  float y0 = cy - sy * 0.5f;
  float dx = sx / static_cast<float>(w);
  float dy = sy / static_cast<float>(h);

  for (uint16_t py = 0; py < h; py++) {
    float ci = y0 + (h - 1 - py) * dy;
    uint8_t* row = b->pixels + py * w;
    for (uint16_t px = 0; px < w; px++) {
      float cr = x0 + px * dx;
      float zr = 0.0f, zi = 0.0f;
      int iter = 0;
      while (iter < maxIter) {
        float zr2 = zr * zr;
        float zi2 = zi * zi;
        if (zr2 + zi2 > 4.0f) break;
        zi = 2.0f * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        iter++;
      }
      if (iter >= maxIter) {
        row[px] = 0;  // Inside Mandelbrot set -> black
      } else {
        row[px] = static_cast<uint8_t>(16 + ((iter * 7) % 208));
      }
    }
  }
  return rcvr;
}

// Fast toroidal Conway's Game of Life step (`nextBmp lifeFrom: curBmp`)
static Oop primBmpLifeFrom(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjBmp* dst = reqBmp(vm, rcvr, "lifeFrom:");
  ObjBmp* src = reqBmp(vm, args[0], "lifeFrom:");
  if (dst->wid != src->wid || dst->hei != src->hei || dst->wid == 0 || dst->hei == 0) {
    vm.toss("lifeFrom: Bmp dimensions must match");
  }
  const int w = dst->wid, h = dst->hei;
  for (int y = 0; y < h; y++) {
    int ym = (y == 0) ? (h - 1) : (y - 1);
    int yp = (y + 1 == h) ? 0 : (y + 1);
    const uint8_t* rM = src->pixels + ym * w;
    const uint8_t* r0 = src->pixels + y  * w;
    const uint8_t* rP = src->pixels + yp * w;
    uint8_t* outRow   = dst->pixels + y  * w;
    for (int x = 0; x < w; x++) {
      int xm = (x == 0) ? (w - 1) : (x - 1);
      int xp = (x + 1 == w) ? 0 : (x + 1);
      int n = (rM[xm] != 0) + (rM[x] != 0) + (rM[xp] != 0)
            + (r0[xm] != 0)                + (r0[xp] != 0)
            + (rP[xm] != 0) + (rP[x] != 0) + (rP[xp] != 0);
      uint8_t cur = r0[x];
      if (cur != 0) {
        if (n == 2 || n == 3) {
          // Age color gently along rainbow palette!
          outRow[x] = (cur >= 16 && cur < 220) ? static_cast<uint8_t>(cur + 2) : 48;
        } else {
          outRow[x] = 0;
        }
      } else {
        outRow[x] = (n == 3) ? 32 : 0;
      }
    }
  }
  return rcvr;
}

// ---------------------------------------------------------------------------
// RLE + Base64 Marshalling for Bmp (`Bmp >> pack`, `BmpCls >> unpack:`)
// ---------------------------------------------------------------------------
static std::string b64EncodeBytes(const uint8_t* data, size_t len) {
  static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2u) / 3u) * 4u);
  for (size_t i = 0; i < len; i += 3u) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1u < len) v |= static_cast<uint32_t>(data[i + 1u]) << 8;
    if (i + 2u < len) v |= static_cast<uint32_t>(data[i + 2u]);
    out.push_back(kTbl[(v >> 18) & 0x3Fu]);
    out.push_back(kTbl[(v >> 12) & 0x3Fu]);
    out.push_back((i + 1u < len) ? kTbl[(v >> 6) & 0x3Fu] : '=');
    out.push_back((i + 2u < len) ? kTbl[v & 0x3Fu] : '=');
  }
  return out;
}

static int b64Val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static void b64DecodeBytes(const char* s, size_t len, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve((len * 3u) / 4u);
  uint32_t buf = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '=') break;
    int v = b64Val(c);
    if (v < 0) continue;
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFFu));
    }
  }
}

static Oop primBmpPack(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjBmp* b = reqBmp(vm, rcvr, "pack");
  const size_t total = static_cast<size_t>(b->wid) * b->hei;
  const uint8_t* pix = b->pixels;

  // PackBits-style RLE:
  //   0x80 | (run - 3) + [val]       => repeat `val` 3..130 times
  //   0x00 | (lit - 1) + [b0..bn-1]  => literal 1..128 bytes
  std::vector<uint8_t> rle;
  rle.reserve(total / 4u + 32u);
  size_t i = 0;
  while (i < total) {
    size_t run = 1;
    while (i + run < total && run < 130u && pix[i + run] == pix[i]) run++;
    if (run >= 3u) {
      rle.push_back(static_cast<uint8_t>(0x80u | (run - 3u)));
      rle.push_back(pix[i]);
      i += run;
    } else {
      size_t lit = 1;
      while (i + lit < total && lit < 128u) {
        if (i + lit + 2u < total &&
            pix[i + lit] == pix[i + lit + 1u] &&
            pix[i + lit] == pix[i + lit + 2u]) {
          break;
        }
        lit++;
      }
      rle.push_back(static_cast<uint8_t>(lit - 1u));
      rle.insert(rle.end(), pix + i, pix + i + lit);
      i += lit;
    }
  }

  std::string b64 = b64EncodeBytes(rle.data(), rle.size());
  uint16_t w = b->wid, h = b->hei;
  std::string pal64;
  if (b->palette) {
    pal64 = b64EncodeBytes(reinterpret_cast<const uint8_t*>(b->palette), 256u * sizeof(uint16_t));
  }

  // Free vector temporary before allocating VM heap objects!
  rle.clear();
  rle.shrink_to_fit();

  Oop d = vm.newDict(8);
  TempRootGuard rg(vm, d);
  vm.dictPut(d, vm.newStr("__bmp"), makeInt30(1));
  vm.dictPut(d, vm.newStr("w"),     makeInt30(w));
  vm.dictPut(d, vm.newStr("h"),     makeInt30(h));
  Oop rleStr = vm.newStr(b64.data(), b64.size());
  rg.push(rleStr);
  vm.dictPut(d, vm.newStr("rle64"), rleStr);
  if (!pal64.empty()) {
    Oop pStr = vm.newStr(pal64.data(), pal64.size());
    rg.push(pStr);
    vm.dictPut(d, vm.newStr("pal64"), pStr);
  }
  return d;
}

static Oop primBmpClsUnpack(VM& vm, Oop, const Oop* args, uint8_t) {
  if (!isObj(args[0]) || objFromOop(args[0])->format != OBJ_DICT) {
    vm.toss("Bmp unpack: expected Dict");
  }
  Oop d = args[0];
  TempRootGuard rg(vm, d);
  int32_t w = reqNumI(vm, vm.dictGet(d, vm.newStr("w")), "Bmp unpack:");
  int32_t h = reqNumI(vm, vm.dictGet(d, vm.newStr("h")), "Bmp unpack:");
  Oop rleOop = vm.dictGet(d, vm.newStr("rle64"));
  Oop palOop = vm.dictGet(d, vm.newStr("pal64"));
  std::string rleB64 = vm.toStdString(rleOop);
  std::string palB64 = isNil(palOop) ? "" : vm.toStdString(palOop);

  Oop whArgs[2] = {makeInt30(w), makeInt30(h)};
  Oop bmpOop = primBmpClsNew(vm, kOopNil, whArgs, 2);
  rg.push(bmpOop);
  auto* bmp = reinterpret_cast<ObjBmp*>(objFromOop(bmpOop));

  std::vector<uint8_t> rle;
  b64DecodeBytes(rleB64.data(), rleB64.size(), rle);

  const size_t total = static_cast<size_t>(bmp->wid) * bmp->hei;
  size_t outPos = 0;
  size_t inPos  = 0;
  while (inPos < rle.size() && outPos < total) {
    uint8_t ctrl = rle[inPos++];
    if (ctrl & 0x80u) {
      if (inPos >= rle.size()) break;
      size_t run = static_cast<size_t>(ctrl & 0x7Fu) + 3u;
      uint8_t val = rle[inPos++];
      if (outPos + run > total) run = total - outPos;
      std::memset(bmp->pixels + outPos, val, run);
      outPos += run;
    } else {
      size_t lit = static_cast<size_t>(ctrl) + 1u;
      if (inPos + lit > rle.size()) lit = rle.size() - inPos;
      if (outPos + lit > total) lit = total - outPos;
      std::memcpy(bmp->pixels + outPos, rle.data() + inPos, lit);
      inPos  += lit;
      outPos += lit;
    }
  }

  if (!palB64.empty()) {
    std::vector<uint8_t> palBytes;
    b64DecodeBytes(palB64.data(), palB64.size(), palBytes);
    if (palBytes.size() >= 256u * sizeof(uint16_t)) {
      ensureCustomPalette(vm, bmp);
      std::memcpy(bmp->palette, palBytes.data(), 256u * sizeof(uint16_t));
    }
  }
  return bmpOop;
}

// ---------------------------------------------------------------------------
// Window Handle (`Win`) Primitives
// ---------------------------------------------------------------------------
static Window* reqWindow(VM& vm, Oop rcvr) {
  if (!gWm || !isObj(rcvr) || objFromOop(rcvr)->format != OBJ_WIN) {
    vm.toss("Expected Win");
  }
  auto* ow = reinterpret_cast<ObjWin*>(objFromOop(rcvr));
  return gWm->findById(ow->winId);
}

static Oop primWinClsOpenTitle(VM& vm, Oop, const Oop* args, uint8_t) {
  if (!gWm) return kOopNil;
  std::string title = vm.toStdString(args[1]);
  int cascade = (gWm->zCount % 5) * 10;
  Window* w = gWm->createTerseWindow(args[0], title.c_str(),
                                     static_cast<int16_t>(12 + cascade),
                                     static_cast<int16_t>(22 + cascade),
                                     256, 186);
  return w ? w->winObj : kOopNil;
}

static Oop primWinClsOpenRect(VM& vm, Oop, const Oop* args, uint8_t) {
  if (!gWm) return kOopNil;
  std::string title = vm.toStdString(args[1]);
  int16_t x = 0, y = 0, ww = 0, hh = 0;
  reqXYInt(vm, args[2], x, y, "Win open:title:at:size:");
  reqXYInt(vm, args[3], ww, hh, "Win open:title:at:size:");
  Window* w = gWm->createTerseWindow(args[0], title.c_str(), x, y, ww, hh);
  return w ? w->winObj : kOopNil;
}

static Oop runWinPrompt(VM& vm, Oop rcvr, const Oop* args, bool isMultiline) {
  if (!gWm) return rcvr;
  std::string title = vm.toStdString(args[0]);
  std::string init  = vm.toStdString(args[1]);
  Oop blk = args[2];
  if (!isObj(blk) || objFromOop(blk)->format != OBJ_BLK) {
    vm.toss("Win prompt expected Blk");
  }
  uint32_t pSym = vm.internSym("__prompt_blk");
  vm.dictPut(vm.globalsDict, makeSym(pSym), blk);
  VM* vmPtr = &vm;
  WindowManager* wmPtr = gWm;
  gWm->openTextPrompt(title.c_str(), init, isMultiline, [vmPtr, wmPtr, pSym](const std::string& result) {
    Oop b = vmPtr->dictGet(vmPtr->globalsDict, makeSym(pSym));
    vmPtr->dictDel(vmPtr->globalsDict, makeSym(pSym));
    if (isObj(b) && objFromOop(b)->format == OBJ_BLK) {
      Oop arg = vmPtr->newStr(result.data(), result.size());
      TempRootGuard rg(*vmPtr, arg);
      try {
        vmPtr->ticksRemaining = 2000000;
        vmPtr->evalBlk(b, &arg, 1);
      } catch (const TerseError& e) {
        wmPtr->showBanner(e.message);
      }
      wmPtr->invalidateAll();
    }
  });
  return rcvr;
}

static Oop primWinClsPrompt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return runWinPrompt(vm, rcvr, args, false);
}

static Oop primWinClsPromptMulti(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return runWinPrompt(vm, rcvr, args, true);
}

static Oop primWinClsBanner(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  if (gWm) {
    gWm->showBanner(vm.toStdString(args[0]));
  }
  return rcvr;
}

static Oop primWinTitleSet(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    w->title = vm.toStdString(args[0]);
    gWm->invalidateAll();
  }
  return rcvr;
}

static Oop primWinDirty(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    gWm->invalidateWindow(*w);
  }
  return rcvr;
}

static Oop primWinEvery(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    int32_t ms = reqNumI(vm, args[0], "every:");
    w->animIntervalMs = (ms > 0) ? static_cast<uint32_t>(ms) : 0u;
    w->lastStepMs     = gHal ? gHal->millis() : 0u;
  }
  return rcvr;
}

static Oop primWinMaximize(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (gWm) gWm->setMaximized(true);
  return rcvr;
}

static Oop primWinOverlap(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (gWm) gWm->setMaximized(false);
  return rcvr;
}

static Oop primWinClose(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    int slot = static_cast<int>(w - gWm->windows);
    gWm->closeWindowSlot(slot);
  }
  return kOopNil;
}

static Oop primWinWid(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    int16_t cx, cy, cw, ch;
    gWm->getClientRect(*w, gWm->topWindow() == w, cx, cy, cw, ch);
    return makeInt30(cw);
  }
  return makeInt30(0);
}

static Oop primWinHei(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (Window* w = reqWindow(vm, rcvr)) {
    int16_t cx, cy, cw, ch;
    gWm->getClientRect(*w, gWm->topWindow() == w, cx, cy, cw, ch);
    return makeInt30(ch);
  }
  return makeInt30(0);
}

}  // namespace

Oop createScreenPen(VM& vm, int16_t vpX, int16_t vpY, int16_t vpW, int16_t vpH) {
  auto* p = reinterpret_cast<ObjPen*>(vm.allocObj(CLS_PEN, OBJ_PEN, 0, sizeof(ObjPen)));
  p->vpX       = vpX;
  p->vpY       = vpY;
  p->vpW       = vpW;
  p->vpH       = vpH;
  p->targetBmp = kOopNil;
  p->color565  = Color::kWhite;
  p->palIdx    = 1;
  p->thickness = 1;
  p->font      = 1;
  p->penDown   = true;
  p->hasWorld  = false;
  p->turtleX   = vpW * 0.5f;
  p->turtleY   = vpH * 0.5f;
  p->turtleDeg = 0.0f;
  return oopFromObj(&p->hdr);
}

void installGraphicsPrimitives(VM& vm, DisplayHal* hal, WindowManager* wm) {
  gHal = hal;
  gWm  = wm;
  (void)primPenWid;

  // Pen / Ink
  vm.addPrim(CLS_PEN, "wid",          "w",          primPenWidSafe);
  vm.addPrim(CLS_PEN, "hei",          "h",          primPenHeiSafe);
  vm.addPrim(CLS_PEN, "size",         "",           primPenSize);
  vm.addPrim(CLS_PEN, "clear",        "clsPen",     primPenClear);
  vm.addPrim(CLS_PEN, "clear:",       "bg:",        primPenClearCol);
  vm.addPrim(CLS_PEN, "color:",       "fg:",        primPenColor999);
  vm.addPrim(CLS_PEN, "ink:",         "",           primPenColor999);
  vm.addPrim(CLS_PEN, "rgb:g:b:",     "",           primPenRgb);
  vm.addPrim(CLS_PEN, "hue:",         "",           primPenHue);
  vm.addPrim(CLS_PEN, "hsv:s:v:",     "",           primPenHsv);
  vm.addPrim(CLS_PEN, "pal:",         "",           primPenPal);
  vm.addPrim(CLS_PEN, "black",        "",           primPenBlack);
  vm.addPrim(CLS_PEN, "white",        "",           primPenWhite);
  vm.addPrim(CLS_PEN, "red",          "",           primPenRed);
  vm.addPrim(CLS_PEN, "green",        "",           primPenGreen);
  vm.addPrim(CLS_PEN, "blue",         "",           primPenBlue);
  vm.addPrim(CLS_PEN, "yellow",       "",           primPenYellow);
  vm.addPrim(CLS_PEN, "cyan",         "",           primPenCyan);
  vm.addPrim(CLS_PEN, "magenta",      "",           primPenMagenta);
  vm.addPrim(CLS_PEN, "orange",       "",           primPenOrange);
  vm.addPrim(CLS_PEN, "gray",         "",           primPenGray);
  vm.addPrim(CLS_PEN, "thick:",       "",           primPenThick);
  vm.addPrim(CLS_PEN, "scale:",       "",           primPenScale);

  vm.addPrim(CLS_PEN, "dot:",         "",           primPenDot);
  vm.addPrim(CLS_PEN, "pix:y:",       "",           primPenPixXY);
  vm.addPrim(CLS_PEN, "line:to:",     "",           primPenLineTo);
  vm.addPrim(CLS_PEN, "box:size:",    "fill:size:", primPenBoxSize);
  vm.addPrim(CLS_PEN, "rect:size:",   "",           primPenRectSize);
  vm.addPrim(CLS_PEN, "rbox:size:r:", "",           primPenRBox);
  vm.addPrim(CLS_PEN, "rrect:size:r:","",           primPenRRect);
  vm.addPrim(CLS_PEN, "circ:r:",      "",           primPenCircR);
  vm.addPrim(CLS_PEN, "disc:r:",      "",           primPenDiscR);
  vm.addPrim(CLS_PEN, "tri:p2:p3:",   "",           primPenTri);
  vm.addPrim(CLS_PEN, "ftri:p2:p3:",  "",           primPenFTri);
  vm.addPrim(CLS_PEN, "poly:",        "",           primPenPoly);
  vm.addPrim(CLS_PEN, "text:at:",     "",           primPenTextAt);
  vm.addPrim(CLS_PEN, "at:text:",     "",           primPenAtText);
  vm.addPrim(CLS_PEN, "fontW",        "",           primPenFontW);
  vm.addPrim(CLS_PEN, "fontH",        "",           primPenFontH);
  vm.addPrim(CLS_PEN, "fontSize",     "",           primPenFontSize);
  vm.addPrim(CLS_PEN, "scrollBar:at:size:top:", "", primPenScrollBar);
  vm.addPrim(CLS_PEN, "blit:",        "",           primPenBlitFull);
  vm.addPrim(CLS_PEN, "blit:at:",     "",           primPenBlitAt);
  vm.addPrim(CLS_PEN, "blit:at:size:","",           primPenBlitAtSize);

  vm.addPrim(CLS_PEN, "world:to:",    "",           primPenWorldTo);
  vm.addPrim(CLS_PEN, "screenCoords", "",           primPenScreenCoords);
  vm.addPrim(CLS_PEN, "toScreen:",    "",           primPenToScreen);
  vm.addPrim(CLS_PEN, "toWorld:",     "",           primPenToWorld);
  vm.addPrim(CLS_PEN, "axes",         "",           primPenAxes);
  vm.addPrim(CLS_PEN, "wdot:",        "",           primPenWDot);
  vm.addPrim(CLS_PEN, "wline:to:",    "",           primPenWLineTo);
  vm.addPrim(CLS_PEN, "plot:",        "",           primPenPlot);
  vm.addPrim(CLS_PEN, "param:from:to:steps:", "",   primPenParam);

  vm.addPrim(CLS_PEN, "home",         "",           primPenHome);
  vm.addPrim(CLS_PEN, "goto:",        "",           primPenGoto);
  vm.addPrim(CLS_PEN, "jump:",        "",           primPenJump);
  vm.addPrim(CLS_PEN, "go:",          "fd:",        primPenGo);
  vm.addPrim(CLS_PEN, "turn:",        "",           primPenTurn);
  vm.addPrim(CLS_PEN, "rt:",          "",           primPenRt);
  vm.addPrim(CLS_PEN, "head:",        "",           primPenHead);
  vm.addPrim(CLS_PEN, "down",         "pd",         primPenDown);
  vm.addPrim(CLS_PEN, "upPen",        "pu",         primPenUp);
  vm.addPrim(CLS_PEN, "pos",          "",           primPenPos);
  vm.addPrim(CLS_PEN, "heading",      "",           primPenHeading);

  // Bmp
  vm.addPrim(CLS_BMP_CLS, "wid:hei:", "new:hei:",   primBmpClsNew);
  vm.addPrim(CLS_BMP_CLS, "unpack:",  "",           primBmpClsUnpack);
  vm.addPrim(CLS_BMP, "wid",          "w",          primBmpWid);
  vm.addPrim(CLS_BMP, "hei",          "h",          primBmpHei);
  vm.addPrim(CLS_BMP, "clear",        "",           primBmpClear);
  vm.addPrim(CLS_BMP, "clear:",       "",           primBmpClearIdx);
  vm.addPrim(CLS_BMP, "at:y:",        "",           primBmpAtXY);
  vm.addPrim(CLS_BMP, "wrap:y:",      "",           primBmpWrapAtXY);
  vm.addPrim(CLS_BMP, "at:y:put:",    "",           primBmpAtXYPut);
  vm.addPrim(CLS_BMP, "at:",          "",           primBmpAtPt);
  vm.addPrim(CLS_BMP, "at:put:",      "",           primBmpAtPtPut);
  vm.addPrim(CLS_BMP, "pal:color:",   "",           primBmpPalRgb999);
  vm.addPrim(CLS_BMP, "pen",          "",           primBmpPen);
  vm.addPrim(CLS_BMP, "pack",         "",           primBmpPack);
  vm.addPrim(CLS_BMP, "mandel:span:max:", "",       primBmpMandel);
  vm.addPrim(CLS_BMP, "lifeFrom:",    "",           primBmpLifeFrom);

  // Win
  vm.addPrim(CLS_WIN_CLS, "open:title:", "",        primWinClsOpenTitle);
  vm.addPrim(CLS_WIN_CLS, "open:title:at:size:", "", primWinClsOpenRect);
  vm.addPrim(CLS_WIN_CLS, "prompt:init:then:", "",  primWinClsPrompt);
  vm.addPrim(CLS_WIN_CLS, "promptMulti:init:then:", "", primWinClsPromptMulti);
  vm.addPrim(CLS_WIN_CLS, "banner:",  "",           primWinClsBanner);
  vm.addPrim(CLS_WIN_CLS, "scrollHit:at:size:lines:top:", "", primWinScrollHit);
  vm.addPrim(CLS_WIN, "title:",       "",           primWinTitleSet);

  vm.addPrim(CLS_SYS_CLS, "fontSize",   "",         primSysFontSize);
  vm.addPrim(CLS_SYS_CLS, "fontSize:",  "",         primSysFontSizeSet);
  vm.addPrim(CLS_SYS_CLS, "brightness", "",         primSysBrightness);
  vm.addPrim(CLS_SYS_CLS, "brightness:","",         primSysBrightnessSet);
  vm.addPrim(CLS_WIN, "dirty",        "redraw",     primWinDirty);
  vm.addPrim(CLS_WIN, "every:",       "",           primWinEvery);
  vm.addPrim(CLS_WIN, "maximize",     "",           primWinMaximize);
  vm.addPrim(CLS_WIN, "overlap",      "",           primWinOverlap);
  vm.addPrim(CLS_WIN, "close",        "",           primWinClose);
  vm.addPrim(CLS_WIN, "wid",          "",           primWinWid);
  vm.addPrim(CLS_WIN, "hei",          "",           primWinHei);
}

}  // namespace tt
