#include "tt_ide.h"
#include "tt_pen.h"
#include "vm/tt_world.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tt {

namespace {

// ---------------------------------------------------------------------------
// 1. Live Graphics Pad (`GraphicsPadController`)
// ---------------------------------------------------------------------------
class GraphicsPadController : public NativeWinController {
 public:
  std::string code;
  std::string lastError;

  explicit GraphicsPadController(const char* initCode) {
    if (initCode && initCode[0]) {
      code = initCode;
    } else {
      code = "p clear: 001 $ world: -6.3 @ -1.6 to: 6.3 @ 1.6 $ axes.\n"
             "p yellow plot: FN(x: x sin).\n"
             "p cyan plot: FN(x: (x * 3) cos * 0.6)";
    }
  }

  const char* appName() const override { return "GraphicsPad"; }

  void onPaint(VM& vm, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    const int16_t barH   = 13;
    const int16_t canvH  = (ch > barH + 8) ? static_cast<int16_t>(ch - barH) : ch;
    lastError.clear();

    // Create screen Pen `a` (aliased to `p = a` in snippet wrapper) for canvas region
    Oop pen = createScreenPen(vm, cx, cy, cw, canvH);
    TempRootGuard rg(vm, pen);
    Oop tmp = vm.newUsr(CLS_TMP);
    rg.push(tmp);

    std::string wrapped = "p = a. " + code;
    try {
      vm.ticksRemaining = 2000000;
      vm.evalCode(tmp, wrapped.c_str(), &pen, 1);
    } catch (const TerseError& e) {
      lastError = e.message;
    }

    // Restore clip rect for bottom code strip
    hal.setClipRect(cx, cy, cw, ch);
    if (!lastError.empty()) {
      hal.fillRect(cx, cy + canvH - 11, cw, 11, 0x6000);
      hal.drawTextClipped(cx + 3, cy + canvH - 9, cw - 6, lastError.c_str(), Color::kWhite);
    }

    if (ch > barH + 8) {
      int16_t by = static_cast<int16_t>(cy + canvH);
      hal.fillRect(cx, by, cw, barH, Color::kBarBg);
      hal.drawHLine(cx, by, cw, Color::kBorder);
      hal.fillRoundRect(cx + 2, by + 2, 30, barH - 3, 2, Color::kMenuSel);
      hal.drawText(cx + 5, by + 3, "Edit", Color::kAccent);
      // Display single-line summary of code
      std::string oneLine = code;
      for (char& ch2 : oneLine) if (ch2 == '\n') ch2 = ' ';
      hal.drawTextClipped(cx + 36, by + 3, cw - 38, oneLine.c_str(), Color::kCyan);
    }
  }

  void openEditor(WindowManager& wm, Window& win) {
    GraphicsPadController* self = this;
    Window* wPtr = &win;
    wm.openTextPrompt("Graphics Pad (`p` = Pen)", code, true,
                      [self, &wm, wPtr](const std::string& updated) {
                        self->code = updated;
                        wm.invalidateWindow(*wPtr);
                      });
  }

