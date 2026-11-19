#include "tt_ide.h"
#include "tt_pen.h"
#include "tt_scrollbar.h"
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
    const int16_t fh     = hal.fontH();
    const int16_t fw     = hal.fontW();
    const int16_t barH   = static_cast<int16_t>(fh + 5);
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
      int16_t errH = static_cast<int16_t>(fh + 3);
      hal.fillRect(cx, cy + canvH - errH, cw, errH, 0x6000);
      hal.drawTextClipped(cx + 3, cy + canvH - errH + 2, cw - 6, lastError.c_str(), Color::kWhite);
    }

    if (ch > barH + 8) {
      int16_t by   = static_cast<int16_t>(cy + canvH);
      int16_t btnW = static_cast<int16_t>(4 * fw + 6);
      hal.fillRect(cx, by, cw, barH, Color::kBarBg);
      hal.drawHLine(cx, by, cw, Color::kBorder);
      hal.fillRoundRect(cx + 2, by + 2, btnW, barH - 3, 2, Color::kMenuSel);
      hal.drawText(cx + 5, by + 3, "Edit", Color::kAccent);
      std::string oneLine = code;
      for (char& ch2 : oneLine) if (ch2 == '\n') ch2 = ' ';
      hal.drawTextClipped(cx + btnW + 6, by + 3, cw - btnW - 8, oneLine.c_str(), Color::kCyan);
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
    const int16_t barH = static_cast<int16_t>(wm.hal.fontH() + 5);
    if (ly >= ch - barH - 1) {
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
  int  logScroll    = 0;
  bool logFollow    = true;
  bool logThumbDrag = false;

  explicit WorkspaceController(const char* initExpr) {
    expr = (initExpr && initExpr[0]) ? initExpr : "(1 to: 10) map: FN(x: x sq)";
    history.push_back("Welcome to TerseTalk-CYD!");
    history.push_back("Tap [Edit] to write code, [Eval] to run.");
  }

  const char* appName() const override { return "Workspace"; }

  void pushLogLines(const std::string& prefix, const std::string& text) {
    size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string seg = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
      history.push_back((first ? prefix : "   ") + seg);
      first = false;
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    while (history.size() > 120) history.erase(history.begin());
    logFollow = true;
  }

  void evalNow(VM& vm, WindowManager& wm, Window& win) {
    pushLogLines("> ", expr);
    Oop tmp = vm.newUsr(CLS_TMP);
    TempRootGuard rg(vm, tmp);
    try {
      vm.ticksRemaining = 2000000;
      Oop res = vm.evalCode(tmp, expr.c_str());
      pushLogLines("=> ", vm.toReprString(res));
    } catch (const TerseError& e) {
      pushLogLines("ERR: ", e.message);
    }
    wm.invalidateWindow(win);
  }

  void openEditor(VM& vm, WindowManager& wm, Window& win) {
    wm.openTextPrompt("Workspace Expression", expr, true,
                      [this, &vm, &wm, &win](const std::string& s) {
                        expr = s;
                        evalNow(vm, wm, win);
                      });
  }

  static int16_t topBarHeight(const DisplayHal& hal) {
    return static_cast<int16_t>(hal.fontH() + 14);
  }

  void onPaint(VM&, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);

    const int16_t fw   = hal.fontW();
    const int16_t fh   = hal.fontH();
    const int16_t topH = topBarHeight(hal);
    const int16_t btnW = static_cast<int16_t>(4 * fw + 6);
    const int16_t btnH = static_cast<int16_t>(topH - 5);
    const int16_t ty   = static_cast<int16_t>(cy + (topH - fh) / 2);

    // Top input bar
    hal.fillRect(cx, cy, cw, topH, Color::kBlack);
    hal.drawHLine(cx, cy + topH, cw, Color::kBorder);

    // [Edit] & [Eval] buttons on right
    int16_t evalX = static_cast<int16_t>(cx + cw - btnW - 2);
    int16_t editX = static_cast<int16_t>(evalX - btnW - 2);
    hal.fillRoundRect(editX, cy + 2, btnW, btnH, 2, Color::kKeySpec);
    hal.drawText(editX + 3, ty, "Edit", Color::kAccent);
    hal.fillRoundRect(evalX, cy + 2, btnW, btnH, 2, 0x03E0);
    hal.drawText(evalX + 3, ty, "Eval", Color::kWhite);

    std::string oneLine = expr;
    for (char& c : oneLine) if (c == '\n') c = ' ';
    hal.drawTextClipped(cx + 3, ty, editX - (cx + 6), oneLine.c_str(), Color::kCyan);

    // Scrollable Output Log Box below (leave 8px SE grow-box margin at bottom-right)
    const int16_t logY   = static_cast<int16_t>(cy + topH + 1);
    const int16_t logH   = static_cast<int16_t>(ch - topH - 1);
    const int16_t sbH    = std::max<int16_t>(14, static_cast<int16_t>(logH - 8));
    const int16_t rowH   = static_cast<int16_t>(fh + 2);
    const int visLines   = logH / rowH;
    const int totalLines = static_cast<int>(history.size());
    if (visLines < 1) return;

    if (logFollow) {
      logScroll = std::max(0, totalLines - visLines);
    }
    logScroll = ScrollBar::clampScroll(logScroll, totalLines, visLines);

    const bool hasSb  = ScrollBar::isNeeded(totalLines, visLines);
    const int16_t sbW = hasSb ? ScrollBar::width(hal) : 0;
    const int16_t textW = static_cast<int16_t>(cw - 6 - (hasSb ? (sbW + 2) : 0));

    int16_t ly = static_cast<int16_t>(logY + 2);
    for (int i = 0; i < visLines && (logScroll + i) < totalLines; i++) {
      const std::string& ln = history[static_cast<size_t>(logScroll + i)];
      uint16_t col = Color::kWhite;
      if (ln.rfind("=>", 0) == 0) col = Color::kGreen;
      else if (ln.rfind("ERR:", 0) == 0) col = Color::kOrange;
      else if (ln.rfind(">", 0) == 0) col = Color::kAccent;
      hal.drawTextClipped(cx + 4, ly, textW, ln.c_str(), col);
      ly = static_cast<int16_t>(ly + rowH);
    }

    if (hasSb) {
      ScrollBar::draw(hal, cx, logY, cw, sbH, totalLines, visLines, logScroll);
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    const DisplayHal& hal = wm.hal;
    const int16_t topH    = topBarHeight(hal);
    const int16_t logH    = static_cast<int16_t>(ch - topH - 1);
    const int16_t sbH     = std::max<int16_t>(14, static_cast<int16_t>(logH - 8));
    const int16_t rowH    = static_cast<int16_t>(hal.fontH() + 2);
    const int visLines    = std::max(1, static_cast<int>(logH / rowH));
    const int totalLines  = static_cast<int>(history.size());

    if (ScrollBar::handleTouch(hal, kind, lx, ly,
                               0, topH + 1, cw, sbH,
                               totalLines, visLines, logScroll, logThumbDrag)) {
      logFollow = (logScroll >= totalLines - visLines);
      wm.invalidateWindow(win);
      return;
    }

    if (std::strcmp(kind, "down") != 0) return;
    if (ly <= topH) {
      int16_t btnW = static_cast<int16_t>(4 * hal.fontW() + 6);
      if (lx >= cw - btnW - 3) evalNow(vm, wm, win);
      else openEditor(vm, wm, win);
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
    out.push_back({"Edit Expression", "", [this, &vm, &wm, &win]() { openEditor(vm, wm, win); }});
    out.push_back({"Eval Now", "", [this, &vm, &wm, &win]() { evalNow(vm, wm, win); }});
    out.push_back({"Clear Log", "", [this, &wm, &win]() {
      history.clear();
      logScroll = 0;
      logFollow = true;
      wm.invalidateWindow(win);
    }});
    out.push_back({"Show RAM / GC", "", [this, &vm, &wm, &win]() {
      vm.runGC();
      char buf[96];
      std::snprintf(buf, sizeof(buf), "Heap: %u/%u B used (%u GCs)",
                    static_cast<unsigned>(vm.heap.usedBytes),
                    static_cast<unsigned>(vm.heap.totalBytes),
                    static_cast<unsigned>(vm.heap.gcCount));
      pushLogLines("", buf);
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
  bool        clsThumbDrag   = false;
  bool        methThumbDrag  = false;

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

    const int16_t fw      = hal.fontW();
    const int16_t fh      = hal.fontH();
    const int16_t hdrH    = static_cast<int16_t>(fh + 5);
    const int16_t rowH    = static_cast<int16_t>(fh + 2);
    const int16_t splitW  = static_cast<int16_t>((cw * 2) / 5);
    const int16_t splitX  = static_cast<int16_t>(cx + splitW);
    const int16_t mthBtnW = static_cast<int16_t>(4 * fw + 6);

    hal.drawVLine(splitX, cy, ch, Color::kBorder);

    // Header row
    hal.fillRect(cx, cy, cw, hdrH, Color::kBarBg);
    hal.drawHLine(cx, cy + hdrH, cw, Color::kBorder);
    hal.drawTextClipped(cx + 3, cy + 3, splitW - 5,
                        showAllClasses ? "All [v]" : "Usr [v]", Color::kAccent);

    ObjCls* c = currentObjCls(vm);
    if (c) {
      std::string hdr = vm.toStdString(c->nameStr);
      if (c->superClsId) {
        hdr += " < " + vm.toStdString(vm.clsById(c->superClsId)->nameStr);
      }
      hal.drawTextClipped(splitX + 4, cy + 3, cx + cw - splitX - mthBtnW - 8, hdr.c_str(), Color::kCyan);
      hal.fillRoundRect(cx + cw - mthBtnW - 2, cy + 1, mthBtnW, hdrH - 2, 2, Color::kMenuSel);
      hal.drawText(cx + cw - mthBtnW + 1, cy + 3, "+Mth", Color::kWhite);
    }

    const int16_t bodyY = static_cast<int16_t>(cy + hdrH + 1);
    const int16_t bodyH = static_cast<int16_t>(ch - hdrH - 1);
    const int visRows   = bodyH / rowH;
    if (visRows < 1) return;

    // Left Pane: Classes + Right-edge ScrollBar
    std::vector<std::string> classes;
    collectClasses(vm, classes);
    const int totalCls = static_cast<int>(classes.size());
    clsScroll = ScrollBar::clampScroll(clsScroll, totalCls, visRows);
    const bool clsHasSb  = ScrollBar::isNeeded(totalCls, visRows);
    const int16_t clsSbW = clsHasSb ? ScrollBar::width(hal) : 0;
    const int16_t clsTxtW = static_cast<int16_t>(splitW - 5 - (clsHasSb ? (clsSbW + 1) : 0));

    for (int i = 0; i < visRows && (clsScroll + i) < totalCls; i++) {
      const std::string& nm = classes[static_cast<size_t>(clsScroll + i)];
      int16_t ry = static_cast<int16_t>(bodyY + 1 + i * rowH);
      bool sel = (nm == selectedCls);
      Oop co = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(nm.c_str(), nm.size())));
      bool mod = (isObj(co) && objFromOop(co)->format == OBJ_CLS) &&
                 isClassModified(reinterpret_cast<ObjCls*>(objFromOop(co)));
      if (sel) hal.fillRect(cx + 1, ry - 1, splitW - 2 - clsSbW, rowH, Color::kMenuSel);
      std::string disp = mod ? ("*" + nm) : nm;
      uint16_t fg = sel ? Color::kAccent : (mod ? Color::kCyan : Color::kWhite);
      hal.drawTextClipped(cx + 3, ry, clsTxtW, disp.c_str(), fg);
    }
    if (clsHasSb) {
      ScrollBar::draw(hal, cx, bodyY, splitW, bodyH, totalCls, visRows, clsScroll);
    }

    // Right Pane: Vars row + Methods List Box + Right-edge ScrollBar
    if (!c) return;
    std::string varsLine = c->varsModified ? "*vars: " : "vars: ";
    auto* vl = reinterpret_cast<ObjList*>(objFromOop(c->myVarsList));
    for (uint32_t i = 0; i < vl->len; i++) {
      if (i > 0) varsLine.push_back(' ');
      varsLine += vm.toStdString(vl->items[i]);
    }
    hal.drawTextClipped(splitX + 4, bodyY + 1, cx + cw - splitX - 6, varsLine.c_str(), Color::kGreen);
    hal.drawHLine(splitX + 1, bodyY + rowH, cx + cw - splitX - 1, 0x2945);

    const int16_t mBoxX = static_cast<int16_t>(splitX + 1);
    const int16_t mBoxY = static_cast<int16_t>(bodyY + rowH + 1);
    const int16_t mBoxW = static_cast<int16_t>(cw - splitW - 1);
    const int16_t mBoxH = static_cast<int16_t>(bodyH - rowH - 1);
    const int mVis      = mBoxH / rowH;
    if (mVis < 1) return;

    std::vector<std::string> meths;
    collectMethods(vm, c, meths);
    const int totalMeth = static_cast<int>(meths.size());
    methScroll = ScrollBar::clampScroll(methScroll, totalMeth, mVis);
    const bool mHasSb   = ScrollBar::isNeeded(totalMeth, mVis);
    const int16_t mSbW  = mHasSb ? ScrollBar::width(hal) : 0;
    const int16_t mTxtW = static_cast<int16_t>(mBoxW - 5 - (mHasSb ? (mSbW + 1) : 0));

    for (int i = 0; i < mVis && (methScroll + i) < totalMeth; i++) {
      const std::string& ms = meths[static_cast<size_t>(methScroll + i)];
      bool mod = isMethModified(vm, c, ms);
      std::string disp = mod ? ("*" + ms) : ms;
      int16_t ry = static_cast<int16_t>(mBoxY + 1 + i * rowH);
      hal.drawTextClipped(mBoxX + 3, ry, mTxtW, disp.c_str(),
                          mod ? Color::kAccent : Color::kWhite);
    }
    if (mHasSb) {
      int16_t mSbH = std::max<int16_t>(14, static_cast<int16_t>(mBoxH - 8));
      ScrollBar::draw(hal, mBoxX, mBoxY, mBoxW, mSbH, totalMeth, mVis, methScroll);
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    const DisplayHal& hal = wm.hal;
    const int16_t fw      = hal.fontW();
    const int16_t fh      = hal.fontH();
    const int16_t hdrH    = static_cast<int16_t>(fh + 5);
    const int16_t rowH    = static_cast<int16_t>(fh + 2);
    const int16_t splitL  = static_cast<int16_t>((cw * 2) / 5);
    const int16_t mthBtnW = static_cast<int16_t>(4 * fw + 6);

    const int16_t bodyL = static_cast<int16_t>(hdrH + 1);
    const int16_t bodyH = static_cast<int16_t>(ch - hdrH - 1);
    const int visRows   = std::max(1, static_cast<int>(bodyH / rowH));

    // 1. Check Left Class ScrollBar (handles down/move/up)
    std::vector<std::string> classes;
    collectClasses(vm, classes);
    if (!methThumbDrag &&
        ScrollBar::handleTouch(hal, kind, lx, ly,
                               0, bodyL, splitL, bodyH,
                               static_cast<int>(classes.size()), visRows,
                               clsScroll, clsThumbDrag)) {
      wm.invalidateWindow(win);
      return;
    }

    // 2. Check Right Method ScrollBar (handles down/move/up)
    ObjCls* c = currentObjCls(vm);
    if (c) {
      std::vector<std::string> meths;
      collectMethods(vm, c, meths);
      const int16_t mBoxY = static_cast<int16_t>(bodyL + rowH + 1);
      const int16_t mBoxW = static_cast<int16_t>(cw - splitL - 1);
      const int16_t mBoxH = static_cast<int16_t>(bodyH - rowH - 1);
      const int16_t mSbH  = std::max<int16_t>(14, static_cast<int16_t>(mBoxH - 8));
      const int mVis      = std::max(1, static_cast<int>(mBoxH / rowH));
      if (!clsThumbDrag &&
          ScrollBar::handleTouch(hal, kind, lx, ly,
                                 splitL + 1, mBoxY, mBoxW, mSbH,
                                 static_cast<int>(meths.size()), mVis,
                                 methScroll, methThumbDrag)) {
        wm.invalidateWindow(win);
        return;
      }
    }

    if (std::strcmp(kind, "down") != 0) return;

    if (ly <= hdrH) {
      if (lx < splitL) {
        showAllClasses = !showAllClasses;
        clsScroll = 0;
        wm.invalidateWindow(win);
      } else if (lx >= cw - mthBtnW - 4 && c != nullptr) {
        wm.openTextPrompt("New Method Selector (e.g. draw:)", "", false,
                          [this, &vm, &wm, &win, c](const std::string& sel) {
                            if (!sel.empty()) editMethod(vm, wm, win, c, sel);
                          });
      }
      return;
    }

    if (lx < splitL) {
      int row = (ly - bodyL - 1) / rowH;
      int idx = clsScroll + row;
      if (row >= 0 && row < visRows && idx >= 0 && idx < static_cast<int>(classes.size())) {
        selectedCls = classes[static_cast<size_t>(idx)];
        methScroll  = 0;
        wm.invalidateWindow(win);
      }
    } else if (c != nullptr) {
      if (ly < bodyL + rowH + 1) {
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
                            win.dirty = true;
                            wm.invalidateAll();
                          });
      } else {
        std::vector<std::string> meths;
        collectMethods(vm, c, meths);
        const int16_t mBoxY = static_cast<int16_t>(bodyL + rowH + 1);
        const int16_t mBoxH = static_cast<int16_t>(bodyH - rowH - 1);
        const int mVis      = std::max(1, static_cast<int>(mBoxH / rowH));
        int mRow = (ly - mBoxY - 1) / rowH;
        int idx  = methScroll + mRow;
        if (mRow >= 0 && mRow < mVis && idx >= 0 && idx < static_cast<int>(meths.size())) {
          editMethod(vm, wm, win, c, meths[static_cast<size_t>(idx)]);
        }
      }
    }
  }

  void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                       std::vector<MenuItem>& out) override {
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
                          win.dirty = true;
                          wm.invalidateAll();
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
  std::string              currentDir = "/sd/docs";
  std::string              previewName;
  std::string              previewBody;
  std::vector<std::string> previewLines;
  int                      fileScroll       = 0;
  int                      previewScroll    = 0;
  bool                     fileThumbDrag    = false;
  bool                     previewThumbDrag = false;
  int16_t                  openBtnY         = -1;
  int16_t                  openBtnW         = 0;

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

  void rebuildPreviewLines(int maxCols) {
    previewLines.clear();
    if (maxCols < 8) maxCols = 8;
    size_t p = 0;
    while (p <= previewBody.size()) {
      size_t nl  = previewBody.find('\n', p);
      size_t len = (nl == std::string::npos) ? (previewBody.size() - p) : (nl - p);
      if (len > 0 && previewBody[p + len - 1] == '\r') len--;
      if (len == 0) {
        previewLines.emplace_back("");
      } else {
        size_t off = 0;
        while (off < len) {
          size_t chunk = std::min<size_t>(static_cast<size_t>(maxCols), len - off);
          previewLines.emplace_back(previewBody.substr(p + off, chunk));
          off += chunk;
        }
      }
      if (nl == std::string::npos) break;
      p = nl + 1;
    }
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

  struct TabSpan {
    const char* label;
    const char* dir;
    int16_t     lx;
    int16_t     w;
  };

  static void computeTabs(const DisplayHal& hal, TabSpan out[4]) {
    static const char* kLabels[4] = {"Docs", "/sd", "Worlds", "Flash"};
    static const char* kDirs[4]   = {"/sd/docs", "/sd", "/sd/worlds", "/flash"};
    const int16_t fw = hal.fontW();
    int16_t x = 2;
    for (int i = 0; i < 4; i++) {
      int16_t w = static_cast<int16_t>(std::strlen(kLabels[i]) * fw + 7);
      out[i] = {kLabels[i], kDirs[i], x, w};
      x = static_cast<int16_t>(x + w + 2);
    }
  }

  void onPaint(VM& vm, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);
    openBtnY = -1;
    openBtnW = 0;

    const int16_t fw   = hal.fontW();
    const int16_t fh   = hal.fontH();
    const int16_t tabH = static_cast<int16_t>(fh + 6);
    const int16_t rowH = static_cast<int16_t>(fh + 2);

    // Tab strip: [Docs] [/sd] [Worlds] [Flash] (+Save)
    hal.fillRect(cx, cy, cw, tabH, Color::kBarBg);
    hal.drawHLine(cx, cy + tabH, cw, Color::kBorder);
    TabSpan tabs[4];
    computeTabs(hal, tabs);
    for (int i = 0; i < 4; i++) {
      bool act = (currentDir == tabs[i].dir);
      hal.fillRoundRect(cx + tabs[i].lx, cy + 1, tabs[i].w, tabH - 2, 2,
                        act ? Color::kMenuSel : Color::kKeyBg);
      hal.drawText(cx + tabs[i].lx + 3, cy + 3, tabs[i].label,
                   act ? Color::kAccent : Color::kWhite);
    }
    if (currentDir == "/sd/worlds") {
      int16_t saveW = static_cast<int16_t>(5 * fw + 6);
      hal.fillRoundRect(cx + cw - saveW - 2, cy + 1, saveW, tabH - 2, 2, 0x03E0);
      hal.drawText(cx + cw - saveW + 1, cy + 3, "+Save", Color::kWhite);
    }

    std::vector<std::pair<std::string, uint32_t>> files;
    vm.listVfsFiles(currentDir.c_str(), files);
    std::sort(files.begin(), files.end());
    const int totalFiles = static_cast<int>(files.size());

    const int16_t listY = static_cast<int16_t>(cy + tabH + 1);
    const int16_t availH = static_cast<int16_t>(ch - tabH - 1);
    if (availH < rowH) return;

    int16_t listH = previewName.empty()
                  ? availH
                  : static_cast<int16_t>(std::max<int16_t>(rowH * 3 + 2, (availH * 2) / 5));
    if (listH > availH) listH = availH;
    const int visFiles = std::max(1, static_cast<int>(listH / rowH));
    fileScroll = ScrollBar::clampScroll(fileScroll, totalFiles, visFiles);
    const bool fHasSb  = ScrollBar::isNeeded(totalFiles, visFiles);
    const int16_t fSbW = fHasSb ? ScrollBar::width(hal) : 0;
    const int16_t fTxtW = static_cast<int16_t>(cw - 6 - (fHasSb ? (fSbW + 2) : 0));

    if (files.empty()) {
      hal.drawTextClipped(cx + 6, listY + 2, cw - 12, "(no files in directory)", 0x8410);
    }
    for (int i = 0; i < visFiles && (fileScroll + i) < totalFiles; i++) {
      const auto& entry = files[static_cast<size_t>(fileScroll + i)];
      char rowBuf[80];
      std::snprintf(rowBuf, sizeof(rowBuf), "%-18s %6u B",
                    entry.first.c_str(), static_cast<unsigned>(entry.second));
      int16_t ry = static_cast<int16_t>(listY + 1 + i * rowH);
      bool sel   = (entry.first == previewName);
      if (sel) hal.fillRect(cx + 1, ry - 1, cw - 2 - fSbW, rowH, Color::kMenuSel);
      hal.drawTextClipped(cx + 4, ry, fTxtW, rowBuf, sel ? Color::kAccent : Color::kWhite);
    }
    if (fHasSb) {
      int16_t fSbH = previewName.empty() ? std::max<int16_t>(14, static_cast<int16_t>(listH - 8)) : listH;
      ScrollBar::draw(hal, cx, listY, cw, fSbH, totalFiles, visFiles, fileScroll);
    }

    // Bottom Preview Header + Scrollable Text Box
    const int16_t prevHdrY = static_cast<int16_t>(listY + listH);
    if (!previewName.empty() && prevHdrY + rowH + 6 < cy + ch) {
      hal.drawHLine(cx, prevHdrY, cw, Color::kBorder);
      const int16_t hdrBarH = static_cast<int16_t>(fh + 5);
      hal.fillRect(cx, prevHdrY + 1, cw, hdrBarH, Color::kBarBg);

      if (isDocFile(previewName)) {
        openBtnW = static_cast<int16_t>(8 * fw + 6);
        openBtnY = static_cast<int16_t>((prevHdrY + 1) - cy);
        hal.fillRoundRect(cx + cw - openBtnW - 2, prevHdrY + 2, openBtnW, hdrBarH - 2, 2, 0x03E0);
        hal.drawText(cx + cw - openBtnW + 1, prevHdrY + 3, "Open Doc", Color::kWhite);
      } else if (isStateFile(currentDir, previewName)) {
        openBtnW = static_cast<int16_t>(10 * fw + 6);
        openBtnY = static_cast<int16_t>((prevHdrY + 1) - cy);
        hal.fillRoundRect(cx + cw - openBtnW - 2, prevHdrY + 2, openBtnW, hdrBarH - 2, 2, 0x03E0);
        hal.drawText(cx + cw - openBtnW + 1, prevHdrY + 3, "Load World", Color::kWhite);
      }
      hal.drawTextClipped(cx + 4, prevHdrY + 3, cw - openBtnW - 10, previewName.c_str(), Color::kCyan);

      const int16_t pBoxY = static_cast<int16_t>(prevHdrY + hdrBarH + 2);
      const int16_t pBoxH = static_cast<int16_t>((cy + ch) - pBoxY);
      const int visPrev   = pBoxH / (fh + 1);
      if (visPrev >= 1) {
        int wrapCols = std::max(8, static_cast<int>((cw - ScrollBar::width(hal) - 8) / fw));
        rebuildPreviewLines(wrapCols);
        const int totalPrev = static_cast<int>(previewLines.size());
        previewScroll = ScrollBar::clampScroll(previewScroll, totalPrev, visPrev);
        const bool pHasSb  = ScrollBar::isNeeded(totalPrev, visPrev);
        const int16_t pSbW = pHasSb ? ScrollBar::width(hal) : 0;
        const int16_t pTxtW = static_cast<int16_t>(cw - 6 - (pHasSb ? (pSbW + 2) : 0));

        for (int i = 0; i < visPrev && (previewScroll + i) < totalPrev; i++) {
          int16_t ry = static_cast<int16_t>(pBoxY + i * (fh + 1));
          hal.drawTextClipped(cx + 4, ry, pTxtW,
                              previewLines[static_cast<size_t>(previewScroll + i)].c_str(),
                              Color::kGreen);
        }
        if (pHasSb) {
          int16_t pSbH = std::max<int16_t>(14, static_cast<int16_t>(pBoxH - 8));
          ScrollBar::draw(hal, cx, pBoxY, cw, pSbH, totalPrev, visPrev, previewScroll);
        }
      }
    }
  }

  void onTouch(VM& vm, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    const DisplayHal& hal = wm.hal;
    const int16_t fw      = hal.fontW();
    const int16_t fh      = hal.fontH();
    const int16_t tabH    = static_cast<int16_t>(fh + 6);
    const int16_t rowH    = static_cast<int16_t>(fh + 2);
    const int16_t availH  = static_cast<int16_t>(ch - tabH - 1);
    if (availH < rowH) return;

    std::vector<std::pair<std::string, uint32_t>> files;
    vm.listVfsFiles(currentDir.c_str(), files);
    std::sort(files.begin(), files.end());
    const int totalFiles = static_cast<int>(files.size());

    int16_t listH = previewName.empty()
                  ? availH
                  : static_cast<int16_t>(std::max<int16_t>(rowH * 3 + 2, (availH * 2) / 5));
    if (listH > availH) listH = availH;
    const int visFiles = std::max(1, static_cast<int>(listH / rowH));

    // 1. Check Top File List ScrollBar (down/move/up)
    int16_t fSbH = previewName.empty() ? std::max<int16_t>(14, static_cast<int16_t>(listH - 8)) : listH;
    if (!previewThumbDrag &&
        ScrollBar::handleTouch(hal, kind, lx, ly,
                               0, tabH + 1, cw, fSbH,
                               totalFiles, visFiles, fileScroll, fileThumbDrag)) {
      wm.invalidateWindow(win);
      return;
    }

    // 2. Check Bottom Preview Text ScrollBar (down/move/up)
    const int16_t hdrBarH  = static_cast<int16_t>(fh + 5);
    const int16_t prevHdrL = static_cast<int16_t>(tabH + 1 + listH);
    const int16_t pBoxL    = static_cast<int16_t>(prevHdrL + hdrBarH + 2);
    const int16_t pBoxH    = static_cast<int16_t>(ch - pBoxL);
    if (!previewName.empty() && pBoxH >= (fh + 1)) {
      int wrapCols = std::max(8, static_cast<int>((cw - ScrollBar::width(hal) - 8) / fw));
      rebuildPreviewLines(wrapCols);
      int visPrev = std::max(1, static_cast<int>(pBoxH / (fh + 1)));
      int16_t pSbH = std::max<int16_t>(14, static_cast<int16_t>(pBoxH - 8));
      if (!fileThumbDrag &&
          ScrollBar::handleTouch(hal, kind, lx, ly,
                                 0, pBoxL, cw, pSbH,
                                 static_cast<int>(previewLines.size()), visPrev,
                                 previewScroll, previewThumbDrag)) {
        wm.invalidateWindow(win);
        return;
      }
    }

    if (std::strcmp(kind, "down") != 0) return;

    // 3. Tab bar & [+Save] button
    if (ly <= tabH) {
      int16_t saveW = static_cast<int16_t>(5 * fw + 6);
      if (currentDir == "/sd/worlds" && lx >= cw - saveW - 4) {
        wm.promptSaveClassState();
        return;
      }
      TabSpan tabs[4];
      computeTabs(hal, tabs);
      for (int i = 0; i < 4; i++) {
        if (lx >= tabs[i].lx && lx < tabs[i].lx + tabs[i].w + 2) {
          currentDir = tabs[i].dir;
          fileScroll = 0;
          previewScroll = 0;
          previewName.clear();
          previewBody.clear();
          previewLines.clear();
          wm.invalidateWindow(win);
          return;
        }
      }
      return;
    }

    // 4. [Open Doc] or [Load World] button in preview header
    if (openBtnY > 0 && ly >= openBtnY && ly <= openBtnY + hdrBarH && lx >= cw - openBtnW - 4) {
      if (isDocFile(previewName)) {
        openSelectedDoc(vm, wm);
        return;
      }
      if (isStateFile(currentDir, previewName)) {
        wm.requestLoadClassState(currentDir + "/" + previewName);
        return;
      }
    }

    // 5. File row selection in top list box
    if (ly > tabH && ly < tabH + 1 + listH) {
      int row = (ly - (tabH + 2)) / rowH;
      int idx = fileScroll + row;
      if (row >= 0 && row < visFiles && idx >= 0 && idx < totalFiles) {
        previewName   = files[static_cast<size_t>(idx)].first;
        previewScroll = 0;
        std::string full = currentDir + "/" + previewName;
        previewBody.clear();
        vm.readVfsFile(full.c_str(), previewBody);
        wm.invalidateWindow(win);
      }
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
          previewLines.clear();
        }
        wm.invalidateWindow(win);
      }});
    }
  }
};

