#include "tt_wm.h"
#include "tt_ide.h"
#include "tt_keyboard.h"
#include "tt_pen.h"
#include "vm/tt_world.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tt {

namespace {

static int16_t gNextWinId = 1;
static constexpr int16_t kMenuRowH = 12;

}  // namespace

WindowManager::WindowManager(VM& v, DisplayHal& h) : vm(v), hal(h) {
  keyboard = new VirtualKeyboard();
  // Register GC root marker on VM while preserving `vm.host.user` (owned by DisplayHal!)
  static WindowManager* sActiveWm = nullptr;
  sActiveWm = this;
  vm.host.markUiRoots = [](void*, VM& targetVm) {
    if (sActiveWm) sActiveWm->markRoots(targetVm);
  };
}

WindowManager::~WindowManager() {
  for (int i = 0; i < kMaxWindows; i++) {
    delete windows[i].native;
    windows[i].native = nullptr;
  }
  delete keyboard;
  keyboard = nullptr;
}

void WindowManager::markRoots(VM& v) {
  for (int i = 0; i < kMaxWindows; i++) {
    if (!windows[i].alive) continue;
    v.markOop(windows[i].appObj);
    v.markOop(windows[i].winObj);
    if (windows[i].native) windows[i].native->markGcRoots(v);
  }
}

void WindowManager::initDesktop() {
  installGraphicsPrimitives(vm, &hal, this);

  // Boot with two overlapping windows showcasing TerseTalk-CYD's desktop & math/graphics:
  // 1) Workspace in the background (upper-left)
  createNativeWindow(createWorkspaceController(), "Workspace", 6, 20, 214, 152);
  // 2) GraphApp (or GraphicsPad fallback) focused on top (offset lower-right)
  if (!launchTerseAppByClassName("GraphApp")) {
    createNativeWindow(createGraphicsPadController(), "Graphics Pad", 68, 44, 246, 190);
  }
  needsRedraw = true;
}

int WindowManager::allocWindowSlot() {
  for (int i = 0; i < kMaxWindows; i++) {
    if (!windows[i].alive) return i;
  }
  // If full, close bottom-most window to make room
  if (zCount > 0) {
    int oldest = zOrder[0];
    closeWindowSlot(oldest);
    return oldest;
  }
  return -1;
}

Window* WindowManager::createTerseWindow(Oop appObj, const char* title,
                                         int16_t x, int16_t y, int16_t w, int16_t h) {
  int slot = allocWindowSlot();
  if (slot < 0) return nullptr;

  TempRootGuard rg(vm, appObj);
  auto* ow = reinterpret_cast<ObjWin*>(vm.allocObj(CLS_WIN, OBJ_WIN, 0, sizeof(ObjWin)));
  int16_t wid = gNextWinId++;
  ow->winId  = wid;
  ow->appObj = appObj;

  Window& win        = windows[slot];
  win.id             = wid;
  win.alive          = true;
  win.x              = std::max<int16_t>(0, std::min<int16_t>(x, kScreenW - 80));
  win.y              = std::max<int16_t>(kTopBarH, std::min<int16_t>(y, kScreenH - 60));
  win.w              = std::max<int16_t>(96, std::min<int16_t>(w, kScreenW - win.x));
  win.h              = std::max<int16_t>(64, std::min<int16_t>(h, kScreenH - win.y));
  win.title          = title ? title : "App";
  win.appObj         = appObj;
  win.winObj         = oopFromObj(&ow->hdr);
  win.native         = nullptr;
  win.animIntervalMs = 0;
  win.lastStepMs     = hal.millis();
  win.closeArmed     = false;
  win.dirty          = true;

  zOrder[zCount++] = slot;
  needsRedraw      = true;
  return &win;
}

Window* WindowManager::createNativeWindow(NativeWinController* ctrl, const char* title,
                                          int16_t x, int16_t y, int16_t w, int16_t h) {
  int slot = allocWindowSlot();
  if (slot < 0) {
    delete ctrl;
    return nullptr;
  }
  Window& win        = windows[slot];
  win.id             = gNextWinId++;
  win.alive          = true;
  win.x              = std::max<int16_t>(0, std::min<int16_t>(x, kScreenW - 80));
  win.y              = std::max<int16_t>(kTopBarH, std::min<int16_t>(y, kScreenH - 60));
  win.w              = std::max<int16_t>(96, std::min<int16_t>(w, kScreenW - win.x));
  win.h              = std::max<int16_t>(64, std::min<int16_t>(h, kScreenH - win.y));
  win.title          = title ? title : ctrl->appName();
  win.appObj         = kOopNil;
  win.winObj         = kOopNil;
  win.native         = ctrl;
  win.animIntervalMs = 0;
  win.lastStepMs     = hal.millis();
  win.closeArmed     = false;
  win.dirty          = true;

  zOrder[zCount++] = slot;
  needsRedraw      = true;
  return &win;
}

Window* WindowManager::findById(int16_t winId) {
  for (int i = 0; i < kMaxWindows; i++) {
    if (windows[i].alive && windows[i].id == winId) return &windows[i];
  }
  return nullptr;
}

Window* WindowManager::topWindow() {
  if (zCount <= 0) return nullptr;
  return &windows[zOrder[zCount - 1]];
}

Oop WindowManager::currentApp() {
  Window* top = topWindow();
  return top ? top->appObj : kOopNil;
}

void WindowManager::focusWindowSlot(int slot) {
  int pos = -1;
  for (int i = 0; i < zCount; i++) {
    windows[zOrder[i]].closeArmed = false;
    if (zOrder[i] == slot) pos = i;
  }
  if (pos < 0 || pos == zCount - 1) return;
  for (int i = pos; i < zCount - 1; i++) zOrder[i] = zOrder[i + 1];
  zOrder[zCount - 1] = slot;
  needsRedraw = true;
}