  void onTouch(VM&, WindowManager& wm, Window& win,
               const char* kind, int16_t, int16_t ly) override {
    if (std::strcmp(kind, "down") != 0) return;
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    if (ly >= ch - 14) {
      openEditor(wm, win);
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
    out.push_back({"Edit Pad Code...", "", [this, &wm, &win]() { openEditor(wm, win); }});
    out.push_back({"Preset: Trig Waves", "", [this, &wm, &win]() {
      code = "p clear: 001 $ world: -6.3 @ -1.6 to: 6.3 @ 1.6 $ axes.\n"
             "p yellow plot: FN(x: x sin).\n"
             "p cyan plot: FN(x: (x * 3) cos * 0.6)";
      wm.invalidateWindow(win);
    }});
    out.push_back({"Preset: Polar Rose", "", [this, &wm, &win]() {
      code = "p clear $ world: -1.2 @ -1.2 to: 1.2 @ 1.2 $ axes.\n"
             "p magenta param: FN(t: r = (t * 5) cos. (r * t cos) @ (r * t sin)) from: 0 to: Num tau steps: 240";
      wm.invalidateWindow(win);
    }});
    out.push_back({"Preset: Rainbow Rings", "", [this, &wm, &win]() {
      code = "p clear. c = (p wid / 2) @ (p hei / 2).\n"
             "FOR(i: 36) DO(p hue: i * 10 $ circ: c r: i * 3 + 4)";
      wm.invalidateWindow(win);
    }});
    out.push_back({"Preset: Lissajous", "", [this, &wm, &win]() {
      code = "p clear: 001 $ world: -1.2 @ -1.2 to: 1.2 @ 1.2 $ axes.\n"
             "p green param: FN(t: (t * 3) sin @ (t * 4) cos) from: 0 to: Num tau steps: 240";
      wm.invalidateWindow(win);
    }});
    out.push_back({"Save to /sd/pad.tt", "", [this, &vm, &wm]() {
      if (vm.writeVfsFile("/sd/pad.tt", code.data(), code.size(), false)) {
        wm.showBanner("Saved /sd/pad.tt");
      } else {
        wm.showBanner("Save failed (check SD)");
      }
    }});
  }
};

// ---------------------------------------------------------------------------
// 2. Interactive Workspace REPL (`WorkspaceController`)
// ---------------------------------------------------------------------------
class WorkspaceController : public NativeWinController {
 public:
  std::string expr;
  std::vector<std::string> history;

  explicit WorkspaceController(const char* initExpr) {
    expr = (initExpr && initExpr[0]) ? initExpr : "(1 to: 10) map: FN(x: x sq)";
    history.push_back("Welcome to TerseTalk-CYD!");
    history.push_back("Tap [Edit] to write code, [Eval] to run.");
  }

  const char* appName() const override { return "Workspace"; }

  void evalNow(VM& vm, WindowManager& wm, Window& win) {
    history.push_back("> " + expr);
    Oop tmp = vm.newUsr(CLS_TMP);
    TempRootGuard rg(vm, tmp);
    try {
      vm.ticksRemaining = 2000000;
      Oop res = vm.evalCode(tmp, expr.c_str());
      history.push_back("=> " + vm.toReprString(res));
    } catch (const TerseError& e) {
      history.push_back("ERR: " + e.message);
    }
    while (history.size() > 24) history.erase(history.begin());
    wm.invalidateWindow(win);
  }

  void openEditor(VM& vm, WindowManager& wm, Window& win) {
    wm.openTextPrompt("Workspace Expression", expr, true,
                      [this, &vm, &wm, &win](const std::string& s) {
                        expr = s;
                        evalNow(vm, wm, win);
                      });
  }

  void onPaint(VM&, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);

    // Top input bar (height 26)
    hal.fillRect(cx, cy, cw, 26, Color::kBlack);
    hal.drawHLine(cx, cy + 26, cw, Color::kBorder);

    // [Edit] & [Eval] buttons on right
    int16_t btnX = static_cast<int16_t>(cx + cw - 66);
    hal.fillRoundRect(btnX, cy + 2, 30, 21, 2, Color::kKeySpec);
    hal.drawText(btnX + 3, cy + 9, "Edit", Color::kAccent);
    hal.fillRoundRect(btnX + 33, cy + 2, 31, 21, 2, 0x03E0);
    hal.drawText(btnX + 37, cy + 9, "Eval", Color::kWhite);

    std::string oneLine = expr;
    for (char& c : oneLine) if (c == '\n') c = ' ';
    hal.drawTextClipped(cx + 3, cy + 9, cw - 72, oneLine.c_str(), Color::kCyan);

