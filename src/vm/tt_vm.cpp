#include "tt_vm.h"
#include "tt_compiler.h"
#include "tt_world.h"
#include "prelude_rodata.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace tt {

// ---------------------------------------------------------------------------
// Logging, Errors & Host VFS Fallbacks
// ---------------------------------------------------------------------------
void VM::say(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  logRing[logWriteIdx] = buf;
  logWriteIdx = (logWriteIdx + 1u) % kLogRingSize;
  if (logCount < kLogRingSize) logCount++;

  if (host.onSay) {
    host.onSay(host.user, buf);
  } else {
    std::fprintf(stderr, "%s\n", buf);
  }
}

[[noreturn]] void VM::toss(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  throw TerseError{std::string(buf), false};
}

bool VM::readVfsFile(const char* path, std::string& out) const {
  if (host.vfsRead) {
    return host.vfsRead(host.user, path, out);
  }
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(sz));
  if (sz > 0) {
    size_t got = std::fread(&out[0], 1, static_cast<size_t>(sz), f);
    out.resize(got);
  }
  std::fclose(f);
  return true;
}

bool VM::writeVfsFile(const char* path, const char* data, size_t len, bool append) {
  if (host.vfsWrite) {
    return host.vfsWrite(host.user, path, data, len, append);
  }
  FILE* f = std::fopen(path, append ? "ab" : "wb");
  if (!f) return false;
  size_t wr = (len > 0) ? std::fwrite(data, 1, len, f) : 0;
  std::fclose(f);
  return wr == len;
}

bool VM::deleteVfsFile(const char* path) {
  if (host.vfsDelete) {
    return host.vfsDelete(host.user, path);
  }
  return std::remove(path) == 0;
}

bool VM::listVfsFiles(const char* dir, std::vector<std::pair<std::string, uint32_t>>& out) const {
  out.clear();
  if (host.vfsList) {
    return host.vfsList(host.user, dir, out);
  }
  const char* dpath = (dir && dir[0]) ? dir : ".";
  DIR* d = opendir(dpath);
  if (!d) return false;
  struct dirent* ent = nullptr;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    std::string full = std::string(dpath) + "/" + ent->d_name;
    struct stat st{};
    uint32_t sz = 0;
    if (stat(full.c_str(), &st) == 0) sz = static_cast<uint32_t>(st.st_size);
    out.emplace_back(ent->d_name, sz);
  }
  closedir(d);
  return true;
}

uint32_t VM::nowMillis() const {
  if (host.millisFn) return host.millisFn(host.user);
  return 0;
}

void VM::appendWorldEntry(const char* headerLine, const char* bodyText) {
  if (loadingWorld || worldPath.empty()) return;
  uint32_t ts = nowMillis() / 1000u;
  std::string rec = "( " + std::to_string(ts) + " " + headerLine + "\n";
  if (bodyText && bodyText[0]) {
    const char* p = bodyText;
    while (*p) {
      const char* nl = std::strchr(p, '\n');
      size_t l = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
      rec.push_back(' ');
      rec.append(p, l);
      rec.push_back('\n');
      if (!nl) break;
      p = nl + 1;
    }
  }
  rec += ") " + std::to_string(ts) + "\n";
  writeVfsFile(worldPath.c_str(), rec.data(), rec.size(), true);
}

// ---------------------------------------------------------------------------
// Class & Method Hierarchy Management
// ---------------------------------------------------------------------------
static uint16_t gGenerationCounter = 1;

static ObjCls* allocRawCls(VM& vm, uint16_t metaClsId, uint16_t thisClsId,
                           uint16_t superClsId, const char* name, bool isUsr) {
  auto* c = reinterpret_cast<ObjCls*>(vm.allocObj(metaClsId, OBJ_CLS, 0, sizeof(ObjCls)));
  Oop cOop = oopFromObj(&c->hdr);
  if (thisClsId >= kMaxClasses) vm.toss("Class table overflow (%s)", name);
  vm.classById[thisClsId] = cOop;
  if (thisClsId > vm.classCount) vm.classCount = thisClsId;

  c->clsId          = thisClsId;
  c->superClsId     = superClsId;
  c->generation     = gGenerationCounter++;
  c->isUsrCls       = isUsr;
  c->trace          = false;
  c->nameSym        = vm.internSym(name);
  c->nameStr        = vm.newStr(name);
  c->methDict       = vm.newDict(8);
  c->myVarsList     = vm.newList(2);
  c->subsList       = vm.newList(2);
  c->savedInstsDict = vm.newDict(4);
  for (int i = 0; i < kUsrLruSize; i++) c->lruCache[i] = kOopNil;

  // Inherit superclass instVars
  if (superClsId != 0) {
    ObjCls* sup = vm.clsById(superClsId);
    if (sup) {
      vm.listAppend(sup->subsList, c->nameStr);
      if (sup->allVarCount > 0) {
        c->allVarCount = sup->allVarCount;
        c->allVarSyms  = static_cast<uint32_t*>(vm.allocExternal(c->allVarCount * sizeof(uint32_t)));
        std::memcpy(c->allVarSyms, sup->allVarSyms, c->allVarCount * sizeof(uint32_t));
      }
    }
  }

  vm.dictPut(vm.globalsDict, makeSym(c->nameSym), cOop);
  return c;
}

Oop VM::defineClass(const char* name, uint16_t superClsId, bool isUsr,
                    uint16_t fixedClsId, uint16_t fixedMetaId) {
  uint32_t nameSym = internSym(name);
  Oop existing = dictGet(globalsDict, makeSym(nameSym));
  if (existing != kOopNil) {
    toss("Class or global <%s> already exists", name);
  }
  ObjCls* sup = clsById(superClsId);
  if (!sup) toss("Superclass id %u does not exist for <%s>", superClsId, name);

  std::string metaName = std::string(name) + "Cls";
  uint16_t metaId = fixedMetaId ? fixedMetaId : static_cast<uint16_t>(classCount + 1);
  uint16_t clsId  = fixedClsId  ? fixedClsId  : static_cast<uint16_t>(metaId + 1);

  uint16_t supMetaId = sup->hdr.clsId;
  allocRawCls(*this, CLS_METACLS, metaId, supMetaId, metaName.c_str(), false);
  ObjCls* newCls = allocRawCls(*this, metaId, clsId, superClsId, name, isUsr || sup->isUsrCls);

  if (!loadingWorld) {
    std::string supName = toStdString(oopFromObj(&sup->hdr));
    std::string hdr = "cls " + std::string(name) + " " + supName;
    appendWorldEntry(hdr.c_str(), nullptr);
  }
  return oopFromObj(&newCls->hdr);
}

