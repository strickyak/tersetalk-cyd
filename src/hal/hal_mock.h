#pragma once

#include "display_hal.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace tt {

struct MockTouchSample {
  bool    pressed = false;
  int16_t x       = 0;
  int16_t y       = 0;
};

class HalMock : public DisplayHal {
 public:
  uint16_t fb[kScreenW * kScreenH];
  uint32_t fakeMillis = 1000;

  std::deque<MockTouchSample> touchQueue;
  MockTouchSample             currentTouch{};

  std::deque<std::string>     serialInLines;
  std::string                 serialOutLog;

  // In-memory VFS for `/flash/...` and `/sd/...` so host tests are hermetic & fast
  std::map<std::string, std::string> vfsFiles;

  HalMock();
  ~HalMock() override = default;

  bool init() override;
  void drawPixel(int16_t x, int16_t y, uint16_t color565) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) override;

  bool pollTouch(int16_t& outX, int16_t& outY) override;
  uint32_t millis() const override { return fakeMillis; }
  void advanceMillis(uint32_t delta) { fakeMillis += delta; }

  bool pollSerialLine(std::string& outLine) override;
  void serialPrint(const char* s) override;

  void attachToVm(VM& vm) override;

  // Helpers for automated UI tests & visual inspection
  void pushTap(int16_t x, int16_t y, int holdFrames = 2);
  void pushStroke(const std::vector<std::pair<int16_t, int16_t>>& pts);
  void pushSerialCommand(const std::string& line);
  uint16_t pixelAt(int16_t x, int16_t y) const;
  bool savePPM(const char* filename) const;
};

}  // namespace tt