    // Output log lines below
    int maxLines = (ch - 30) / (kFontH + 2);
    if (maxLines < 1) return;
    int startIdx = static_cast<int>(history.size()) - maxLines;
    if (startIdx < 0) startIdx = 0;
    int16_t ly = static_cast<int16_t>(cy + 30);
    for (size_t i = static_cast<size_t>(startIdx); i < history.size(); i++) {
      const std::string& ln = history[i];
      uint16_t col = Color::kWhite;
      if (ln.rfind("=>", 0) == 0) col = Color::kGreen;
      else if (ln.rfind("ERR:", 0) == 0) col = Color::kOrange;
      else if (ln.rfind(">", 0) == 0) col = Color::kAccent;
      hal.drawTextClipped(cx + 4, ly, cw - 8, ln.c_str(), col);
      ly = static_cast<int16_t>(ly + kFontH + 2);
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    if (std::strcmp(kind, "down") != 0) return;
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    if (ly <= 26) {
      if (lx >= cw - 33) evalNow(vm, wm, win);
      else openEditor(vm, wm, win);
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
    out.push_back({"Edit Expression", "", [this, &vm, &wm, &win]() { openEditor(vm, wm, win); }});
    out.push_back({"Eval Now", "", [this, &vm, &wm, &win]() { evalNow(vm, wm, win); }});
    out.push_back({"Clear Log", "", [this, &wm, &win]() {
      history.clear();
      wm.invalidateWindow(win);
    }});
    out.push_back({"Show RAM / GC", "", [this, &vm, &wm, &win]() {
      vm.runGC();
      char buf[96];
      std::snprintf(buf, sizeof(buf), "Heap: %u/%u B used (%u GCs)",
                    static_cast<unsigned>(vm.heap.usedBytes),
                    static_cast<unsigned>(vm.heap.totalBytes),
                    static_cast<unsigned>(vm.heap.gcCount));
      history.push_back(buf);
      wm.invalidateWindow(win);
    }});
  }
};

// ---------------------------------------------------------------------------
// 3. Live Class & Method Browser (`ClassBrowserController`)
// ---------------------------------------------------------------------------
class ClassBrowserController : public NativeWinController {
 public:
  bool        showAllClasses = false;
  std::string selectedCls;
  int         clsScroll      = 0;
  int         methScroll     = 0;

  explicit ClassBrowserController(const char* focusCls) {
    selectedCls = (focusCls && focusCls[0]) ? focusCls : "GraphApp";
  }

  const char* appName() const override { return "Browser"; }

  void collectClasses(VM& vm, std::vector<std::string>& out) const {
    out.clear();
    for (uint16_t i = 1; i <= vm.classCount; i++) {
      ObjCls* c = vm.clsById(i);
      if (!c) continue;
      std::string nm = vm.toStdString(c->nameStr);
      if (!showAllClasses) {
        // Default view: focus on Usr/App classes + Pen/Bmp/List/Dict/Num/Str
        bool keep = c->isUsrCls || i == CLS_PEN || i == CLS_BMP || i == CLS_LIST ||
                    i == CLS_DICT || i == CLS_NUM || i == CLS_STR;
        if (!keep || (nm.size() > 3 && nm.substr(nm.size() - 3) == "Cls")) {
          if (nm != selectedCls) continue;
        }
      }
      out.push_back(nm);
    }
    std::sort(out.begin(), out.end());
  }

  ObjCls* currentObjCls(VM& vm) const {
    if (selectedCls.empty()) return nullptr;
    Oop cOop = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(selectedCls.c_str(), selectedCls.size())));
    if (isObj(cOop) && objFromOop(cOop)->format == OBJ_CLS) {
      return reinterpret_cast<ObjCls*>(objFromOop(cOop));
    }
    return nullptr;
  }

  void collectMethods(VM& vm, ObjCls* c, std::vector<std::string>& out) const {
    out.clear();
    if (!c) return;
    auto* md = reinterpret_cast<ObjDict*>(objFromOop(c->methDict));
    for (uint32_t i = 0; i < md->cap; i++) {
      Oop k = md->entries[i].key;
      if (k == kOopNull || k == kOopSuper) continue;
      out.push_back(vm.toStdString(k));
    }
    std::sort(out.begin(), out.end());
  }

