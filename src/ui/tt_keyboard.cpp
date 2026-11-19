#include "tt_keyboard.h"

#include <cctype>
#include <cstring>

namespace tt {

namespace {

// Keep VirtualKeyboard fixed at the compact 5x7 font (6x8 cell)
static constexpr int16_t kFontW = 6;
static constexpr int16_t kFontH = 8;

struct QuickToken {
  const char* label;
  const char* insert;
};

static const QuickToken kQuickBar[] = {
  {":",   ": "},
  {"$",   "$ "},
  {"@",   " @ "},
  {"=",   " = "},
  {".",   ". "},
  {",",   ", "},
  {";",   "; "},
  {"'",   "'"},
  {"(",   "("},
  {")",   ")"},
  {"me",  "me "},
  {"IF",  "IF() THEN()"},
  {"FOR", "FOR(x: ) DO()"},
  {"FN",  "FN(x: )"},
};
static constexpr int kQuickCount = sizeof(kQuickBar) / sizeof(kQuickBar[0]);

static const char* kQwertyRows[3] = {
  "1234567890-+",
  "qwertyuiop*/",
  "asdfghjkl[]<",
};
static const char* kQwertyRowsShift[3] = {
  "!\"#$%&'()_~=",
  "QWERTYUIOP?\\",
  "ASDFGHJKL{}>",
};

}  // namespace

void VirtualKeyboard::open(const char* promptTitle, const std::string& initialText,
                           bool isMultiline, std::function<void(const std::string&)> acceptCb) {
  active       = true;
  title        = promptTitle ? promptTitle : "Edit TerseTalk";
  text         = initialText;
  cursor       = text.size();
  multiline    = isMultiline;
  shiftState   = 0;
  lastGrafChar = 0;
  strokePts.clear();
  onAccept     = std::move(acceptCb);
}

void VirtualKeyboard::close() {
  active = false;
  strokePts.clear();
}

void VirtualKeyboard::insertStr(const char* s) {
  if (!s) return;
  size_t len = std::strlen(s);
  if (cursor > text.size()) cursor = text.size();
  text.insert(cursor, s);
  cursor += len;
  // Place cursor conveniently inside parens for templates like `IF() THEN()` or `FN(x: )`
  if (std::strcmp(s, "IF() THEN()") == 0) cursor -= 8;
  else if (std::strcmp(s, "FOR(x: ) DO()") == 0) cursor -= 6;
  else if (std::strcmp(s, "FN(x: )") == 0) cursor -= 1;
}

void VirtualKeyboard::insertChar(char c) {
  if (c == '\n' && !multiline) {
    auto cb = onAccept;
    std::string res = text;
    close();
    if (cb) cb(res);
    return;
  }
  if (shiftState == 1 && std::isalpha(static_cast<unsigned char>(c))) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    shiftState = 0;
  } else if (shiftState == 2 && std::isalpha(static_cast<unsigned char>(c))) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (cursor > text.size()) cursor = text.size();
  text.insert(text.begin() + static_cast<std::ptrdiff_t>(cursor), c);
  cursor++;
}

