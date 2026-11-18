#pragma once

#include "hal/display_hal.h"
#include "tt_graffiti.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tt {

class VirtualKeyboard {
 public:
  bool        active        = false;
  bool        graffitiMode  = false;
  bool        multiline     = true;
  uint8_t     shiftState    = 0;  // 0 = lower, 1 = one-shot upper, 2 = caps lock
  std::string title;
  std::string text;
  size_t      cursor        = 0;
  char        lastGrafChar  = 0;

  std::vector<GraffitiPt> strokePts;
  uint8_t                 strokePane = GRAFFITI_LETTERS;

  std::function<void(const std::string&)> onAccept;

  void open(const char* promptTitle, const std::string& initialText,
            bool isMultiline, std::function<void(const std::string&)> acceptCb);
  void close();

  void render(DisplayHal& hal);
  void onTouchDown(DisplayHal& hal, int16_t tx, int16_t ty, bool& outNeedsRedraw);
  void onTouchMove(DisplayHal& hal, int16_t tx, int16_t ty, bool& outNeedsRedraw);
  void onTouchUp(DisplayHal& hal, int16_t tx, int16_t ty, bool& outNeedsRedraw);

  void insertStr(const char* s);
  void insertChar(char c);
  void backspace();
};

}  // namespace tt