  void editMethod(VM& vm, WindowManager& wm, Window& win, ObjCls* c, const std::string& sel) {
    Oop mOop = vm.dictGet(c->methDict, makeSym(vm.internSym(sel.c_str(), sel.size())));
    std::string initSrc;
    if (isObj(mOop) && objFromOop(mOop)->format == OBJ_METH) {
      auto* m = reinterpret_cast<ObjMeth*>(objFromOop(mOop));
      if (m->isPrim) {
        wm.showBanner(sel + " is a C++ primitive");
        return;
      }
      if (m->source) initSrc.assign(m->source, m->sourceLen);
    }
    std::string clsName = vm.toStdString(c->nameStr);
    std::string pTitle  = clsName + " >> " + sel;
    wm.openTextPrompt(pTitle.c_str(), initSrc, true,
                      [&vm, &wm, &win, clsName, sel](const std::string& newCode) {
                        Oop co = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(clsName.c_str(), clsName.size())));
                        if (isObj(co) && objFromOop(co)->format == OBJ_CLS) {
                          auto* targetCls = reinterpret_cast<ObjCls*>(objFromOop(co));
                          try {
                            vm.addUsrMeth(targetCls, sel.c_str(), "", newCode.data(), newCode.size(), false, true);
                            wm.showBanner("Saved " + clsName + ">>" + sel);
                          } catch (const TerseError& e) {
                            wm.showBanner(e.message);
                          }
                        }
                        win.dirty = true;
                        wm.invalidateAll();
                      });
  }

  static bool isClassModified(ObjCls* c) {
    if (!c) return false;
    if (!c->isPrimordial || c->varsModified) return true;
    if (!isObj(c->methDict)) return false;
    auto* md = reinterpret_cast<ObjDict*>(objFromOop(c->methDict));
    for (uint32_t i = 0; i < md->cap; i++) {
      Oop k = md->entries[i].key;
      if (k == kOopNull || k == kOopSuper) continue;
      Oop v = md->entries[i].val;
      if (isObj(v) && objFromOop(v)->format == OBJ_METH) {
        auto* m = reinterpret_cast<ObjMeth*>(objFromOop(v));
        if (!m->isPrim && !m->isPrimordial && !m->isAutoAccessor) return true;
      }
    }
    return false;
  }

  static bool isMethModified(VM& vm, ObjCls* c, const std::string& sel) {
    if (!c || !isObj(c->methDict)) return false;
    Oop mOop = vm.dictGet(c->methDict, makeSym(vm.internSym(sel.c_str(), sel.size())));
    if (!isObj(mOop) || objFromOop(mOop)->format != OBJ_METH) return false;
    auto* m = reinterpret_cast<ObjMeth*>(objFromOop(mOop));
    return !m->isPrim && !m->isPrimordial && !m->isAutoAccessor;
  }

  void onPaint(VM& vm, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);
    int16_t splitX = static_cast<int16_t>(cx + (cw * 2) / 5);
    hal.drawVLine(splitX, cy, ch, Color::kBorder);

    // Header row (height 13)
    hal.fillRect(cx, cy, cw, 13, Color::kBarBg);
    hal.drawHLine(cx, cy + 13, cw, Color::kBorder);
    hal.drawText(cx + 3, cy + 3, showAllClasses ? "All [v]" : "Usr [v]", Color::kAccent);

    ObjCls* c = currentObjCls(vm);
    if (c) {
      std::string hdr = vm.toStdString(c->nameStr);
      if (c->superClsId) {
        hdr += " < " + vm.toStdString(vm.clsById(c->superClsId)->nameStr);
      }
      hal.drawTextClipped(splitX + 4, cy + 3, cx + cw - splitX - 36, hdr.c_str(), Color::kCyan);
      hal.fillRoundRect(cx + cw - 30, cy + 1, 28, 11, 2, Color::kMenuSel);
      hal.drawText(cx + cw - 28, cy + 3, "+Mth", Color::kWhite);
    }

    int visRows = (ch - 15) / (kFontH + 2);
    if (visRows < 1) return;

    // Left Pane: Classes
    std::vector<std::string> classes;
    collectClasses(vm, classes);
    if (clsScroll > static_cast<int>(classes.size()) - visRows) {
      clsScroll = std::max(0, static_cast<int>(classes.size()) - visRows);
    }
    for (int i = 0; i < visRows && (clsScroll + i) < static_cast<int>(classes.size()); i++) {
      const std::string& nm = classes[clsScroll + i];
      int16_t ry = static_cast<int16_t>(cy + 15 + i * (kFontH + 2));
      bool sel = (nm == selectedCls);
      Oop co = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(nm.c_str(), nm.size())));
      bool mod = (isObj(co) && objFromOop(co)->format == OBJ_CLS) &&
                 isClassModified(reinterpret_cast<ObjCls*>(objFromOop(co)));
      if (sel) hal.fillRect(cx + 1, ry - 1, splitX - cx - 2, kFontH + 2, Color::kMenuSel);
      std::string disp = mod ? ("*" + nm) : nm;
      uint16_t fg = sel ? Color::kAccent : (mod ? Color::kCyan : Color::kWhite);
      hal.drawTextClipped(cx + 3, ry, splitX - cx - 5, disp.c_str(), fg);
    }

    // Right Pane: Vars + Methods
    if (!c) return;
    std::string varsLine = c->varsModified ? "*vars: " : "vars: ";
    auto* vl = reinterpret_cast<ObjList*>(objFromOop(c->myVarsList));
    for (uint32_t i = 0; i < vl->len; i++) {
      if (i > 0) varsLine.push_back(' ');
      varsLine += vm.toStdString(vl->items[i]);
    }
    hal.drawTextClipped(splitX + 4, cy + 15, cx + cw - splitX - 6, varsLine.c_str(), Color::kGreen);

    std::vector<std::string> meths;
    collectMethods(vm, c, meths);
    int mVis = visRows - 1;
    if (methScroll > static_cast<int>(meths.size()) - mVis) {
      methScroll = std::max(0, static_cast<int>(meths.size()) - mVis);
    }
    for (int i = 0; i < mVis && (methScroll + i) < static_cast<int>(meths.size()); i++) {
      const std::string& ms = meths[methScroll + i];
      bool mod = isMethModified(vm, c, ms);
      std::string disp = mod ? ("*" + ms) : ms;
      int16_t ry = static_cast<int16_t>(cy + 15 + (i + 1) * (kFontH + 2));
      hal.drawTextClipped(splitX + 4, ry, cx + cw - splitX - 6, disp.c_str(),
                          mod ? Color::kAccent : Color::kWhite);
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    if (std::strcmp(kind, "down") != 0) return;
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    int16_t splitL = static_cast<int16_t>((cw * 2) / 5);

    if (ly < 13) {
      if (lx < splitL) {
        showAllClasses = !showAllClasses;
        clsScroll = 0;
        win.dirty = true; wm.invalidateAll();
      } else if (lx >= cw - 32) {
        // +Mth: prompt for new selector name, then method body
        if (ObjCls* c = currentObjCls(vm)) {
          wm.openTextPrompt("New Method Selector (e.g. draw:)", "", false,
                            [this, &vm, &wm, &win, c](const std::string& sel) {
                              if (!sel.empty()) editMethod(vm, wm, win, c, sel);
                            });
        }
      }
      return;
    }

    int row = (ly - 15) / (kFontH + 2);
    if (lx < splitL) {
      std::vector<std::string> classes;
      collectClasses(vm, classes);
      int idx = clsScroll + row;
      if (idx >= 0 && idx < static_cast<int>(classes.size())) {
        selectedCls = classes[idx];
        methScroll  = 0;
        win.dirty   = true;
        wm.invalidateAll();
      }
    } else {
      ObjCls* c = currentObjCls(vm);
      if (!c) return;
      if (row == 0) {
        // Edit instance variables
        std::string curVars;
        auto* vl = reinterpret_cast<ObjList*>(objFromOop(c->myVarsList));
        for (uint32_t i = 0; i < vl->len; i++) {
          if (i > 0) curVars.push_back(' ');
          curVars += vm.toStdString(vl->items[i]);
        }
        std::string clsName = vm.toStdString(c->nameStr);
        wm.openTextPrompt(("InstVars of " + clsName).c_str(), curVars, false,
                          [&vm, &wm, &win, clsName](const std::string& nv) {
                            Oop co = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(clsName.c_str(), clsName.size())));
                            if (isObj(co) && objFromOop(co)->format == OBJ_CLS) {
                              try {
                                vm.defineVars(reinterpret_cast<ObjCls*>(objFromOop(co)), nv.c_str(), true);
                              } catch (const TerseError& e) {
                                wm.showBanner(e.message);
                              }
                            }
                            win.dirty = true; wm.invalidateAll();
                          });
      } else {
        std::vector<std::string> meths;
        collectMethods(vm, c, meths);
        int idx = methScroll + (row - 1);
        if (idx >= 0 && idx < static_cast<int>(meths.size())) {
          editMethod(vm, wm, win, c, meths[idx]);
        }
      }
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
    out.push_back({"Scroll Down", "", [this, &wm, &win]() {
      clsScroll += 5; methScroll += 5; win.dirty = true; wm.invalidateAll();
    }});
    out.push_back({"Scroll Up", "", [this, &wm, &win]() {
      clsScroll = std::max(0, clsScroll - 5);
      methScroll = std::max(0, methScroll - 5);
      win.dirty = true; wm.invalidateAll();
    }});
    out.push_back({"New App Subclass...", "", [this, &vm, &wm, &win]() {
      wm.openTextPrompt("New App Subclass Name", "MyApp", false,
                        [this, &vm, &wm, &win](const std::string& nm) {
                          if (nm.empty()) return;
                          Oop appCls = vm.dictGet(vm.globalsDict, makeSym(vm.internSym("app")));
                          uint16_t supId = (isObj(appCls) && objFromOop(appCls)->format == OBJ_CLS)
                                         ? reinterpret_cast<ObjCls*>(objFromOop(appCls))->clsId
                                         : static_cast<uint16_t>(CLS_USR);
                          try {
                            vm.defineClass(nm.c_str(), supId, true);
                            selectedCls = nm;
                            wm.showBanner("Created class " + nm);
                          } catch (const TerseError& e) {
                            wm.showBanner(e.message);
                          }
                          win.dirty = true; wm.invalidateAll();
                        });
    }});
    out.push_back({"Launch Selected Class", "", [this, &wm]() {
      wm.launchTerseAppByClassName(selectedCls.c_str());
    }});
    out.push_back({"Save World", "", [&wm]() {
      wm.promptSaveClassState();
    }});
    out.push_back({"Load World", "", [&wm]() {
      if (wm.maximized) wm.maximized = false;
      wm.createNativeWindow(createFilesController("/sd/worlds"), "Worlds (/sd/worlds)", 18, 24, 284, 198);
    }});
    out.push_back({"Reset to Prelude...", "", [&wm]() {
      wm.requestLoadClassState("");
    }});
  }
};

