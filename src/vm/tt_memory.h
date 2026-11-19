#pragma once

#include "tt_bytecode.h"
#include "tt_value.h"

#include <cstddef>
#include <cstdint>

namespace tt {

struct VM;
struct ObjCls;
struct ObjMeth;
struct ObjFrame;
struct CompiledCode;

// ---------------------------------------------------------------------------
// Heap Object Formats
// ---------------------------------------------------------------------------
enum ObjFormat : uint8_t {
  OBJ_FREE  = 0,
  OBJ_SLOTS = 1,   // Usr / Obj instance with inline Oop slots[]
  OBJ_STR   = 2,   // Immutable UTF-8/ASCII string with inline char chars[]
  OBJ_BYTES = 3,   // Immutable byte array with inline uint8_t bytes[]
  OBJ_BUF   = 4,   // Mutable string builder (external char*)
  OBJ_LIST  = 5,   // Mutable dynamic List / Vec (external Oop*)
  OBJ_DICT  = 6,   // Mutable hash Dict (external DictEntry*)
  OBJ_CLS   = 7,   // Class / Metaclass object
  OBJ_METH  = 8,   // Method (Primitive C++ fn or compiled UsrMeth)
  OBJ_BLK   = 9,   // Lexical Block closure (FN(...) or macro arg)
  OBJ_FRAME = 10,  // Heap-captured activation Frame (for closures)
  OBJ_REX   = 11,  // Regular expression pattern
  OBJ_PEN   = 12,  // Graphics Pen / Ink bound to Window or Bmp
  OBJ_BMP   = 13,  // 8-bit indexed pixel buffer
  OBJ_WIN   = 14,  // Window handle wrapper
};

enum GcFlags : uint8_t {
  GC_MARKED = 1u << 0,
};

struct alignas(8) ObjHeader {
  uint16_t clsId;    // Index into VM::classById[]
  uint8_t  format;   // ObjFormat
  uint8_t  gcFlags;  // GcFlags
  uint32_t size;     // Slot count (OBJ_SLOTS/FRAME) or byte len (OBJ_STR/BYTES)
};

static_assert(sizeof(ObjHeader) == 8, "ObjHeader must be 8 bytes");

// Usr / Slotted Object
struct ObjSlots {
  ObjHeader hdr;
  uint32_t  oid;       // Transient numeric object ID (0 until requested)
  uint32_t  onameSym;  // Persistent symbol name (0 if none)
  Oop       slots[1];  // Variable length: hdr.size Oops
};

// Immutable String
struct ObjStr {
  ObjHeader hdr;       // hdr.size = byte length (excluding NUL)
  uint32_t  hash;
  char      chars[4];  // Variable length: hdr.size + 1 bytes
};

// Immutable Byte Array
struct ObjBytes {
  ObjHeader hdr;       // hdr.size = byte length
  uint8_t   bytes[8];  // Variable length: hdr.size bytes
};

// Mutable String Buffer
struct ObjBuf {
  ObjHeader hdr;
  char*     data;
  uint32_t  len;
  uint32_t  cap;
};

// Mutable Dynamic List (also aliased as Vec)
struct ObjList {
  ObjHeader hdr;
  Oop*      items;
  uint32_t  len;
  uint32_t  cap;
};

// Mutable Hash Dictionary
struct DictEntry {
  Oop      key;    // kOopNull = empty, kOopSuper = tombstone
  Oop      val;
  uint32_t hash;
};

struct ObjDict {
  ObjHeader  hdr;
  DictEntry* entries;
  uint32_t   len;      // Active key-value pairs
  uint32_t   used;     // Active + tombstones
  uint32_t   cap;      // Power of 2
};

// Inline Call-Site Cache Entry inside CompiledCode
struct SendSite {
  uint32_t selectorSym;
  uint16_t cachedClsId;
  uint16_t cachedGen;
  ObjMeth* cachedMeth;
  uint16_t sourceLoc;
  uint16_t pad;
};

// Block Parameter Descriptor (supports simple var or tuple destructuring!)
struct BlkParamVar {
  uint8_t  isInst;  // 0 = local slot in Frame, 1 = instVar in receiver
  uint16_t index;
};

struct BlkParam {
  uint8_t     isTuple;    // 0 = single var (vars[0]), 1 = tuple of varCount vars
  uint8_t     varCount;
  BlkParamVar vars[6];
};

// Compiled RAM Bytecode Container (for MethTop or Block)
struct CompiledCode {
  uint32_t*     code;          // Aligned RAM bytecode array
  uint32_t      codeLen;
  Oop*          consts;        // Literal Oops (Strings, Floats/big ints, sub-block Meths)
  uint16_t      constCount;
  SendSite*     sendSites;     // Per-callsite selector + monomorphic cache
  uint16_t      sendSiteCount;
  uint16_t      numArgs;
  uint16_t      numLocals;     // Total local slots in method Frame (args + locals + loop temps)
  uint16_t      onClsId;
  uint32_t      methNameSym;
  bool          capturesFrame; // True if method creates any ObjBlk or pushes Frame
  uint8_t       blkParamCount;
  BlkParam*     blkParams;     // Non-null for Block code
};

using PrimFn = Oop (*)(VM& vm, Oop rcvr, const Oop* args, uint8_t numArgs);

struct ObjMeth {
  ObjHeader     hdr;
  uint16_t      onClsId;
  bool          isPrim;
  bool          ownsSource;     // False if source points into .rodata / static prelude
  bool          trace;
  bool          isPrimordial;   // True for methods installed by C++ bootstrap or prelude.tt
  bool          isAutoAccessor; // True for getter/setter synthesized by defineVars
  uint32_t      nameSym;
  uint32_t      abbrevSym;
  PrimFn        prim;
  CompiledCode* comp;           // Lazily compiled for UsrMeth, or block code
  const char*   source;         // Source code (owned heap char* or non-owned .rodata)
  uint32_t      sourceLen;
};

static constexpr int kUsrLruSize = 16;

struct ObjCls {
  ObjHeader hdr;
  uint16_t  clsId;             // Unique ID in VM::classById[]
  uint16_t  superClsId;        // 0 if none (Ur)
  uint16_t  generation;        // Incremented when methods/vars change
  uint8_t   isUsrCls     : 1;  // True for Usr and its subclasses
  uint8_t   trace        : 1;
  uint8_t   isPrimordial : 1;  // True for classes defined by C++ bootstrap or prelude.tt
  uint8_t   varsModified : 1;  // True if defineVars was run after prelude.tt
  uint8_t   flagsPad     : 4;
  uint8_t   padByte;
  Oop       nameStr;           // Original-case Str
  uint32_t  nameSym;           // Lowercase SymId
  Oop       methDict;          // ObjDict (Str/Sym -> ObjMeth)
  uint16_t  myVarCount;
  uint16_t  allVarCount;
  uint32_t* allVarSyms;        // Array of lowercase SymIds [allVarCount]
  Oop       myVarsList;        // ObjList of Str (declared on this class)
  Oop       subsList;          // ObjList of Str (direct subclass names)
  Oop       savedInstsDict;    // ObjDict (Str name -> Str code or realized ObjSlots)
  Oop       lruCache[kUsrLruSize];
};

// Heap-allocated activation Frame (used when capturesFrame == true or Sys frame)
struct ObjFrame {
  ObjHeader     hdr;           // hdr.size = numLocals
  Oop           prevFrame;     // ObjFrame or nil
  Oop           self;          // Receiver (`me`)
  Oop           methObj;       // ObjMeth
  uint16_t      level;
  uint16_t      pad;
  Oop           locals[1];     // Variable length: hdr.size Oops
};

// Lexical Block Closure (`Blk`)
struct ObjBlk {
  ObjHeader hdr;
  Oop       blockMeth;         // ObjMeth wrapping CompiledCode for this block
  Oop       homeFrame;         // ObjFrame capturing `me` and method `locals[]`
};

// Compiled Regular Expression (`Rex`)
struct ObjRex {
  ObjHeader hdr;
  Oop       patternStr;        // ObjStr
};

// Graphics Pen / Ink
struct ObjPen {
  ObjHeader hdr;
  int16_t   vpX, vpY, vpW, vpH; // Screen viewport in 320x240
  Oop       targetBmp;          // kOopNil for screen viewport, or ObjBmp
  uint16_t  color565;
  uint8_t   palIdx;             // Active palette index when drawing to Bmp
  uint8_t   thickness;
  uint8_t   font;
  bool      penDown;
  bool      hasWorld;
  float     turtleX, turtleY;
  float     turtleDeg;          // Degrees (0 = right/+X, CCW math/turtle)
  float     wx0, wy0, wx1, wy1; // Math world coordinate bounds
};

// 8-Bit Indexed Pixel Buffer (`Bmp`)
struct ObjBmp {
  ObjHeader hdr;
  uint16_t  wid;
  uint16_t  hei;
  uint8_t*  pixels;             // wid * hei bytes (palette indices 0..255)
  uint16_t* palette;            // Optional 256-entry RGB565 palette (null = default rainbow/RGB332)
};

// Window Handle Wrapper (`Win`)
struct ObjWin {
  ObjHeader hdr;
  int16_t   winId;              // Index into WindowManager::windows[]
  int16_t   pad;
  Oop       appObj;             // Owning App instance
};

// ---------------------------------------------------------------------------
// Coalescing 8-Byte-Aligned Mark-Sweep Arena
// ---------------------------------------------------------------------------
struct HeapArena {
  uint8_t* base          = nullptr;
  uint32_t totalBytes    = 0;
  uint32_t usedBytes     = 0;
  uint32_t externalBytes = 0;
  uint32_t gcThreshold   = 0;
  uint32_t gcCount       = 0;
  uint32_t searchCursor  = 4;  // Next-fit cursor (points to 4B block header)

  bool init(uint32_t arenaBytes);
  void resetInPlace();
  void destroy();

  // Allocates an 8-byte-aligned block of at least `payloadBytes` (zero-initialized).
  void* allocRaw(uint32_t payloadBytes, bool isGcObj);
};

void freeCompiledCode(CompiledCode* cc, HeapArena& arena);

}  // namespace tt
