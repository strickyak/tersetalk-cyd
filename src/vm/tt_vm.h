#pragma once

#include "tt_memory.h"

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tt {

static constexpr uint32_t kDefaultArenaBytes = 131072u;  // 128 KB default on desktop host
static constexpr uint16_t kMaxClasses        = 192;
static constexpr uint16_t kMaxSymbols        = 1024;
static constexpr uint16_t kMaxTempRoots      = 128;
static constexpr uint32_t kValueStackSize    = 1024;
static constexpr uint16_t kMaxCallDepth      = 96;
static constexpr uint16_t kMaxTryDepth       = 24;
static constexpr size_t   kLogRingSize       = 24;

// Built-in Well-Known Class IDs (1-based; 0 = invalid)
enum BuiltinClsId : uint16_t {
  CLS_NONE = 0,
  CLS_UR,
  CLS_UR_CLS,
  CLS_OBJ,
  CLS_OBJ_CLS,
  CLS_CLS,
  CLS_CLS_CLS,
  CLS_METACLS,
  CLS_METACLS_CLS,
  CLS_SUPER,
  CLS_SUPER_CLS,
  CLS_UNDEFINED,
  CLS_UNDEFINED_CLS,
  CLS_NUM,
  CLS_NUM_CLS,
  CLS_INT,
  CLS_INT_CLS,
  CLS_FLOAT,
  CLS_FLOAT_CLS,
  CLS_CHAR,
  CLS_CHAR_CLS,
  CLS_STR,
  CLS_STR_CLS,
  CLS_BUF,
  CLS_BUF_CLS,
  CLS_BYTES,
  CLS_BYTES_CLS,
  CLS_REX,
  CLS_REX_CLS,
  CLS_LIST,
  CLS_LIST_CLS,
  CLS_DICT,
  CLS_DICT_CLS,
  CLS_BLK,
  CLS_BLK_CLS,
  CLS_METH,
  CLS_METH_CLS,
  CLS_PRIM_METH,
  CLS_PRIM_METH_CLS,
  CLS_USR_METH,
  CLS_USR_METH_CLS,
  CLS_FRAME,
  CLS_FRAME_CLS,
  CLS_USR,
  CLS_USR_CLS,
  CLS_TMP,
  CLS_TMP_CLS,
  CLS_ORPHAN,
  CLS_ORPHAN_CLS,
  CLS_SYS,
  CLS_SYS_CLS,
  CLS_FILE,
  CLS_FILE_CLS,
  CLS_JSON,
  CLS_JSON_CLS,
  CLS_HEX,
  CLS_HEX_CLS,
  CLS_PEN,
  CLS_PEN_CLS,
  CLS_BMP,
  CLS_BMP_CLS,
  CLS_WIN,
  CLS_WIN_CLS,
  CLS_FIRST_DYNAMIC
};

struct CallFrame {
  ObjMeth*       meth;
  CompiledCode*  comp;
  const uint32_t* pc;
  Oop            self;
  Oop            heapFrame;   // ObjFrame if comp->capturesFrame, else kOopNil
  uint32_t       stackBase;   // Index into vm.stack[] where locals (if uncaptured) or args start
  uint32_t       exprBase;    // Index into vm.stack[] where expression temporaries start
};

struct TryHandler {
  uint16_t       callDepth;
  uint32_t       stackTop;
  const uint32_t* catchPc;
};

// Host callbacks for VFS, UI roots, logging, and preemptive interruption
struct HostCallbacks {
  void* user = nullptr;
  void (*onSay)(void* user, const char* msg) = nullptr;
  bool (*pollInterrupt)(void* user) = nullptr;  // Return true to interrupt running script
  void (*markUiRoots)(void* user, VM& vm) = nullptr;

  // VFS hooks (optional; falls back to POSIX stdio on host)
  bool (*vfsRead)(void* user, const char* path, std::string& outContent) = nullptr;
  bool (*vfsWrite)(void* user, const char* path, const char* data, size_t len, bool append) = nullptr;
  bool (*vfsDelete)(void* user, const char* path) = nullptr;
  bool (*vfsList)(void* user, const char* dir, std::vector<std::pair<std::string, uint32_t>>& outFiles) = nullptr;
  uint32_t (*millisFn)(void* user) = nullptr;
};

// C++ exception used internally to unwind VM stack on `err` / `toss` / `TooManyTicks`
struct TerseError {
  std::string message;
  bool        isInterrupt = false;
};

struct TempRootGuard {
  VM&      vm;
  uint16_t saved;
  explicit TempRootGuard(VM& v);
  TempRootGuard(VM& v, Oop o);
  void push(Oop o);
  ~TempRootGuard();
};

struct VM {
  HeapArena     heap;
  HostCallbacks host;

  // Interned lowercase symbols (1-based; 0 = none)
  char*         symChars[kMaxSymbols];
  uint16_t      symLens[kMaxSymbols];
  uint32_t      symHashes[kMaxSymbols];
  uint16_t      symCount = 0;

  // Class table (clsId -> Oop of ObjCls)
  Oop           classById[kMaxClasses];
  uint16_t      classCount = 0;

  // Global dictionary (lowercase SymId -> Oop of ObjCls)
  Oop           globalsDict = kOopNil;

  // Execution stacks
  Oop           stack[kValueStackSize];
  uint32_t      sp = 0;
  CallFrame     frames[kMaxCallDepth];
  uint16_t      frameCount = 0;

  // Exception / tick state
  int32_t       ticksRemaining = 2000000;
  uint32_t      pollCountdown  = 1024;
  bool          interrupted    = false;

  // GC temporary C++ roots
  Oop           tempRoots[kMaxTempRoots];
  uint16_t      tempRootCount = 0;
  bool          gcInhibit     = false;

  // World persistence state
  std::string   worldName     = "main";
  std::string   worldPath     = "/sd/worlds/w_main.tt";
  bool          loadingWorld  = false;
  uint32_t      nextOid       = 101;

  // Cached selector SymIds for fast bytecode & primitive dispatch
  uint32_t      symAdd = 0, symSub = 0, symMul = 0, symDiv = 0, symMod = 0;
  uint32_t      symBitAnd = 0, symBitOr = 0, symBitXor = 0;
  uint32_t      symEq = 0, symNe = 0, symLt = 0, symLe = 0, symGt = 0, symGe = 0;
  uint32_t      symValue = 0, symValue1 = 0, symValue2 = 0;
  uint32_t      symJam = 0, symStr = 0, symEquals = 0;

  // Ring buffer for `Sys said`
  std::string   logRing[kLogRingSize];
  size_t        logWriteIdx = 0;
  size_t        logCount    = 0;

  // Lifecycle
  bool init(uint32_t arenaBytes = kDefaultArenaBytes, bool loadBuiltinPrelude = true);
  void destroy();

  // Symbols
  uint32_t internSym(const char* s, size_t len);
  uint32_t internSym(const char* s);
  const char* symCStr(uint32_t symId) const;

  // Allocation & GC
  ObjHeader* allocObj(uint16_t clsId, ObjFormat fmt, uint32_t hdrSizeField, size_t totalStructBytes);
  void*      allocExternal(size_t bytes);
  void*      reallocExternal(void* ptr, size_t oldBytes, size_t newBytes);
  void       freeExternal(void* ptr, size_t bytes);
  void       runGC();
  void       markOop(Oop v);

  // Constructors for built-in heap types
  Oop newStr(const char* s, size_t len);
  Oop newStr(const char* s);
  Oop newBytes(const uint8_t* b, size_t len);
  Oop newBuf(const char* initStr = nullptr, size_t len = 0);
  Oop newList(uint32_t initialCap = 4);
  Oop newListFrom(const Oop* elems, uint32_t count);
  Oop newDict(uint32_t initialCap = 8);
  Oop newUsr(uint16_t clsId);
  Oop makeFloatChecked(float f);

  // List & Dict operations
  void listAppend(Oop listOop, Oop val);
  Oop  dictGet(Oop dictOop, Oop key) const;
  void dictPut(Oop dictOop, Oop key, Oop val);
  Oop  dictDel(Oop dictOop, Oop key);
  void dictSortedKeys(Oop dictOop, std::vector<Oop>& outKeys) const;

  // Type & Class Helpers
  uint16_t oopClsId(Oop v) const;
  ObjCls*  oopCls(Oop v) const;
  ObjCls*  clsById(uint16_t id) const;
  bool     isa(Oop v, uint16_t targetClsId) const;
  bool     truth(Oop v) const;
  uint32_t oopHash(Oop v) const;
  bool     oopEquals(Oop a, Oop b) const;
  int      oopCompare(Oop a, Oop b) const;
  std::string toStdString(Oop v);
  std::string toReprString(Oop v);

  // Classes & Methods
  Oop      defineClass(const char* name, uint16_t superClsId, bool isUsr, uint16_t fixedClsId = 0, uint16_t fixedMetaId = 0);
  Oop      ensureSavedInstsDict(ObjCls* cls);
  void     defineVars(ObjCls* cls, const char* spaceSepVars, bool autoAccessors);
  ObjMeth* addPrim(uint16_t clsId, const char* name, const char* abbrev, PrimFn fn);
  ObjMeth* addUsrMeth(ObjCls* cls, const char* name, const char* abbrev,
                      const char* src, size_t srcLen, bool ownsSrc, bool compileNow);
  ObjMeth* lookupMeth(uint16_t startClsId, uint32_t selectorSym) const;

  // Execution & Evaluation
  Oop invokeMeth(ObjMeth* meth, Oop rcvr, const Oop* args, uint8_t numArgs);
  Oop send(Oop rcvr, uint32_t selectorSym, const Oop* args, uint8_t numArgs);
  Oop evalBlk(Oop blkOop, const Oop* args, uint8_t numArgs);
  void blkStoreKV(Oop blkOop, Oop k, Oop v);
  Oop evalCode(Oop rcvr, const char* code, const Oop* args = nullptr, uint8_t numArgs = 0);

  // Logging & Errors
  void say(const char* fmt, ...);
  [[noreturn]] void toss(const char* fmt, ...);

  // World File Persistence
  void appendWorldEntry(const char* headerLine, const char* bodyText);
  bool readVfsFile(const char* path, std::string& out) const;
  bool writeVfsFile(const char* path, const char* data, size_t len, bool append);
  bool deleteVfsFile(const char* path);
  bool listVfsFiles(const char* dir, std::vector<std::pair<std::string, uint32_t>>& out) const;
  uint32_t nowMillis() const;
};

// Installed by tt_prims.cpp
void installCorePrimitives(VM& vm);

}  // namespace tt
