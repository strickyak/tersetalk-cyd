#pragma once

#include "hal/display_hal.h"
#include "vm/tt_vm.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tt {

class WindowManager;
struct Window;

struct MenuItem {
  std::string label;
  std::string selector;               // TerseTalk selector to send to `currentApp` (if non-empty)
  std::function<void()> nativeAction; // Or C++ callback (for Global Menu / Native IDE windows)
};

struct ModalButton {
  std::string label;
  uint16_t    bgColor = 0x2104;
  std::function<void()> action;
};

// Optional C++ controller for built-in IDE / Scratchpad windows
class NativeWinController {
 public:
  virtual ~NativeWinController() = default;
  virtual const char* appName() const = 0;
  virtual void onPaint(VM& vm, DisplayHal& hal, Window& win,
                       int16_t cx, int16_t cy, int16_t cw, int16_t ch) = 0;
  virtual void onTouch(VM& vm, WindowManager& wm, Window& win,
                       const char* kind, int16_t lx, int16_t ly) = 0;
  virtual void populateAppMenu(VM& vm, WindowManager& wm, Window& win,
                               std::vector<MenuItem>& outItems) {}
  virtual void markGcRoots(VM& vm) { (void)vm; }
};

struct Window {
  int16_t     id             = -1;
  bool        alive          = false;
  int16_t     x              = 12;
  int16_t     y              = 24;
  int16_t     w              = 220;
  int16_t     h              = 170;
  std::string title;
  Oop         appObj         = kOopNil;  // TerseTalk `App` instance (if TerseTalk app)
  Oop         winObj         = kOopNil;  // `ObjWin` handle
  NativeWinController* native = nullptr; // Owned C++ controller (if built-in tool)
  uint32_t    animIntervalMs = 0;        // >0 for periodic `onTick` animation
  uint32_t    lastStepMs     = 0;
  bool        closeArmed     = false;
  bool        dirty          = true;
};

static constexpr int kMaxWindows      = 12;
static constexpr int16_t kTopBarH     = 16;
static constexpr int16_t kWinTitleH   = 12;
static constexpr int16_t kWedgeBoxOverlap = 16;
static constexpr int16_t kWedgeBoxMax     = 14;

enum class DragMode : uint8_t {
  NONE = 0,
  MOVE_WIN,
  RESIZE_WIN,
  CLIENT_TOUCH,
  DROPDOWN,
  KEYBOARD,
};

class VirtualKeyboard;

class WindowManager {
 public:
  VM&         vm;
  DisplayHal& hal;

  Window      windows[kMaxWindows];
  int         zOrder[kMaxWindows];   // Indices into `windows[]`, bottom (0) to top (zCount-1)
  int         zCount        = 0;
  bool        maximized     = false;
  bool        needsRedraw   = true;   // Full desktop + all windows repaint
  bool        dropdownDirty = false;  // Fast overlay-only dropdown repaint
  bool        keyboardDirty = false;  // Fast keyboard-only repaint

  // Active dropdown menu state (0 = none, 1 = NW Global Menu, 2 = NE App Menu)
  uint8_t               activeMenu = 0;
  std::vector<MenuItem> menuItems;
  int                   menuHoverIdx = -1;
  int16_t               menuX = 0, menuY = 0, menuW = 0, menuH = 0;

  // Touch gesture tracking
  bool        prevTouchDown = false;
  int16_t     lastTouchX    = 0;
  int16_t     lastTouchY    = 0;
  int16_t     dragStartX    = 0;
  int16_t     dragStartY    = 0;
  int16_t     dragOrigWinX  = 0;
  int16_t     dragOrigWinY  = 0;
  int16_t     dragOrigWinW  = 0;
  int16_t     dragOrigWinH  = 0;
  int         dragWinSlot   = -1;
  DragMode    dragMode      = DragMode::NONE;

  // Toast / Error banner message (shown briefly at bottom if TerseError occurs)
  std::string statusBanner;
  uint32_t    bannerExpireMs = 0;

  // Modal Choice Overlay (e.g. asking whether to rename auto-log before loading Class State)
  bool                     modalActive = false;
  std::string              modalTitle;
  std::string              modalSubtitle;
  std::vector<ModalButton> modalButtons;

  VirtualKeyboard* keyboard = nullptr;

  WindowManager(VM& v, DisplayHal& h);
  ~WindowManager();

  void initDesktop();

  // Window creation & lifecycle
  Window* createTerseWindow(Oop appObj, const char* title,
                            int16_t x, int16_t y, int16_t w, int16_t h);
  Window* createNativeWindow(NativeWinController* ctrl, const char* title,
                             int16_t x, int16_t y, int16_t w, int16_t h);
  Window* findById(int16_t winId);
  Window* topWindow();
  Oop     currentApp();

  void focusWindowSlot(int slot);
  void closeWindowSlot(int slot);
  void closeAllWindows(bool runOnClose = false);
  void setMaximized(bool maxMode);
  void toggleMaximized() { setMaximized(!maximized); }
  void invalidateAll() { needsRedraw = true; }
  void invalidateWindow(Window& win);

  // Launchers & Class-State Savepoints
  Window* launchTerseAppByClassName(const char* clsName);
  void    openEditAppCodeBrowser();
  void    showBanner(const std::string& msg, uint32_t durationMs = 3500);
  void    openModalChoice(const std::string& title, const std::string& subtitle,
                          std::vector<ModalButton> buttons);
  std::string nextAutoLogBackupName() const;
  std::string nextSavepointName() const;
  void    promptSaveClassState();
  void    requestLoadClassState(const std::string& vfsPath);
  bool    performLoadClassState(const std::string& vfsPath, const std::string& renameAutoLogTo,
                                std::string* outStatus = nullptr);

  // Shared Stylus Keyboard / Graffiti Text Prompt Overlay
  void openTextPrompt(const char* promptTitle, const std::string& initialText,
                      bool multiline, std::function<void(const std::string&)> onAccept);

  // Main Frame Tick (polls touch, dispatches animations, renders dirty regions)
  void tick();
  void render();

  // Client content rectangle for a window in current desktop mode
  void getClientRect(const Window& win, bool isTop,
                     int16_t& cx, int16_t& cy, int16_t& cw, int16_t& ch) const;

  // GC Root Marking
  void markRoots(VM& v);

 private:
  int  allocWindowSlot();
  void openGlobalMenu();
  void openAppMenu();
  void buildGlobalMenu(std::vector<MenuItem>& out);
  void buildAppMenu(std::vector<MenuItem>& out);
  void layoutDropdown(bool rightAlign);
  void renderWindow(Window& win, bool isTop);
  void renderWindowClient(Window& win, bool isTop);
  void renderTopBar();
  void renderMaximizedWedges();
  void renderDropdown();
  void renderModal();
  void handleTouchDown(int16_t tx, int16_t ty);
  void handleTouchMove(int16_t tx, int16_t ty);
  void handleTouchUp(int16_t tx, int16_t ty);
  void sendAppTouch(Window& win, const char* kind, int16_t lx, int16_t ly);
};

}  // namespace tt