static void recalcVarsBelow(VM& vm, ObjCls* c) {
  std::vector<uint32_t> merged;
  if (c->superClsId != 0) {
    ObjCls* sup = vm.clsById(c->superClsId);
    if (sup && sup->allVarCount > 0) {
      merged.assign(sup->allVarSyms, sup->allVarSyms + sup->allVarCount);
    }
  }
  if (isObj(c->myVarsList)) {
    auto* mv = reinterpret_cast<ObjList*>(objFromOop(c->myVarsList));
    for (uint32_t i = 0; i < mv->len; i++) {
      auto* s = reinterpret_cast<ObjStr*>(objFromOop(mv->items[i]));
      uint32_t sym = vm.internSym(s->chars, s->hdr.size);
      for (uint32_t prev : merged) {
        if (prev == sym) {
          vm.toss("Duplicate inst var <%s> on class <%s>", s->chars, vm.symCStr(c->nameSym));
        }
      }
      merged.push_back(sym);
    }
  }
  if (c->allVarSyms) {
    vm.freeExternal(c->allVarSyms, c->allVarCount * sizeof(uint32_t));
    c->allVarSyms = nullptr;
  }
  c->allVarCount = static_cast<uint16_t>(merged.size());
  if (c->allVarCount > 0) {
    c->allVarSyms = static_cast<uint32_t*>(vm.allocExternal(c->allVarCount * sizeof(uint32_t)));
    std::memcpy(c->allVarSyms, merged.data(), c->allVarCount * sizeof(uint32_t));
  }
  c->generation = gGenerationCounter++;

  // Recurse into direct subclasses
  if (isObj(c->subsList)) {
    auto* subs = reinterpret_cast<ObjList*>(objFromOop(c->subsList));
    for (uint32_t i = 0; i < subs->len; i++) {
      auto* subStr = reinterpret_cast<ObjStr*>(objFromOop(subs->items[i]));
      uint32_t subSym = vm.internSym(subStr->chars, subStr->hdr.size);
      Oop subOop = vm.dictGet(vm.globalsDict, makeSym(subSym));
      if (isObj(subOop) && objFromOop(subOop)->format == OBJ_CLS) {
        recalcVarsBelow(vm, reinterpret_cast<ObjCls*>(objFromOop(subOop)));
      }
    }
  }
}

void VM::defineVars(ObjCls* cls, const char* spaceSepVars, bool autoAccessors) {
  if (!cls) return;
  auto* mv = reinterpret_cast<ObjList*>(objFromOop(cls->myVarsList));
  mv->len = 0;

  std::vector<std::string> names;
  const char* p = spaceSepVars ? spaceSepVars : "";
  while (*p) {
    while (*p && std::isspace(static_cast<unsigned char>(*p))) p++;
    if (!*p) break;
    const char* start = p;
    while (*p && !std::isspace(static_cast<unsigned char>(*p))) p++;
    std::string vname(start, static_cast<size_t>(p - start));
    names.push_back(vname);
    listAppend(cls->myVarsList, newStr(vname.c_str(), vname.size()));
  }
  cls->myVarCount = static_cast<uint16_t>(names.size());
  recalcVarsBelow(*this, cls);

  // If defining class-side vars on `FooCls` (whose sole instance `Foo` already exists
  // as an `ObjCls`), store class-instance variables in `Foo`'s `savedInstsDict` or resize!
  // Wait: in TerseTalk, `XCls defVars: 'y'` accesses `y` when `me` is `X` (`ObjCls`)!
  // Let's check: how does `OP_GET_INST` work when `me` is an `ObjCls`?!
  // Look at `execute`: if `self` is `OBJ_CLS`, we can store class-instance vars in a small
  // `List` or `ObjSlots` — let's check `OP_GET_INST` / `OP_SET_INST` below so `XCls defVars: 'y'`
  // works 100% identically to `ExprTest.testClassVar`!!

  if (autoAccessors) {
    bool prevLoad = loadingWorld;
    loadingWorld = true;
    for (const auto& nm : names) {
      std::string setterSel  = nm + ":";
      std::string setterBody = nm + "=a. me";
      addUsrMeth(cls, nm.c_str(), "", nm.c_str(), nm.size(), false, false);
      addUsrMeth(cls, setterSel.c_str(), "", setterBody.c_str(), setterBody.size(), false, false);
    }
    loadingWorld = prevLoad;
  }

  if (!loadingWorld) {
    std::string cname = toStdString(oopFromObj(&cls->hdr));
    std::string hdr   = "vars " + cname;
    appendWorldEntry(hdr.c_str(), spaceSepVars);
  }
}

ObjMeth* VM::addPrim(uint16_t clsId, const char* name, const char* abbrev, PrimFn fn) {
  ObjCls* c = clsById(clsId);
  if (!c) return nullptr;
  auto* m = reinterpret_cast<ObjMeth*>(allocObj(CLS_PRIM_METH, OBJ_METH, 0, sizeof(ObjMeth)));
  m->onClsId   = clsId;
  m->isPrim    = true;
  m->ownsSource= false;
  m->trace     = false;
  m->nameSym   = internSym(name);
  m->abbrevSym = (abbrev && abbrev[0]) ? internSym(abbrev) : m->nameSym;
  m->prim      = fn;
  m->comp      = nullptr;
  m->source    = nullptr;
  m->sourceLen = 0;

  Oop mOop = oopFromObj(&m->hdr);
  TempRootGuard rg(*this, mOop);
  dictPut(c->methDict, makeSym(m->nameSym), mOop);
  if (m->abbrevSym != m->nameSym) {
    dictPut(c->methDict, makeSym(m->abbrevSym), mOop);
  }
  c->generation = gGenerationCounter++;
  return m;
}

ObjMeth* VM::addUsrMeth(ObjCls* cls, const char* name, const char* abbrev,
                        const char* src, size_t srcLen, bool borrowStaticSrc, bool compileNow) {
  if (!cls) return nullptr;
  uint32_t nSym = internSym(name);
  uint32_t aSym = (abbrev && abbrev[0]) ? internSym(abbrev) : nSym;

  CompiledCode* cc = nullptr;
  if (compileNow) {
    cc = compileMethod(*this, cls, nSym, src, srcLen);
  }

  auto* m = reinterpret_cast<ObjMeth*>(allocObj(CLS_USR_METH, OBJ_METH, 0, sizeof(ObjMeth)));
  m->onClsId   = cls->clsId;
  m->isPrim    = false;
  m->trace     = false;
  m->nameSym   = nSym;
  m->abbrevSym = aSym;
  m->prim      = nullptr;
  m->comp      = cc;
  m->sourceLen = static_cast<uint32_t>(srcLen);

  if (borrowStaticSrc) {
    m->source     = src;
    m->ownsSource = false;
  } else {
    char* copy = static_cast<char*>(allocExternal(srcLen + 1u));
    if (srcLen > 0 && src) std::memcpy(copy, src, srcLen);
    copy[srcLen] = '\0';
    m->source     = copy;
    m->ownsSource = true;
  }

  Oop mOop = oopFromObj(&m->hdr);
  TempRootGuard rg(*this, mOop);
  dictPut(cls->methDict, makeSym(nSym), mOop);
  if (aSym != nSym) {
    dictPut(cls->methDict, makeSym(aSym), mOop);
  }
  cls->generation = gGenerationCounter++;

  if (!loadingWorld) {
    std::string cname = toStdString(oopFromObj(&cls->hdr));
    std::string body(m->source, m->sourceLen);
    std::string hdr1 = "meth " + cname + " " + symCStr(nSym);
    appendWorldEntry(hdr1.c_str(), body.c_str());
    if (aSym != nSym) {
      std::string hdr2 = "meth " + cname + " " + symCStr(aSym);
      appendWorldEntry(hdr2.c_str(), body.c_str());
    }
  }
  return m;
}

