#pragma once

#include "hal/display_hal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace tt {

struct ScrollBar {
  static bool isNeeded(int totalLines, int visLines) {
    return visLines >= 1 && totalLines > visLines;
  }

  static int16_t width(const DisplayHal& hal) {
    return hal.fontSizePx();  // 7, 8, 9, or 10 pixels wide!
  }

  static int clampScroll(int scrollTop, int totalLines, int visLines) {
    if (visLines < 1 || totalLines <= visLines) return 0;
    int maxScroll = totalLines - visLines;
    if (scrollTop < 0) return 0;
    if (scrollTop > maxScroll) return maxScroll;
    return scrollTop;
  }

  static int16_t capHeight(int16_t barW, int16_t boxH) {
    int16_t want = static_cast<int16_t>(barW + 2);
    if (want < 9) want = 9;
    int16_t quarter = static_cast<int16_t>(boxH / 4);
    if (quarter < 4) quarter = 4;
    return (want < quarter) ? want : quarter;
  }

  // Draws a right-docked vertical scrollbar inside `(boxX, boxY, boxW, boxH)` when `totalLines > visLines`.
  // Returns true if the scrollbar was drawn.
  static bool draw(DisplayHal& hal,
                   int16_t boxX, int16_t boxY, int16_t boxW, int16_t boxH,
                   int totalLines, int visLines, int scrollTop) {
    if (!isNeeded(totalLines, visLines) || boxH < 14 || boxW < 12) return false;

    const int16_t barW = width(hal);
    const int16_t barX = static_cast<int16_t>(boxX + boxW - barW);
    const int16_t capH = capHeight(barW, boxH);
    const int maxScroll = totalLines - visLines;
    scrollTop = clampScroll(scrollTop, totalLines, visLines);

    // Outer recessed track
    hal.fillRect(barX, boxY, barW, boxH, Color::kBlack);
    hal.drawRect(barX, boxY, barW, boxH, Color::kBorder);

    // Top Cap: Page UP
    hal.fillRect(barX + 1, boxY + 1, barW - 2, capH - 1, Color::kKeyBg);
    hal.drawHLine(barX, boxY + capH, barW, Color::kBorder);
    {
      int16_t mx = static_cast<int16_t>(barX + barW / 2);
      int16_t hw = std::max<int16_t>(1, static_cast<int16_t>((barW - 4) / 2));
      int16_t ty0 = static_cast<int16_t>(boxY + 2);
      int16_t ty1 = static_cast<int16_t>(boxY + capH - 3);
      if (ty1 > ty0) {
        hal.fillTriangle(mx, ty0, static_cast<int16_t>(mx - hw), ty1,
                         static_cast<int16_t>(mx + hw), ty1, Color::kAccent);
      }
    }

    // Bottom Cap: Page DOWN
    const int16_t botY = static_cast<int16_t>(boxY + boxH - capH);
    hal.fillRect(barX + 1, botY, barW - 2, capH - 1, Color::kKeyBg);
    hal.drawHLine(barX, botY - 1, barW, Color::kBorder);
    {
      int16_t mx = static_cast<int16_t>(barX + barW / 2);
      int16_t hw = std::max<int16_t>(1, static_cast<int16_t>((barW - 4) / 2));
      int16_t by0 = static_cast<int16_t>(botY + 2);
      int16_t by1 = static_cast<int16_t>(boxY + boxH - 3);
      if (by1 > by0) {
        hal.fillTriangle(static_cast<int16_t>(mx - hw), by0, static_cast<int16_t>(mx + hw), by0,
                         mx, by1, Color::kAccent);
      }
    }

    // Middle Proportional Slider Thumb
    const int16_t trackY = static_cast<int16_t>(boxY + capH + 1);
    const int16_t trackH = static_cast<int16_t>(boxH - 2 * capH - 2);
    if (trackH >= 4) {
      int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(trackH) * visLines) / totalLines);
      if (thumbH < 6) thumbH = std::min<int16_t>(trackH, 6);
      if (thumbH > trackH) thumbH = trackH;
      const int16_t avail = static_cast<int16_t>(trackH - thumbH);
      const int16_t thumbY = static_cast<int16_t>(
        trackY + (avail > 0 ? ((static_cast<int32_t>(avail) * scrollTop) / maxScroll) : 0));

