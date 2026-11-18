#include "tt_vm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tt {

uint8_t* gArenaBase = nullptr;

static constexpr uint32_t BLK_USED  = 1u;
static constexpr uint32_t BLK_GCOBJ = 2u;
static constexpr uint32_t BLK_MASK  = ~7u;

static inline uint32_t alignUp8(uint32_t n) {
  return (n + 7u) & ~7u;
}

static uint32_t fnv1aBytes(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

static uint32_t fnv1aLower(const char* s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c + 32);
    h ^= c;
    h *= 16777619u;
  }
  return h ? h : 1u;
}

static inline uint32_t hashInt32(int32_t v) {
  uint32_t x = static_cast<uint32_t>(v);
  x = ((x >> 16) ^ x) * 0x45d9f3bu;
  x = ((x >> 16) ^ x) * 0x45d9f3bu;
  x = (x >> 16) ^ x;
  return x ? x : 1u;
}

// ---------------------------------------------------------------------------
// TempRootGuard
// ---------------------------------------------------------------------------
TempRootGuard::TempRootGuard(VM& v) : vm(v), saved(v.tempRootCount) {}

TempRootGuard::TempRootGuard(VM& v, Oop o) : vm(v), saved(v.tempRootCount) {
  push(o);
}

void TempRootGuard::push(Oop o) {
  if (vm.tempRootCount < kMaxTempRoots) {
    vm.tempRoots[vm.tempRootCount++] = o;
  }
}

TempRootGuard::~TempRootGuard() {
  vm.tempRootCount = saved;
}

// ---------------------------------------------------------------------------
// HeapArena Implementation
// ---------------------------------------------------------------------------
bool HeapArena::init(uint32_t arenaBytes) {
  arenaBytes = alignUp8(arenaBytes);
  if (arenaBytes < 4096u) arenaBytes = 4096u;
#if defined(TT_HOST_BUILD)
  void* mem = nullptr;
  if (posix_memalign(&mem, 8, arenaBytes) != 0) return false;
  base = static_cast<uint8_t*>(mem);
#else
  base = static_cast<uint8_t*>(std::malloc(arenaBytes));
  if (!base) return false;
#endif
  std::memset(base, 0, arenaBytes);
  totalBytes    = arenaBytes;
  usedBytes     = 0;
  externalBytes = 0;
  gcThreshold   = (arenaBytes * 3u) / 4u;
  gcCount       = 0;
  searchCursor  = 4u;

  // Initial single free block at hdrOff = 4 (payload at 8), spanning totalBytes - 8
  uint32_t initSpan = totalBytes - 8u;
  *reinterpret_cast<uint32_t*>(base + 4u) = initSpan;
  // End sentinel at totalBytes - 4
  *reinterpret_cast<uint32_t*>(base + totalBytes - 4u) = BLK_USED;

  gArenaBase = base;
  return true;
}

void HeapArena::destroy() {
  if (base) {
    std::free(base);
    base = nullptr;
  }
  if (gArenaBase == base) {
    gArenaBase = nullptr;
  }
  totalBytes = usedBytes = externalBytes = 0;
}

void* HeapArena::allocRaw(uint32_t payloadBytes, bool isGcObj) {
  uint32_t neededSpan = alignUp8(payloadBytes + 4u);
  if (neededSpan < 16u) neededSpan = 16u;

  const uint32_t endHdrOff = totalBytes - 4u;
  uint32_t startOff = searchCursor;
  if (startOff < 4u || startOff >= endHdrOff) startOff = 4u;

  // Two-phase next-fit scan: [startOff .. endHdrOff) then [4 .. startOff)
  for (int pass = 0; pass < 2; pass++) {
    uint32_t off   = (pass == 0) ? startOff : 4u;
    uint32_t limit = (pass == 0) ? endHdrOff : startOff;
    while (off < limit) {
      uint32_t hdr  = *reinterpret_cast<uint32_t*>(base + off);
      uint32_t span = hdr & BLK_MASK;
      if (span == 0u) break;
      if (!(hdr & BLK_USED) && span >= neededSpan) {
        uint32_t flags = BLK_USED | (isGcObj ? BLK_GCOBJ : 0u);
        if (span >= neededSpan + 16u) {
          // Split block
          *reinterpret_cast<uint32_t*>(base + off) = neededSpan | flags;
          uint32_t remOff  = off + neededSpan;
          uint32_t remSpan = span - neededSpan;
          *reinterpret_cast<uint32_t*>(base + remOff) = remSpan;
          searchCursor = remOff;
          usedBytes += neededSpan;
        } else {
          *reinterpret_cast<uint32_t*>(base + off) = span | flags;
          searchCursor = (off + span < endHdrOff) ? (off + span) : 4u;
          usedBytes += span;
        }
        void* payload = base + off + 4u;
        std::memset(payload, 0, (span >= neededSpan + 16u ? neededSpan : span) - 4u);
        return payload;
      }
      off += span;
    }
  }
  return nullptr;
}