ObjMeth* VM::lookupMeth(uint16_t startClsId, uint32_t selectorSym) const {
  Oop key = makeSym(selectorSym);
  uint16_t cid = startClsId;
  while (cid != 0) {
    ObjCls* c = clsById(cid);
    if (!c) break;
    Oop mOop = dictGet(c->methDict, key);
    if (isObj(mOop)) {
      return reinterpret_cast<ObjMeth*>(objFromOop(mOop));
    }
    cid = c->superClsId;
  }
  return nullptr;
}

// Helper to access instVar slots on either `ObjSlots` (`Usr`) OR `ObjCls` (for class vars on `*Cls`!)
static Oop getInstSlot(VM& vm, Oop self, uint16_t idx) {
  if (!isObj(self)) return kOopNil;
  ObjHeader* h = objFromOop(self);
  if (h->format == OBJ_SLOTS) {
    auto* s = reinterpret_cast<ObjSlots*>(h);
    return (idx < h->size) ? s->slots[idx] : kOopNil;
  }
  if (h->format == OBJ_CLS) {
    // Store class-instance variables in `savedInstsDict` keyed by `makeInt30(-1 - idx)`!
    auto* c = reinterpret_cast<ObjCls*>(h);
    return vm.dictGet(c->savedInstsDict, makeInt30(-1 - static_cast<int32_t>(idx)));
  }
  return kOopNil;
}

static void setInstSlot(VM& vm, Oop self, uint16_t idx, Oop val) {
  if (!isObj(self)) return;
  ObjHeader* h = objFromOop(self);
  if (h->format == OBJ_SLOTS) {
    auto* s = reinterpret_cast<ObjSlots*>(h);
    if (idx < h->size) s->slots[idx] = val;
    return;
  }
  if (h->format == OBJ_CLS) {
    auto* c = reinterpret_cast<ObjCls*>(h);
    vm.dictPut(c->savedInstsDict, makeInt30(-1 - static_cast<int32_t>(idx)), val);
  }
}

// ---------------------------------------------------------------------------
// Fast Iterative Bytecode Interpreter Loop
// ---------------------------------------------------------------------------
static Oop pushUsrFrameAndPrepare(VM& vm, ObjMeth* meth, Oop rcvr,
                                  const Oop* args, uint8_t numArgs,
                                  Oop existingHeapFrame = kOopNil) {
  if (!meth->comp) {
    ObjCls* onCls = vm.clsById(meth->onClsId);
    meth->comp = compileMethod(vm, onCls, meth->nameSym, meth->source, meth->sourceLen);
  }
  CompiledCode* comp = meth->comp;
  if (vm.frameCount >= kMaxCallDepth) {
    vm.toss("Call stack overflow (depth %u) calling <%s>", vm.frameCount, vm.symCStr(meth->nameSym));
  }

  uint32_t stackBase = vm.sp;
  uint32_t exprBase  = vm.sp;
  Oop      hFrame    = existingHeapFrame;

  if (hFrame != kOopNil) {
    // Executing a Block inside its captured home ObjFrame
    exprBase = vm.sp;
  } else if (comp->capturesFrame) {
    // Allocate heap ObjFrame so nested Blks can capture locals safely
    TempRootGuard rg(vm, rcvr);
    for (uint8_t i = 0; i < numArgs; i++) rg.push(args[i]);
    uint16_t nLoc = comp->numLocals;
    size_t fBytes = offsetof(ObjFrame, locals) + (nLoc > 0 ? nLoc : 1u) * sizeof(Oop);
    auto* f = reinterpret_cast<ObjFrame*>(vm.allocObj(CLS_FRAME, OBJ_FRAME, nLoc, fBytes));
    f->prevFrame = (vm.frameCount > 0) ? vm.frames[vm.frameCount - 1].heapFrame : kOopNil;
    f->self      = rcvr;
    f->methObj   = oopFromObj(&meth->hdr);
    f->level     = vm.frameCount;
    for (uint16_t i = 0; i < nLoc; i++) {
      f->locals[i] = (i < numArgs) ? args[i] : kOopNil;
    }
    hFrame = oopFromObj(&f->hdr);
  } else {
    // Zero-allocation stack locals!
    uint16_t nLoc = comp->numLocals;
    if (vm.sp + nLoc + 32u >= kValueStackSize) {
      vm.toss("Value stack overflow");
    }
    for (uint16_t i = 0; i < nLoc; i++) {
      vm.stack[vm.sp + i] = (i < numArgs) ? args[i] : kOopNil;
    }
    vm.sp += nLoc;
    exprBase = vm.sp;
  }

  CallFrame& cf = vm.frames[vm.frameCount++];
  cf.meth      = meth;
  cf.comp      = comp;
  cf.pc        = comp->code;
  cf.self      = rcvr;
  cf.heapFrame = hFrame;
  cf.stackBase = stackBase;
  cf.exprBase  = exprBase;
  return kOopNull;
}

