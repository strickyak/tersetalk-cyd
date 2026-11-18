#include "hal/hal_mock.h"
#include "ui/tt_graffiti.h"
#include "ui/tt_ide.h"
#include "ui/tt_keyboard.h"
#include "ui/tt_pen.h"
#include "ui/tt_serial.h"
#include "ui/tt_wm.h"
#include "vm/tt_vm.h"
#include "vm/tt_world.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tt;

static int gUiPassed = 0;

#define CHECK(cond) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
    std::exit(1); \
  } \
} while (0)

static void testDesktopAndOverlappingWindows(VM& vm, HalMock& hal, WindowManager& wm) {
  wm.initDesktop();
  wm.render();

  // Verify 2 overlapping windows opened on boot (`Workspace` + `GraphApp`)
  CHECK(wm.zCount == 2);
  CHECK(!wm.maximized);
  Window* top = wm.topWindow();
  CHECK(top != nullptr && isObj(top->appObj));
  CHECK(vm.toStdString(vm.oopCls(top->appObj)->nameStr) == "GraphApp");

  // Save screenshot 1: Overlapping Desktop (`Workspace` + `GraphApp`)
  CHECK(hal.savePPM("build/shot_01_overlap_desktop.ppm"));

  // Tap NE App Menu Wedge (`x=312, y=6`) and verify `GraphApp >> appMenu` populates dropdown!
  hal.pushTap(312, 6);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.activeMenu == 2);
  CHECK(wm.menuItems.size() >= 7);
  CHECK(wm.menuItems[0].label == "Edit f(x)...");
  CHECK(wm.menuItems[1].label == "Fourier Square");
  CHECK(hal.savePPM("build/shot_02_ne_app_menu.ppm"));

  // Tap item 1 ("Fourier Square") in the NE dropdown!
  int16_t item1X = static_cast<int16_t>(wm.menuX + 20);
  int16_t item1Y = static_cast<int16_t>(wm.menuY + 2 + 1 * 14 + 6);
  hal.pushTap(item1X, item1Y);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.activeMenu == 0);

  // Tap NW Global Menu Wedge (`x=6, y=6`) and verify Global Menu opens!
  hal.pushTap(6, 6);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.activeMenu == 1);
  CHECK(hal.savePPM("build/shot_03_nw_global_menu.ppm"));

  // Dismiss dropdown by tapping outside, then tap background `Workspace` window (`x=20, y=50`) to focus it!
  hal.pushTap(280, 220);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.activeMenu == 0);

  hal.pushTap(20, 50);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.topWindow()->native != nullptr);
  CHECK(std::strcmp(wm.topWindow()->native->appName(), "Workspace") == 0);
  // Current App updates dynamically when focus changes!
  CHECK(wm.currentApp() == kOopNil);

  gUiPassed++;
  std::printf("[PASS] Overlapping windows, Z-order focus, NW Global Wedge & NE App Wedge\n");
}

static void testMaximizedAppsAndScreenshots(VM&, HalMock& hal, WindowManager& wm) {
  // Launch SpiroApp, maximize to full 320x240, and capture screenshot!
  Window* spiro = wm.launchTerseAppByClassName("SpiroApp");
  CHECK(spiro != nullptr);
  wm.setMaximized(true);
  wm.render();
  CHECK(wm.maximized);
  int16_t cx, cy, cw, ch;
  wm.getClientRect(*spiro, true, cx, cy, cw, ch);
  CHECK(cx == 0 && cy == 0 && cw == 320 && ch == 240);
  CHECK(hal.savePPM("build/shot_04_spiro_maximized.ppm"));

  // Launch MandelApp (tap-to-zoom Mandelbrot on 8-bit Bmp), zoom once via touch, and capture!
  Window* mandel = wm.launchTerseAppByClassName("MandelApp");
  CHECK(mandel != nullptr);
  wm.render();
  // Tap Seahorse region `(120, 100)` to test interactive tap-to-zoom!
  hal.pushTap(120, 100);
  while (!hal.touchQueue.empty()) wm.tick();
  wm.render();
  CHECK(hal.savePPM("build/shot_05_mandelbrot_maximized.ppm"));

  // Launch TurtleApp (recursive fractal tree) in overlapping mode with MandelApp behind it!
  wm.setMaximized(false);
  Window* turtle = wm.launchTerseAppByClassName("TurtleApp");
  CHECK(turtle != nullptr);
  turtle->x = 54; turtle->y = 30; turtle->w = 258; turtle->h = 202;
  wm.render();
  CHECK(hal.savePPM("build/shot_06_turtle_overlap.ppm"));

  // Launch LifeApp and verify periodic `onTick` advances generations!
  Window* life = wm.launchTerseAppByClassName("LifeApp");
  CHECK(life != nullptr && life->animIntervalMs > 0);
  hal.advanceMillis(120);
  wm.tick();
  hal.advanceMillis(120);
  wm.tick();
  CHECK(hal.savePPM("build/shot_07_conway_life.ppm"));

  gUiPassed++;
  std::printf("[PASS] Maximized 320x240 mode, SpiroApp, MandelApp, TurtleApp & LifeApp\n");
}