void freeCompiledCode(CompiledCode* cc, HeapArena& arena) {
  if (!cc) return;
  if (cc->code) {
    size_t b = cc->codeLen * sizeof(uint32_t);
    std::free(cc->code);
    if (arena.externalBytes >= b) arena.externalBytes -= static_cast<uint32_t>(b);
  }
  if (cc->consts) {
    size_t b = cc->constCount * sizeof(Oop);
    std::free(cc->consts);
    if (arena.externalBytes >= b) arena.externalBytes -= static_cast<uint32_t>(b);
  }
  if (cc->sendSites) {
    size_t b = cc->sendSiteCount * sizeof(SendSite);
    std::free(cc->sendSites);
    if (arena.externalBytes >= b) arena.externalBytes -= static_cast<uint32_t>(b);
  }
  if (cc->blkParams) {
    size_t b = cc->blkParamCount * sizeof(BlkParam);
    std::free(cc->blkParams);
    if (arena.externalBytes >= b) arena.externalBytes -= static_cast<uint32_t>(b);
  }
  std::free(cc);
  if (arena.externalBytes >= sizeof(CompiledCode)) {
    arena.externalBytes -= sizeof(CompiledCode);
  }
}

static void finalizeObj(VM& vm, ObjHeader* hdr) {
  switch (static_cast<ObjFormat>(hdr->format)) {
    case OBJ_BUF: {
      auto* b = reinterpret_cast<ObjBuf*>(hdr);
      if (b->data) vm.freeExternal(b->data, b->cap);
      break;
    }
    case OBJ_LIST: {
      auto* l = reinterpret_cast<ObjList*>(hdr);
      if (l->items) vm.freeExternal(l->items, l->cap * sizeof(Oop));
      break;
    }
    case OBJ_DICT: {
      auto* d = reinterpret_cast<ObjDict*>(hdr);
      if (d->entries) vm.freeExternal(d->entries, d->cap * sizeof(DictEntry));
      break;
    }
    case OBJ_CLS: {
      auto* c = reinterpret_cast<ObjCls*>(hdr);
      if (c->allVarSyms) vm.freeExternal(c->allVarSyms, c->allVarCount * sizeof(uint32_t));
      break;
    }
    case OBJ_METH: {
      auto* m = reinterpret_cast<ObjMeth*>(hdr);
      if (m->comp) {
        freeCompiledCode(m->comp, vm.heap);
        m->comp = nullptr;
      }
      if (m->ownsSource && m->source) {
        vm.freeExternal(const_cast<char*>(m->source), m->sourceLen + 1u);
        m->source = nullptr;
      }
      break;
    }
    case OBJ_BMP: {
      auto* bmp = reinterpret_cast<ObjBmp*>(hdr);
      if (bmp->pixels) {
        vm.freeExternal(bmp->pixels, static_cast<size_t>(bmp->wid) * bmp->hei);
      }
      if (bmp->palette) {
        vm.freeExternal(bmp->palette, 256u * sizeof(uint16_t));
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// VM External Allocation & Garbage Collector
// ---------------------------------------------------------------------------
void* VM::allocExternal(size_t bytes) {
  if (bytes == 0) return nullptr;
  if (!gcInhibit && heap.externalBytes + bytes > heap.totalBytes) {
    runGC();
  }
  void* p = std::malloc(bytes);
  if (!p) {
    runGC();
    p = std::malloc(bytes);
    if (!p) toss("Out of external memory (%u bytes)", static_cast<unsigned>(bytes));
  }
  heap.externalBytes += static_cast<uint32_t>(bytes);
  return p;
}

void* VM::reallocExternal(void* ptr, size_t oldBytes, size_t newBytes) {
  if (newBytes == 0) {
    freeExternal(ptr, oldBytes);
    return nullptr;
  }
  if (!ptr) return allocExternal(newBytes);
  void* p = std::realloc(ptr, newBytes);
  if (!p) {
    runGC();
    p = std::realloc(ptr, newBytes);
    if (!p) toss("Out of external memory on realloc (%u bytes)", static_cast<unsigned>(newBytes));
  }
  if (newBytes >= oldBytes) {
    heap.externalBytes += static_cast<uint32_t>(newBytes - oldBytes);
  } else if (heap.externalBytes >= (oldBytes - newBytes)) {
    heap.externalBytes -= static_cast<uint32_t>(oldBytes - newBytes);
  }
  return p;
}

void VM::freeExternal(void* ptr, size_t bytes) {
  if (!ptr) return;
  std::free(ptr);
  if (heap.externalBytes >= bytes) {
    heap.externalBytes -= static_cast<uint32_t>(bytes);
  } else {
    heap.externalBytes = 0;
  }
}

ObjHeader* VM::allocObj(uint16_t clsId, ObjFormat fmt, uint32_t hdrSizeField, size_t totalStructBytes) {
  if (!gcInhibit && (heap.usedBytes + totalStructBytes + 16u > heap.gcThreshold ||
                     heap.externalBytes > heap.totalBytes)) {
    runGC();
  }
  void* mem = heap.allocRaw(static_cast<uint32_t>(totalStructBytes), true);
  if (!mem) {
    runGC();
    mem = heap.allocRaw(static_cast<uint32_t>(totalStructBytes), true);
    if (!mem) {
      toss("Out of TerseTalk heap memory (used=%u/%u)", heap.usedBytes, heap.totalBytes);
    }
  }
  auto* hdr    = static_cast<ObjHeader*>(mem);
  hdr->clsId   = clsId;
  hdr->format  = static_cast<uint8_t>(fmt);
  hdr->gcFlags = 0;
  hdr->size    = hdrSizeField;
  return hdr;
}

void VM::markOop(Oop v) {
  if (!isObj(v)) return;
  ObjHeader* root = objFromOop(v);
  if (!root || (root->gcFlags & GC_MARKED)) return;

  std::vector<ObjHeader*> work;
  work.reserve(256);
  root->gcFlags |= GC_MARKED;
  work.push_back(root);

  auto pushOop = [&](Oop child) {
    if (!isObj(child)) return;
    ObjHeader* ch = objFromOop(child);
    if (ch && !(ch->gcFlags & GC_MARKED)) {
      ch->gcFlags |= GC_MARKED;
      work.push_back(ch);
    }
  };

  while (!work.empty()) {
    ObjHeader* cur = work.back();
    work.pop_back();

    switch (static_cast<ObjFormat>(cur->format)) {
      case OBJ_SLOTS: {
        auto* s = reinterpret_cast<ObjSlots*>(cur);
        for (uint32_t i = 0; i < cur->size; i++) pushOop(s->slots[i]);
        break;
      }
      case OBJ_LIST: {
        auto* l = reinterpret_cast<ObjList*>(cur);
        for (uint32_t i = 0; i < l->len; i++) pushOop(l->items[i]);
        break;
      }
      case OBJ_DICT: {
        auto* d = reinterpret_cast<ObjDict*>(cur);
        for (uint32_t i = 0; i < d->cap; i++) {
          Oop k = d->entries[i].key;
          if (k != kOopNull && k != kOopSuper) {
            pushOop(k);
            pushOop(d->entries[i].val);
          }
        }
        break;
      }
      case OBJ_CLS: {
        auto* c = reinterpret_cast<ObjCls*>(cur);
        pushOop(c->nameStr);
        pushOop(c->methDict);
        pushOop(c->myVarsList);
        pushOop(c->subsList);
        pushOop(c->savedInstsDict);
        for (int i = 0; i < kUsrLruSize; i++) pushOop(c->lruCache[i]);
        break;
      }
      case OBJ_METH: {
        auto* m = reinterpret_cast<ObjMeth*>(cur);
        if (m->comp) {
          for (uint16_t i = 0; i < m->comp->constCount; i++) {
            pushOop(m->comp->consts[i]);
          }
          for (uint16_t i = 0; i < m->comp->sendSiteCount; i++) {
            if (m->comp->sendSites[i].cachedMeth) {
              pushOop(oopFromObj(&m->comp->sendSites[i].cachedMeth->hdr));
            }
          }
        }
        break;
      }
      case OBJ_BLK: {
        auto* b = reinterpret_cast<ObjBlk*>(cur);
        pushOop(b->blockMeth);
        pushOop(b->homeFrame);
        break;
      }
      case OBJ_FRAME: {
        auto* f = reinterpret_cast<ObjFrame*>(cur);
        pushOop(f->prevFrame);
        pushOop(f->self);
        pushOop(f->methObj);
        for (uint32_t i = 0; i < cur->size; i++) pushOop(f->locals[i]);
        break;
      }
      case OBJ_REX: {
        auto* r = reinterpret_cast<ObjRex*>(cur);
        pushOop(r->patternStr);
        break;
      }
      case OBJ_PEN: {
        auto* p = reinterpret_cast<ObjPen*>(cur);
        pushOop(p->targetBmp);
        break;
      }
      case OBJ_WIN: {
        auto* w = reinterpret_cast<ObjWin*>(cur);
        pushOop(w->appObj);
        break;
      }
      default:
        break;
    }
  }
}

void VM::runGC() {
  if (gcInhibit || !heap.base) return;
  heap.gcCount++;

  // 1. Mark all roots
  markOop(globalsDict);
  for (uint16_t i = 1; i <= classCount; i++) {
    markOop(classById[i]);
  }
  for (uint32_t i = 0; i < sp; i++) {
    markOop(stack[i]);
  }
  for (uint16_t i = 0; i < frameCount; i++) {
    markOop(frames[i].self);
    markOop(frames[i].heapFrame);
    if (frames[i].meth) {
      markOop(oopFromObj(&frames[i].meth->hdr));
    }
  }
  for (uint16_t i = 0; i < tempRootCount; i++) {
    markOop(tempRoots[i]);
  }
  if (host.markUiRoots) {
    host.markUiRoots(host.user, *this);
  }

  // 2. Single-pass linear Sweep + Backward Coalesce
  const uint32_t endHdrOff = heap.totalBytes - 4u;
  uint32_t off = 4u;
  uint32_t prevFreeOff = 0u;
  uint32_t newUsedBytes = 0u;

  while (off < endHdrOff) {
    uint32_t hdr  = *reinterpret_cast<uint32_t*>(heap.base + off);
    uint32_t span = hdr & BLK_MASK;
    if (span == 0u) break;

    bool isLive = false;
    if (hdr & BLK_USED) {
      if (hdr & BLK_GCOBJ) {
        auto* obj = reinterpret_cast<ObjHeader*>(heap.base + off + 4u);
        if (obj->gcFlags & GC_MARKED) {
          obj->gcFlags &= ~GC_MARKED;
          isLive = true;
        } else {
          finalizeObj(*this, obj);
        }
      } else {
        isLive = true;
      }
    }

    if (isLive) {
      prevFreeOff = 0u;
      newUsedBytes += span;
    } else {
      if (prevFreeOff != 0u) {
        uint32_t prevSpan = *reinterpret_cast<uint32_t*>(heap.base + prevFreeOff) & BLK_MASK;
        *reinterpret_cast<uint32_t*>(heap.base + prevFreeOff) = prevSpan + span;
      } else {
        *reinterpret_cast<uint32_t*>(heap.base + off) = span;
        prevFreeOff = off;
      }
    }
    off += span;
  }

  heap.usedBytes    = newUsedBytes;
  heap.searchCursor = 4u;
  uint32_t nextThresh = newUsedBytes + (heap.totalBytes - newUsedBytes) / 2u;
  uint32_t minThresh  = heap.totalBytes / 2u;
  heap.gcThreshold    = (nextThresh < minThresh) ? minThresh : nextThresh;
}

// ---------------------------------------------------------------------------
// Symbols (Case-Folded Lowercase Interning)
// ---------------------------------------------------------------------------
uint32_t VM::internSym(const char* s, size_t len) {
  uint32_t h = fnv1aLower(s, len);
  for (uint16_t i = 1; i <= symCount; i++) {
    if (symHashes[i] == h && symLens[i] == len) {
      bool match = true;
      for (size_t k = 0; k < len; k++) {
        uint8_t c = static_cast<uint8_t>(s[k]);
        if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c + 32);
        if (static_cast<uint8_t>(symChars[i][k]) != c) {
          match = false;
          break;
        }
      }
      if (match) return i;
    }
  }
  if (symCount + 1 >= kMaxSymbols) {
    toss("Symbol table overflow (%u symbols)", symCount);
  }
  uint16_t id = ++symCount;
  char* copy = static_cast<char*>(std::malloc(len + 1u));
  for (size_t k = 0; k < len; k++) {
    uint8_t c = static_cast<uint8_t>(s[k]);
    if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c + 32);
    copy[k] = static_cast<char>(c);
  }
  copy[len] = '\0';
  symChars[id]  = copy;
  symLens[id]   = static_cast<uint16_t>(len);
  symHashes[id] = h;
  return id;
}

uint32_t VM::internSym(const char* s) {
  return internSym(s, std::strlen(s));
}

const char* VM::symCStr(uint32_t symId) const {
  if (symId == 0 || symId > symCount) return "";
  return symChars[symId];
}

// ---------------------------------------------------------------------------
// Built-In Object Constructors & Value Operations
// ---------------------------------------------------------------------------
Oop VM::makeFloatChecked(float f) {
  if (std::isnan(f)) toss("NotANumber");
  if (std::isinf(f)) toss("IsInfinite");
  return makeFloatRaw(f);
}

Oop VM::newStr(const char* s, size_t len) {
  size_t total = offsetof(ObjStr, chars) + len + 1u;
  auto* o = reinterpret_cast<ObjStr*>(allocObj(CLS_STR, OBJ_STR, static_cast<uint32_t>(len), total));
  if (len > 0 && s) {
    std::memcpy(o->chars, s, len);
  }
  o->chars[len] = '\0';
  o->hash = fnv1aBytes(o->chars, len);
  return oopFromObj(&o->hdr);
}

Oop VM::newStr(const char* s) {
  return newStr(s ? s : "", s ? std::strlen(s) : 0u);
}

Oop VM::newBytes(const uint8_t* b, size_t len) {
  size_t total = offsetof(ObjBytes, bytes) + (len > 0 ? len : 1u);
  auto* o = reinterpret_cast<ObjBytes*>(allocObj(CLS_BYTES, OBJ_BYTES, static_cast<uint32_t>(len), total));
  if (len > 0 && b) {
    std::memcpy(o->bytes, b, len);
  }
  return oopFromObj(&o->hdr);
}

Oop VM::newBuf(const char* initStr, size_t len) {
  auto* o = reinterpret_cast<ObjBuf*>(allocObj(CLS_BUF, OBJ_BUF, 0, sizeof(ObjBuf)));
  TempRootGuard rg(*this, oopFromObj(&o->hdr));
  uint32_t cap = (len + 16u < 32u) ? 32u : static_cast<uint32_t>(len + 16u);
  o->data = static_cast<char*>(allocExternal(cap));
  if (len > 0 && initStr) {
    std::memcpy(o->data, initStr, len);
  }
  o->data[len] = '\0';
  o->len = static_cast<uint32_t>(len);
  o->cap = cap;
  return oopFromObj(&o->hdr);
}

Oop VM::newList(uint32_t initialCap) {
  auto* o = reinterpret_cast<ObjList*>(allocObj(CLS_LIST, OBJ_LIST, 0, sizeof(ObjList)));
  TempRootGuard rg(*this, oopFromObj(&o->hdr));
  if (initialCap > 0) {
    o->items = static_cast<Oop*>(allocExternal(initialCap * sizeof(Oop)));
    o->cap   = initialCap;
  }
  o->len = 0;
  return oopFromObj(&o->hdr);
}

Oop VM::newListFrom(const Oop* elems, uint32_t count) {
  // Root input elements temporarily in case allocObj triggers GC
  TempRootGuard rg(*this);
  for (uint32_t i = 0; i < count; i++) rg.push(elems[i]);
  Oop listOop = newList(count > 0 ? count : 2u);
  auto* l = reinterpret_cast<ObjList*>(objFromOop(listOop));
  for (uint32_t i = 0; i < count; i++) {
    l->items[i] = elems[i];
  }
  l->len = count;
  return listOop;
}

void VM::listAppend(Oop listOop, Oop val) {
  TempRootGuard rg(*this, listOop);
  rg.push(val);
  auto* l = reinterpret_cast<ObjList*>(objFromOop(listOop));
  if (l->len >= l->cap) {
    uint32_t newCap = (l->cap == 0) ? 4u : (l->cap * 2u);
    l->items = static_cast<Oop*>(reallocExternal(l->items, l->cap * sizeof(Oop), newCap * sizeof(Oop)));
    l->cap   = newCap;
  }
  l->items[l->len++] = val;
}

Oop VM::newDict(uint32_t initialCap) {
  uint32_t cap = 8u;
  while (cap < initialCap) cap <<= 1u;
  auto* o = reinterpret_cast<ObjDict*>(allocObj(CLS_DICT, OBJ_DICT, 0, sizeof(ObjDict)));
  TempRootGuard rg(*this, oopFromObj(&o->hdr));
  o->entries = static_cast<DictEntry*>(allocExternal(cap * sizeof(DictEntry)));
  std::memset(o->entries, 0, cap * sizeof(DictEntry));
  o->len  = 0;
  o->used = 0;
  o->cap  = cap;
  return oopFromObj(&o->hdr);
}

Oop VM::newUsr(uint16_t clsId) {
  ObjCls* c = clsById(clsId);
  uint16_t nVars = c ? c->allVarCount : 0;
  size_t total = offsetof(ObjSlots, slots) + (nVars > 0 ? nVars : 1u) * sizeof(Oop);
  auto* o = reinterpret_cast<ObjSlots*>(allocObj(clsId, OBJ_SLOTS, nVars, total));
  o->oid      = 0;
  o->onameSym = 0;
  for (uint16_t i = 0; i < nVars; i++) {
    o->slots[i] = kOopNil;
  }
  Oop res = oopFromObj(&o->hdr);
  if (c && c->isUsrCls) {
    // Store in class's LRU ring
    for (int i = 0; i < kUsrLruSize - 1; i++) {
      c->lruCache[i] = c->lruCache[i + 1];
    }
    c->lruCache[kUsrLruSize - 1] = res;
  }
  return res;
}

uint16_t VM::oopClsId(Oop v) const {
  switch (oopTag(v)) {
    case TAG_INT:   return CLS_INT;
    case TAG_FLOAT: return CLS_FLOAT;
    case TAG_OBJ: {
      ObjHeader* h = objFromOop(v);
      return h ? h->clsId : static_cast<uint16_t>(CLS_UNDEFINED);
    }
    case TAG_OTHER:
      if (v == kOopNil) return CLS_UNDEFINED;
      if (v == kOopSuper) return CLS_SUPER;
      if (isChar(v)) return CLS_CHAR;
      if (isSym(v)) return CLS_STR;
      return CLS_UNDEFINED;
  }
  return CLS_UNDEFINED;
}

ObjCls* VM::clsById(uint16_t id) const {
  if (id == 0 || id > classCount) return nullptr;
  Oop co = classById[id];
  return isObj(co) ? reinterpret_cast<ObjCls*>(objFromOop(co)) : nullptr;
}

ObjCls* VM::oopCls(Oop v) const {
  return clsById(oopClsId(v));
}

bool VM::isa(Oop v, uint16_t targetClsId) const {
  uint16_t cid = oopClsId(v);
  while (cid != 0) {
    if (cid == targetClsId) return true;
    ObjCls* c = clsById(cid);
    if (!c) break;
    cid = c->superClsId;
  }
  return false;
}

bool VM::truth(Oop v) const {
  if (v == kOopNil || v == kOopFalse) return false;
  if (isInt(v)) return asInt30(v) != 0;
  if (isFloat(v)) return toNearestIntUnchecked(v) != 0;
  if (isObj(v)) {
    ObjHeader* h = objFromOop(v);
    switch (static_cast<ObjFormat>(h->format)) {
      case OBJ_STR:
      case OBJ_BYTES: return h->size > 0;
      case OBJ_LIST:  return reinterpret_cast<ObjList*>(h)->len > 0;
      case OBJ_DICT:  return reinterpret_cast<ObjDict*>(h)->len > 0;
      default:        return true;
    }
  }
  return true;
}

uint32_t VM::oopHash(Oop v) const {
  if (isInt(v)) return hashInt32(asInt30(v));
  if (isFloat(v)) {
    float f = asFloat(v);
    int32_t iv = static_cast<int32_t>(f);
    if (static_cast<float>(iv) == f && iv >= kMinInt30 && iv <= kMaxInt30) {
      return hashInt32(iv);
    }
    return hashInt32(static_cast<int32_t>(v));
  }
  if (isSym(v)) {
    uint32_t sid = asSym(v);
    return (sid <= symCount) ? symHashes[sid] : sid;
  }
  if (isChar(v)) return hashInt32(static_cast<int32_t>(asChar(v)));
  if (v == kOopNil) return 0x9E3779B9u;
  if (isObj(v)) {
    ObjHeader* h = objFromOop(v);
    switch (static_cast<ObjFormat>(h->format)) {
      case OBJ_STR:   return reinterpret_cast<ObjStr*>(h)->hash;
      case OBJ_BYTES: return fnv1aBytes(reinterpret_cast<ObjBytes*>(h)->bytes, h->size);
      case OBJ_LIST: {
        auto* l = reinterpret_cast<ObjList*>(h);
        uint32_t acc = 17u;
        for (uint32_t i = 0; i < l->len; i++) acc = acc * 31u + oopHash(l->items[i]);
        return acc ? acc : 1u;
      }
      default:
        return hashInt32(static_cast<int32_t>(v));
    }
  }
  return 1u;
}

bool VM::oopEquals(Oop a, Oop b) const {
  if (a == b) return true;
  if (isNum(a) && isNum(b)) {
    if (isInt(a) && isInt(b)) return a == b;
    return toFloatUnchecked(a) == toFloatUnchecked(b);
  }
  if (!isObj(a) || !isObj(b)) return false;
  ObjHeader* ha = objFromOop(a);
  ObjHeader* hb = objFromOop(b);
  if (ha->format != hb->format || ha->clsId != hb->clsId) return false;

  switch (static_cast<ObjFormat>(ha->format)) {
    case OBJ_STR: {
      if (ha->size != hb->size) return false;
      auto* sa = reinterpret_cast<ObjStr*>(ha);
      auto* sb = reinterpret_cast<ObjStr*>(hb);
      if (sa->hash != sb->hash) return false;
      return std::memcmp(sa->chars, sb->chars, ha->size) == 0;
    }
    case OBJ_BYTES: {
      if (ha->size != hb->size) return false;
      return std::memcmp(reinterpret_cast<ObjBytes*>(ha)->bytes,
                         reinterpret_cast<ObjBytes*>(hb)->bytes, ha->size) == 0;
    }
    case OBJ_LIST: {
      auto* la = reinterpret_cast<ObjList*>(ha);
      auto* lb = reinterpret_cast<ObjList*>(hb);
      if (la->len != lb->len) return false;
      for (uint32_t i = 0; i < la->len; i++) {
        if (!oopEquals(la->items[i], lb->items[i])) return false;
      }
      return true;
    }
    case OBJ_DICT: {
      auto* da = reinterpret_cast<ObjDict*>(ha);
      auto* db = reinterpret_cast<ObjDict*>(hb);
      if (da->len != db->len) return false;
      for (uint32_t i = 0; i < da->cap; i++) {
        Oop k = da->entries[i].key;
        if (k != kOopNull && k != kOopSuper) {
          Oop vb = dictGet(b, k);
          if (vb == kOopNull) return false;
          // Note: dictGet returns kOopNil if missing, so check existence explicitly:
          bool found = false;
          uint32_t mask = db->cap - 1u;
          uint32_t idx  = da->entries[i].hash & mask;
          for (uint32_t step = 0; step < db->cap; step++) {
            const DictEntry& e = db->entries[idx];
            if (e.key == kOopNull) break;
            if (e.key != kOopSuper && e.hash == da->entries[i].hash && oopEquals(e.key, k)) {
              found = oopEquals(da->entries[i].val, e.val);
              break;
            }
            idx = (idx + 1u) & mask;
          }
          if (!found) return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

int VM::oopCompare(Oop a, Oop b) const {
  if (oopEquals(a, b)) return 0;
  if (isNum(a) && isNum(b)) {
    if (isInt(a) && isInt(b)) return (asInt30(a) < asInt30(b)) ? -1 : 1;
    float fa = toFloatUnchecked(a);
    float fb = toFloatUnchecked(b);
    return (fa < fb) ? -1 : (fa > fb ? 1 : 0);
  }
  if (isChar(a) && isChar(b)) {
    return (asChar(a) < asChar(b)) ? -1 : 1;
  }
  uint16_t ca = oopClsId(a);
  uint16_t cb = oopClsId(b);
  if (ca != cb) {
    ObjCls* clsa = clsById(ca);
    ObjCls* clsb = clsById(cb);
    const char* na = (clsa && isObj(clsa->nameStr)) ? reinterpret_cast<ObjStr*>(objFromOop(clsa->nameStr))->chars : "";
    const char* nb = (clsb && isObj(clsb->nameStr)) ? reinterpret_cast<ObjStr*>(objFromOop(clsb->nameStr))->chars : "";
    return std::strcmp(na, nb);
  }
  if (isObj(a) && isObj(b)) {
    ObjHeader* ha = objFromOop(a);
    ObjHeader* hb = objFromOop(b);
    if (ha->format == OBJ_STR) {
      auto* sa = reinterpret_cast<ObjStr*>(ha);
      auto* sb = reinterpret_cast<ObjStr*>(hb);
      uint32_t minLen = std::min(ha->size, hb->size);
      int cmp = std::memcmp(sa->chars, sb->chars, minLen);
      if (cmp != 0) return cmp;
      return (ha->size < hb->size) ? -1 : (ha->size > hb->size ? 1 : 0);
    }
    if (ha->format == OBJ_LIST) {
      auto* la = reinterpret_cast<ObjList*>(ha);
      auto* lb = reinterpret_cast<ObjList*>(hb);
      uint32_t minLen = std::min(la->len, lb->len);
      for (uint32_t i = 0; i < minLen; i++) {
        int c = oopCompare(la->items[i], lb->items[i]);
        if (c != 0) return c;
      }
      return (la->len < lb->len) ? -1 : (la->len > lb->len ? 1 : 0);
    }
  }
  return (a < b) ? -1 : 1;
}

// ---------------------------------------------------------------------------
// Dict Open-Addressed Hash Table Operations
// ---------------------------------------------------------------------------
static void dictRehash(VM& vm, ObjDict* d, uint32_t newCap) {
  DictEntry* oldEntries = d->entries;
  uint32_t   oldCap     = d->cap;

  DictEntry* fresh = static_cast<DictEntry*>(vm.allocExternal(newCap * sizeof(DictEntry)));
  std::memset(fresh, 0, newCap * sizeof(DictEntry));

  uint32_t mask = newCap - 1u;
  for (uint32_t i = 0; i < oldCap; i++) {
    Oop k = oldEntries[i].key;
    if (k != kOopNull && k != kOopSuper) {
      uint32_t idx = oldEntries[i].hash & mask;
      while (fresh[idx].key != kOopNull) {
        idx = (idx + 1u) & mask;
      }
      fresh[idx] = oldEntries[i];
    }
  }
  vm.freeExternal(oldEntries, oldCap * sizeof(DictEntry));
  d->entries = fresh;
  d->cap     = newCap;
  d->used    = d->len;
}

Oop VM::dictGet(Oop dictOop, Oop key) const {
  if (!isObj(dictOop)) return kOopNil;
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(dictOop));
  if (d->len == 0 || d->cap == 0) return kOopNil;
  uint32_t h    = oopHash(key);
  uint32_t mask = d->cap - 1u;
  uint32_t idx  = h & mask;
  for (uint32_t step = 0; step < d->cap; step++) {
    const DictEntry& e = d->entries[idx];
    if (e.key == kOopNull) return kOopNil;
    if (e.key != kOopSuper && e.hash == h && oopEquals(e.key, key)) {
      return e.val;
    }
    idx = (idx + 1u) & mask;
  }
  return kOopNil;
}

void VM::dictPut(Oop dictOop, Oop key, Oop val) {
  TempRootGuard rg(*this, dictOop);
  rg.push(key);
  rg.push(val);

  auto* d = reinterpret_cast<ObjDict*>(objFromOop(dictOop));
  if ((d->used + 1u) * 4u >= d->cap * 3u) {
    dictRehash(*this, d, d->cap * 2u);
  }
  uint32_t h    = oopHash(key);
  uint32_t mask = d->cap - 1u;
  uint32_t idx  = h & mask;
  int32_t  firstTomb = -1;

  for (uint32_t step = 0; step < d->cap; step++) {
    DictEntry& e = d->entries[idx];
    if (e.key == kOopNull) {
      DictEntry& target = (firstTomb >= 0) ? d->entries[firstTomb] : e;
      if (firstTomb < 0) d->used++;
      target.key  = key;
      target.val  = val;
      target.hash = h;
      d->len++;
      return;
    }
    if (e.key == kOopSuper) {
      if (firstTomb < 0) firstTomb = static_cast<int32_t>(idx);
    } else if (e.hash == h && oopEquals(e.key, key)) {
      e.val = val;
      return;
    }
    idx = (idx + 1u) & mask;
  }
}

Oop VM::dictDel(Oop dictOop, Oop key) {
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(dictOop));
  if (d->len == 0 || d->cap == 0) return kOopNil;
  uint32_t h    = oopHash(key);
  uint32_t mask = d->cap - 1u;
  uint32_t idx  = h & mask;
  for (uint32_t step = 0; step < d->cap; step++) {
    DictEntry& e = d->entries[idx];
    if (e.key == kOopNull) return kOopNil;
    if (e.key != kOopSuper && e.hash == h && oopEquals(e.key, key)) {
      Oop removed = e.val;
      e.key = kOopSuper;  // Tombstone
      e.val = kOopNil;
      d->len--;
      return removed;
    }
    idx = (idx + 1u) & mask;
  }
  return kOopNil;
}

void VM::dictSortedKeys(Oop dictOop, std::vector<Oop>& outKeys) const {
  outKeys.clear();
  if (!isObj(dictOop)) return;
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(dictOop));
  outKeys.reserve(d->len);
  for (uint32_t i = 0; i < d->cap; i++) {
    Oop k = d->entries[i].key;
    if (k != kOopNull && k != kOopSuper) {
      outKeys.push_back(k);
    }
  }
  std::sort(outKeys.begin(), outKeys.end(), [this](Oop a, Oop b) {
    return oopCompare(a, b) < 0;
  });
}

// ---------------------------------------------------------------------------
// String Formatting (`str` and `repr`)
// ---------------------------------------------------------------------------
std::string VM::toStdString(Oop v) {
  char buf[64];
  if (v == kOopNil) return "Nil";
  if (v == kOopSuper) return "up";
  if (isInt(v)) {
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(asInt30(v)));
    return buf;
  }
  if (isFloat(v)) {
    float f = asFloat(v);
    int32_t tr = static_cast<int32_t>(f);
    if (static_cast<float>(tr) == f && std::fabs(f) < 1e8f) {
      std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(tr));
    } else {
      std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(f));
    }
    return buf;
  }
  if (isChar(v)) {
    uint32_t cp = asChar(v);
    if (cp < 0x80u) return std::string(1, static_cast<char>(cp));
    std::snprintf(buf, sizeof(buf), "Char(%u)", cp);
    return buf;
  }
  if (isSym(v)) {
    return symCStr(asSym(v));
  }
  if (isObj(v)) {
    ObjHeader* h = objFromOop(v);
    switch (static_cast<ObjFormat>(h->format)) {
      case OBJ_STR: {
        auto* s = reinterpret_cast<ObjStr*>(h);
        return std::string(s->chars, h->size);
      }
      case OBJ_BYTES: {
        auto* b = reinterpret_cast<ObjBytes*>(h);
        return std::string(reinterpret_cast<const char*>(b->bytes), h->size);
      }
      case OBJ_BUF: {
        auto* b = reinterpret_cast<ObjBuf*>(h);
        return b->data ? std::string(b->data, b->len) : std::string();
      }
      case OBJ_CLS: {
        auto* c = reinterpret_cast<ObjCls*>(h);
        if (isObj(c->nameStr)) {
          auto* ns = reinterpret_cast<ObjStr*>(objFromOop(c->nameStr));
          return std::string(ns->chars, ns->hdr.size);
        }
        return symCStr(c->nameSym);
      }
      case OBJ_METH: {
        auto* m = reinterpret_cast<ObjMeth*>(h);
        if (!m->isPrim && m->source) {
          return std::string(m->source, m->sourceLen);
        }
        ObjCls* oc = clsById(m->onClsId);
        std::string cn = oc ? toStdString(oopFromObj(&oc->hdr)) : "?";
        return "\"<" + cn + " " + symCStr(m->nameSym) + "> Built-In\"";
      }
      case OBJ_LIST: {
        auto* l = reinterpret_cast<ObjList*>(h);
        std::string out = "LIST(";
        for (uint32_t i = 0; i < l->len; i++) {
          out += toReprString(l->items[i]);
          out += "; ";
        }
        out += ")";
        return out;
      }
      case OBJ_DICT: {
        std::vector<Oop> keys;
        dictSortedKeys(v, keys);
        std::string out = "DICT(";
        for (Oop k : keys) {
          out += "(" + toReprString(k) + "), (" + toReprString(dictGet(v, k)) + "); ";
        }
        out += ")";
        return out;
      }
      case OBJ_BLK: {
        auto* b = reinterpret_cast<ObjBlk*>(h);
        if (isObj(b->blockMeth)) {
          auto* bm = reinterpret_cast<ObjMeth*>(objFromOop(b->blockMeth));
          if (bm->source) return "FN(" + std::string(bm->source, bm->sourceLen) + ")";
        }
        return "FN(...)";
      }
      default: {
        ObjCls* c = clsById(h->clsId);
        std::string cn = c ? toStdString(oopFromObj(&c->hdr)) : "Obj";
        std::snprintf(buf, sizeof(buf), "~%u", static_cast<unsigned>(v));
        return cn + buf;
      }
    }
  }
  return "Nil";
}

std::string VM::toReprString(Oop v) {
  if (isObj(v)) {
    ObjHeader* h = objFromOop(v);
    if (h->format == OBJ_STR) {
      auto* s = reinterpret_cast<ObjStr*>(h);
      std::string out = "'";
      for (uint32_t i = 0; i < h->size; i++) {
        char c = s->chars[i];
        if (c == '\'') out += "''";
        else if (c == '[') out += "[[";
        else if (c == ']') out += "]]";
        else out += c;
      }
      out += "'";
      return out;
    }
    if (h->format == OBJ_BYTES) {
      auto* b = reinterpret_cast<ObjBytes*>(h);
      static const char kHex[] = "0123456789abcdef";
      std::string out = "(Hex de: '";
      for (uint32_t i = 0; i < h->size; i++) {
        out += kHex[(b->bytes[i] >> 4) & 0xF];
        out += kHex[b->bytes[i] & 0xF];
      }
      out += "')";
      return out;
    }
  }
  return toStdString(v);
}

}  // namespace tt