// ---------------------------------------------------------------------------
// 5. Display & Font Configuration Panel (`ConfigController`)
// ---------------------------------------------------------------------------
class ConfigController : public NativeWinController {
 public:
  uint8_t draggingSlider = 0;  // 0 = none, 1 = Font Size (7..10), 2 = Brightness (16..255)

  const char* appName() const override { return "Config"; }

  struct Layout {
    int16_t btnW;
    int16_t sliderH;
    int16_t trackX0;  // Local X inside client rect
    int16_t trackW;
    int16_t fontLblY;
    int16_t fontBarY;
    int16_t brtLblY;
    int16_t brtBarY;
    int16_t specY;
  };

  static Layout computeLayout(const DisplayHal& hal, int16_t cw) {
    const int16_t fw = hal.fontW();
    const int16_t fh = hal.fontH();
    Layout L{};
    L.btnW     = static_cast<int16_t>(fw * 2 + 10);
    L.sliderH  = static_cast<int16_t>(fh + 10);
    L.trackX0  = static_cast<int16_t>(8 + L.btnW + 6);
    L.trackW   = std::max<int16_t>(40, static_cast<int16_t>(cw - 16 - 2 * (L.btnW + 6)));
    L.fontLblY = 6;
    L.fontBarY = static_cast<int16_t>(L.fontLblY + fh + 4);
    L.brtLblY  = static_cast<int16_t>(L.fontBarY + L.sliderH + 8);
    L.brtBarY  = static_cast<int16_t>(L.brtLblY + fh + 4);
    L.specY    = static_cast<int16_t>(L.brtBarY + L.sliderH + 8);
    return L;
  }

