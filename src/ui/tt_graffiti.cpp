#include "tt_graffiti.h"

#ifdef ARDUINO
#include <Arduino.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P std::memcpy
#endif
#endif

#include <cmath>
#include <cstring>

namespace tt {

namespace {

static constexpr int kNumResamplePts = 16;
static constexpr int kMaxStrokeWork  = 128;

struct FloatPt {
  float x;
  float y;
};

struct GraffitiTemplate {
  char    ch;
  uint8_t pane;
  uint8_t numPts;
  uint8_t pts[14][2];
};

static const GraffitiTemplate kTemplates[] PROGMEM = {
  // ======================= LEFT PANE: LETTERS (a-z) ========================
  {'a', GRAFFITI_LETTERS, 3,  {{5, 95}, {50, 5}, {95, 95}}},
  {'b', GRAFFITI_LETTERS, 13, {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {90, 18}, {90, 36}, {68, 48}, {20, 48},
                               {70, 50}, {94, 62}, {94, 82}, {68, 94}, {12, 94}}},
  {'b', GRAFFITI_LETTERS, 12, {{12, 94}, {12, 8}, {68, 8}, {90, 18}, {90, 36}, {68, 48}, {20, 48},
                               {70, 50}, {94, 62}, {94, 82}, {68, 94}, {12, 94}}},
  {'c', GRAFFITI_LETTERS, 6,  {{90, 12}, {35, 5}, {8, 28}, {8, 72}, {35, 95}, {90, 88}}},
  {'d', GRAFFITI_LETTERS, 9,  {{12, 6}, {12, 94}, {16, 8}, {62, 8}, {90, 24}, {95, 50}, {90, 76}, {62, 94}, {12, 94}}},
  {'d', GRAFFITI_LETTERS, 8,  {{12, 94}, {12, 8}, {62, 8}, {90, 24}, {95, 50}, {90, 76}, {62, 94}, {12, 94}}},
  {'e', GRAFFITI_LETTERS, 7,  {{90, 10}, {18, 8}, {8, 32}, {75, 50}, {8, 68}, {18, 92}, {90, 90}}},
  {'f', GRAFFITI_LETTERS, 3,  {{95, 8}, {12, 8}, {12, 95}}},
  {'g', GRAFFITI_LETTERS, 7,  {{90, 15}, {22, 5}, {5, 50}, {22, 95}, {90, 85}, {90, 50}, {48, 50}}},
  {'h', GRAFFITI_LETTERS, 6,  {{12, 5}, {12, 95}, {18, 50}, {55, 45}, {88, 55}, {90, 95}}},
  {'i', GRAFFITI_LETTERS, 2,  {{50, 5}, {50, 95}}},
  {'j', GRAFFITI_LETTERS, 5,  {{82, 5}, {82, 72}, {65, 94}, {35, 94}, {10, 72}}},
  {'k', GRAFFITI_LETTERS, 7,  {{90, 8}, {18, 86}, {8, 68}, {8, 38}, {26, 30}, {48, 42}, {92, 94}}},
  {'k', GRAFFITI_LETTERS, 6,  {{10, 5}, {10, 95}, {10, 50}, {88, 10}, {22, 50}, {90, 95}}},
  {'l', GRAFFITI_LETTERS, 3,  {{12, 5}, {12, 95}, {95, 95}}},
  {'m', GRAFFITI_LETTERS, 5,  {{5, 95}, {15, 8}, {50, 82}, {85, 8}, {95, 95}}},
  {'n', GRAFFITI_LETTERS, 4,  {{8, 95}, {8, 8}, {92, 95}, {92, 8}}},
  {'o', GRAFFITI_LETTERS, 9,  {{50, 5}, {18, 15}, {5, 50}, {18, 85}, {50, 95}, {82, 85}, {95, 50}, {82, 15}, {50, 5}}},
  {'o', GRAFFITI_LETTERS, 9,  {{50, 5}, {82, 15}, {95, 50}, {82, 85}, {50, 95}, {18, 85}, {5, 50}, {18, 15}, {50, 5}}},
  {'p', GRAFFITI_LETTERS, 8,  {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {92, 20}, {92, 40}, {68, 52}, {12, 52}}},
  {'p', GRAFFITI_LETTERS, 7,  {{12, 94}, {12, 8}, {68, 8}, {92, 20}, {92, 40}, {68, 52}, {12, 52}}},
  {'q', GRAFFITI_LETTERS, 8,  {{50, 8}, {8, 28}, {8, 72}, {48, 95}, {85, 72}, {82, 25}, {45, 8}, {98, 22}}},
  {'r', GRAFFITI_LETTERS, 9,  {{12, 6}, {12, 94}, {16, 8}, {68, 8}, {90, 20}, {90, 40}, {65, 50}, {18, 50}, {94, 94}}},
  {'r', GRAFFITI_LETTERS, 8,  {{12, 94}, {12, 8}, {68, 8}, {90, 20}, {90, 40}, {65, 50}, {18, 50}, {94, 94}}},
  {'s', GRAFFITI_LETTERS, 6,  {{88, 15}, {25, 5}, {8, 32}, {92, 68}, {75, 95}, {12, 85}}},
  {'t', GRAFFITI_LETTERS, 3,  {{5, 8}, {92, 8}, {92, 95}}},
  {'u', GRAFFITI_LETTERS, 6,  {{10, 8}, {10, 70}, {30, 94}, {70, 94}, {90, 70}, {90, 8}}},
  {'v', GRAFFITI_LETTERS, 4,  {{5, 8}, {38, 95}, {72, 8}, {98, 8}}},
  {'v', GRAFFITI_LETTERS, 3,  {{8, 8}, {50, 95}, {92, 8}}},
  {'w', GRAFFITI_LETTERS, 5,  {{5, 8}, {22, 95}, {50, 22}, {78, 95}, {95, 8}}},
  {'x', GRAFFITI_LETTERS, 4,  {{10, 8}, {90, 92}, {90, 8}, {10, 92}}},
  {'y', GRAFFITI_LETTERS, 7,  {{8, 8}, {45, 45}, {88, 8}, {88, 88}, {42, 96}, {20, 72}, {88, 55}}},
  {'y', GRAFFITI_LETTERS, 4,  {{8, 8}, {50, 50}, {92, 8}, {15, 95}}},
  {'z', GRAFFITI_LETTERS, 4,  {{8, 8}, {92, 8}, {8, 92}, {92, 92}}},

  // =================== RIGHT PANE: DIGITS (0-9) & MATH =====================
  {'0', GRAFFITI_DIGITS, 7,   {{50, 5}, {10, 25}, {5, 72}, {50, 95}, {95, 72}, {90, 25}, {50, 5}}},
  {'0', GRAFFITI_DIGITS, 7,   {{50, 5}, {90, 25}, {95, 72}, {50, 95}, {5, 72}, {10, 25}, {50, 5}}},
  {'1', GRAFFITI_DIGITS, 2,   {{50, 5}, {50, 95}}},
  {'2', GRAFFITI_DIGITS, 5,   {{10, 25}, {50, 5}, {90, 25}, {8, 95}, {95, 95}}},
  {'3', GRAFFITI_DIGITS, 7,   {{12, 10}, {88, 10}, {85, 42}, {25, 50}, {90, 60}, {85, 92}, {12, 90}}},
  {'4', GRAFFITI_DIGITS, 3,   {{12, 5}, {12, 92}, {95, 92}}},
  {'5', GRAFFITI_DIGITS, 7,   {{92, 8}, {12, 8}, {12, 45}, {85, 45}, {95, 78}, {50, 95}, {10, 85}}},
  {'6', GRAFFITI_DIGITS, 7,   {{80, 5}, {12, 42}, {10, 82}, {52, 96}, {92, 78}, {80, 50}, {15, 55}}},
  {'7', GRAFFITI_DIGITS, 3,   {{8, 8}, {95, 8}, {28, 95}}},
  {'8', GRAFFITI_DIGITS, 7,   {{50, 5}, {10, 22}, {90, 75}, {50, 95}, {10, 75}, {90, 22}, {50, 5}}},
  {'8', GRAFFITI_DIGITS, 7,   {{50, 5}, {90, 22}, {10, 75}, {50, 95}, {90, 75}, {10, 22}, {50, 5}}},
  {'9', GRAFFITI_DIGITS, 6,   {{88, 25}, {45, 5}, {8, 25}, {45, 48}, {88, 25}, {88, 95}}},
  {'+', GRAFFITI_DIGITS, 4,   {{50, 8}, {50, 92}, {8, 50}, {92, 50}}},
  {'-', GRAFFITI_DIGITS, 2,   {{10, 50}, {90, 50}}},
  {'*', GRAFFITI_DIGITS, 4,   {{10, 10}, {90, 90}, {90, 10}, {10, 90}}},
  {'/', GRAFFITI_DIGITS, 2,   {{85, 10}, {15, 90}}},
  {'=', GRAFFITI_DIGITS, 4,   {{10, 28}, {90, 28}, {10, 72}, {90, 72}}},
  {'(', GRAFFITI_DIGITS, 5,   {{68, 5}, {35, 25}, {25, 50}, {35, 75}, {68, 95}}},
  {')', GRAFFITI_DIGITS, 5,   {{32, 5}, {65, 25}, {75, 50}, {65, 75}, {32, 95}}},
};

static constexpr int kNumTemplates = sizeof(kTemplates) / sizeof(kTemplates[0]);

static float polyPathLength(const FloatPt* pts, int n) {
  float len = 0.0f;
  for (int i = 1; i < n; i++) {
    len += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
  }
  return len;
}

static float resampleAndNormalize16(const FloatPt* inPts, int inCount, FloatPt out16[kNumResamplePts]) {
  if (inCount <= 0) {
    std::memset(out16, 0, sizeof(FloatPt) * kNumResamplePts);
    return 0.5f;
  }
  if (inCount == 1) {
    for (int i = 0; i < kNumResamplePts; i++) out16[i] = inPts[0];
    return 0.5f;
  }
  float totalLen = polyPathLength(inPts, inCount);
  if (totalLen < 1e-3f) {
    for (int i = 0; i < kNumResamplePts; i++) out16[i] = inPts[0];
    return 0.5f;
  }

  float step = totalLen / static_cast<float>(kNumResamplePts - 1);
  out16[0]   = inPts[0];
  int outIdx = 1;
  float accum = 0.0f;
  FloatPt prev = inPts[0];
  int i = 1;
  while (i < inCount && outIdx < kNumResamplePts - 1) {
    FloatPt cur = inPts[i];
    float d = std::hypot(cur.x - prev.x, cur.y - prev.y);
    if (accum + d >= step && d > 1e-4f) {
      float t = (step - accum) / d;
      FloatPt q = {prev.x + t * (cur.x - prev.x), prev.y + t * (cur.y - prev.y)};
      out16[outIdx++] = q;
      prev  = q;
      accum = 0.0f;
    } else {
      accum += d;
      prev = cur;
      i++;
    }
  }
  while (outIdx < kNumResamplePts) {
    out16[outIdx++] = inPts[inCount - 1];
  }

  float minX = out16[0].x, maxX = out16[0].x;
  float minY = out16[0].y, maxY = out16[0].y;
  for (int k = 1; k < kNumResamplePts; k++) {
    if (out16[k].x < minX) minX = out16[k].x;
    if (out16[k].x > maxX) maxX = out16[k].x;
    if (out16[k].y < minY) minY = out16[k].y;
    if (out16[k].y > maxY) maxY = out16[k].y;
  }
  float spanX  = maxX - minX;
  float spanY  = maxY - minY;
  float cx     = 0.5f * (minX + maxX);
  float cy     = 0.5f * (minY + maxY);
  float scaleX = std::fmax(spanX, 0.42f * spanY);
  float scaleY = std::fmax(spanY, 0.42f * spanX);
  if (scaleX < 1.0f) scaleX = 1.0f;
  if (scaleY < 1.0f) scaleY = 1.0f;

  for (int k = 0; k < kNumResamplePts; k++) {
    out16[k].x = (out16[k].x - cx) * (200.0f / scaleX);
    out16[k].y = (out16[k].y - cy) * (200.0f / scaleY);
  }
  float denom = spanX + spanY;
  return (denom > 1e-3f) ? (spanX / denom) : 0.5f;
}

static float weightedPathDistance(const FloatPt u[kNumResamplePts],
                                  const FloatPt t[kNumResamplePts],
                                  float cosA, float sinA) {
  float sum = 0.0f, wSum = 0.0f;
  for (int i = 0; i < kNumResamplePts; i++) {
    float rx = u[i].x * cosA - u[i].y * sinA;
    float ry = u[i].x * sinA + u[i].y * cosA;
    float d  = std::hypot(rx - t[i].x, ry - t[i].y);
    float w  = (i == 0 || i == kNumResamplePts - 1) ? 1.6f
             : ((i == 1 || i == kNumResamplePts - 2) ? 1.25f : 1.0f);
    sum  += w * d;
    wSum += w;
  }
  return sum / wSum;
}

}  // namespace

GraffitiResult recognizeGraffitiStroke(const GraffitiPt* pts, int count, uint8_t pane) {
  GraffitiResult res{kGraffitiNone, 0};
  if (!pts || count <= 0) return res;

  FloatPt work[kMaxStrokeWork];
  int n = 0;
  for (int i = 0; i < count && n < kMaxStrokeWork; i++) {
    float x = static_cast<float>(pts[i].x);
    float y = static_cast<float>(pts[i].y);
    if (n == 0 || std::fabs(x - work[n - 1].x) >= 1.0f || std::fabs(y - work[n - 1].y) >= 1.0f) {
      work[n++] = {x, y};
    }
  }

  float minX = work[0].x, maxX = work[0].x;
  float minY = work[0].y, maxY = work[0].y;
  for (int i = 1; i < n; i++) {
    if (work[i].x < minX) minX = work[i].x;
    if (work[i].x > maxX) maxX = work[i].x;
    if (work[i].y < minY) minY = work[i].y;
    if (work[i].y > maxY) maxY = work[i].y;
  }
  float spanX  = maxX - minX;
  float spanY  = maxY - minY;
  float rawLen = polyPathLength(work, n);

  if (rawLen < 12.0f && spanX < 10.0f && spanY < 10.0f) {
    res.ch         = '.';
    res.confidence = 99;
    return res;
  }

  if (n >= 5) {
    FloatPt prev = work[0];
    for (int i = 1; i < n - 1; i++) {
      FloatPt cur = work[i];
      work[i].x = 0.25f * prev.x + 0.50f * cur.x + 0.25f * work[i + 1].x;
      work[i].y = 0.25f * prev.y + 0.50f * cur.y + 0.25f * work[i + 1].y;
      prev = cur;
    }
  }

  float smoothLen = polyPathLength(work, n);
  float dx0N      = work[n - 1].x - work[0].x;
  float dy0N      = work[n - 1].y - work[0].y;
  float endDist   = std::hypot(dx0N, dy0N);

  if (smoothLen >= 13.0f && endDist / smoothLen >= 0.85f) {
    float deg = std::atan2(dy0N, dx0N) * 57.2957795f;
    if (deg >= -28.0f && deg <= 28.0f) {
      res.ch         = (pane == GRAFFITI_DIGITS) ? '-' : ' ';
      res.confidence = 95;
      return res;
    }
    if (deg >= 152.0f || deg <= -152.0f) {
      res.ch         = kGraffitiBackspace;
      res.confidence = 95;
      return res;
    }
    if (deg >= -118.0f && deg <= -62.0f) {
      res.ch         = kGraffitiShift;
      res.confidence = 95;
      return res;
    }
    if (deg >= 62.0f && deg <= 118.0f) {
      res.ch         = (pane == GRAFFITI_DIGITS) ? '1' : 'i';
      res.confidence = 95;
      return res;
    }
    if (deg > 118.0f && deg < 152.0f) {
      // Palm Graffiti Return slash (top-right -> bottom-left) in Letters pane, '/' in Digits pane!
      res.ch         = (pane == GRAFFITI_LETTERS) ? kGraffitiReturn : '/';
      res.confidence = 92;
      return res;
    }
    if (deg > -62.0f && deg < -28.0f) {
      res.ch         = '/';
      res.confidence = 92;
      return res;
    }
  }

  FloatPt user16[kNumResamplePts];
  float userAspect = resampleAndNormalize16(work, n, user16);

  static const float kCosA[3] = {0.9876883f, 1.0f, 0.9876883f};
  static const float kSinA[3] = {-0.1564345f, 0.0f, 0.1564345f};

  float bestScore = 1e9f;
  char  bestChar  = kGraffitiNone;

  for (int tIdx = 0; tIdx < kNumTemplates; tIdx++) {
    GraffitiTemplate tpl;
    memcpy_P(&tpl, &kTemplates[tIdx], sizeof(GraffitiTemplate));
    if (tpl.pane != pane || tpl.numPts < 2) continue;

    FloatPt tplRaw[14];
    for (int k = 0; k < tpl.numPts; k++) {
      tplRaw[k].x = static_cast<float>(tpl.pts[k][0]);
      tplRaw[k].y = static_cast<float>(tpl.pts[k][1]);
    }
    FloatPt tpl16[kNumResamplePts];
    float tplAspect = resampleAndNormalize16(tplRaw, tpl.numPts, tpl16);
    float aspectPen = 24.0f * std::fabs(userAspect - tplAspect);

    for (int a = 0; a < 3; a++) {
      float d = weightedPathDistance(user16, tpl16, kCosA[a], kSinA[a]) + aspectPen;
      if (d < bestScore) {
        bestScore = d;
        bestChar  = tpl.ch;
      }
    }
  }

  if (bestChar != kGraffitiNone && bestScore < 115.0f) {
    int conf = static_cast<int>(100.0f - bestScore * 0.7f);
    if (conf < 10) conf = 10;
    if (conf > 99) conf = 99;
    res.ch         = bestChar;
    res.confidence = static_cast<uint8_t>(conf);
  }
  return res;
}

}  // namespace tt