      hal.fillRect(barX + 1, thumbY, barW - 2, thumbH, Color::kMenuSel);
      hal.drawRect(barX + 1, thumbY, barW - 2, thumbH, Color::kCyan);
      if (thumbH >= 9 && barW >= 7) {
        int16_t gy = static_cast<int16_t>(thumbY + thumbH / 2);
        hal.drawHLine(barX + 2, gy, barW - 4, Color::kAccent);
      }
    }
    return true;
  }

  // Handles `"down"`, `"move"`, `"up"` touches in the same coordinate space as `(boxX, boxY, boxW, boxH)`.
  // Returns true if the scrollbar consumed the touch event.
  static bool handleTouch(const DisplayHal& hal, const char* kind,
                          int16_t tx, int16_t ty,
                          int16_t boxX, int16_t boxY, int16_t boxW, int16_t boxH,
                          int totalLines, int visLines,
                          int& ioScrollTop, bool& ioDraggingThumb) {
    if (!isNeeded(totalLines, visLines) || boxH < 14 || boxW < 12) {
      ioDraggingThumb = false;
      return false;
    }

    const int16_t barW = width(hal);
    const int16_t barX = static_cast<int16_t>(boxX + boxW - barW);
    const int16_t capH = capHeight(barW, boxH);
    const int maxScroll = totalLines - visLines;
    const int pageStep  = std::max(1, visLines - 1);

    auto updateFromTrackY = [&](int16_t curY) {
      const int16_t trackY = static_cast<int16_t>(boxY + capH + 1);
      const int16_t trackH = static_cast<int16_t>(boxH - 2 * capH - 2);
      if (trackH <= 0) return;
      int16_t thumbH = static_cast<int16_t>((static_cast<int32_t>(trackH) * visLines) / totalLines);
      if (thumbH < 6) thumbH = std::min<int16_t>(trackH, 6);
      if (thumbH > trackH) thumbH = trackH;
      const int16_t avail = static_cast<int16_t>(trackH - thumbH);
      if (avail <= 0) {
        ioScrollTop = 0;
        return;
      }
      int32_t rel = static_cast<int32_t>(curY) - static_cast<int32_t>(trackY) - thumbH / 2;
      if (rel < 0) rel = 0;
      if (rel > avail) rel = avail;
      ioScrollTop = static_cast<int>((rel * maxScroll + avail / 2) / avail);
      ioScrollTop = clampScroll(ioScrollTop, totalLines, visLines);
    };

    if (std::strcmp(kind, "up") == 0) {
      if (ioDraggingThumb) {
        ioDraggingThumb = false;
        return true;
      }
      return false;
    }

    if (std::strcmp(kind, "move") == 0) {
      if (ioDraggingThumb) {
        updateFromTrackY(ty);
        return true;
      }
      return false;
    }

    if (std::strcmp(kind, "down") == 0) {
      // Allow a 2px inward stylus tolerance on the narrow scrollbar edge
      if (tx < barX - 2 || tx > boxX + boxW + 1 || ty < boxY || ty >= boxY + boxH) {
        ioDraggingThumb = false;
        return false;
      }
      if (ty < boxY + capH) {
        ioScrollTop     = clampScroll(ioScrollTop - pageStep, totalLines, visLines);
        ioDraggingThumb = false;
        return true;
      }
      if (ty >= boxY + boxH - capH) {
        ioScrollTop     = clampScroll(ioScrollTop + pageStep, totalLines, visLines);
        ioDraggingThumb = false;
        return true;
      }
      ioDraggingThumb = true;
      updateFromTrackY(ty);
      return true;
    }

    return false;
  }
};

}  // namespace tt