void WindowManager::closeWindowSlot(int slot) {
  if (slot < 0 || slot >= kMaxWindows || !windows[slot].alive) return;
  Window& win = windows[slot];
  if (isObj(win.appObj)) {
    uint32_t closeSym = vm.internSym("onclose");
    if (vm.lookupMeth(vm.oopClsId(win.appObj), closeSym)) {
      try {
        vm.ticksRemaining = 500000;
        vm.send(win.appObj, closeSym, nullptr, 0);
      } catch (const TerseError&) {}
    }
  }
  delete win.native;
  win.native         = nullptr;
  win.appObj         = kOopNil;
  win.winObj         = kOopNil;
  win.alive          = false;
  win.animIntervalMs = 0;

  int out = 0;
  for (int i = 0; i < zCount; i++) {
    if (zOrder[i] != slot) zOrder[out++] = zOrder[i];
  }
  zCount = out;
  if (zCount == 0) maximized = false;
  // Reclaim any large offscreen Bmp buffers immediately upon window close
  vm.runGC();
  needsRedraw = true;
}

void WindowManager::closeAllWindows(bool runOnClose) {
  for (int i = 0; i < kMaxWindows; i++) {
    if (!windows[i].alive) continue;
    if (runOnClose && isObj(windows[i].appObj)) {
      uint32_t closeSym = vm.internSym("onclose");
      if (vm.lookupMeth(vm.oopClsId(windows[i].appObj), closeSym)) {
        try {
          vm.ticksRemaining = 500000;
          vm.send(windows[i].appObj, closeSym, nullptr, 0);
        } catch (const TerseError&) {}
      }
    }
    delete windows[i].native;
    windows[i].native         = nullptr;
    windows[i].appObj         = kOopNil;
    windows[i].winObj         = kOopNil;
    windows[i].alive          = false;
    windows[i].animIntervalMs = 0;
  }
  zCount      = 0;
  maximized   = false;
  activeMenu  = 0;
  modalActive = false;
  if (keyboard) keyboard->close();
  needsRedraw = true;
}

void WindowManager::setMaximized(bool maxMode) {
  if (maxMode && zCount == 0) return;
  if (keyboard) keyboard->close();
  maximized     = maxMode;
  activeMenu    = 0;
  dropdownDirty = false;
  needsRedraw   = true;
}

void WindowManager::invalidateWindow(Window& win) {
  win.dirty = true;
  if (&win != topWindow()) {
    needsRedraw = true;
  }
}

Window* WindowManager::launchTerseAppByClassName(const char* clsName) {
  if (!clsName || !clsName[0]) return nullptr;
  if (keyboard) keyboard->close();
  Oop clsOop = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(clsName)));
  if (!isObj(clsOop) || objFromOop(clsOop)->format != OBJ_CLS) return nullptr;

  try {
    vm.ticksRemaining = 2000000;
    uint32_t openSym = vm.internSym("open");
    if (vm.lookupMeth(vm.oopClsId(clsOop), openSym)) {
      Oop wOop = vm.send(clsOop, openSym, nullptr, 0);
      if (isObj(wOop) && objFromOop(wOop)->format == OBJ_WIN) {
        return findById(reinterpret_cast<ObjWin*>(objFromOop(wOop))->winId);
      }
      return topWindow();
    }
    // Generic fallback if class doesn't define `open`
    Oop inst = vm.send(clsOop, vm.internSym("new"), nullptr, 0);
    TempRootGuard rg(vm, inst);
    int cascade = (zCount % 5) * 12;
    return createTerseWindow(inst, clsName,
                             static_cast<int16_t>(48 + cascade),
                             static_cast<int16_t>(32 + cascade),
                             248, 184);
  } catch (const TerseError& e) {
    showBanner(e.message);
    return nullptr;
  }
}

void WindowManager::openEditAppCodeBrowser() {
  std::string focusCls = "GraphApp";
  if (Window* top = topWindow()) {
    if (isObj(top->appObj)) {
      focusCls = vm.toStdString(vm.oopCls(top->appObj)->nameStr);
    }
  }
  if (maximized) maximized = false;
  createNativeWindow(createClassBrowserController(focusCls.c_str()),
                     ("Browse: " + focusCls).c_str(), 14, 24, 292, 204);
}

void WindowManager::showBanner(const std::string& msg, uint32_t durationMs) {
  statusBanner   = msg;
  bannerExpireMs = hal.millis() + durationMs;
  needsRedraw    = true;
}

void WindowManager::openTextPrompt(const char* promptTitle, const std::string& initialText,
                                   bool multiline, std::function<void(const std::string&)> onAccept) {
  activeMenu  = 0;
  modalActive = false;
  keyboard->open(promptTitle, initialText, multiline, std::move(onAccept));
  needsRedraw = true;
}

void WindowManager::openModalChoice(const std::string& title, const std::string& subtitle,
                                    std::vector<ModalButton> buttons) {
  activeMenu    = 0;
  dropdownDirty = false;
  if (keyboard) keyboard->close();
  modalTitle    = title;
  modalSubtitle = subtitle;
  modalButtons  = std::move(buttons);
  modalActive   = true;
  needsRedraw   = true;
}

static std::string baseFileName(const std::string& p) {
  size_t sl = p.rfind('/');
  return (sl == std::string::npos) ? p : p.substr(sl + 1);
}

std::string WindowManager::nextAutoLogBackupName() const {
  std::vector<std::pair<std::string, uint32_t>> files;
  vm.listVfsFiles("/sd/worlds", files);
  for (int idx = 1; idx < 1000; idx++) {
    std::string cand = "w_bak_" + std::to_string(idx);
    std::string fn   = cand + ".tt";
    bool exists = false;
    for (const auto& kv : files) {
      if (kv.first == fn) { exists = true; break; }
    }
    if (!exists) return cand;
  }
  return "w_bak";
}

