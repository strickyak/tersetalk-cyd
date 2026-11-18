#pragma once

#include "tt_vm.h"

namespace tt {

// Compiles a TerseTalk method (or standalone expression when methName is "__eval__")
// into a RAM-resident 32-bit word bytecode CompiledCode structure.
// Throws TerseError on syntax/compile errors.
CompiledCode* compileMethod(VM& vm, ObjCls* onCls, uint32_t methNameSym,
                            const char* src, size_t srcLen);

}  // namespace tt