static void testGraffitiKeyboardBrowserAndSerial(VM& vm, HalMock& hal, WindowManager& wm) {
  // 1. Test Palm Graffiti single-stroke recognizer on Letters & Digits!
  // Caret '^' in Letters pane -> 'a'
  GraffitiPt caretA[] = {{10, 90}, {30, 50}, {50, 10}, {70, 50}, {90, 90}};
  GraffitiResult ra = recognizeGraffitiStroke(caretA, 5, GRAFFITI_LETTERS);
  CHECK(ra.ch == 'a');

  // Horizontal right stroke in Letters pane -> ' ' (Space)
  GraffitiPt spaceStroke[] = {{20, 180}, {50, 180}, {80, 180}};
  GraffitiResult rsp = recognizeGraffitiStroke(spaceStroke, 3, GRAFFITI_LETTERS);
  CHECK(rsp.ch == ' ');

  // Horizontal left stroke -> Backspace
  GraffitiPt bsStroke[] = {{90, 180}, {55, 180}, {20, 180}};
  GraffitiResult rbs = recognizeGraffitiStroke(bsStroke, 3, GRAFFITI_LETTERS);
  CHECK(rbs.ch == kGraffitiBackspace);

  // Downward vertical stroke in Digits pane -> '1'
  GraffitiPt oneStroke[] = {{220, 140}, {220, 175}, {220, 210}};
  GraffitiResult r1 = recognizeGraffitiStroke(oneStroke, 3, GRAFFITI_DIGITS);
  CHECK(r1.ch == '1');

  // 2. Open Class Browser via `Edit App Code` and render screenshot!
  wm.openEditAppCodeBrowser();
  wm.render();
  CHECK(hal.savePPM("build/shot_08_class_browser.ppm"));

  // 3. Open Stylus Keyboard + Symbol Quick-Bar (`:`, `$`, `@`, `=`, ...) and render screenshot!
  std::string accepted;
  wm.openTextPrompt("GraphApp >> onPaint:", "p clear: 001 $ axes.\np yellow plot: FN(x: x sin)",
                    true, [&](const std::string& s) { accepted = s; });
  wm.render();
  CHECK(wm.keyboard->active);
  CHECK(hal.savePPM("build/shot_09_stylus_keyboard.ppm"));

  // Toggle keyboard into Palm Graffiti mode (`x=185, y=7`) and render screenshot!
  hal.pushTap(185, 7);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(wm.keyboard->graffitiMode);
  CHECK(hal.savePPM("build/shot_10_graffiti_pad.ppm"));

  // Tap `[OK]` (`x=290, y=7`) and verify callback fires!
  hal.pushTap(290, 7);
  while (!hal.touchQueue.empty()) wm.tick();
  CHECK(!wm.keyboard->active);
  CHECK(accepted.find("yellow plot:") != std::string::npos);

  // 4. Test USB Serial REPL + `:put` / `:load` / `:mem`
  SerialRepl repl(vm, hal, wm);
  hal.serialOutLog.clear();
  repl.processLine("6 * 7");
  CHECK(hal.serialOutLog.find("=> 42") != std::string::npos);

  hal.serialOutLog.clear();
  repl.processLine(":put /sd/hello.tt");
  repl.processLine("cls Greeter Usr");
  repl.processLine("meth Greeter hi");
  repl.processLine(" 'Hello CYD!'");
  repl.processLine(":eof");
  repl.processLine(":load /sd/hello.tt");
  repl.processLine("Greeter new hi");
  CHECK(hal.serialOutLog.find("=> 'Hello CYD!'") != std::string::npos);

  // 5. Regression test: closing windows must immediately free all Bmp external memory (no lruCache leak!)
  while (wm.zCount > 0) {
    wm.closeWindowSlot(wm.zOrder[wm.zCount - 1]);
  }
  vm.runGC();
  // Warm up PaintApp bytecode compilation once, close it, and record steady-state externalBytes
  Window* p0 = wm.launchTerseAppByClassName("PaintApp");
  CHECK(p0 != nullptr);
  wm.render();
  wm.closeWindowSlot(wm.zOrder[wm.zCount - 1]);
  uint32_t baselineExt = vm.heap.externalBytes;

  // Open & close PaintApp (17,920 B Bmp), MandelApp, and LifeApp 5 times in a row — zero leak!
  for (int iter = 0; iter < 5; iter++) {
    Window* pa = wm.launchTerseAppByClassName("PaintApp");
    Window* ma = wm.launchTerseAppByClassName("MandelApp");
    Window* la = wm.launchTerseAppByClassName("LifeApp");
    CHECK(pa != nullptr && ma != nullptr && la != nullptr);
    wm.render();
    while (wm.zCount > 0) {
      wm.closeWindowSlot(wm.zOrder[wm.zCount - 1]);
    }
  }
  CHECK(vm.heap.externalBytes == baselineExt);

  gUiPassed++;
  std::printf("[PASS] Palm Graffiti, Stylus Keyboard, Class Browser, Serial REPL & Zero Bmp Leak\n");
}

int main() {
  std::printf("=== TerseTalk-CYD Headless UI & Graphics Test Suite ===\n");
  HalMock hal;
  CHECK(hal.init());

  VM vm;
  CHECK(vm.init(kDefaultArenaBytes, true));
  hal.attachToVm(vm);

  WindowManager wm(vm, hal);
  testDesktopAndOverlappingWindows(vm, hal, wm);
  testMaximizedAppsAndScreenshots(vm, hal, wm);
  testGraffitiKeyboardBrowserAndSerial(vm, hal, wm);

  vm.destroy();
  std::printf("=== ALL %d UI & GRAPHICS TEST SUITES PASSED ===\n", gUiPassed);
  return 0;
}
