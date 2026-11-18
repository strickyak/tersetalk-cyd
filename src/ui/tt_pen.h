#pragma once

#include "hal/display_hal.h"
#include "vm/tt_vm.h"

namespace tt {

class WindowManager;

// Allocates a screen-targeted ObjPen clipped to viewport (vpX, vpY, vpW, vpH)
Oop createScreenPen(VM& vm, int16_t vpX, int16_t vpY, int16_t vpW, int16_t vpH);

// Registers Pen (Ink), Bmp, and Win primitives onto `vm`
void installGraphicsPrimitives(VM& vm, DisplayHal* hal, WindowManager* wm);

}  // namespace tt
