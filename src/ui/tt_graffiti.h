#pragma once

#include <cstdint>

namespace tt {

enum GraffitiPane : uint8_t {
  GRAFFITI_LETTERS = 1,
  GRAFFITI_DIGITS  = 2,
};

static constexpr char kGraffitiNone      = 0;
static constexpr char kGraffitiBackspace = '\b';
static constexpr char kGraffitiShift     = '\x0E';
static constexpr char kGraffitiReturn    = '\n';

struct GraffitiPt {
  int16_t x;
  int16_t y;
};

struct GraffitiResult {
  char    ch;
  uint8_t confidence;
};

GraffitiResult recognizeGraffitiStroke(const GraffitiPt* pts, int count, uint8_t pane);

}  // namespace tt