static Oop runInterpreterFrom(VM& vm, uint16_t stopDepth) {
  TryHandler tryStack[kMaxTryDepth];
  uint16_t   tryCount = 0;

  CallFrame*      cf        = nullptr;
  CompiledCode*   comp      = nullptr;
  const uint32_t* pc        = nullptr;
  Oop             self      = kOopNil;
  Oop*            localsPtr = nullptr;

load_frame:
  cf   = &vm.frames[vm.frameCount - 1];
  comp = cf->comp;
  pc   = cf->pc;
  self = cf->self;
  if (cf->heapFrame != kOopNil) {
    localsPtr = reinterpret_cast<ObjFrame*>(objFromOop(cf->heapFrame))->locals;
  } else {
    localsPtr = &vm.stack[cf->stackBase];
  }

  while (true) {
    try {
      while (true) {
        if (--vm.ticksRemaining <= 0) {
          throw TerseError{"TooManyTicks: execution limit exceeded", true};
        }
        if (--vm.pollCountdown == 0) {
          vm.pollCountdown = 1024;
          if (vm.interrupted || (vm.host.pollInterrupt && vm.host.pollInterrupt(vm.host.user))) {
            vm.interrupted = false;
            throw TerseError{"Interrupted", true};
          }
        }

        uint32_t ins = *pc++;
        Op       op  = insOp(ins);

        switch (op) {
          case OP_NOP:
            break;

          case OP_PUSH_NIL:
            vm.stack[vm.sp++] = kOopNil;
            break;

          case OP_PUSH_ME:
            vm.stack[vm.sp++] = self;
            break;

          case OP_PUSH_UP:
            vm.stack[vm.sp++] = kOopSuper;
            break;

          case OP_PUSH_FRAME:
            vm.stack[vm.sp++] = cf->heapFrame;
            break;

          case OP_PUSH_INT16:
            vm.stack[vm.sp++] = makeInt30(insSBC(ins));
            break;

          case OP_PUSH_CONST:
            vm.stack[vm.sp++] = comp->consts[insBC(ins)];
            break;

          case OP_POP:
            vm.sp--;
            break;

          case OP_DUP:
            vm.stack[vm.sp] = vm.stack[vm.sp - 1];
            vm.sp++;
            break;

          case OP_GET_LOCAL:
            vm.stack[vm.sp++] = localsPtr[insBC(ins)];
            break;

          case OP_SET_LOCAL:
            localsPtr[insBC(ins)] = vm.stack[vm.sp - 1];
            break;

          case OP_GET_INST:
            vm.stack[vm.sp++] = getInstSlot(vm, self, insBC(ins));
            break;

          case OP_SET_INST:
            setInstSlot(vm, self, insBC(ins), vm.stack[vm.sp - 1]);
            break;

          case OP_GET_GLOBAL: {
            uint32_t sym = insBC(ins);
            Oop val = vm.dictGet(vm.globalsDict, makeSym(sym));
            if (val == kOopNil) {
              cf->pc = pc;
              vm.toss("Global <%s> is not defined", vm.symCStr(sym));
            }
            vm.stack[vm.sp++] = val;
            break;
          }

          case OP_UNPACK_LIST: {
            uint16_t need = insBC(ins);
            Oop top = vm.stack[vm.sp - 1];
            if (!isObj(top) || objFromOop(top)->format != OBJ_LIST) {
              cf->pc = pc;
              vm.toss("Destructuring expected List, got <%s>", vm.toStdString(top).c_str());
            }
            auto* l = reinterpret_cast<ObjList*>(objFromOop(top));
            if (l->len < need) {
              cf->pc = pc;
              vm.toss("Destructuring needed %u items, got %u", need, l->len);
            }
            // Push items[need-1 .. 0] so items[0] is on top, keeping `top` underneath!
            for (int i = static_cast<int>(need) - 1; i >= 0; i--) {
              vm.stack[vm.sp++] = l->items[i];
            }
            break;
          }

          case OP_MAKE_LIST: {
            uint16_t n = insBC(ins);
            cf->pc = pc;
            Oop listOop = vm.newListFrom(&vm.stack[vm.sp - n], n);
            vm.sp -= n;
            vm.stack[vm.sp++] = listOop;
            break;
          }

          case OP_MAKE_DICT: {
            uint16_t n = insBC(ins);
            cf->pc = pc;
            Oop dictOop = vm.newDict(n * 2u + 4u);
            TempRootGuard rg(vm, dictOop);
            Oop* base = &vm.stack[vm.sp - n];
            if (n == 2 && (!isObj(base[0]) || objFromOop(base[0])->format != OBJ_LIST)) {
              // Single `DICT(k, v)` pair without trailing `;`!
              vm.dictPut(dictOop, base[0], base[1]);
            } else {
              for (uint16_t i = 0; i < n; i++) {
                Oop pair = base[i];
                if (!isObj(pair) || objFromOop(pair)->format != OBJ_LIST) {
                  vm.toss("DICT(...) expected 2-element List pair, got <%s>", vm.toStdString(pair).c_str());
                }
                auto* pl = reinterpret_cast<ObjList*>(objFromOop(pair));
                if (pl->len != 2) {
                  vm.toss("DICT(...) pair must have length 2, got %u", pl->len);
                }
                vm.dictPut(dictOop, pl->items[0], pl->items[1]);
              }
            }
            vm.sp -= n;
            vm.stack[vm.sp++] = dictOop;
            break;
          }

          case OP_MAKE_BLK: {
            cf->pc = pc;
            Oop bm = comp->consts[insBC(ins)];
            auto* blk = reinterpret_cast<ObjBlk*>(vm.allocObj(CLS_BLK, OBJ_BLK, 0, sizeof(ObjBlk)));
            blk->blockMeth = bm;
            blk->homeFrame = cf->heapFrame;
            vm.stack[vm.sp++] = oopFromObj(&blk->hdr);
            break;
          }

          case OP_STR_JAM: {
            uint16_t n = insBC(ins);
            if (n == 1 && isObj(vm.stack[vm.sp - 1]) && objFromOop(vm.stack[vm.sp - 1])->format == OBJ_STR) {
              break;  // Already a Str on top of stack!
            }
            cf->pc = pc;
            std::string acc;
            for (uint16_t i = 0; i < n; i++) {
              acc += vm.toStdString(vm.stack[vm.sp - n + i]);
            }
            Oop s = vm.newStr(acc.data(), acc.size());
            vm.sp -= n;
            vm.stack[vm.sp++] = s;
            break;
          }

          // -----------------------------------------------------------------
          // Fast-Path Arithmetic & Comparisons (1-cycle Int30 & HW-FPU Float)
          // -----------------------------------------------------------------
          case OP_ADD: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isInt(a) && isInt(b)) {
              int64_t r = static_cast<int64_t>(asInt30(a)) + static_cast<int64_t>(asInt30(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeInt64OrFloat(r);
              break;
            }
            if (isNum(a) && isNum(b)) {
              cf->pc = pc;
              Oop r = vm.makeFloatChecked(toFloatUnchecked(a) + toFloatUnchecked(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = r;
              break;
            }
            goto generic_send;
          }

          case OP_SUB: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isInt(a) && isInt(b)) {
              int64_t r = static_cast<int64_t>(asInt30(a)) - static_cast<int64_t>(asInt30(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeInt64OrFloat(r);
              break;
            }
            if (isNum(a) && isNum(b)) {
              cf->pc = pc;
              Oop r = vm.makeFloatChecked(toFloatUnchecked(a) - toFloatUnchecked(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = r;
              break;
            }
            goto generic_send;
          }

          case OP_MUL: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isInt(a) && isInt(b)) {
              int64_t r = static_cast<int64_t>(asInt30(a)) * static_cast<int64_t>(asInt30(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeInt64OrFloat(r);
              break;
            }
            if (isNum(a) && isNum(b)) {
              cf->pc = pc;
              Oop r = vm.makeFloatChecked(toFloatUnchecked(a) * toFloatUnchecked(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = r;
              break;
            }
            goto generic_send;
          }

          case OP_DIV: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isNum(a) && isNum(b)) {
              cf->pc = pc;
              float fa = toFloatUnchecked(a);
              float fb = toFloatUnchecked(b);
              Oop r = vm.makeFloatChecked(fa / fb);
              vm.sp--;
              vm.stack[vm.sp - 1] = r;
              break;
            }
            goto generic_send;
          }

          case OP_MOD: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isInt(a) && isInt(b)) {
              int32_t ib = asInt30(b);
              if (ib == 0) { cf->pc = pc; vm.toss("Modulo by zero"); }
              int32_t r = asInt30(a) % ib;
              if (r < 0) r += (ib > 0 ? ib : -ib);
              vm.sp--;
              vm.stack[vm.sp - 1] = makeInt30(r);
              break;
            }
            if (isNum(a) && isNum(b)) {
              cf->pc = pc;
              float fa = toFloatUnchecked(a);
              float fb = toFloatUnchecked(b);
              float r  = std::fmod(fa, fb);
              if (r < 0.0f) r += fb;
              Oop ro = vm.makeFloatChecked(r);
              vm.sp--;
              vm.stack[vm.sp - 1] = ro;
              break;
            }
            goto generic_send;
          }

          case OP_BIT_AND:
          case OP_BIT_OR:
          case OP_BIT_XOR: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isNum(a) && isNum(b)) {
              int32_t ia = isInt(a) ? asInt30(a) : static_cast<int32_t>(asFloat(a));
              int32_t ib = isInt(b) ? asInt30(b) : static_cast<int32_t>(asFloat(b));
              int32_t r  = (op == OP_BIT_AND) ? (ia & ib) : (op == OP_BIT_OR ? (ia | ib) : (ia ^ ib));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeInt64OrFloat(r);
              break;
            }
            goto generic_send;
          }

          case OP_EQ:
          case OP_NE: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isNum(a) && isNum(b)) {
              bool eq = (isInt(a) && isInt(b)) ? (a == b) : (toFloatUnchecked(a) == toFloatUnchecked(b));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeBool(op == OP_EQ ? eq : !eq);
              break;
            }
            goto generic_send;
          }

          case OP_LT:
          case OP_LE:
          case OP_GT:
          case OP_GE: {
            Oop b = vm.stack[vm.sp - 1];
            Oop a = vm.stack[vm.sp - 2];
            if (isInt(a) && isInt(b)) {
              int32_t ia = asInt30(a), ib = asInt30(b);
              bool r = (op == OP_LT) ? (ia < ib) : (op == OP_LE ? (ia <= ib) : (op == OP_GT ? (ia > ib) : (ia >= ib)));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeBool(r);
              break;
            }
            if (isNum(a) && isNum(b)) {
              float fa = toFloatUnchecked(a), fb = toFloatUnchecked(b);
              bool r = (op == OP_LT) ? (fa < fb) : (op == OP_LE ? (fa <= fb) : (op == OP_GT ? (fa > fb) : (fa >= fb)));
              vm.sp--;
              vm.stack[vm.sp - 1] = makeBool(r);
              break;
            }
            goto generic_send;
          }

          case OP_SEND:
          generic_send: {
            uint8_t   nArgs = insA(ins);
            SendSite& ss    = comp->sendSites[insBC(ins)];
            Oop       rcvr  = vm.stack[vm.sp - nArgs - 1];
            ObjMeth*  targetMeth = nullptr;

            if (rcvr == kOopSuper) {
              // Superclass lookup starting above the lexical owner class of current method!
              ObjCls* lexicalCls = vm.clsById(comp->onClsId);
              uint16_t supId = lexicalCls ? lexicalCls->superClsId : 0;
              targetMeth = vm.lookupMeth(supId, ss.selectorSym);
              rcvr = self;
              vm.stack[vm.sp - nArgs - 1] = self;
            } else {
              uint16_t rClsId = vm.oopClsId(rcvr);
              ObjCls*  rCls   = vm.clsById(rClsId);
              if (rCls && ss.cachedClsId == rClsId && ss.cachedGen == rCls->generation && ss.cachedMeth) {
                targetMeth = ss.cachedMeth;
              } else {
                targetMeth = vm.lookupMeth(rClsId, ss.selectorSym);
                if (targetMeth && rCls) {
                  ss.cachedClsId = rClsId;
                  ss.cachedGen   = rCls->generation;
                  ss.cachedMeth  = targetMeth;
                }
              }
            }

            if (!targetMeth) {
              cf->pc = pc;
              ObjCls* rCls = vm.oopCls(rcvr);
              std::string cname = rCls ? vm.toStdString(oopFromObj(&rCls->hdr)) : "?";
              vm.toss("MessageNotUnderstood: <%s> on <%s> (%s)",
                      vm.symCStr(ss.selectorSym), cname.c_str(), vm.toStdString(rcvr).c_str());
            }

            cf->pc = pc;
            const Oop* argPtr = &vm.stack[vm.sp - nArgs];
            if (targetMeth->isPrim) {
              Oop res = targetMeth->prim(vm, rcvr, argPtr, nArgs);
              vm.sp -= (nArgs + 1u);
              vm.stack[vm.sp++] = res;
              // Refresh pointers in case primitive modified stack frames? No, frameCount unchanged!
            } else {
              // Copy args out before popping receiver+args from caller's expression stack
              Oop argBuf[16];
              if (nArgs > 16) vm.toss("Too many method arguments (%u)", nArgs);
              for (uint8_t i = 0; i < nArgs; i++) argBuf[i] = argPtr[i];
              vm.sp -= (nArgs + 1u);
              pushUsrFrameAndPrepare(vm, targetMeth, rcvr, argBuf, nArgs);
              goto load_frame;
            }
            break;
          }

          case OP_SUPER_SEND:
            break;

          // -----------------------------------------------------------------
          // Control Flow & Zero-Allocation FOR Loops
          // -----------------------------------------------------------------
          case OP_JMP:
            pc += insSBC(ins);
            break;

          case OP_JMP_FALSE: {
            Oop cond = vm.stack[--vm.sp];
            if (!vm.truth(cond)) pc += insSBC(ins);
            break;
          }

          case OP_JMP_TRUE: {
            Oop cond = vm.stack[--vm.sp];
            if (vm.truth(cond)) pc += insSBC(ins);
            break;
          }

          case OP_JMP_FALSE_PEEK:
            if (!vm.truth(vm.stack[vm.sp - 1])) pc += insSBC(ins);
            break;

          case OP_JMP_TRUE_PEEK:
            if (vm.truth(vm.stack[vm.sp - 1])) pc += insSBC(ins);
            break;

          case OP_JMP_NOT_NIL_PEEK:
            if (vm.stack[vm.sp - 1] != kOopNil) pc += insSBC(ins);
            break;

          case OP_CASE_MATCH: {
            Oop key    = vm.stack[--vm.sp];
            Oop target = vm.stack[vm.sp - 1];
            if (vm.oopEquals(target, key)) {
              vm.sp--;  // pop target too!
              pc += insSBC(ins);
            }
            break;
          }

          case OP_FOR_PREP: {
            uint8_t tBase = insA(ins);
            Oop coll = vm.stack[--vm.sp];
            if (isNum(coll)) {
              int32_t n = toNearestIntUnchecked(coll);
              if (n < 0) n = 0;
              localsPtr[tBase + 0] = kOopNil;
              localsPtr[tBase + 1] = makeInt30(n);
              localsPtr[tBase + 2] = makeInt30(0);
            } else if (isObj(coll)) {
              ObjHeader* h = objFromOop(coll);
              if (h->format == OBJ_LIST) {
                auto* l = reinterpret_cast<ObjList*>(h);
                localsPtr[tBase + 0] = coll;
                localsPtr[tBase + 1] = makeInt30(static_cast<int32_t>(l->len));
                localsPtr[tBase + 2] = makeInt30(0);
              } else if (h->format == OBJ_DICT) {
                cf->pc = pc;
                std::vector<Oop> keys;
                vm.dictSortedKeys(coll, keys);
                Oop pairs = vm.newList(static_cast<uint32_t>(keys.size()));
                TempRootGuard rg(vm, pairs);
                for (Oop k : keys) {
                  Oop kv[2] = {k, vm.dictGet(coll, k)};
                  vm.listAppend(pairs, vm.newListFrom(kv, 2));
                }
                localsPtr[tBase + 0] = pairs;
                // Tag length with makeSym so OP_FOR_STEP knows items are Dict [k,v] pairs!
                localsPtr[tBase + 1] = makeSym(static_cast<uint32_t>(keys.size()));
                localsPtr[tBase + 2] = makeInt30(0);
              } else if (h->format == OBJ_STR) {
                localsPtr[tBase + 0] = coll;
                localsPtr[tBase + 1] = makeInt30(static_cast<int32_t>(h->size));
                localsPtr[tBase + 2] = makeInt30(0);
              } else {
                cf->pc = pc;
                vm.toss("FOR(...) requires Num, List, Dict, or Str, got <%s>", vm.toStdString(coll).c_str());
              }
            } else {
              cf->pc = pc;
              vm.toss("FOR(...) requires Num, List, Dict, or Str, got <%s>", vm.toStdString(coll).c_str());
            }
            break;
          }

          case OP_FOR_STEP_0:
          case OP_FOR_STEP_1:
          case OP_FOR_STEP_2: {
            uint8_t tBase  = insA(ins);
            Oop     coll   = localsPtr[tBase + 0];
            Oop     lenOop = localsPtr[tBase + 1];
            int32_t idx    = asInt30(localsPtr[tBase + 2]);
            bool    isDict = isSym(lenOop);
            int32_t len    = isDict ? static_cast<int32_t>(asSym(lenOop)) : asInt30(lenOop);

            if (idx >= len) {
              pc += insSBC(ins);
              break;
            }
            localsPtr[tBase + 2] = makeInt30(idx + 1);

            if (op == OP_FOR_STEP_0) break;

            Oop kVal = makeInt30(idx);
            Oop vVal = kVal;
            if (coll != kOopNil) {
              ObjHeader* h = objFromOop(coll);
              if (isDict) {
                auto* pl   = reinterpret_cast<ObjList*>(h);
                auto* pair = reinterpret_cast<ObjList*>(objFromOop(pl->items[idx]));
                kVal = pair->items[0];
                vVal = pair->items[1];
              } else if (h->format == OBJ_LIST) {
                auto* l = reinterpret_cast<ObjList*>(h);
                vVal = (static_cast<uint32_t>(idx) < l->len) ? l->items[idx] : kOopNil;
              } else if (h->format == OBJ_STR) {
                cf->pc = pc;
                auto* s = reinterpret_cast<ObjStr*>(h);
                vVal = vm.newStr(s->chars + idx, 1);
              }
            }

            if (op == OP_FOR_STEP_1) {
              vm.stack[vm.sp++] = vVal;
            } else {
              vm.stack[vm.sp++] = kVal;
              vm.stack[vm.sp++] = vVal;
            }
            break;
          }

          case OP_TRY_PUSH: {
            if (tryCount >= kMaxTryDepth) {
              cf->pc = pc;
              vm.toss("TRY nesting overflow");
            }
            TryHandler& th = tryStack[tryCount++];
            th.callDepth   = vm.frameCount;
            th.stackTop    = vm.sp;
            th.catchPc     = pc + insSBC(ins);
            break;
          }

          case OP_TRY_POP:
            if (tryCount > 0) tryCount--;
            break;

          case OP_RET: {
            Oop retVal = vm.stack[--vm.sp];
            vm.sp = cf->stackBase;  // unwinds uncaptured locals + expr temps!
            vm.frameCount--;
            if (vm.frameCount == stopDepth) {
              return retVal;
            }
            vm.stack[vm.sp++] = retVal;
            goto load_frame;
          }

          default:
            cf->pc = pc;
            vm.toss("Unknown bytecode opcode %u", static_cast<unsigned>(op));
        }
      }
    } catch (const TerseError& err) {
      if (!err.isInterrupt && tryCount > 0) {
        TryHandler th = tryStack[--tryCount];
        vm.frameCount = th.callDepth;
        vm.sp         = th.stackTop;
        Oop errStr    = vm.newStr(err.message.c_str(), err.message.size());
        vm.stack[vm.sp++] = errStr;
        vm.frames[vm.frameCount - 1].pc = th.catchPc;
        goto load_frame;
      }
      // Unwind frames belonging to this runInterpreterFrom invocation and rethrow
      while (vm.frameCount > stopDepth) {
        vm.sp = vm.frames[vm.frameCount - 1].stackBase;
        vm.frameCount--;
      }
      throw;
    }
  }
}