std::string WindowManager::nextSavepointName() const {
  std::vector<std::pair<std::string, uint32_t>> files;
  vm.listVfsFiles("/sd/worlds", files);
  for (int idx = 1; idx < 1000; idx++) {
    std::string cand = "save" + std::to_string(idx);
    std::string fn   = cand + ".tt";
    bool exists = false;
    for (const auto& kv : files) {
      if (kv.first == fn) { exists = true; break; }
    }
    if (!exists) return cand;
  }
  return "save";
}

void WindowManager::promptSaveClassState() {
  openTextPrompt("Save World (/sd/worlds/*.tt)", nextSavepointName(), false,
                 [this](const std::string& nm) {
                   if (nm.empty()) return;
                   std::string resolved, err;
                   if (saveClassState(vm, nm.c_str(), &resolved, &err)) {
                     showBanner("Saved world: " + baseFileName(resolved));
                   } else {
                     showBanner(err);
                   }
                 });
}

bool WindowManager::performLoadClassState(const std::string& vfsPath,
                                          const std::string& renameAutoLogTo,
                                          std::string* outStatus) {
  // 1. Read target savepoint file BEFORE touching w_main.tt (in case vfsPath == w_main.tt!)
  std::string targetContent;
  if (!vfsPath.empty()) {
    if (!vm.readVfsFile(vfsPath.c_str(), targetContent)) {
      std::string msg = "Cannot read " + vfsPath;
      if (outStatus) *outStatus = msg;
      showBanner(msg);
      return false;
    }
  }

  // 2. Optionally rename auto-log w_main.tt first
  std::string renamedNote;
  if (!renameAutoLogTo.empty() && !vm.worldPath.empty()) {
    std::string bakPath = normalizeStatePath(renameAutoLogTo);
    if (bakPath == vm.worldPath) {
      std::string msg = "Backup name cannot be w_main";
      if (outStatus) *outStatus = msg;
      showBanner(msg);
      return false;
    }
    std::string oldLog;
    if (vm.readVfsFile(vm.worldPath.c_str(), oldLog) && !oldLog.empty()) {
      if (!vm.writeVfsFile(bakPath.c_str(), oldLog.data(), oldLog.size(), false)) {
        std::string msg = "Failed writing backup " + bakPath;
        if (outStatus) *outStatus = msg;
        showBanner(msg);
        return false;
      }
      renamedNote = "w_main->" + baseFileName(bakPath) + "; ";
    }
  }

  // 3. Clear w_main.tt before loading
  if (!vm.worldPath.empty()) {
    vm.deleteVfsFile(vm.worldPath.c_str());
  }

  // 4. Close all windows & reset VM cleanly in-place to Primordial Prelude
  closeAllWindows(false);
  std::string err;
  if (!vm.resetToPrelude(&err)) {
    std::string msg = "Prelude reset failed: " + err;
    if (outStatus) *outStatus = msg;
    showBanner(msg);
    return false;
  }
  installGraphicsPrimitives(vm, &hal, this);
  vm.markPrimordialState();

  // 5. Replay selected savepoint (if non-empty) and seed clean w_main.tt so future boots match
  bool ok = true;
  if (!vfsPath.empty()) {
    ok = loadWorldBuffer(vm, vm.worldName.c_str(), targetContent.data(), targetContent.size(), false, &err);
    if (!vm.worldPath.empty()) {
      std::string freshDiff = serializeClassState(vm);
      if (!freshDiff.empty()) {
        std::string seeded = "# Base state: " + baseFileName(vfsPath) + "\n\n" + freshDiff;
        vm.writeVfsFile(vm.worldPath.c_str(), seeded.data(), seeded.size(), false);
      }
    }
  }

  // 6. Re-open fresh desktop windows
  initDesktop();

  std::string msg;
  if (!ok) {
    msg = "Load err: " + err;
  } else if (vfsPath.empty()) {
    msg = renamedNote + "Reset to Primordial Prelude";
  } else {
    msg = renamedNote + "Loaded " + baseFileName(vfsPath);
  }
  if (outStatus) *outStatus = msg;
  showBanner(msg, 4500);
  return ok;
}

void WindowManager::requestLoadClassState(const std::string& vfsPath) {
  if (!vfsPath.empty()) {
    std::string probe;
    if (!vm.readVfsFile(vfsPath.c_str(), probe)) {
      showBanner("Cannot read " + vfsPath);
      return;
    }
  }

  std::string oldLog;
  bool hasAutoLog = !vm.worldPath.empty() &&
                    vm.readVfsFile(vm.worldPath.c_str(), oldLog) &&
                    !oldLog.empty();

  if (!hasAutoLog) {
    performLoadClassState(vfsPath, "");
    return;
  }

  std::string title = vfsPath.empty()
                    ? "Reset to Primordial Prelude"
                    : ("Load World: " + baseFileName(vfsPath));
  openModalChoice(
    title,
    "Rename auto-log w_main.tt before clearing?",
    {
      {"Yes: Rename w_main.tt First...", 0x03E0, [this, vfsPath]() {
        openTextPrompt("Rename w_main.tt as (/sd/worlds/*.tt)", nextAutoLogBackupName(), false,
                       [this, vfsPath](const std::string& bak) {
                         if (!bak.empty()) performLoadClassState(vfsPath, bak);
                       });
      }},
      {"No: Clear w_main.tt & Load", 0x8200, [this, vfsPath]() {
        performLoadClassState(vfsPath, "");
      }},
      {"Cancel", Color::kKeyBg, []() {}}
    });
}

void WindowManager::getClientRect(const Window& win, bool isTop,
                                  int16_t& cx, int16_t& cy, int16_t& cw, int16_t& ch) const {
  if (maximized && isTop) {
    cx = 0;
    cy = 0;
    cw = kScreenW;
    ch = kScreenH;
    return;
  }
  cx = static_cast<int16_t>(win.x + 1);
  cy = static_cast<int16_t>(win.y + kWinTitleH);
  cw = static_cast<int16_t>(win.w - 2);
  ch = static_cast<int16_t>(win.h - kWinTitleH - 1);
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
}

