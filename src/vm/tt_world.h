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

// Normalizes a bare savepoint name (e.g. `save1`) to `/sd/worlds/save1.tt`.
std::string normalizeStatePath(const std::string& nameOrPath);

// Serializes all non-primordial classes, modified vars, and modified/added methods as `.tt` source.
std::string serializeClassState(VM& vm);

// Saves the current non-primordial Class State delta to `/sd/worlds/<name>.tt` (independent of `w_main.tt`).
bool saveClassState(VM& vm, const char* nameOrPath,
                    std::string* outResolvedPath = nullptr, std::string* outErr = nullptr);

}  // namespace tt