Oop VM::invokeMeth(ObjMeth* meth, Oop rcvr, const Oop* args, uint8_t numArgs) {
  if (meth->isPrim) {
    return meth->prim(*this, rcvr, args, numArgs);
  }
  uint16_t savedDepth = frameCount;
  pushUsrFrameAndPrepare(*this, meth, rcvr, args, numArgs);
  return runInterpreterFrom(*this, savedDepth);
}

Oop VM::send(Oop rcvr, uint32_t selectorSym, const Oop* args, uint8_t numArgs) {
  uint16_t cid = oopClsId(rcvr);
  ObjMeth* m   = lookupMeth(cid, selectorSym);
  if (!m) {
    ObjCls* c = oopCls(rcvr);
    std::string cn = c ? toStdString(oopFromObj(&c->hdr)) : "?";
    toss("MessageNotUnderstood: <%s> on <%s>", symCStr(selectorSym), cn.c_str());
  }
  return invokeMeth(m, rcvr, args, numArgs);
}

static void storeAtSingleBlkParam(VM& vm, ObjFrame* hf, const BlkParam& bp, Oop val) {
  if (!bp.isTuple) {
    if (bp.vars[0].isInst) {
      setInstSlot(vm, hf->self, bp.vars[0].index, val);
    } else if (bp.vars[0].index < hf->hdr.size) {
      hf->locals[bp.vars[0].index] = val;
    }
  } else {
    if (!isObj(val) || objFromOop(val)->format != OBJ_LIST) {
      vm.toss("Block tuple param expected List, got <%s>", vm.toStdString(val).c_str());
    }
    auto* l = reinterpret_cast<ObjList*>(objFromOop(val));
    if (l->len < bp.varCount) {
      vm.toss("Block tuple param needed %u items, got %u", bp.varCount, l->len);
    }
    for (uint8_t i = 0; i < bp.varCount; i++) {
      if (bp.vars[i].isInst) {
        setInstSlot(vm, hf->self, bp.vars[i].index, l->items[i]);
      } else if (bp.vars[i].index < hf->hdr.size) {
        hf->locals[bp.vars[i].index] = l->items[i];
      }
    }
  }
}

