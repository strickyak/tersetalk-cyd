#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace tt {

struct ObjHeader;

// Base of the VM heap arena (used on 64-bit hosts so 32-bit Oop offsets resolve in 1 add).
extern uint8_t* gArenaBase;

// ---------------------------------------------------------------------------
// 32-Bit 2-Bit-Tagged Object Pointer (Oop)
//   bits[1:0] == 00 : 4/8-byte-aligned ObjHeader* (0 == kOopNull internal sentinel)
//   bits[1:0] == 01 : 30-bit signed Int   [-536,870,912 .. +536,870,911]
//   bits[1:0] == 10 : 30-bit immediate Float (IEEE-754 float32 rounded to top 30 bits)
//   bits[1:0] == 11 : Other immediates (subtag in bits[4:2], 27-bit payload in bits[31:5])
// ---------------------------------------------------------------------------
using Oop = uint32_t;

enum OopTag : uint32_t {
  TAG_OBJ   = 0u,
  TAG_INT   = 1u,
  TAG_FLOAT = 2u,
  TAG_OTHER = 3u,
};

enum OtherSubtag : uint32_t {
  OTHER_SPECIAL = 0u,  // nil, up (super)
  OTHER_CHAR    = 1u,  // 21-bit Unicode codepoint
  OTHER_SYM     = 2u,  // 27-bit interned lowercase Symbol ID
};

static constexpr Oop kOopNull  = 0u;
static constexpr Oop kOopNil   = (0u << 5) | (OTHER_SPECIAL << 2) | TAG_OTHER;  // 0x03
static constexpr Oop kOopSuper = (1u << 5) | (OTHER_SPECIAL << 2) | TAG_OTHER;  // 0x23

static constexpr int32_t kMinInt30 = -(1 << 29);        // -536,870,912
static constexpr int32_t kMaxInt30 = (1 << 29) - 1;     // +536,870,911

static inline uint32_t oopTag(Oop v) { return v & 3u; }

static inline bool isObj(Oop v)   { return v != 0u && (v & 3u) == TAG_OBJ; }
static inline bool isInt(Oop v)   { return (v & 3u) == TAG_INT; }
static inline bool isFloat(Oop v) { return (v & 3u) == TAG_FLOAT; }
static inline bool isNum(Oop v)   { uint32_t t = v & 3u; return t == TAG_INT || t == TAG_FLOAT; }
static inline bool isNil(Oop v)   { return v == kOopNil; }
static inline bool isSuper(Oop v) { return v == kOopSuper; }

static inline bool isChar(Oop v) {
  return (v & 0x1Fu) == ((OTHER_CHAR << 2) | TAG_OTHER);
}
static inline bool isSym(Oop v) {
  return (v & 0x1Fu) == ((OTHER_SYM << 2) | TAG_OTHER);
}

static inline constexpr Oop makeInt30(int32_t i) {
  return (static_cast<uint32_t>(i) << 2) | TAG_INT;
}
static inline int32_t asInt30(Oop v) {
  return static_cast<int32_t>(v) >> 2;
}

static constexpr Oop kOopFalse = makeInt30(0);
static constexpr Oop kOopTrue  = makeInt30(1);
static inline Oop makeBool(bool b) { return b ? kOopTrue : kOopFalse; }

// Encode IEEE-754 float32 into 30-bit immediate Float using round-to-nearest-even.
static inline Oop makeFloatRaw(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  if ((u & 0x7FFFFFFFu) == 0u) {
    return TAG_FLOAT;  // Canonicalize +0.0f and -0.0f to 0x00000002
  }
  if ((u & 0x7F800000u) != 0x7F800000u) {
    // Round-to-nearest-ties-to-even at bit 2, clamping at max finite float32
    if ((u & 0x7FFFFFFFu) <= 0x7F7FFFFCu) {
      u += 1u + ((u >> 2) & 1u);
    }
  }
  return (u & ~3u) | TAG_FLOAT;
}

static inline float asFloat(Oop v) {
  uint32_t bits = v & ~3u;
  float f = 0.0f;
  std::memcpy(&f, &bits, 4);
  return f;
}

static inline float toFloatUnchecked(Oop v) {
  return isInt(v) ? static_cast<float>(asInt30(v)) : asFloat(v);
}

static inline Oop makeInt64OrFloat(int64_t v) {
  if (v >= kMinInt30 && v <= kMaxInt30) {
    return makeInt30(static_cast<int32_t>(v));
  }
  return makeFloatRaw(static_cast<float>(v));
}

static inline int32_t toNearestIntUnchecked(Oop v) {
  if (isInt(v)) return asInt30(v);
  return static_cast<int32_t>(std::floor(asFloat(v) + 0.5f));
}

static inline Oop makeChar(uint32_t cp) {
  return ((cp & 0x1FFFFFu) << 5) | (OTHER_CHAR << 2) | TAG_OTHER;
}
static inline uint32_t asChar(Oop v) {
  return v >> 5;
}

static inline Oop makeSym(uint32_t symId) {
  return (symId << 5) | (OTHER_SYM << 2) | TAG_OTHER;
}
static inline uint32_t asSym(Oop v) {
  return v >> 5;
}

// Pointer <-> Oop conversion:
// - 32-bit ESP32: zero-overhead direct pointer cast.
// - 64-bit Host : 8-byte-aligned offset from gArenaBase.
static inline Oop oopFromObj(const ObjHeader* p) {
  if (!p) return kOopNull;
#if UINTPTR_MAX == 0xFFFFFFFFu
  return static_cast<Oop>(reinterpret_cast<uintptr_t>(p));
#else
  return static_cast<Oop>(reinterpret_cast<const uint8_t*>(p) - gArenaBase);
#endif
}

static inline ObjHeader* objFromOop(Oop v) {
  if (!isObj(v)) return nullptr;
#if UINTPTR_MAX == 0xFFFFFFFFu
  return reinterpret_cast<ObjHeader*>(static_cast<uintptr_t>(v));
#else
  return reinterpret_cast<ObjHeader*>(gArenaBase + v);
#endif
}

}  // namespace tt