  void onPaint(VM&, DisplayHal& hal, Window&,
               int16_t cx, int16_t cy, int16_t cw, int16_t ch) override {
    hal.fillRect(cx, cy, cw, ch, Color::kWinBg);

    const int16_t fh    = hal.fontH();
    const FontSpec spec = getFontSpec(hal.activeFontSize);
    const Layout L      = computeLayout(hal, cw);

    auto drawCapBtn = [&](int16_t bx, int16_t by, char sym) {
      hal.fillRoundRect(bx, by, L.btnW, L.sliderH, 3, Color::kKeySpec);
      hal.drawRoundRect(bx, by, L.btnW, L.sliderH, 3, Color::kBorder);
      hal.drawChar(bx + (L.btnW - hal.fontW()) / 2, by + (L.sliderH - fh) / 2, sym, Color::kAccent);
    };

    // 1. Font Size Slider (7, 8, 9, 10)
    char fontHdr[64];
    std::snprintf(fontHdr, sizeof(fontHdr), "Font Size: %u (%ux%u px)",
                  static_cast<unsigned>(spec.fontSize),
                  static_cast<unsigned>(spec.glyphW),
                  static_cast<unsigned>(spec.glyphH));
    hal.drawTextClipped(cx + 8, cy + L.fontLblY, cw - 16, fontHdr, Color::kAccent);

    int16_t fy = static_cast<int16_t>(cy + L.fontBarY);
    drawCapBtn(cx + 8, fy, '-');
    drawCapBtn(cx + cw - 8 - L.btnW, fy, '+');

    int16_t tx0 = static_cast<int16_t>(cx + L.trackX0);
    hal.fillRoundRect(tx0, fy + 2, L.trackW, L.sliderH - 4, 3, Color::kBlack);
    hal.drawRoundRect(tx0, fy + 2, L.trackW, L.sliderH - 4, 3, Color::kBorder);

    const int16_t thumbW = 16;
    const int16_t usable = std::max<int16_t>(1, static_cast<int16_t>(L.trackW - thumbW));
    for (int step = 0; step < 4; step++) {
      int16_t dx = static_cast<int16_t>(tx0 + thumbW / 2 + (usable * step) / 3);
      hal.drawVLine(dx, fy + 4, L.sliderH - 8, 0x6B4D);
    }
    int fontStep = static_cast<int>(spec.fontSize) - 7;
    int16_t fThumbX = static_cast<int16_t>(tx0 + (usable * fontStep) / 3);
    hal.fillRoundRect(fThumbX, fy + 1, thumbW, L.sliderH - 2, 3, Color::kMenuSel);
    hal.drawRoundRect(fThumbX, fy + 1, thumbW, L.sliderH - 2, 3, Color::kCyan);

    // 2. Screen Brightness Slider (16..255)
    int pct = (static_cast<int>(hal.getBacklight()) * 100 + 127) / 255;
    char brtHdr[64];
    std::snprintf(brtHdr, sizeof(brtHdr), "Brightness: %d%%", pct);
    hal.drawTextClipped(cx + 8, cy + L.brtLblY, cw - 16, brtHdr, Color::kCyan);

    int16_t by = static_cast<int16_t>(cy + L.brtBarY);
    drawCapBtn(cx + 8, by, '-');
    drawCapBtn(cx + cw - 8 - L.btnW, by, '+');

    hal.fillRoundRect(tx0, by + 2, L.trackW, L.sliderH - 4, 3, Color::kBlack);
    int32_t brtNum = static_cast<int32_t>(hal.getBacklight()) - kMinBrightness;
    int32_t brtDen = 255 - kMinBrightness;
    int16_t fillW  = static_cast<int16_t>((brtNum * (L.trackW - 4)) / brtDen);
    if (fillW > 0) {
      hal.fillRect(tx0 + 2, by + 4, fillW, L.sliderH - 8, 0x0451);
    }
    hal.drawRoundRect(tx0, by + 2, L.trackW, L.sliderH - 4, 3, Color::kBorder);

    int16_t bThumbX = static_cast<int16_t>(tx0 + (usable * brtNum) / brtDen);
    hal.fillRoundRect(bThumbX, by + 1, thumbW, L.sliderH - 2, 3, Color::kMenuSel);
    hal.drawRoundRect(bThumbX, by + 1, thumbW, L.sliderH - 2, 3, Color::kAccent);

    // 3. Live Specimen Box
    int16_t sy = static_cast<int16_t>(cy + L.specY);
    int16_t sh = static_cast<int16_t>((cy + ch) - sy - 5);
    if (sh >= fh + 6) {
      hal.fillRect(cx + 8, sy, cw - 16, sh, Color::kBlack);
      hal.drawRect(cx + 8, sy, cw - 16, sh, Color::kBorder);
      hal.drawTextClipped(cx + 12, sy + 3, cw - 24,
                          "Abgjpqy 0O1lI ()[]{} FN(x: x sin)", Color::kGreen);
      if (sh >= 2 * fh + 8) {
        char gridInfo[64];
        std::snprintf(gridInfo, sizeof(gridInfo), "Cell %dx%d | ScrollBar %dpx | %dx%d ch",
                      static_cast<int>(hal.fontW()), static_cast<int>(hal.fontH()),
                      static_cast<int>(hal.fontSizePx()),
                      static_cast<int>(kScreenW / hal.fontW()),
                      static_cast<int>(kScreenH / hal.fontH()));
        hal.drawTextClipped(cx + 12, sy + 5 + fh, cw - 24, gridInfo, Color::kWhite);
      }
    }
  }

