#pragma once

#include "tt_vm.h"

#include <cstddef>
#include <string>

namespace tt {

extern const char   kEmbeddedPrelude[];
extern const size_t kEmbeddedPreludeLen;

// Loads a TerseTalk world/prelude buffer (`cls`, `vars`, `meth`, `inst`, `equals`, `stop`,
// and timestamped `( <ts> ... ) <ts>` blocks).
// If `borrowStaticSource` is true, `meth` source slices point directly into `buf` without copying.
bool loadWorldBuffer(VM& vm, const char* worldTag, const char* buf, size_t len,
                     bool borrowStaticSource, std::string* outErr = nullptr);

// Loads a world/prelude file via VFS (`vm.readVfsFile`).
bool loadWorldFile(VM& vm, const char* worldTag, const char* vfsPath,
                   std::string* outErr = nullptr);

}  // namespace tt