// ---------------------------------------------------------------------------
// 4. Files & Documents Browser (`/sd/docs`, `/sd`, `/sd/worlds`, `/flash`)
// ---------------------------------------------------------------------------
class FilesController : public NativeWinController {
 public:
  std::string currentDir = "/sd/docs";
  std::string previewName;
  std::string previewBody;
  int16_t     openBtnY   = -1;

  explicit FilesController(const char* initDir) {
    if (initDir && initDir[0]) currentDir = initDir;
  }

  const char* appName() const override { return "Files"; }

  static bool isDocFile(const std::string& nm) {
    return nm.size() > 4 && nm.substr(nm.size() - 4) == ".doc";
  }

  static bool isStateFile(const std::string& dir, const std::string& nm) {
    return dir != "/flash" && nm.size() > 3 && nm.substr(nm.size() - 3) == ".tt";
  }

  void openSelectedDoc(VM& vm, WindowManager& wm) {
    if (previewName.empty()) return;
    std::string full = currentDir + "/" + previewName;
    Oop pStr = vm.newStr(full.data(), full.size());
    TempRootGuard rg(vm, pStr);
    Oop tmp = vm.newUsr(CLS_TMP);
    rg.push(tmp);
    try {
      vm.ticksRemaining = 2000000;
      vm.evalCode(tmp, "App fileIn: a", &pStr, 1);
    } catch (const TerseError& e) {
      wm.showBanner(e.message);
    }
  }

