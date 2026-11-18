#include "hal_mock.h"
#include "vm/tt_world.h"

#include <cstdio>
#include <cstring>

namespace tt {

HalMock::HalMock() {
  std::memset(fb, 0, sizeof(fb));
}

bool HalMock::init() {
  resetClipRect();
  clear(Color::kDarkBg);
  if (kEmbeddedPreludeLen > 0) {
    vfsFiles["/flash/prelude.tt"] = std::string(kEmbeddedPrelude, kEmbeddedPreludeLen);
  }
  return true;
}

void HalMock::drawPixel(int16_t x, int16_t y, uint16_t color565) {
  if (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1) return;
  fb[y * kScreenW + x] = color565;
}

void HalMock::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565) {
  if (w <= 0 || h <= 0) return;
  int16_t x0 = (x < clipX0) ? clipX0 : x;
  int16_t y0 = (y < clipY0) ? clipY0 : y;
  int16_t x1 = (x + w > clipX1) ? clipX1 : static_cast<int16_t>(x + w);
  int16_t y1 = (y + h > clipY1) ? clipY1 : static_cast<int16_t>(y + h);
  if (x0 >= x1 || y0 >= y1) return;

  for (int16_t py = y0; py < y1; py++) {
    uint16_t* row = fb + py * kScreenW;
    for (int16_t px = x0; px < x1; px++) {
      row[px] = color565;
    }
  }
}

bool HalMock::pollTouch(int16_t& outX, int16_t& outY) {
  if (!touchQueue.empty()) {
    currentTouch = touchQueue.front();
    touchQueue.pop_front();
  } else {
    currentTouch = MockTouchSample{false, 0, 0};
  }
  if (currentTouch.pressed) {
    outX = currentTouch.x;
    outY = currentTouch.y;
    return true;
  }
  return false;
}

bool HalMock::pollSerialLine(std::string& outLine) {
  if (serialInLines.empty()) return false;
  outLine = serialInLines.front();
  serialInLines.pop_front();
  return true;
}

void HalMock::serialPrint(const char* s) {
  if (s) serialOutLog += s;
}

void HalMock::attachToVm(VM& vm) {
  vm.host.user     = this;
  vm.host.millisFn = [](void* u) -> uint32_t {
    return static_cast<HalMock*>(u)->millis();
  };
  vm.host.vfsRead = [](void* u, const char* path, std::string& out) -> bool {
    auto* self = static_cast<HalMock*>(u);
    auto it = self->vfsFiles.find(path ? path : "");
    if (it == self->vfsFiles.end()) return false;
    out = it->second;
    return true;
  };
  vm.host.vfsWrite = [](void* u, const char* path, const char* data, size_t len, bool append) -> bool {
    auto* self = static_cast<HalMock*>(u);
    if (!path || !path[0]) return false;
    std::string& slot = self->vfsFiles[path];
    if (!append) slot.clear();
    if (len > 0 && data) slot.append(data, len);
    return true;
  };
  vm.host.vfsDelete = [](void* u, const char* path) -> bool {
    auto* self = static_cast<HalMock*>(u);
    return self->vfsFiles.erase(path ? path : "") > 0;
  };
  vm.host.vfsList = [](void* u, const char* dir, std::vector<std::pair<std::string, uint32_t>>& out) -> bool {
    auto* self = static_cast<HalMock*>(u);
    out.clear();
    std::string prefix = (dir && dir[0]) ? dir : "/sd";
    if (prefix.back() != '/') prefix.push_back('/');
    for (const auto& kv : self->vfsFiles) {
      if (kv.first.rfind(prefix, 0) == 0) {
        std::string rest = kv.first.substr(prefix.size());
        if (!rest.empty() && rest.find('/') == std::string::npos) {
          out.emplace_back(rest, static_cast<uint32_t>(kv.second.size()));
        }
      }
    }
    return true;
  };
}

void HalMock::pushTap(int16_t x, int16_t y, int holdFrames) {
  for (int i = 0; i < holdFrames; i++) {
    touchQueue.push_back(MockTouchSample{true, x, y});
  }
  touchQueue.push_back(MockTouchSample{false, x, y});
  touchQueue.push_back(MockTouchSample{false, x, y});
}

void HalMock::pushStroke(const std::vector<std::pair<int16_t, int16_t>>& pts) {
  for (const auto& pt : pts) {
    touchQueue.push_back(MockTouchSample{true, pt.first, pt.second});
  }
  if (!pts.empty()) {
    touchQueue.push_back(MockTouchSample{false, pts.back().first, pts.back().second});
  }
  touchQueue.push_back(MockTouchSample{false, 0, 0});
}

void HalMock::pushSerialCommand(const std::string& line) {
  serialInLines.push_back(line);
}

uint16_t HalMock::pixelAt(int16_t x, int16_t y) const {
  if (x < 0 || x >= kScreenW || y < 0 || y >= kScreenH) return 0;
  return fb[y * kScreenW + x];
}

bool HalMock::savePPM(const char* filename) const {
  FILE* f = std::fopen(filename, "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", kScreenW, kScreenH);
  uint8_t row[kScreenW * 3];
  for (int y = 0; y < kScreenH; y++) {
    for (int x = 0; x < kScreenW; x++) {
      uint16_t c = fb[y * kScreenW + x];
      uint8_t r5 = (c >> 11) & 0x1Fu;
      uint8_t g6 = (c >> 5)  & 0x3Fu;
      uint8_t b5 =  c        & 0x1Fu;
      row[3 * x + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
      row[3 * x + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
      row[3 * x + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    }
    std::fwrite(row, 1, sizeof(row), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace tt