  void applyFontTrackX(WindowManager& wm, const Layout& L, int16_t lx) {
    const int16_t thumbW = 16;
    const int16_t usable = std::max<int16_t>(1, static_cast<int16_t>(L.trackW - thumbW));
    int32_t rel = static_cast<int32_t>(lx) - static_cast<int32_t>(L.trackX0) - thumbW / 2;
    if (rel < 0) rel = 0;
    if (rel > usable) rel = usable;
    uint8_t targetSz = static_cast<uint8_t>(7 + (rel * 3 + usable / 2) / usable);
    if (targetSz != wm.hal.activeFontSize) {
      wm.hal.setFontSize(targetSz);
      wm.onFontSizeChanged();
    }
  }

  void applyBrightTrackX(WindowManager& wm, Window& win, const Layout& L, int16_t lx) {
    const int16_t thumbW = 16;
    const int16_t usable = std::max<int16_t>(1, static_cast<int16_t>(L.trackW - thumbW));
    int32_t rel = static_cast<int32_t>(lx) - static_cast<int32_t>(L.trackX0) - thumbW / 2;
    if (rel < 0) rel = 0;
    if (rel > usable) rel = usable;
    uint8_t lvl = static_cast<uint8_t>(
      kMinBrightness + (rel * (255 - kMinBrightness) + usable / 2) / usable);
    if (lvl != wm.hal.getBacklight()) {
      wm.hal.setBacklight(lvl);
      wm.invalidateWindow(win);
    }
  }