void VirtualKeyboard::backspace() {
  if (cursor > 0 && !text.empty()) {
    if (cursor > text.size()) cursor = text.size();
    text.erase(text.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
    cursor--;
  }
}

void VirtualKeyboard::render(DisplayHal& hal) {
  if (!active) return;
  struct ScopedFont7 {
    DisplayHal& h;
    uint8_t saved;
    explicit ScopedFont7(DisplayHal& d) : h(d), saved(d.activeFontSize) { h.activeFontSize = 7; }
    ~ScopedFont7() { h.activeFontSize = saved; }
  } fontGuard(hal);

  hal.resetClipRect();
  hal.fillRect(0, 0, kScreenW, kScreenH, Color::kDarkBg);

  // 1. Header Bar (y = 0..14)
  hal.fillRect(0, 0, kScreenW, 15, Color::kTitleFocus);
  hal.drawTextClipped(4, 4, 156, title.c_str(), Color::kAccent);

  // [Graf/Qwer] toggle button (x = 164..215)
  hal.fillRoundRect(164, 1, 52, 13, 2, Color::kKeySpec);
  hal.drawText(168, 4, graffitiMode ? "QWERTY" : "Graff.", Color::kCyan);

  // [Cancel] button (x = 220..265)
  hal.fillRoundRect(220, 1, 46, 13, 2, 0x6000);
  hal.drawText(225, 4, "Cancel", Color::kWhite);

  // [OK] button (x = 270..317)
  hal.fillRoundRect(270, 1, 48, 13, 2, 0x03E0);
  hal.drawText(282, 4, "OK", Color::kWhite);

  // 2. Editable Code Area (y = 16..101)
  hal.fillRect(2, 16, kScreenW - 4, 86, Color::kBlack);
  hal.drawRect(2, 16, kScreenW - 4, 86, Color::kBorder);

  const int maxCols = 51;
  const int maxRows = 9;
  // Render text with visible block/bar cursor at `cursor`
  int col = 0, row = 0;
  // Find scroll row if text exceeds 9 lines
  int cursorRow = 0, tmpCol = 0;
  for (size_t i = 0; i < cursor && i < text.size(); i++) {
    if (text[i] == '\n' || tmpCol >= maxCols - 1) {
      cursorRow++;
      tmpCol = 0;
    } else {
      tmpCol++;
    }
  }
  int scrollRow = (cursorRow >= maxRows) ? (cursorRow - maxRows + 1) : 0;

  for (size_t i = 0; i <= text.size(); i++) {
    int visRow = row - scrollRow;
    if (i == cursor && visRow >= 0 && visRow < maxRows) {
      int16_t cx = static_cast<int16_t>(5 + col * kFontW);
      int16_t cy = static_cast<int16_t>(19 + visRow * (kFontH + 1));
      hal.fillRect(cx, cy, 2, kFontH, Color::kAccent);
    }
    if (i == text.size()) break;
    char ch = text[i];
    if (ch == '\n') {
      row++;
      col = 0;
      continue;
    }
    if (visRow >= 0 && visRow < maxRows) {
      int16_t cx = static_cast<int16_t>(5 + col * kFontW);
      int16_t cy = static_cast<int16_t>(19 + visRow * (kFontH + 1));
      hal.drawChar(cx, cy, ch, Color::kWhite);
    }
    if (++col >= maxCols) {
      col = 0;
      row++;
    }
  }

  // 3. TerseTalk 1-Tap Symbol Quick-Bar (y = 103..119)
  hal.fillRect(0, 103, kScreenW, 17, Color::kBarBg);
  int16_t qx = 1;
  for (int i = 0; i < kQuickCount; i++) {
    size_t lblLen = std::strlen(kQuickBar[i].label);
    int16_t bw = (lblLen == 1) ? 19 : static_cast<int16_t>(lblLen * 6 + 7);
    if (i == kQuickCount - 1) bw = static_cast<int16_t>(kScreenW - 1 - qx);
    hal.fillRoundRect(qx, 104, bw - 1, 15, 2, Color::kKeySpec);
    int16_t tx = static_cast<int16_t>(qx + (bw - 1 - static_cast<int16_t>(lblLen * 6)) / 2);
    hal.drawText(tx, 108, kQuickBar[i].label, Color::kAccent);
    qx = static_cast<int16_t>(qx + bw);
  }

  // 4. Bottom Input Area (y = 121..239)
  if (!graffitiMode) {
    // Stylus QWERTY Keyboard
    const char** rows = (shiftState > 0) ? kQwertyRowsShift : kQwertyRows;
    for (int r = 0; r < 3; r++) {
      int16_t ky = static_cast<int16_t>(122 + r * 29);
      for (int c = 0; c < 12; c++) {
        int16_t kx = static_cast<int16_t>(2 + c * 26 + (c >= 6 ? 2 : 0));
        hal.fillRoundRect(kx, ky, 25, 27, 3, Color::kKeyBg);
        hal.drawRoundRect(kx, ky, 25, 27, 3, Color::kBorder);
        hal.drawChar(kx + 10, ky + 10, rows[r][c], Color::kWhite);
      }
    }
    // Row 3: [Shf] z x c v b n m [Space] [Bksp] [Ret]
    int16_t ky = 209;
    uint16_t shfCol = (shiftState == 2) ? Color::kMenuSel : (shiftState == 1 ? 0x4208 : Color::kKeySpec);
    hal.fillRoundRect(2, ky, 34, 28, 3, shfCol);
    hal.drawText(8, ky + 10, shiftState == 2 ? "CAPS" : "Shf", Color::kAccent);

    const char* zRow = (shiftState > 0) ? "ZXCVBNM" : "zxcvbnm";
    for (int i = 0; i < 7; i++) {
      int16_t kx = static_cast<int16_t>(38 + i * 24);
      hal.fillRoundRect(kx, ky, 23, 28, 3, Color::kKeyBg);
      hal.drawRoundRect(kx, ky, 23, 28, 3, Color::kBorder);
      hal.drawChar(kx + 9, ky + 10, zRow[i], Color::kWhite);
    }
    // Space (x = 206..251)
    hal.fillRoundRect(206, ky, 45, 28, 3, Color::kKeyBg);
    hal.drawRoundRect(206, ky, 45, 28, 3, Color::kBorder);
    hal.drawText(217, ky + 10, "Spc", Color::kCyan);
    // Backspace (x = 253..284)
    hal.fillRoundRect(253, ky, 31, 28, 3, Color::kKeySpec);
    hal.drawText(260, ky + 10, "<-", Color::kOrange);
    // Return / Eval (x = 286..317)
    hal.fillRoundRect(286, ky, 32, 28, 3, multiline ? Color::kKeySpec : 0x03E0);
    hal.drawText(293, ky + 10, multiline ? "Ret" : "Go", Color::kWhite);
  } else {
    // Palm Graffiti Dual-Pane Writing Pad
    hal.fillRoundRect(3, 122, 184, 115, 4, Color::kWinBg);
    hal.drawRoundRect(3, 122, 184, 115, 4, Color::kBorder);
    hal.drawText(8, 126, "LETTERS (a-z, -> spc, <- bs)", 0x7BEF);

    hal.fillRoundRect(190, 122, 88, 115, 4, Color::kWinBg);
    hal.drawRoundRect(190, 122, 88, 115, 4, Color::kBorder);
    hal.drawText(195, 126, "0-9 +-*/", 0x7BEF);

    // Right-hand convenience buttons
    hal.fillRoundRect(281, 122, 36, 26, 3, shiftState ? Color::kMenuSel : Color::kKeySpec);
    hal.drawText(290, 131, shiftState == 2 ? "CAP" : "Shf", Color::kAccent);

    hal.fillRoundRect(281, 151, 36, 26, 3, Color::kKeySpec);
    hal.drawText(290, 160, "Spc", Color::kCyan);

    hal.fillRoundRect(281, 180, 36, 26, 3, Color::kKeySpec);
    hal.drawText(290, 189, "<-", Color::kOrange);

    hal.fillRoundRect(281, 209, 36, 28, 3, Color::kKeySpec);
    hal.drawText(290, 219, "Ret", Color::kWhite);

    if (lastGrafChar >= 32 && lastGrafChar <= 126) {
      hal.drawChar(172, 126, lastGrafChar, Color::kAccent);
    }
  }
}

void VirtualKeyboard::onTouchDown(DisplayHal& hal, int16_t tx, int16_t ty, bool& outNeedsRedraw) {
  if (!active) return;

  // 1. Header buttons
  if (ty < 16) {
    if (tx >= 164 && tx < 216) {
      graffitiMode = !graffitiMode;
      outNeedsRedraw = true;
    } else if (tx >= 220 && tx < 266) {
      close();
      outNeedsRedraw = true;
    } else if (tx >= 270) {
      auto cb = onAccept;
      std::string res = text;
      close();
      outNeedsRedraw = true;
      if (cb) cb(res);
    }
    return;
  }

  // 2. Text Box cursor tap (y = 16..101)
  if (ty >= 16 && ty <= 101) {
    int tapCol = (tx - 5) / kFontW;
    int tapRow = (ty - 19) / (kFontH + 1);
    if (tapCol < 0) tapCol = 0;
    if (tapRow < 0) tapRow = 0;
    int r = 0, c = 0;
    size_t bestIdx = text.size();
    for (size_t i = 0; i <= text.size(); i++) {
      if (r == tapRow && c >= tapCol) { bestIdx = i; break; }
      if (r > tapRow) { bestIdx = (i > 0 ? i - 1 : 0); break; }
      if (i == text.size()) break;
      if (text[i] == '\n') { r++; c = 0; }
      else if (++c >= 51) { r++; c = 0; }
    }
    cursor = bestIdx;
    outNeedsRedraw = true;
    return;
  }

  // 3. Symbol Quick-Bar (y = 103..120)
  if (ty >= 103 && ty <= 120) {
    int16_t qx = 1;
    for (int i = 0; i < kQuickCount; i++) {
      size_t lblLen = std::strlen(kQuickBar[i].label);
      int16_t bw = (lblLen == 1) ? 19 : static_cast<int16_t>(lblLen * 6 + 7);
      if (i == kQuickCount - 1) bw = static_cast<int16_t>(kScreenW - 1 - qx);
      if (tx >= qx && tx < qx + bw) {
        insertStr(kQuickBar[i].insert);
        outNeedsRedraw = true;
        return;
      }
      qx = static_cast<int16_t>(qx + bw);
    }
    return;
  }

  // 4. Bottom Input Area
  if (!graffitiMode) {
    if (ty >= 122 && ty < 209) {
      int r = (ty - 122) / 29;
      if (r >= 0 && r < 3) {
        for (int c = 0; c < 12; c++) {
          int16_t kx = static_cast<int16_t>(2 + c * 26 + (c >= 6 ? 2 : 0));
          if (tx >= kx && tx < kx + 25) {
            const char** rows = (shiftState > 0) ? kQwertyRowsShift : kQwertyRows;
            insertChar(rows[r][c]);
            outNeedsRedraw = true;
            return;
          }
        }
      }
      return;
    }
    if (ty >= 209) {
      if (tx < 37) {
        shiftState = (shiftState + 1) % 3;
      } else if (tx >= 38 && tx < 206) {
        int idx = (tx - 38) / 24;
        if (idx >= 0 && idx < 7) {
          insertChar("zxcvbnm"[idx]);
        }
      } else if (tx >= 206 && tx < 252) {
        insertChar(' ');
      } else if (tx >= 253 && tx < 285) {
        backspace();
      } else if (tx >= 286) {
        insertChar('\n');
      }
      outNeedsRedraw = true;
    }
  } else {
    // Graffiti Pad or side buttons
    if (tx >= 280) {
      if (ty < 150) shiftState = (shiftState + 1) % 3;
      else if (ty < 179) insertChar(' ');
      else if (ty < 208) backspace();
      else insertChar('\n');
      outNeedsRedraw = true;
      return;
    }
    strokePts.clear();
    strokePane = (tx < 189) ? GRAFFITI_LETTERS : GRAFFITI_DIGITS;
    strokePts.push_back(GraffitiPt{tx, ty});
    hal.resetClipRect();
    hal.fillCircle(tx, ty, 2, Color::kGreen);
  }
}

void VirtualKeyboard::onTouchMove(DisplayHal& hal, int16_t tx, int16_t ty, bool&) {
  if (!active || !graffitiMode || strokePts.empty()) return;
  GraffitiPt prev = strokePts.back();
  if (prev.x == tx && prev.y == ty) return;
  if (strokePts.size() < 256) {
    strokePts.push_back(GraffitiPt{tx, ty});
  }
  hal.resetClipRect();
  hal.drawLine(prev.x, prev.y, tx, ty, Color::kCyan, 2);
}

void VirtualKeyboard::onTouchUp(DisplayHal&, int16_t, int16_t, bool& outNeedsRedraw) {
  if (!active || !graffitiMode || strokePts.empty()) return;
  GraffitiResult r = recognizeGraffitiStroke(strokePts.data(),
                                             static_cast<int>(strokePts.size()),
                                             strokePane);
  strokePts.clear();
  if (r.ch == kGraffitiBackspace) {
    backspace();
  } else if (r.ch == kGraffitiShift) {
    shiftState = (shiftState + 1) % 3;
  } else if (r.ch == kGraffitiReturn) {
    insertChar('\n');
  } else if (r.ch != kGraffitiNone) {
    lastGrafChar = r.ch;
    insertChar(r.ch);
  }
  outNeedsRedraw = true;
}

}  // namespace tt