  void onPaint(VM& vm, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);
    openBtnY = -1;

    // Tab strip: [Docs] [/sd] [Worlds] [Flash]
    hal.fillRect(cx, cy, cw, 14, Color::kBarBg);
    hal.drawHLine(cx, cy + 14, cw, Color::kBorder);
    auto drawTab = [&](int16_t tx, int16_t tw, const char* label, const char* dir) {
      bool act = (currentDir == dir);
      hal.fillRoundRect(tx, cy + 1, tw, 12, 2, act ? Color::kMenuSel : Color::kKeyBg);
      hal.drawText(tx + 4, cy + 3, label, act ? Color::kAccent : Color::kWhite);
    };
    drawTab(cx + 2,   34, "Docs",   "/sd/docs");
    drawTab(cx + 39,  30, "/sd",    "/sd");
    drawTab(cx + 72,  46, "Worlds", "/sd/worlds");
    drawTab(cx + 121, 40, "Flash",  "/flash");
    if (currentDir == "/sd/worlds") {
      hal.fillRoundRect(cx + cw - 38, cy + 1, 36, 12, 2, 0x03E0);
      hal.drawText(cx + cw - 34, cy + 3, "+Save", Color::kWhite);
    }

    std::vector<std::pair<std::string, uint32_t>> files;
    vm.listVfsFiles(currentDir.c_str(), files);
    std::sort(files.begin(), files.end());