void VM::blkStoreKV(Oop blkOop, Oop k, Oop v) {
  if (!isObj(blkOop) || objFromOop(blkOop)->format != OBJ_BLK) return;
  auto* b  = reinterpret_cast<ObjBlk*>(objFromOop(blkOop));
  auto* bm = reinterpret_cast<ObjMeth*>(objFromOop(b->blockMeth));
  auto* hf = reinterpret_cast<ObjFrame*>(objFromOop(b->homeFrame));
  if (!bm || !bm->comp || !hf) return;
  if (bm->comp->blkParamCount == 1) {
    storeAtSingleBlkParam(*this, hf, bm->comp->blkParams[0], v);
  } else if (bm->comp->blkParamCount >= 2) {
    storeAtSingleBlkParam(*this, hf, bm->comp->blkParams[0], k);
    storeAtSingleBlkParam(*this, hf, bm->comp->blkParams[1], v);
  }
}

Oop VM::evalBlk(Oop blkOop, const Oop* args, uint8_t numArgs) {
  if (!isObj(blkOop) || objFromOop(blkOop)->format != OBJ_BLK) {
    toss("Expected Blk, got <%s>", toStdString(blkOop).c_str());
  }
  auto* b  = reinterpret_cast<ObjBlk*>(objFromOop(blkOop));
  auto* bm = reinterpret_cast<ObjMeth*>(objFromOop(b->blockMeth));
  auto* hf = reinterpret_cast<ObjFrame*>(objFromOop(b->homeFrame));
  if (!bm || !bm->comp || !hf) toss("Invalid Blk closure");

  if (numArgs > bm->comp->blkParamCount) {
    toss("Block takes %u params, called with %u args", bm->comp->blkParamCount, numArgs);
  }
  for (uint8_t i = 0; i < numArgs; i++) {
    storeAtSingleBlkParam(*this, hf, bm->comp->blkParams[i], args[i]);
  }

  uint16_t savedDepth = frameCount;
  pushUsrFrameAndPrepare(*this, bm, hf->self, nullptr, 0, b->homeFrame);
  return runInterpreterFrom(*this, savedDepth);
}