// ---------------------------------------------------------------------------
// Menus (NW Global Menu & NE Current-App Menu)
// ---------------------------------------------------------------------------
void WindowManager::buildGlobalMenu(std::vector<MenuItem>& out) {
  out.clear();
  if (zCount > 0) {
    out.push_back({maximized ? "Overlap Windows" : "Maximize Top", "",
                   [this]() { toggleMaximized(); }});
  }
  out.push_back({"Open Doc (/sd/docs)","", [this]() {
    if (maximized) maximized = false;
    createNativeWindow(createFilesController("/sd/docs"), "Documents & Files", 18, 24, 284, 198);
  }});
  out.push_back({"Save World",        "", [this]() { promptSaveClassState(); }});
  out.push_back({"Load World",        "", [this]() {
    if (maximized) maximized = false;
    createNativeWindow(createFilesController("/sd/worlds"), "Worlds (/sd/worlds)", 18, 24, 284, 198);
  }});
  out.push_back({"+ Graphics Pad",   "", [this]() { launchTerseAppByClassName("PadApp"); }});
  out.push_back({"+ Note Pad",       "", [this]() { launchTerseAppByClassName("NoteApp"); }});
  out.push_back({"+ Workspace",      "", [this]() {
    if (maximized) maximized = false;
    int c = (zCount % 4) * 10;
    createNativeWindow(createWorkspaceController(), "Workspace",
                       static_cast<int16_t>(12 + c), static_cast<int16_t>(22 + c), 240, 168);
  }});
  out.push_back({"App: Graph Plot",  "", [this]() { launchTerseAppByClassName("GraphApp"); }});
  out.push_back({"App: Spirograph",  "", [this]() { launchTerseAppByClassName("SpiroApp"); }});
  out.push_back({"App: Turtle Art",  "", [this]() { launchTerseAppByClassName("TurtleApp"); }});
  out.push_back({"App: Mandelbrot",  "", [this]() { launchTerseAppByClassName("MandelApp"); }});
  out.push_back({"App: Conway Life", "", [this]() { launchTerseAppByClassName("LifeApp"); }});
  out.push_back({"App: Paint Pad",   "", [this]() { launchTerseAppByClassName("PaintApp"); }});
  out.push_back({"Class Browser",    "", [this]() { openEditAppCodeBrowser(); }});
  out.push_back({"Calibrate Touch",  "", [this]() {
    hal.runTouchCalibration();
    needsRedraw = true;
  }});
}

void WindowManager::buildAppMenu(std::vector<MenuItem>& out) {
  out.clear();
  Window* top = topWindow();
  if (!top) {
    out.push_back({"(No Open Window)", "", []() {}});
    return;
  }

  // 1. Dynamic TerseTalk `currentApp appMenu` items + generic Document Save/Save As!
  if (isObj(top->appObj)) {
    uint32_t menuSym = vm.internSym("appmenu");
    if (vm.lookupMeth(vm.oopClsId(top->appObj), menuSym)) {
      try {
        vm.ticksRemaining = 500000;
        Oop listOop = vm.send(top->appObj, menuSym, nullptr, 0);
        if (isObj(listOop) && objFromOop(listOop)->format == OBJ_LIST) {
          auto* l = reinterpret_cast<ObjList*>(objFromOop(listOop));
          for (uint32_t i = 0; i < l->len; i++) {
            Oop entry = l->items[i];
            if (isObj(entry) && objFromOop(entry)->format == OBJ_LIST) {
              auto* pair = reinterpret_cast<ObjList*>(objFromOop(entry));
              if (pair->len >= 2) {
                std::string lbl = vm.toStdString(pair->items[0]);
                std::string sel = vm.toStdString(pair->items[1]);
                out.push_back({lbl, sel, nullptr});
              }
            }
          }
        }
      } catch (const TerseError& e) {
        showBanner(e.message);
      }
    }

    // Generic Document Save / Save As for any App where `canSaveDoc` is truthy!
    uint32_t canSaveSym = vm.internSym("cansavedoc");
    if (vm.lookupMeth(vm.oopClsId(top->appObj), canSaveSym)) {
      try {
        vm.ticksRemaining = 200000;
        Oop ok = vm.send(top->appObj, canSaveSym, nullptr, 0);
        if (vm.truth(ok)) {
          uint32_t dnSym = vm.internSym("docname");
          Oop dnOop = vm.lookupMeth(vm.oopClsId(top->appObj), dnSym)
                    ? vm.send(top->appObj, dnSym, nullptr, 0)
                    : kOopNil;
          if (!isNil(dnOop) && vm.truth(dnOop)) {
            std::string dn = vm.toStdString(dnOop);
            out.push_back({"Save (" + dn + ")", "savedoc", nullptr});
          }
          out.push_back({"Save Doc As...", "savedocprompt", nullptr});
        }
      } catch (const TerseError&) {}
    }

    out.push_back({"Edit App Code", "", [this]() { openEditAppCodeBrowser(); }});
  } else if (top->native) {
    top->native->populateAppMenu(vm, *this, *top, out);
  }

  out.push_back({maximized ? "Overlap Window" : "Maximize Window", "",
                 [this]() { toggleMaximized(); }});
  int topSlot = zOrder[zCount - 1];
  out.push_back({"Close Window", "", [this, topSlot]() { closeWindowSlot(topSlot); }});
}

void WindowManager::layoutDropdown(bool rightAlign) {
  size_t maxChars = 14;
  for (const auto& it : menuItems) {
    if (it.label.size() > maxChars) maxChars = it.label.size();
  }
  menuW = static_cast<int16_t>(std::min<size_t>(kScreenW - 8, maxChars * kFontW + 14));
  menuH = static_cast<int16_t>(menuItems.size() * kMenuRowH + 4);
  if (menuH > kScreenH - 18) menuH = kScreenH - 18;
  menuX = rightAlign ? static_cast<int16_t>(kScreenW - menuW - 2) : 2;
  menuY = 16;
  menuHoverIdx = -1;
}