    int16_t ry = static_cast<int16_t>(cy + 17);
    if (files.empty()) {
      hal.drawText(cx + 6, ry, "(no files in directory)", 0x8410);
    }
    int maxListRows = previewName.empty() ? ((ch - 18) / 10) : 5;
    for (int i = 0; i < maxListRows && i < static_cast<int>(files.size()); i++) {
      char rowBuf[80];
      std::snprintf(rowBuf, sizeof(rowBuf), "%-20s %6u B",
                    files[i].first.c_str(), static_cast<unsigned>(files[i].second));
      bool sel = (files[i].first == previewName);
      if (sel) hal.fillRect(cx + 2, ry - 1, cw - 4, 10, Color::kMenuSel);
      hal.drawTextClipped(cx + 4, ry, cw - 8, rowBuf, sel ? Color::kAccent : Color::kWhite);
      ry += 10;
    }

    if (!previewName.empty() && ry + 14 < cy + ch) {
      hal.drawHLine(cx, ry + 1, cw, Color::kBorder);
      ry += 4;
      hal.drawTextClipped(cx + 4, ry, cw - 72, previewName.c_str(), Color::kCyan);
      if (isDocFile(previewName)) {
        openBtnY = static_cast<int16_t>(ry - cy - 2);
        hal.fillRoundRect(cx + cw - 56, ry - 2, 52, 11, 2, 0x03E0);
        hal.drawText(cx + cw - 52, ry, "Open Doc", Color::kWhite);
      } else if (isStateFile(currentDir, previewName)) {
        openBtnY = static_cast<int16_t>(ry - cy - 2);
        hal.fillRoundRect(cx + cw - 68, ry - 2, 65, 11, 2, 0x03E0);
        hal.drawText(cx + cw - 65, ry, "Load World", Color::kWhite);
      }
      ry += 11;
      size_t p = 0;
      while (p < previewBody.size() && ry + 8 <= cy + ch) {
        size_t nl = previewBody.find('\n', p);
        size_t len = (nl == std::string::npos) ? (previewBody.size() - p) : (nl - p);
        std::string ln = previewBody.substr(p, len);
        hal.drawTextClipped(cx + 4, ry, cw - 8, ln.c_str(), Color::kGreen);
        ry += 9;
        if (nl == std::string::npos) break;
        p = nl + 1;
      }
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    if (std::strcmp(kind, "down") != 0) return;
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);