Oop VM::evalCode(Oop rcvr, const char* code, const Oop* args, uint8_t numArgs) {
  TempRootGuard rg(*this, rcvr);
  for (uint8_t i = 0; i < numArgs; i++) rg.push(args[i]);

  std::string evalSel = "__eval__";
  for (uint8_t i = 0; i < numArgs; i++) evalSel += ":x";
  if (numArgs > 0) {
    evalSel.clear();
    for (uint8_t i = 0; i < numArgs; i++) evalSel += "eval:";
  }

  ObjCls* cls = oopCls(rcvr);
  uint32_t selSym = internSym(evalSel.c_str(), evalSel.size());
  size_t len = code ? std::strlen(code) : 0;

  CompiledCode* cc = compileMethod(*this, cls, selSym, code ? code : "", len);
  auto* m = reinterpret_cast<ObjMeth*>(allocObj(CLS_USR_METH, OBJ_METH, 0, sizeof(ObjMeth)));
  m->onClsId    = cls ? cls->clsId : static_cast<uint16_t>(CLS_TMP);
  m->isPrim     = false;
  m->ownsSource = false;
  m->nameSym    = selSym;
  m->abbrevSym  = selSym;
  m->comp       = cc;
  m->source     = nullptr;
  m->sourceLen  = 0;

  rg.push(oopFromObj(&m->hdr));
  return invokeMeth(m, rcvr, args, numArgs);
}