  void onTouch(VM&, WindowManager& wm, Window& win,
               const char* kind, int16_t lx, int16_t ly) override {
    int16_t cx, cy, cw, ch;
    wm.getClientRect(win, wm.topWindow() == &win, cx, cy, cw, ch);
    const Layout L = computeLayout(wm.hal, cw);

    if (std::strcmp(kind, "up") == 0) {
      if (draggingSlider != 0) {
        draggingSlider = 0;
        wm.hal.saveDisplayConfig();
      }
      return;
    }

    if (std::strcmp(kind, "move") == 0) {
      if (draggingSlider == 1) applyFontTrackX(wm, L, lx);
      else if (draggingSlider == 2) applyBrightTrackX(wm, win, L, lx);
      return;
    }

    if (std::strcmp(kind, "down") != 0) return;

    // 1. Font Size row hit?
    if (ly >= L.fontBarY - 2 && ly <= L.fontBarY + L.sliderH + 2) {
      if (lx < 8 + L.btnW + 2) {
        if (wm.hal.activeFontSize > 7) {
          wm.hal.setFontSize(static_cast<uint8_t>(wm.hal.activeFontSize - 1));
          wm.hal.saveDisplayConfig();
          wm.onFontSizeChanged();
        }
      } else if (lx >= cw - 8 - L.btnW - 2) {
        if (wm.hal.activeFontSize < 10) {
          wm.hal.setFontSize(static_cast<uint8_t>(wm.hal.activeFontSize + 1));
          wm.hal.saveDisplayConfig();
          wm.onFontSizeChanged();
        }
      } else {
        draggingSlider = 1;
        applyFontTrackX(wm, L, lx);
      }
      return;
    }

    // 2. Brightness row hit?
    if (ly >= L.brtBarY - 2 && ly <= L.brtBarY + L.sliderH + 2) {
      if (lx < 8 + L.btnW + 2) {
        int nextB = static_cast<int>(wm.hal.getBacklight()) - 24;
        if (nextB < kMinBrightness) nextB = kMinBrightness;
        wm.hal.setBacklight(static_cast<uint8_t>(nextB));
        wm.hal.saveDisplayConfig();
        wm.invalidateWindow(win);
      } else if (lx >= cw - 8 - L.btnW - 2) {
        int nextB = std::min(255, static_cast<int>(wm.hal.getBacklight()) + 24);
        wm.hal.setBacklight(static_cast<uint8_t>(nextB));
        wm.hal.saveDisplayConfig();
        wm.invalidateWindow(win);
      } else {
        draggingSlider = 2;
        applyBrightTrackX(wm, win, L, lx);
      }
      return;
    }
  }

  void populateAppMenu(VM&, WindowManager& wm, Window&,
                       std::vector<MenuItem>& out) override {
    out.push_back({"Font Size 7 (5x7)", "", [&wm]() {
      wm.hal.setFontSize(7); wm.hal.saveDisplayConfig(); wm.onFontSizeChanged();
    }});
    out.push_back({"Font Size 8 (6x8)", "", [&wm]() {
      wm.hal.setFontSize(8); wm.hal.saveDisplayConfig(); wm.onFontSizeChanged();
    }});
    out.push_back({"Font Size 9 (7x9)", "", [&wm]() {
      wm.hal.setFontSize(9); wm.hal.saveDisplayConfig(); wm.onFontSizeChanged();
    }});
    out.push_back({"Font Size 10 (8x10)", "", [&wm]() {
      wm.hal.setFontSize(10); wm.hal.saveDisplayConfig(); wm.onFontSizeChanged();
    }});
    out.push_back({"Calibrate Touch", "", [&wm]() {
      wm.hal.runTouchCalibration();
      wm.invalidateAll();
    }});
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
NativeWinController* createConfigController() {
  return new ConfigController();
}

}  // namespace tt
