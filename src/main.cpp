#ifndef TT_HOST_BUILD

#include <Arduino.h>

#include "hal/hal_cyd.h"
#include "ui/tt_serial.h"
#include "ui/tt_wm.h"
#include "vm/tt_vm.h"
#include "vm/tt_world.h"

#include <cstdio>

namespace {

tt::HalCyd         gHal;
tt::VM*            gVm   = nullptr;
tt::WindowManager* gWm   = nullptr;
tt::SerialRepl*    gRepl = nullptr;

}  // namespace

void setup() {
  gHal.init();

  gVm = new tt::VM();

  // ESP32 DRAM is split into two non-contiguous regions (~110 KB max single block).
  // Use a 64 KB contiguous object-header arena (~32 KB used at boot), leaving maximum
  // contiguous heap across both DRAM banks for 8-bit Bmp canvases, Lists, Dicts, and bytecodes!
  size_t maxContig = ESP.getMaxAllocHeap();
  uint32_t arenaBytes = 65536u;  // 64 KB object-header arena
  if (maxContig < arenaBytes + 12288u) {
    size_t safe = (maxContig > 20480u) ? (maxContig - 12288u) : (maxContig * 3u / 4u);
    arenaBytes = static_cast<uint32_t>(safe & ~7u);
  }

  if (!gVm->init(arenaBytes, false)) {
    gHal.clear(tt::Color::kRed);
    char msg[80];
    std::snprintf(msg, sizeof(msg), "VM Init Failed (%u / %u)",
                  static_cast<unsigned>(arenaBytes), static_cast<unsigned>(maxContig));
    gHal.drawText(8, 20, msg, tt::Color::kWhite);
    return;
  }
  gHal.attachToVm(*gVm);

  // 1. Load immutable prelude from /flash/prelude.tt (falling back to embedded .rodata)
  std::string err;
  if (!tt::loadWorldFile(*gVm, "pre0", "/flash/prelude.tt", &err)) {
    if (!tt::loadWorldBuffer(*gVm, "pre0", tt::kEmbeddedPrelude, tt::kEmbeddedPreludeLen, true, &err)) {
      gHal.drawTextClipped(4, 4, 312, ("Prelude err: " + err).c_str(), tt::Color::kOrange);
    }
  }
  gVm->markPrimordialState();

  // 2. Check Center-Screen Rescue Touch during boot:
  //    If the stylus/pen is held down in the center of the screen, skip replaying w_main.tt!
  int centerHits = 0;
  for (int i = 0; i < 8; i++) {
    int16_t tx = 0, ty = 0;
    if (gHal.pollTouch(tx, ty)) {
      if (tx >= 70 && tx <= 250 && ty >= 45 && ty <= 195) {
        centerHits++;
      }
    }
    delay(10);
  }
  bool rescueBoot = (centerHits >= 2);
  std::string replayErr;

  if (rescueBoot) {
    gHal.setRgbLed(true, true, false);
    gHal.drawText(34, 114, "RESCUE MODE: skipping w_main.tt...", tt::Color::kAccent);
    for (int w = 0; w < 40; w++) {
      int16_t rx = 0, ry = 0;
      if (!gHal.pollTouch(rx, ry)) break;
      delay(20);
    }
    gHal.setRgbLed(false, false, false);
  } else if (gHal.isSdMounted()) {
    // Normal boot: replay mutable auto-log from MicroSD (/sd/worlds/w_main.tt) if present
    std::string probe;
    if (gVm->readVfsFile("/sd/worlds/w_main.tt", probe) && !probe.empty()) {
      tt::loadWorldBuffer(*gVm, "main", probe.data(), probe.size(), false, &replayErr);
    }
  }

  gWm   = new tt::WindowManager(*gVm, gHal);
  gRepl = new tt::SerialRepl(*gVm, gHal, *gWm);

  gWm->initDesktop();
  if (rescueBoot) {
    gWm->showBanner("RESCUE BOOT: skipped w_main.tt", 8000);
    gHal.serialPrint("[RESCUE BOOT] Center touch held: skipped /sd/worlds/w_main.tt\r\n");
  } else if (!replayErr.empty()) {
    gWm->showBanner("w_main err: " + replayErr, 6000);
  }
  gWm->render();
  gRepl->printBanner();
}

void loop() {
  if (gWm) gWm->tick();
  if (gRepl) gRepl->poll();
  delay(4);
}

#endif  // !TT_HOST_BUILD