void WindowManager::openGlobalMenu() {
  if (activeMenu == 1) { activeMenu = 0; dropdownDirty = false; needsRedraw = true; return; }
  bool wasOtherMenu = (activeMenu != 0);
  buildGlobalMenu(menuItems);
  activeMenu = 1;
  layoutDropdown(false);
  if (wasOtherMenu) needsRedraw = true;
  else dropdownDirty = true;
}

void WindowManager::openAppMenu() {
  if (activeMenu == 2) { activeMenu = 0; dropdownDirty = false; needsRedraw = true; return; }
  bool wasOtherMenu = (activeMenu != 0);
  buildAppMenu(menuItems);
  activeMenu = 2;
  layoutDropdown(true);
  if (wasOtherMenu) needsRedraw = true;
  else dropdownDirty = true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void WindowManager::renderTopBar() {
  hal.resetClipRect();
  hal.fillRect(0, 0, kScreenW, kTopBarH, Color::kBarBg);
  hal.drawHLine(0, kTopBarH - 1, kScreenW, Color::kBorder);

  // NW Global Menu Wedge (downward-pointing triangle in top-left corner)
  hal.drawMenuWedge(0, 0, kWedgeBoxOverlap,
                    activeMenu == 1 ? Color::kMenuSel : Color::kBlack,
                    Color::kAccent);

  // NE App Menu Wedge (downward-pointing triangle in top-right corner)
  hal.drawMenuWedge(kScreenW - kWedgeBoxOverlap, 0, kWedgeBoxOverlap,
                    activeMenu == 2 ? Color::kMenuSel : Color::kBlack,
                    Color::kCyan);

  // Center status: TerseTalk + Current App name + Free RAM
  std::string appLabel = "Desktop";
  if (Window* top = topWindow()) {
    if (isObj(top->appObj)) {
      appLabel = vm.toStdString(vm.oopCls(top->appObj)->nameStr);
    } else if (top->native) {
      appLabel = top->native->appName();
    }
  }
  uint32_t freeKb = (vm.heap.totalBytes - vm.heap.usedBytes) / 1024u;
  char barText[80];
  std::snprintf(barText, sizeof(barText), "TT | %s (%d) %uK",
                appLabel.c_str(), zCount, static_cast<unsigned>(freeKb));
  hal.drawTextClipped(20, 4, kScreenW - 40, barText, Color::kWhite);

  // App menu hint right next to NE wedge
  hal.drawText(kScreenW - kWedgeBoxOverlap - 24, 4, "App", Color::kCyan);
}

void WindowManager::renderMaximizedWedges() {
  hal.resetClipRect();
  // Subtle 1px frame around corner wedges so they remain legible over bright plots
  hal.drawMenuWedge(0, 0, kWedgeBoxMax,
                    activeMenu == 1 ? Color::kMenuSel : Color::kBlack,
                    Color::kAccent);
  hal.drawRect(0, 0, kWedgeBoxMax, kWedgeBoxMax, Color::kBorder);

  hal.drawMenuWedge(kScreenW - kWedgeBoxMax, 0, kWedgeBoxMax,
                    activeMenu == 2 ? Color::kMenuSel : Color::kBlack,
                    Color::kCyan);
  hal.drawRect(kScreenW - kWedgeBoxMax, 0, kWedgeBoxMax, kWedgeBoxMax, Color::kBorder);
}

void WindowManager::renderWindowClient(Window& win, bool isTop) {
  int16_t cx, cy, cw, ch;
  getClientRect(win, isTop, cx, cy, cw, ch);

  // Paint Client Area clipped strictly to `(cx, cy, cw, ch)`
  hal.setClipRect(cx, cy, cw, ch);
  if (isObj(win.appObj)) {
    Oop pen = createScreenPen(vm, cx, cy, cw, ch);
    TempRootGuard rg(vm, pen);
    try {
      vm.ticksRemaining = 2000000;
      uint32_t paintSym = vm.internSym("onpaint:");
      if (!vm.lookupMeth(vm.oopClsId(win.appObj), paintSym)) {
        paintSym = vm.internSym("draw:");
      }
      if (vm.lookupMeth(vm.oopClsId(win.appObj), paintSym)) {
        vm.send(win.appObj, paintSym, &pen, 1);
      } else {
        hal.fillRect(cx, cy, cw, ch, Color::kWinBg);
        hal.drawText(cx + 6, cy + 6, "(define onPaint: p)", Color::kAccent);
      }
    } catch (const TerseError& e) {
      hal.setClipRect(cx, cy, cw, ch);
      hal.fillRect(cx, cy, cw, 22, 0x6000);
      hal.drawTextClipped(cx + 4, cy + 3, cw - 8, "Runtime Error:", Color::kAccent);
      hal.drawTextClipped(cx + 4, cy + 12, cw - 8, e.message.c_str(), Color::kWhite);
    }
  } else if (win.native) {
    win.native->onPaint(vm, hal, win, cx, cy, cw, ch);
  }

  // SE Corner Resize Grip in Overlapping Mode
  if (!(maximized && isTop)) {
    hal.setClipRect(cx, cy, cw, ch);
    int16_t gx = static_cast<int16_t>(cx + cw - 1);
    int16_t gy = static_cast<int16_t>(cy + ch - 1);
    hal.drawLine(gx - 6, gy, gx, gy - 6, Color::kBorder);
    hal.drawLine(gx - 3, gy, gx, gy - 3, Color::kAccent);
  }
  win.dirty = false;
}

void WindowManager::renderWindow(Window& win, bool isTop) {
  if (!(maximized && isTop)) {
    hal.resetClipRect();
    // Subtle 2px drop shadow on overlapping windows
    hal.fillRect(win.x + 2, win.y + win.h, win.w, 2, Color::kBlack);
    hal.fillRect(win.x + win.w, win.y + 2, 2, win.h, Color::kBlack);

    // Outer border & title bar
    uint16_t titleBg = isTop ? Color::kTitleFocus : Color::kTitleBlur;
    hal.drawRect(win.x, win.y, win.w, win.h, isTop ? Color::kAccent : Color::kBorder);
    hal.fillRect(win.x + 1, win.y + 1, win.w - 2, kWinTitleH - 1, titleBg);
    hal.drawHLine(win.x + 1, win.y + kWinTitleH - 1, win.w - 2, Color::kBorder);

    // Title text
    hal.drawTextClipped(win.x + 4, win.y + 2, win.w - 32,
                        win.title.c_str(), isTop ? Color::kAccent : Color::kWhite);

    // [M] Maximize button & [X] 2-tap Close button
    int16_t mx = static_cast<int16_t>(win.x + win.w - 25);
    int16_t xx = static_cast<int16_t>(win.x + win.w - 13);
    hal.fillRect(mx, win.y + 2, 11, kWinTitleH - 3, Color::kKeyBg);
    hal.drawChar(mx + 3, win.y + 2, '^', Color::kCyan);

    hal.fillRect(xx, win.y + 2, 11, kWinTitleH - 3, win.closeArmed ? Color::kRed : Color::kKeyBg);
    hal.drawChar(xx + 3, win.y + 2, win.closeArmed ? '?' : 'x',
                 win.closeArmed ? Color::kWhite : Color::kOrange);
  }

  renderWindowClient(win, isTop);
}

void WindowManager::renderDropdown() {
  if (activeMenu == 0 || menuItems.empty()) return;
  hal.resetClipRect();
  // Drop shadow + crisp panel
  hal.fillRect(menuX + 3, menuY + 3, menuW, menuH, Color::kBlack);
  hal.fillRect(menuX, menuY, menuW, menuH, Color::kMenuBg);
  hal.drawRect(menuX, menuY, menuW, menuH, Color::kAccent);

  int maxRows = (menuH - 4) / kMenuRowH;
  for (int i = 0; i < maxRows && i < static_cast<int>(menuItems.size()); i++) {
    int16_t ry = static_cast<int16_t>(menuY + 2 + i * kMenuRowH);
    bool hov = (i == menuHoverIdx);
    if (hov) hal.fillRect(menuX + 2, ry, menuW - 4, kMenuRowH - 1, Color::kMenuSel);
    hal.drawTextClipped(menuX + 6, ry + 2, menuW - 10,
                        menuItems[i].label.c_str(), hov ? Color::kAccent : Color::kWhite);
  }
}

void WindowManager::renderModal() {
  if (!modalActive) return;
  hal.resetClipRect();
  const int16_t bx = 22, by = 42, bw = 276;
  const int16_t bh = static_cast<int16_t>(42 + modalButtons.size() * 28);
  hal.fillRect(bx + 3, by + 3, bw, bh, Color::kBlack);
  hal.fillRect(bx, by, bw, bh, Color::kMenuBg);
  hal.drawRect(bx, by, bw, bh, Color::kAccent);
  hal.fillRect(bx + 1, by + 1, bw - 2, 14, Color::kTitleFocus);
  hal.drawHLine(bx + 1, by + 15, bw - 2, Color::kBorder);
  hal.drawTextClipped(bx + 6, by + 4, bw - 12, modalTitle.c_str(), Color::kAccent);
  hal.drawTextClipped(bx + 6, by + 21, bw - 12, modalSubtitle.c_str(), Color::kWhite);

  for (size_t i = 0; i < modalButtons.size(); i++) {
    int16_t ry = static_cast<int16_t>(by + 36 + i * 28);
    hal.fillRoundRect(bx + 10, ry, bw - 20, 22, 3, modalButtons[i].bgColor);
    hal.drawRect(bx + 10, ry, bw - 20, 22, Color::kBorder);
    hal.drawTextClipped(bx + 18, ry + 7, bw - 36, modalButtons[i].label.c_str(), Color::kWhite);
  }
}

void WindowManager::render() {
  if (keyboard->active) {
    keyboard->render(hal);
    hal.present();
    needsRedraw   = false;
    keyboardDirty = false;
    dropdownDirty = false;
    return;
  }

  hal.resetClipRect();
  if (maximized && zCount > 0) {
    Window* top = topWindow();
    if (top) renderWindow(*top, true);
    renderMaximizedWedges();
  } else {
    // Desktop background with subtle grid dots
    hal.fillRect(0, kTopBarH, kScreenW, kScreenH - kTopBarH, Color::kDarkBg);
    for (int16_t gy = kTopBarH + 12; gy < kScreenH; gy += 16) {
      for (int16_t gx = 12; gx < kScreenW; gx += 16) {
        hal.drawPixel(gx, gy, 0x2945);
      }
    }
    renderTopBar();
    for (int i = 0; i < zCount; i++) {
      renderWindow(windows[zOrder[i]], i == zCount - 1);
    }
  }

  if (activeMenu != 0) {
    renderDropdown();
  }

  if (modalActive) {
    renderModal();
  }

  if (!statusBanner.empty()) {
    if (hal.millis() < bannerExpireMs) {
      hal.resetClipRect();
      hal.fillRect(4, kScreenH - 15, kScreenW - 8, 13, Color::kMenuSel);
      hal.drawRect(4, kScreenH - 15, kScreenW - 8, 13, Color::kAccent);
      hal.drawTextClipped(8, kScreenH - 12, kScreenW - 16, statusBanner.c_str(), Color::kWhite);
    } else {
      statusBanner.clear();
    }
  }

  hal.resetClipRect();
  hal.present();
  needsRedraw   = false;
  dropdownDirty = false;
  keyboardDirty = false;
}

// ---------------------------------------------------------------------------
// Touch Dispatch & Interactive Window Management
// ---------------------------------------------------------------------------
void WindowManager::sendAppTouch(Window& win, const char* kind, int16_t lx, int16_t ly) {
  if (isObj(win.appObj)) {
    uint32_t tSym = vm.internSym("ontouch:at:");
    if (vm.lookupMeth(vm.oopClsId(win.appObj), tSym)) {
      Oop kStr = vm.newStr(kind);
      TempRootGuard rg(vm, kStr);
      Oop xyArr[2] = {makeInt30(lx), makeInt30(ly)};
      Oop pt = vm.newListFrom(xyArr, 2);
      rg.push(pt);
      Oop args[2] = {kStr, pt};
      try {
        vm.ticksRemaining = 1000000;
        vm.send(win.appObj, tSym, args, 2);
        invalidateWindow(win);
      } catch (const TerseError& e) {
        showBanner(e.message);
      }
    }
  } else if (win.native) {
    win.native->onTouch(vm, *this, win, kind, lx, ly);
  }
}

void WindowManager::handleTouchDown(int16_t tx, int16_t ty) {
  if (keyboard->active) {
    dragMode = DragMode::KEYBOARD;
    keyboard->onTouchDown(hal, tx, ty, keyboardDirty);
    if (!keyboard->active) {
      keyboardDirty = false;
      needsRedraw   = true;
    }
    return;
  }

  if (modalActive) {
    const int16_t bx = 22, by = 42, bw = 276;
    if (tx >= bx + 10 && tx < bx + bw - 10) {
      for (size_t i = 0; i < modalButtons.size(); i++) {
        int16_t ry = static_cast<int16_t>(by + 36 + i * 28);
        if (ty >= ry && ty < ry + 22) {
          auto fn = modalButtons[i].action;
          modalActive = false;
          needsRedraw = true;
          if (fn) fn();
          return;
        }
      }
    }
    return;
  }

  // 1. Check NW / NE Corner Wedges first
  int16_t wedgeSz = (maximized && zCount > 0) ? kWedgeBoxMax : kWedgeBoxOverlap;
  if (ty < wedgeSz + 2) {
    if (tx < wedgeSz + 4) {
      openGlobalMenu();
      return;
    }
    if (tx >= kScreenW - wedgeSz - 28) {
      openAppMenu();
      return;
    }
  }

  // 2. If a Dropdown Menu is open, select item or dismiss
  if (activeMenu != 0) {
    if (tx >= menuX && tx < menuX + menuW && ty >= menuY && ty < menuY + menuH) {
      int idx = (ty - (menuY + 2)) / kMenuRowH;
      if (idx >= 0 && idx < static_cast<int>(menuItems.size())) {
        menuHoverIdx  = idx;
        dragMode      = DragMode::DROPDOWN;
        dropdownDirty = true;
        return;
      }
    }
    activeMenu    = 0;
    dropdownDirty = false;
    needsRedraw   = true;
    return;
  }

  // 3. Maximized Mode: all non-wedge touches go directly to top window's client canvas!
  if (maximized && zCount > 0) {
    Window* top = topWindow();
    if (top) {
      dragWinSlot = zOrder[zCount - 1];
      dragMode    = DragMode::CLIENT_TOUCH;
      sendAppTouch(*top, "down", tx, ty);
    }
    return;
  }

  // 4. Overlapping Mode: hit-test windows top-to-bottom in Z-order
  for (int zi = zCount - 1; zi >= 0; zi--) {
    int slot = zOrder[zi];
    Window& win = windows[slot];
    if (tx < win.x || tx >= win.x + win.w || ty < win.y || ty >= win.y + win.h) {
      continue;
    }
    bool wasAlreadyTop = (zi == zCount - 1);
    bool wasCloseArmed = win.closeArmed;
    focusWindowSlot(slot);

    // Title bar hit?
    if (ty < win.y + kWinTitleH) {
      int16_t mx = static_cast<int16_t>(win.x + win.w - 26);
      int16_t xx = static_cast<int16_t>(win.x + win.w - 13);
      if (tx >= xx) {
        if (wasCloseArmed) {
          closeWindowSlot(slot);
        } else {
          win.closeArmed = true;
          needsRedraw    = true;
        }
        return;
      }
      if (tx >= mx) {
        setMaximized(true);
        return;
      }
      dragMode     = DragMode::MOVE_WIN;
      dragWinSlot  = slot;
      dragStartX   = tx;
      dragStartY   = ty;
      dragOrigWinX = win.x;
      dragOrigWinY = win.y;
      return;
    }

    // First tap inside an unfocused background window's client area raises it cleanly
    if (!wasAlreadyTop) {
      return;
    }

    // SE Resize Corner hit (12x12 bottom-right zone)?
    if (tx >= win.x + win.w - 12 && ty >= win.y + win.h - 12) {
      dragMode     = DragMode::RESIZE_WIN;
      dragWinSlot  = slot;
      dragStartX   = tx;
      dragStartY   = ty;
      dragOrigWinW = win.w;
      dragOrigWinH = win.h;
      return;
    }

    // Client canvas touch
    int16_t cx, cy, cw, ch;
    getClientRect(win, true, cx, cy, cw, ch);
    dragMode    = DragMode::CLIENT_TOUCH;
    dragWinSlot = slot;
    sendAppTouch(win, "down", tx - cx, ty - cy);
    return;
  }
}

void WindowManager::handleTouchMove(int16_t tx, int16_t ty) {
  if (dragMode == DragMode::KEYBOARD) {
    keyboard->onTouchMove(hal, tx, ty, keyboardDirty);
    return;
  }
  if (dragMode == DragMode::DROPDOWN && activeMenu != 0) {
    int idx = -1;
    if (tx >= menuX && tx < menuX + menuW && ty >= menuY && ty < menuY + menuH) {
      idx = (ty - (menuY + 2)) / kMenuRowH;
      if (idx < 0 || idx >= static_cast<int>(menuItems.size())) idx = -1;
    }
    if (idx != menuHoverIdx) {
      menuHoverIdx  = idx;
      dropdownDirty = true;
    }
    return;
  }
  if (dragWinSlot < 0 || !windows[dragWinSlot].alive) return;
  Window& win = windows[dragWinSlot];

  if (dragMode == DragMode::MOVE_WIN) {
    int16_t nx = static_cast<int16_t>(dragOrigWinX + (tx - dragStartX));
    int16_t ny = static_cast<int16_t>(dragOrigWinY + (ty - dragStartY));
    nx = std::max<int16_t>(0, std::min<int16_t>(nx, kScreenW - 48));
    ny = std::max<int16_t>(kTopBarH, std::min<int16_t>(ny, kScreenH - 24));
    if (nx != win.x || ny != win.y) {
      win.x       = nx;
      win.y       = ny;
      needsRedraw = true;
    }
  } else if (dragMode == DragMode::RESIZE_WIN) {
    int16_t nw = static_cast<int16_t>(dragOrigWinW + (tx - dragStartX));
    int16_t nh = static_cast<int16_t>(dragOrigWinH + (ty - dragStartY));
    nw = std::max<int16_t>(96, std::min<int16_t>(nw, kScreenW - win.x));
    nh = std::max<int16_t>(64, std::min<int16_t>(nh, kScreenH - win.y));
    if (nw != win.w || nh != win.h) {
      win.w       = nw;
      win.h       = nh;
      win.dirty   = true;
      needsRedraw = true;
    }
  } else if (dragMode == DragMode::CLIENT_TOUCH) {
    int16_t cx, cy, cw, ch;
    getClientRect(win, topWindow() == &win, cx, cy, cw, ch);
    sendAppTouch(win, "move", tx - cx, ty - cy);
  }
}

void WindowManager::handleTouchUp(int16_t tx, int16_t ty) {
  if (dragMode == DragMode::KEYBOARD) {
    keyboard->onTouchUp(hal, tx, ty, keyboardDirty);
    if (!keyboard->active) {
      keyboardDirty = false;
      needsRedraw   = true;
    }
    dragMode = DragMode::NONE;
    return;
  }
  if (dragMode == DragMode::DROPDOWN && activeMenu != 0) {
    int idx = menuHoverIdx;
    std::vector<MenuItem> itemsCopy = menuItems;
    activeMenu    = 0;
    menuHoverIdx  = -1;
    dropdownDirty = false;
    dragMode      = DragMode::NONE;
    needsRedraw   = true;

    if (idx >= 0 && idx < static_cast<int>(itemsCopy.size())) {
      const MenuItem& chosen = itemsCopy[idx];
      if (!chosen.selector.empty()) {
        Oop app = currentApp();
        if (isObj(app)) {
          try {
            vm.ticksRemaining = 2000000;
            vm.send(app, vm.internSym(chosen.selector.c_str()), nullptr, 0);
          } catch (const TerseError& e) {
            showBanner(e.message);
          }
        }
      } else if (chosen.nativeAction) {
        chosen.nativeAction();
      }
    }
    return;
  }

  if (dragMode == DragMode::CLIENT_TOUCH && dragWinSlot >= 0 && windows[dragWinSlot].alive) {
    Window& win = windows[dragWinSlot];
    int16_t cx, cy, cw, ch;
    getClientRect(win, topWindow() == &win, cx, cy, cw, ch);
    sendAppTouch(win, "up", tx - cx, ty - cy);
  }

  dragMode    = DragMode::NONE;
  dragWinSlot = -1;
}

void WindowManager::tick() {
  // 1. Poll Touch
  int16_t tx = 0, ty = 0;
  bool down = hal.pollTouch(tx, ty);
  if (down && !prevTouchDown) {
    lastTouchX = tx;
    lastTouchY = ty;
    handleTouchDown(tx, ty);
  } else if (down && prevTouchDown) {
    if (tx != lastTouchX || ty != lastTouchY) {
      lastTouchX = tx;
      lastTouchY = ty;
      handleTouchMove(tx, ty);
    }
  } else if (!down && prevTouchDown) {
    handleTouchUp(lastTouchX, lastTouchY);
  }
  prevTouchDown = down;

  // 2. Dispatch periodic `onTick` animation ONLY on the topmost focused window!
  //    Background windows stay paused so they never fight for focus or redraw over/under top!
  if (!keyboard->active && activeMenu == 0 && !modalActive) {
    if (Window* top = topWindow()) {
      uint32_t now = hal.millis();
      if (top->animIntervalMs > 0 && (now - top->lastStepMs) >= top->animIntervalMs) {
        top->lastStepMs = now;
        if (isObj(top->appObj)) {
          uint32_t tickSym = vm.internSym("ontick");
          if (vm.lookupMeth(vm.oopClsId(top->appObj), tickSym)) {
            try {
              vm.ticksRemaining = 1000000;
              vm.send(top->appObj, tickSym, nullptr, 0);
              invalidateWindow(*top);
            } catch (const TerseError& e) {
              top->animIntervalMs = 0;
              showBanner(e.message);
            }
          }
        }
      }
    }
  }

  // 3. Expire status banner cleanly
  if (!statusBanner.empty() && hal.millis() >= bannerExpireMs) {
    statusBanner.clear();
    needsRedraw = true;
  }

  // 4. Layered Tear-Free Partial Rendering (avoids wiping screen or redrawing background windows!)
  if (needsRedraw) {
    render();
    if (Window* top = topWindow()) {
      if (top->animIntervalMs > 0) top->lastStepMs = hal.millis();
    }
  } else if (keyboard->active) {
    if (keyboardDirty) {
      keyboard->render(hal);
      hal.present();
      keyboardDirty = false;
    }
  } else if (activeMenu != 0) {
    if (dropdownDirty) {
      if (maximized && zCount > 0) renderMaximizedWedges();
      else renderTopBar();
      renderDropdown();
      hal.resetClipRect();
      hal.present();
      dropdownDirty = false;
    }
  } else if (Window* top = topWindow()) {
    if (top->dirty) {
      renderWindowClient(*top, true);
      if (maximized) {
        renderMaximizedWedges();
      }
      hal.resetClipRect();
      hal.present();
      if (top->animIntervalMs > 0) {
        top->lastStepMs = hal.millis();
      }
    }
  }
}

}  // namespace tt
