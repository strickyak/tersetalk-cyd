#pragma once

#include "hal/display_hal.h"
#include "tt_wm.h"
#include "vm/tt_vm.h"

#include <string>

namespace tt {

class SerialRepl {
 public:
  VM&            vm;
  DisplayHal&    hal;
  WindowManager& wm;

  bool        puttingFile = false;
  std::string putPath;
  std::string putBuffer;

  SerialRepl(VM& v, DisplayHal& h, WindowManager& w);

  void printBanner();
  void poll();
  void processLine(const std::string& line);
};

}  // namespace tt