    if (ly <= 14) {
      if (currentDir == "/sd/worlds" && lx >= cw - 40) {
        wm.promptSaveClassState();
        return;
      }
      if (lx < 38) currentDir = "/sd/docs";
      else if (lx < 71) currentDir = "/sd";
      else if (lx < 120) currentDir = "/sd/worlds";
      else if (lx < 164) currentDir = "/flash";
      previewName.clear();
      previewBody.clear();
      wm.invalidateWindow(win);
      return;
    }

    // Check [Open Doc] or [Load World] button tap in preview header!
    if (openBtnY > 0 && ly >= openBtnY && ly <= openBtnY + 13) {
      if (lx >= cw - 58 && isDocFile(previewName)) {
        openSelectedDoc(vm, wm);
        return;
      }
      if (lx >= cw - 70 && isStateFile(currentDir, previewName)) {
        std::string full = currentDir + "/" + previewName;
        wm.requestLoadClassState(full);
        return;
      }
    }

    int idx = (ly - 17) / 10;
    std::vector<std::pair<std::string, uint32_t>> files;
    vm.listVfsFiles(currentDir.c_str(), files);
    std::sort(files.begin(), files.end());
    int maxListRows = previewName.empty() ? ((ch - 18) / 10) : 5;
    if (idx >= 0 && idx < maxListRows && idx < static_cast<int>(files.size())) {
      previewName = files[idx].first;
      std::string full = currentDir + "/" + previewName;
      previewBody.clear();
      vm.readVfsFile(full.c_str(), previewBody);
      wm.invalidateWindow(win);
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
    if (isDocFile(previewName)) {
      out.push_back({"Open Selected Doc", "", [this, &vm, &wm]() {
        openSelectedDoc(vm, wm);
      }});
    }
    if (isStateFile(currentDir, previewName)) {
      out.push_back({"Load World", "", [this, &wm]() {
        wm.requestLoadClassState(currentDir + "/" + previewName);
      }});
    }
    out.push_back({"Save World", "", [&wm]() {
      wm.promptSaveClassState();
    }});
    out.push_back({"Reset to Prelude...", "", [&wm]() {
      wm.requestLoadClassState("");
    }});
    out.push_back({"Run Selected Script", "", [this, &vm, &wm, &win]() {
      if (previewName.empty()) return;
      std::string full = currentDir + "/" + previewName;
      std::string src;
      if (vm.readVfsFile(full.c_str(), src)) {
        Oop tmp = vm.newUsr(CLS_TMP);
        TempRootGuard rg(vm, tmp);
        try {
          vm.ticksRemaining = 2000000;
          Oop r = vm.evalCode(tmp, src.c_str());
          wm.showBanner("=> " + vm.toStdString(r));
        } catch (const TerseError& e) {
          wm.showBanner(e.message);
        }
        wm.invalidateWindow(win);
      }
    }});
    out.push_back({"Overlay .tt (No Reset)", "", [this, &vm, &wm, &win]() {
      if (previewName.empty()) return;
      std::string full = currentDir + "/" + previewName;
      std::string err;
      if (loadWorldFile(vm, vm.worldName.c_str(), full.c_str(), &err)) {
        wm.showBanner("Merged " + previewName);
      } else {
        wm.showBanner(err);
      }
      wm.invalidateWindow(win);
    }});
    if (currentDir.rfind("/sd", 0) == 0 && !previewName.empty()) {
      out.push_back({"Delete Selected File", "", [this, &vm, &wm, &win]() {
        std::string full = currentDir + "/" + previewName;
        if (vm.deleteVfsFile(full.c_str())) {
          wm.showBanner("Deleted " + previewName);
          previewName.clear();
          previewBody.clear();
        }
        wm.invalidateWindow(win);
      }});
    }
  }
};

}  // namespace

NativeWinController* createGraphicsPadController(const char* initialCode) {
  return new GraphicsPadController(initialCode);
}
NativeWinController* createWorkspaceController(const char* initialCode) {
  return new WorkspaceController(initialCode);
}
NativeWinController* createClassBrowserController(const char* focusClassName) {
  return new ClassBrowserController(focusClassName);
}
NativeWinController* createFilesController(const char* initialDir) {
  return new FilesController(initialDir);
}

}  // namespace tt
