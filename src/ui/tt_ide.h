#pragma once

#include "tt_wm.h"

#include <string>

namespace tt {

NativeWinController* createGraphicsPadController(const char* initialCode = nullptr);
NativeWinController* createWorkspaceController(const char* initialCode = nullptr);
NativeWinController* createClassBrowserController(const char* focusClassName = nullptr);
NativeWinController* createFilesController();

}  // namespace tt