// ---------------------------------------------------------------------------
// VM Bootstrap & Destruction
// ---------------------------------------------------------------------------
bool VM::init(uint32_t arenaBytes, bool loadBuiltinPrelude) {
  destroy();
  if (!heap.init(arenaBytes)) return false;

  gcInhibit  = true;
  loadingWorld = true;
  sp         = 0;
  frameCount = 0;
  symCount   = 0;
  classCount = 0;

  // Pre-intern common binary & core selectors
  symAdd    = internSym("+");
  symSub    = internSym("-");
  symMul    = internSym("*");
  symDiv    = internSym("/");
  symMod    = internSym("%");
  symBitAnd = internSym("&");
  symBitOr  = internSym("|");
  symBitXor = internSym("^");
  symEq     = internSym("==");
  symNe     = internSym("!=");
  symLt     = internSym("<");
  symLe     = internSym("<=");
  symGt     = internSym(">");
  symGe     = internSym(">=");
  symValue  = internSym("value");
  symValue1 = internSym("value:");
  symValue2 = internSym("value:value:");
  symJam    = internSym("jam");
  symStr    = internSym("str");
  symEquals = internSym("equals:");

  globalsDict = newDict(128);

  // Bootstrap the parallel Class / Metaclass cycle (matching Terp.java!)
  ObjCls* cUrCls      = allocRawCls(*this, CLS_METACLS, CLS_UR_CLS,      0,          "UrCls",      false);
  ObjCls* cUr         = allocRawCls(*this, CLS_UR_CLS,  CLS_UR,          0,          "Ur",         false);
  ObjCls* cObjCls     = allocRawCls(*this, CLS_METACLS, CLS_OBJ_CLS,     CLS_UR_CLS, "ObjCls",     false);
  ObjCls* cObj        = allocRawCls(*this, CLS_OBJ_CLS, CLS_OBJ,         CLS_UR,     "Obj",        false);
  ObjCls* cClsCls     = allocRawCls(*this, CLS_METACLS, CLS_CLS_CLS,     CLS_OBJ_CLS,"ClsCls",     false);
  ObjCls* cCls        = allocRawCls(*this, CLS_CLS_CLS, CLS_CLS,         CLS_OBJ,    "Cls",        false);
  ObjCls* cMetaclsCls = allocRawCls(*this, CLS_METACLS, CLS_METACLS_CLS, CLS_OBJ_CLS,"MetaclsCls", false);
  ObjCls* cMetacls    = allocRawCls(*this, CLS_METACLS_CLS, CLS_METACLS, CLS_CLS,    "Metacls",    false);

  (void)cUr; (void)cObj; (void)cObjCls; (void)cClsCls; (void)cMetaclsCls; (void)cMetacls;
  // Patch UrCls.superCls = Cls (so all *Cls inherit Cls instance methods!)
  cUrCls->superClsId = CLS_CLS;
  listAppend(cCls->subsList, cUrCls->nameStr);

  auto bootPair = [&](uint16_t clsId, uint16_t metaId, const char* name,
                      uint16_t supId, uint16_t supMetaId, bool isUsr) {
    std::string mname = std::string(name) + "Cls";
    allocRawCls(*this, CLS_METACLS, metaId, supMetaId, mname.c_str(), false);
    allocRawCls(*this, metaId, clsId, supId, name, isUsr);
  };

  bootPair(CLS_SUPER,     CLS_SUPER_CLS,     "Super",     CLS_UR,   CLS_UR_CLS,   false);
  bootPair(CLS_UNDEFINED, CLS_UNDEFINED_CLS, "Undefined", CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_NUM,       CLS_NUM_CLS,       "Num",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_INT,       CLS_INT_CLS,       "Int",       CLS_NUM,  CLS_NUM_CLS,  false);
  bootPair(CLS_FLOAT,     CLS_FLOAT_CLS,     "Float",     CLS_NUM,  CLS_NUM_CLS,  false);
  bootPair(CLS_CHAR,      CLS_CHAR_CLS,      "Char",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_STR,       CLS_STR_CLS,       "Str",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_BUF,       CLS_BUF_CLS,       "Buf",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_BYTES,     CLS_BYTES_CLS,     "Bytes",     CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_REX,       CLS_REX_CLS,       "Rex",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_LIST,      CLS_LIST_CLS,      "List",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_DICT,      CLS_DICT_CLS,      "Dict",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_BLK,       CLS_BLK_CLS,       "Blk",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_METH,      CLS_METH_CLS,      "Meth",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_PRIM_METH, CLS_PRIM_METH_CLS, "PrimMeth",  CLS_METH, CLS_METH_CLS, false);
  bootPair(CLS_USR_METH,  CLS_USR_METH_CLS,  "UsrMeth",   CLS_METH, CLS_METH_CLS, false);
  bootPair(CLS_FRAME,     CLS_FRAME_CLS,     "Frame",     CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_USR,       CLS_USR_CLS,       "Usr",       CLS_OBJ,  CLS_OBJ_CLS,  true);
  bootPair(CLS_TMP,       CLS_TMP_CLS,       "Tmp",       CLS_USR,  CLS_USR_CLS,  true);
  bootPair(CLS_ORPHAN,    CLS_ORPHAN_CLS,    "Orphan",    CLS_USR,  CLS_USR_CLS,  true);
  bootPair(CLS_SYS,       CLS_SYS_CLS,       "Sys",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_FILE,      CLS_FILE_CLS,      "File",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_JSON,      CLS_JSON_CLS,      "Json",      CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_HEX,       CLS_HEX_CLS,       "Hex",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_PEN,       CLS_PEN_CLS,       "Pen",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_BMP,       CLS_BMP_CLS,       "Bmp",       CLS_OBJ,  CLS_OBJ_CLS,  false);
  bootPair(CLS_WIN,       CLS_WIN_CLS,       "Win",       CLS_OBJ,  CLS_OBJ_CLS,  false);

  // Register compatibility aliases: `Vec` -> `List`, `VecCls` -> `ListCls`, `Ink` -> `Pen`, `JavMeth` -> `PrimMeth`
  dictPut(globalsDict, makeSym(internSym("vec")),     classById[CLS_LIST]);
  dictPut(globalsDict, makeSym(internSym("veccls")),  classById[CLS_LIST_CLS]);
  dictPut(globalsDict, makeSym(internSym("ink")),     classById[CLS_PEN]);
  dictPut(globalsDict, makeSym(internSym("inkcls")),  classById[CLS_PEN_CLS]);
  dictPut(globalsDict, makeSym(internSym("javmeth")), classById[CLS_PRIM_METH]);

  installCorePrimitives(*this);

  gcInhibit = false;

  if (loadBuiltinPrelude && kEmbeddedPreludeLen > 0) {
    std::string err;
    if (!loadWorldBuffer(*this, "pre0", kEmbeddedPrelude, kEmbeddedPreludeLen, true, &err)) {
      say("Prelude error: %s", err.c_str());
      loadingWorld = false;
      return false;
    }
  }
  loadingWorld = false;
  return true;
}

void VM::destroy() {
  if (heap.base) {
    // Sweep everything to release external buffers
    const uint32_t endHdrOff = heap.totalBytes - 4u;
    uint32_t off = 4u;
    while (off < endHdrOff) {
      uint32_t hdr  = *reinterpret_cast<uint32_t*>(heap.base + off);
      uint32_t span = hdr & ~7u;
      if (span == 0u) break;
      if ((hdr & 3u) == 3u) {
        auto* obj = reinterpret_cast<ObjHeader*>(heap.base + off + 4u);
        obj->gcFlags = 0;
      }
      off += span;
    }
    // Clear roots and runGC once so finalizeObj frees all external mallocs cleanly
    globalsDict = kOopNil;
    for (uint16_t i = 0; i <= classCount; i++) classById[i] = kOopNil;
    sp = 0;
    frameCount = 0;
    tempRootCount = 0;
    auto savedUiMark = host.markUiRoots;
    host.markUiRoots = nullptr;
    gcInhibit = false;
    runGC();
    host.markUiRoots = savedUiMark;
    heap.destroy();
  }
  for (uint16_t i = 1; i <= symCount; i++) {
    std::free(symChars[i]);
    symChars[i] = nullptr;
  }
  symCount   = 0;
  classCount = 0;
}

}  // namespace tt
