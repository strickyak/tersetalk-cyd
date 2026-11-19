#include "tt_vm.h"
#include "tt_compiler.h"
#include "tt_world.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace tt {

namespace {

// ---------------------------------------------------------------------------
// Argument Extraction Helpers
// ---------------------------------------------------------------------------
static const char* reqCStr(VM& vm, Oop v, const char* ctx, size_t* outLen = nullptr) {
  if (isObj(v) && objFromOop(v)->format == OBJ_STR) {
    auto* s = reinterpret_cast<ObjStr*>(objFromOop(v));
    if (outLen) *outLen = s->hdr.size;
    return s->chars;
  }
  if (isSym(v)) {
    const char* sc = vm.symCStr(asSym(v));
    if (outLen) *outLen = std::strlen(sc);
    return sc;
  }
  vm.toss("%s expected Str, got <%s>", ctx, vm.toStdString(v).c_str());
}

static std::string reqStdStr(VM& vm, Oop v, const char* ctx) {
  if (isObj(v) && objFromOop(v)->format == OBJ_STR) {
    auto* s = reinterpret_cast<ObjStr*>(objFromOop(v));
    return std::string(s->chars, s->hdr.size);
  }
  if (isChar(v)) {
    char c = static_cast<char>(asChar(v) & 0xFFu);
    return std::string(1, c);
  }
  if (isSym(v)) {
    return std::string(vm.symCStr(asSym(v)));
  }
  vm.toss("%s expected Str, got <%s>", ctx, vm.toStdString(v).c_str());
}

static int32_t reqInt(VM& vm, Oop v, const char* ctx) {
  if (isInt(v)) return asInt30(v);
  if (isFloat(v)) return static_cast<int32_t>(std::floor(asFloat(v) + 0.5f));
  vm.toss("%s expected Num, got <%s>", ctx, vm.toStdString(v).c_str());
}

static float reqFloat(VM& vm, Oop v, const char* ctx) {
  if (isInt(v)) return static_cast<float>(asInt30(v));
  if (isFloat(v)) return asFloat(v);
  vm.toss("%s expected Num, got <%s>", ctx, vm.toStdString(v).c_str());
}

static ObjList* reqList(VM& vm, Oop v, const char* ctx) {
  if (isObj(v) && objFromOop(v)->format == OBJ_LIST) {
    return reinterpret_cast<ObjList*>(objFromOop(v));
  }
  vm.toss("%s expected List, got <%s>", ctx, vm.toStdString(v).c_str());
}

static ObjDict* reqDict(VM& vm, Oop v, const char* ctx) {
  if (isObj(v) && objFromOop(v)->format == OBJ_DICT) {
    return reinterpret_cast<ObjDict*>(objFromOop(v));
  }
  vm.toss("%s expected Dict, got <%s>", ctx, vm.toStdString(v).c_str());
}

// Fast seeded xorshift32 PRNG for Num rand / Num rand:
static uint32_t gRngState = 0x9E3779B9u;
static uint32_t nextRandU32() {
  uint32_t x = gRngState;
  if (x == 0) x = 0x9E3779B9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  gRngState = x;
  return x;
}

// ---------------------------------------------------------------------------
// Compact Backtracking Regex Engine (saves ~120 KB Flash vs std::regex!)
// Supports: ^, $, ., |, (...), [...], [^...], a-z, *, +, ?, \d \D \w \W \s \S
// ---------------------------------------------------------------------------
struct RxGroup {
  int start = -1;
  int end   = -1;
};

struct RxCtx {
  const char* text;
  int         textLen;
  RxGroup     groups[10];
  int         numGroups = 0;
  int         steps     = 0;
};

static constexpr int kMaxRxSteps = 30000;

static int findGroupClose(const char* pat, int p, int pEnd) {
  int depth = 1;
  bool inClass = false;
  for (int i = p; i < pEnd; i++) {
    if (pat[i] == '\\' && i + 1 < pEnd) {
      i++;
      continue;
    }
    if (inClass) {
      if (pat[i] == ']') inClass = false;
      continue;
    }
    if (pat[i] == '[') inClass = true;
    else if (pat[i] == '(') depth++;
    else if (pat[i] == ')') {
      if (--depth == 0) return i;
    }
  }
  return -1;
}

static int findClassClose(const char* pat, int p, int pEnd) {
  if (p < pEnd && pat[p] == '^') p++;
  if (p < pEnd && pat[p] == ']') p++;  // literal ']' right at start
  for (int i = p; i < pEnd; i++) {
    if (pat[i] == '\\' && i + 1 < pEnd) {
      i++;
      continue;
    }
    if (pat[i] == ']') return i;
  }
  return -1;
}

static char decodeEscChar(char e) {
  switch (e) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    default:  return e;
  }
}

static bool matchEscPred(char e, unsigned char ch) {
  switch (e) {
    case 'd': return std::isdigit(ch) != 0;
    case 'D': return std::isdigit(ch) == 0;
    case 'w': return std::isalnum(ch) != 0 || ch == '_';
    case 'W': return std::isalnum(ch) == 0 && ch != '_';
    case 's': return std::isspace(ch) != 0;
    case 'S': return std::isspace(ch) == 0;
    default:  return ch == static_cast<unsigned char>(decodeEscChar(e));
  }
}

static bool matchCharClass(const char* pat, int cStart, int cEnd, unsigned char ch) {
  bool neg = false;
  int i = cStart;
  if (i < cEnd && pat[i] == '^') {
    neg = true;
    i++;
  }
  bool matched = false;
  while (i < cEnd) {
    unsigned char c1 = 0;
    bool isPred = false;
    if (pat[i] == '\\' && i + 1 < cEnd) {
      char esc = pat[i + 1];
      if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
        if (matchEscPred(esc, ch)) matched = true;
        isPred = true;
      } else {
        c1 = static_cast<unsigned char>(decodeEscChar(esc));
      }
      i += 2;
    } else {
      c1 = static_cast<unsigned char>(pat[i++]);
    }

    if (!isPred) {
      if (i + 1 < cEnd && pat[i] == '-' && pat[i + 1] != ']') {
        i++;  // skip '-'
        unsigned char c2 = 0;
        if (pat[i] == '\\' && i + 1 < cEnd) {
          c2 = static_cast<unsigned char>(decodeEscChar(pat[i + 1]));
          i += 2;
        } else {
          c2 = static_cast<unsigned char>(pat[i++]);
        }
        if (ch >= c1 && ch <= c2) matched = true;
      } else {
        if (ch == c1) matched = true;
      }
    }
  }
  return neg ? !matched : matched;
}

static int countGroups(const char* pat, int pLen) {
  int g = 0;
  bool inClass = false;
  for (int i = 0; i < pLen; i++) {
    if (pat[i] == '\\' && i + 1 < pLen) { i++; continue; }
    if (inClass) {
      if (pat[i] == ']') inClass = false;
    } else if (pat[i] == '[') {
      inClass = true;
    } else if (pat[i] == '(') {
      if (g < 9) g++;
    }
  }
  return g;
}

static int groupIndexAt(const char* pat, int targetOpenPos) {
  int g = 0;
  bool inClass = false;
  for (int i = 0; i <= targetOpenPos; i++) {
    if (pat[i] == '\\') { i++; continue; }
    if (inClass) {
      if (pat[i] == ']') inClass = false;
    } else if (pat[i] == '[') {
      inClass = true;
    } else if (pat[i] == '(') {
      g++;
      if (i == targetOpenPos) return (g <= 9) ? g : 0;
    }
  }
  return 0;
}

static bool rxMatchSeq(RxCtx& ctx, const char* pat, int p, int pEnd, int tPos, int* outEnd, bool requireFull);

// Match regex with top-level `|` alternation inside `[p .. pEnd)`
static bool rxMatchAlt(RxCtx& ctx, const char* pat, int p, int pEnd, int tPos, int* outEnd, bool requireFull) {
  if (++ctx.steps > kMaxRxSteps) return false;
  // Split top-level '|' inside [p .. pEnd)
  int depth = 0;
  bool inClass = false;
  int branchStart = p;
  for (int i = p; i <= pEnd; i++) {
    bool atBranchEnd = (i == pEnd);
    if (!atBranchEnd) {
      if (pat[i] == '\\' && i + 1 < pEnd) { i++; continue; }
      if (inClass) {
        if (pat[i] == ']') inClass = false;
        continue;
      }
      if (pat[i] == '[') { inClass = true; continue; }
      if (pat[i] == '(') { depth++; continue; }
      if (pat[i] == ')') { depth--; continue; }
      if (pat[i] == '|' && depth == 0) atBranchEnd = true;
    }
    if (atBranchEnd) {
      RxGroup saved[10];
      std::memcpy(saved, ctx.groups, sizeof(saved));
      if (rxMatchSeq(ctx, pat, branchStart, i, tPos, outEnd, requireFull)) {
        return true;
      }
      std::memcpy(ctx.groups, saved, sizeof(saved));
      branchStart = i + 1;
    }
  }
  return false;
}

static bool matchSingleAtom(const char* pat, int atomStart, int atomEnd, unsigned char ch) {
  if (pat[atomStart] == '.') {
    return ch != '\n';
  }
  if (pat[atomStart] == '\\' && atomStart + 1 < atomEnd) {
    return matchEscPred(pat[atomStart + 1], ch);
  }
  if (pat[atomStart] == '[') {
    return matchCharClass(pat, atomStart + 1, atomEnd - 1, ch);
  }
  return ch == static_cast<unsigned char>(pat[atomStart]);
}

static bool rxMatchSeq(RxCtx& ctx, const char* pat, int p, int pEnd, int tPos, int* outEnd, bool requireFull) {
  if (++ctx.steps > kMaxRxSteps) return false;

  if (p >= pEnd) {
    if (requireFull && tPos != ctx.textLen) return false;
    *outEnd = tPos;
    return true;
  }

  // Anchors
  if (pat[p] == '^') {
    if (tPos != 0) return false;
    return rxMatchSeq(ctx, pat, p + 1, pEnd, tPos, outEnd, requireFull);
  }
  if (pat[p] == '$') {
    if (tPos != ctx.textLen) return false;
    return rxMatchSeq(ctx, pat, p + 1, pEnd, tPos, outEnd, requireFull);
  }

  // Parse next atom `[atomStart .. atomEnd)`
  int  atomStart = p;
  int  atomEnd   = p + 1;
  bool isGroup   = false;
  int  grpClose  = -1;
  int  grpIdx    = 0;

  if (pat[p] == '(') {
    grpClose = findGroupClose(pat, p + 1, pEnd);
    if (grpClose < 0) return false;
    isGroup = true;
    grpIdx  = groupIndexAt(pat, p);
    atomEnd = grpClose + 1;
  } else if (pat[p] == '[') {
    int cc = findClassClose(pat, p + 1, pEnd);
    if (cc < 0) return false;
    atomEnd = cc + 1;
  } else if (pat[p] == '\\' && p + 1 < pEnd) {
    atomEnd = p + 2;
  }

  // Optional quantifier `*`, `+`, `?`
  char quant    = 0;
  int  restPos  = atomEnd;
  if (restPos < pEnd && (pat[restPos] == '*' || pat[restPos] == '+' || pat[restPos] == '?')) {
    quant = pat[restPos++];
  }

  int minRep = (quant == '+') ? 1 : ((quant == '*' || quant == '?') ? 0 : 1);
  int maxRep = (quant == '*' || quant == '+') ? (ctx.textLen - tPos + 1) : 1;

  if (!isGroup) {
    // Fast iterative greedy match + backtrack for single-character atoms
    int count = 0;
    while (count < maxRep && (tPos + count) < ctx.textLen &&
           matchSingleAtom(pat, atomStart, atomEnd, static_cast<unsigned char>(ctx.text[tPos + count]))) {
      count++;
    }
    for (int k = count; k >= minRep; k--) {
      RxGroup saved[10];
      std::memcpy(saved, ctx.groups, sizeof(saved));
      if (rxMatchSeq(ctx, pat, restPos, pEnd, tPos + k, outEnd, requireFull)) {
        return true;
      }
      std::memcpy(ctx.groups, saved, sizeof(saved));
    }
    return false;
  }

  // Group atom `( ... )` with optional quantifier
  if (quant == 0) {
    int subEnd = tPos;
    // Try matching group body against prefix, backtracking over possible group lengths if needed:
    // Actually, evaluate group body followed by rest via recursive continuation:
    // Try greedy lengths or direct sub-match:
    RxGroup saved[10];
    std::memcpy(saved, ctx.groups, sizeof(saved));
    // Collect candidate end positions of group match at tPos
    std::vector<std::pair<int, RxGroup[10]>> candidates;
    // Simpler & exact: match group body for every valid end position in descending order (greedy):
    for (int tryEnd = ctx.textLen; tryEnd >= tPos; tryEnd--) {
      std::memcpy(ctx.groups, saved, sizeof(saved));
      RxCtx subCtx = ctx;
      subCtx.textLen = tryEnd;
      int gEnd = tPos;
      if (rxMatchAlt(subCtx, pat, p + 1, grpClose, tPos, &gEnd, true) && gEnd == tryEnd) {
        std::memcpy(ctx.groups, subCtx.groups, sizeof(saved));
        ctx.steps = subCtx.steps;
        if (grpIdx > 0 && grpIdx <= 9) {
          ctx.groups[grpIdx].start = tPos;
          ctx.groups[grpIdx].end   = tryEnd;
        }
        if (rxMatchSeq(ctx, pat, restPos, pEnd, tryEnd, outEnd, requireFull)) {
          return true;
        }
      }
    }
    std::memcpy(ctx.groups, saved, sizeof(saved));
    (void)subEnd;
    return false;
  }

  // Quantified group `(...)?`, `(...)*`, `(...)+`
  if (quant == '?') {
    RxGroup saved[10];
    std::memcpy(saved, ctx.groups, sizeof(saved));
    for (int tryEnd = ctx.textLen; tryEnd >= tPos; tryEnd--) {
      std::memcpy(ctx.groups, saved, sizeof(saved));
      RxCtx subCtx = ctx;
      subCtx.textLen = tryEnd;
      int gEnd = tPos;
      if (rxMatchAlt(subCtx, pat, p + 1, grpClose, tPos, &gEnd, true) && gEnd == tryEnd) {
        std::memcpy(ctx.groups, subCtx.groups, sizeof(saved));
        ctx.steps = subCtx.steps;
        if (grpIdx > 0 && grpIdx <= 9) {
          ctx.groups[grpIdx].start = tPos;
          ctx.groups[grpIdx].end   = tryEnd;
        }
        if (rxMatchSeq(ctx, pat, restPos, pEnd, tryEnd, outEnd, requireFull)) {
          return true;
        }
      }
    }
    std::memcpy(ctx.groups, saved, sizeof(saved));
    return rxMatchSeq(ctx, pat, restPos, pEnd, tPos, outEnd, requireFull);
  }

  // `(...)*` or `(...)+`
  RxGroup saved[10];
  std::memcpy(saved, ctx.groups, sizeof(saved));
  // Try 1 non-empty iteration followed by `(...)*`, then (if `*`) 0 iterations
  for (int tryEnd = ctx.textLen; tryEnd > tPos; tryEnd--) {
    std::memcpy(ctx.groups, saved, sizeof(saved));
    RxCtx subCtx = ctx;
    subCtx.textLen = tryEnd;
    int gEnd = tPos;
    if (rxMatchAlt(subCtx, pat, p + 1, grpClose, tPos, &gEnd, true) && gEnd == tryEnd) {
      std::memcpy(ctx.groups, subCtx.groups, sizeof(saved));
      ctx.steps = subCtx.steps;
      if (grpIdx > 0 && grpIdx <= 9) {
        ctx.groups[grpIdx].start = tPos;
        ctx.groups[grpIdx].end   = tryEnd;
      }
      // Rewrite `+` to `*` conceptually by recursing on `(...)*`:
      // Temporarily try more iterations by recursing at `p` if we treat minRep=0:
      // Even simpler: try 0 more first OR more first (greedy -> more first!)
      // Recurse into a helper for `(...)*`:
      std::string starPat(pat + p, static_cast<size_t>(grpClose + 1 - p));
      starPat.push_back('*');
      starPat.append(pat + restPos, static_cast<size_t>(pEnd - restPos));
      if (rxMatchAlt(ctx, starPat.c_str(), 0, static_cast<int>(starPat.size()), tryEnd, outEnd, requireFull)) {
        return true;
      }
    }
  }
  std::memcpy(ctx.groups, saved, sizeof(saved));
  if (quant == '*') {
    return rxMatchSeq(ctx, pat, restPos, pEnd, tPos, outEnd, requireFull);
  }
  return false;
}

static Oop runRegexMatch(VM& vm, const char* pat, int patLen, const char* text, int textLen, bool fullMatch) {
  RxCtx ctx{};
  ctx.text      = text;
  ctx.textLen   = textLen;
  ctx.numGroups = countGroups(pat, patLen);
  int matchEnd  = 0;
  if (!rxMatchAlt(ctx, pat, 0, patLen, 0, &matchEnd, fullMatch)) {
    return kOopNil;
  }
  Oop res = vm.newList(static_cast<uint32_t>(ctx.numGroups + 1));
  TempRootGuard rg(vm, res);
  vm.listAppend(res, vm.newStr(text, static_cast<size_t>(matchEnd)));
  for (int g = 1; g <= ctx.numGroups; g++) {
    if (ctx.groups[g].start >= 0 && ctx.groups[g].end >= ctx.groups[g].start) {
      vm.listAppend(res, vm.newStr(text + ctx.groups[g].start,
                                   static_cast<size_t>(ctx.groups[g].end - ctx.groups[g].start)));
    } else {
      vm.listAppend(res, vm.newStr("", 0));
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// 1. Ur / Obj / Undefined Primitives
// ---------------------------------------------------------------------------
static Oop primCls(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return oopFromObj(&vm.oopCls(rcvr)->hdr);
}

static Oop primStr(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (isObj(rcvr) && objFromOop(rcvr)->format == OBJ_STR) return rcvr;
  std::string s = vm.toStdString(rcvr);
  return vm.newStr(s.data(), s.size());
}

static Oop primRepr(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  std::string s = vm.toReprString(rcvr);
  return vm.newStr(s.data(), s.size());
}

static Oop primTruth(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeBool(vm.truth(rcvr));
}

static Oop primNot(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeBool(!vm.truth(rcvr));
}

static Oop primNull(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeBool(isNil(rcvr));
}

static Oop primHash(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(vm.oopHash(rcvr) & 0x1FFFFFFFu));
}

static Oop primSysHash(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(rcvr & 0x1FFFFFFFu));
}

static Oop primIs(VM&, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(rcvr == args[0]);
}

static Oop primEquals(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(vm.oopEquals(rcvr, args[0]));
}

static Oop primNe(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(!vm.oopEquals(rcvr, args[0]));
}

static Oop primLt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(vm.oopCompare(rcvr, args[0]) < 0);
}

static Oop primLe(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(vm.oopCompare(rcvr, args[0]) <= 0);
}

static Oop primGt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(vm.oopCompare(rcvr, args[0]) > 0);
}

static Oop primGe(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeBool(vm.oopCompare(rcvr, args[0]) >= 0);
}

static Oop primIsa(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop target = args[0];
  if (isObj(target) && objFromOop(target)->format == OBJ_STR) {
    auto* s = reinterpret_cast<ObjStr*>(objFromOop(target));
    target = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(s->chars, s->hdr.size)));
  }
  if (!isObj(target) || objFromOop(target)->format != OBJ_CLS) {
    vm.toss("isa: expected Cls, got <%s>", vm.toStdString(args[0]).c_str());
  }
  auto* tc = reinterpret_cast<ObjCls*>(objFromOop(target));
  return makeBool(vm.isa(rcvr, tc->clsId));
}

static Oop primDoes(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  size_t slen = 0;
  const char* s = reqCStr(vm, args[0], "does:", &slen);
  uint32_t sym = vm.internSym(s, slen);
  return makeBool(vm.lookupMeth(vm.oopClsId(rcvr), sym) != nullptr);
}

static Oop primNearestInt(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeInt64OrFloat(static_cast<int64_t>(std::floor(reqFloat(vm, rcvr, "nearestInt") + 0.5f)));
}

static Oop primMust(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (!vm.truth(rcvr)) {
    vm.toss("Assertion failed (must): got <%s>", vm.toStdString(rcvr).c_str());
  }
  return rcvr;
}

static Oop primMust1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  if (!vm.truth(rcvr)) {
    Oop msg = args[0];
    if (isObj(msg) && objFromOop(msg)->format == OBJ_BLK) {
      msg = vm.evalBlk(msg, nullptr, 0);
    }
    vm.toss("Assertion failed: %s", vm.toStdString(msg).c_str());
  }
  return rcvr;
}

static Oop primCant(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (vm.truth(rcvr)) {
    vm.toss("Assertion failed (cant) on <%s>", vm.toStdString(rcvr).c_str());
  }
  return rcvr;
}

static Oop primCant1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  if (vm.truth(rcvr)) {
    Oop msg = args[0];
    if (isObj(msg) && objFromOop(msg)->format == OBJ_BLK) {
      msg = vm.evalBlk(msg, nullptr, 0);
    }
    vm.toss("Assertion failed (cant): %s", vm.toStdString(msg).c_str());
  }
  return rcvr;
}

static Oop primErr(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  vm.toss("%s", vm.toStdString(rcvr).c_str());
}

static Oop primErr1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.toss("%s: %s", vm.toStdString(rcvr).c_str(), vm.toStdString(args[0]).c_str());
}

static Oop primSay(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  vm.say("#SAY# %s", vm.toStdString(rcvr).c_str());
  return rcvr;
}

static Oop primSay1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.say("#SAY# %s: %s", vm.toStdString(args[0]).c_str(), vm.toStdString(rcvr).c_str());
  return rcvr;
}

static Oop primEval(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string code = reqStdStr(vm, args[0], "eval:");
  return vm.evalCode(rcvr, code.c_str(), nullptr, 0);
}

static Oop primEval1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string code = reqStdStr(vm, args[0], "eval:arg:");
  return vm.evalCode(rcvr, code.c_str(), &args[1], 1);
}

static Oop primEval2(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string code = reqStdStr(vm, args[0], "eval:arg:arg:");
  return vm.evalCode(rcvr, code.c_str(), &args[1], 2);
}

static Oop primSend(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  size_t slen = 0;
  const char* sel = reqCStr(vm, args[0], "send:", &slen);
  return vm.send(rcvr, vm.internSym(sel, slen), nullptr, 0);
}

static Oop primSend1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  size_t slen = 0;
  const char* sel = reqCStr(vm, args[0], "send:arg:", &slen);
  return vm.send(rcvr, vm.internSym(sel, slen), &args[1], 1);
}

static Oop primApplyArgs(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  size_t slen = 0;
  const char* sel = reqCStr(vm, args[0], "apply:args:", &slen);
  ObjList* av = reqList(vm, args[1], "apply:args:");
  return vm.send(rcvr, vm.internSym(sel, slen), av->items, static_cast<uint8_t>(av->len));
}

static Oop primPeekVars(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  ObjCls* c = vm.oopCls(rcvr);
  Oop d = vm.newDict(c->allVarCount * 2u + 4u);
  TempRootGuard rg(vm, d);
  if (isObj(rcvr)) {
    ObjHeader* h = objFromOop(rcvr);
    if (h->format == OBJ_SLOTS) {
      auto* s = reinterpret_cast<ObjSlots*>(h);
      uint16_t n = (c->allVarCount < h->size) ? c->allVarCount : static_cast<uint16_t>(h->size);
      for (uint16_t i = 0; i < n; i++) {
        const char* vname = vm.symCStr(c->allVarSyms[i]);
        vm.dictPut(d, vm.newStr(vname), s->slots[i]);
      }
    } else if (h->format == OBJ_CLS) {
      auto* clsObj = reinterpret_cast<ObjCls*>(h);
      for (uint16_t i = 0; i < c->allVarCount; i++) {
        const char* vname = vm.symCStr(c->allVarSyms[i]);
        Oop val = vm.dictGet(clsObj->savedInstsDict, makeInt30(-1 - static_cast<int32_t>(i)));
        vm.dictPut(d, vm.newStr(vname), val);
      }
    }
  }
  return d;
}

static Oop primPokeVars(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjDict* d = reqDict(vm, args[0], "pokeVars:");
  if (!isObj(rcvr)) return rcvr;
  ObjCls* c = vm.oopCls(rcvr);
  ObjHeader* h = objFromOop(rcvr);
  for (uint32_t capIdx = 0; capIdx < d->cap; capIdx++) {
    Oop k = d->entries[capIdx].key;
    if (k == kOopNull || k == kOopSuper) continue;
    Oop v = d->entries[capIdx].val;
    std::string ks = vm.toStdString(k);
    uint32_t ksym = vm.internSym(ks.c_str(), ks.size());
    for (uint16_t i = 0; i < c->allVarCount; i++) {
      if (c->allVarSyms[i] == ksym) {
        if (h->format == OBJ_SLOTS && i < h->size) {
          reinterpret_cast<ObjSlots*>(h)->slots[i] = v;
        } else if (h->format == OBJ_CLS) {
          Oop sd = vm.ensureSavedInstsDict(reinterpret_cast<ObjCls*>(h));
          vm.dictPut(sd, makeInt30(-1 - static_cast<int32_t>(i)), v);
        }
        break;
      }
    }
  }
  return rcvr;
}

// ---------------------------------------------------------------------------
// 2. Num (Int, Float, NumCls) Primitives
// ---------------------------------------------------------------------------
static Oop scalarBroadcastList(VM& vm, float scalar, ObjList* l, char op, bool scalarLeft) {
  Oop res = vm.newList(l->len);
  TempRootGuard rg(vm, res);
  for (uint32_t i = 0; i < l->len; i++) {
    float v = reqFloat(vm, l->items[i], "Num/List vector op");
    float a = scalarLeft ? scalar : v;
    float b = scalarLeft ? v : scalar;
    float r = 0.0f;
    switch (op) {
      case '+': r = a + b; break;
      case '-': r = a - b; break;
      case '*': r = a * b; break;
      case '/': r = a / b; break;
    }
    if (isInt(l->items[i]) && op != '/' && r == std::floor(r) &&
        r >= static_cast<float>(kMinInt30) && r <= static_cast<float>(kMaxInt30)) {
      vm.listAppend(res, makeInt30(static_cast<int32_t>(r)));
    } else {
      vm.listAppend(res, vm.makeFloatChecked(r));
    }
  }
  return res;
}

static Oop primNumAdd(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    return makeInt64OrFloat(static_cast<int64_t>(asInt30(rcvr)) + static_cast<int64_t>(asInt30(b)));
  }
  if (isNum(b)) {
    return vm.makeFloatChecked(toFloatUnchecked(rcvr) + toFloatUnchecked(b));
  }
  if (isObj(b) && objFromOop(b)->format == OBJ_LIST) {
    return scalarBroadcastList(vm, toFloatUnchecked(rcvr), reinterpret_cast<ObjList*>(objFromOop(b)), '+', true);
  }
  vm.toss("Num + expected Num or List, got <%s>", vm.toStdString(b).c_str());
}

static Oop primNumSub(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    return makeInt64OrFloat(static_cast<int64_t>(asInt30(rcvr)) - static_cast<int64_t>(asInt30(b)));
  }
  if (isNum(b)) {
    return vm.makeFloatChecked(toFloatUnchecked(rcvr) - toFloatUnchecked(b));
  }
  if (isObj(b) && objFromOop(b)->format == OBJ_LIST) {
    return scalarBroadcastList(vm, toFloatUnchecked(rcvr), reinterpret_cast<ObjList*>(objFromOop(b)), '-', true);
  }
  vm.toss("Num - expected Num or List, got <%s>", vm.toStdString(b).c_str());
}

static Oop primNumMul(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    return makeInt64OrFloat(static_cast<int64_t>(asInt30(rcvr)) * static_cast<int64_t>(asInt30(b)));
  }
  if (isNum(b)) {
    return vm.makeFloatChecked(toFloatUnchecked(rcvr) * toFloatUnchecked(b));
  }
  if (isObj(b) && objFromOop(b)->format == OBJ_LIST) {
    return scalarBroadcastList(vm, toFloatUnchecked(rcvr), reinterpret_cast<ObjList*>(objFromOop(b)), '*', true);
  }
  vm.toss("Num * expected Num or List, got <%s>", vm.toStdString(b).c_str());
}

static Oop primNumDiv(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isNum(b)) {
    return vm.makeFloatChecked(toFloatUnchecked(rcvr) / toFloatUnchecked(b));
  }
  if (isObj(b) && objFromOop(b)->format == OBJ_LIST) {
    return scalarBroadcastList(vm, toFloatUnchecked(rcvr), reinterpret_cast<ObjList*>(objFromOop(b)), '/', true);
  }
  vm.toss("Num / expected Num or List, got <%s>", vm.toStdString(b).c_str());
}

static Oop primNumMod(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    int32_t ib = asInt30(b);
    if (ib == 0) vm.toss("Modulo by zero");
    int32_t r = asInt30(rcvr) % ib;
    if (r < 0) r += (ib > 0 ? ib : -ib);
    return makeInt30(r);
  }
  float fb = reqFloat(vm, b, "Num %");
  float r  = std::fmod(toFloatUnchecked(rcvr), fb);
  if (r < 0.0f) r += fb;
  return vm.makeFloatChecked(r);
}

static Oop primNumIdiv(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    int32_t ib = asInt30(b);
    if (ib == 0) vm.toss("Integer division by zero");
    return makeInt64OrFloat(static_cast<int64_t>(asInt30(rcvr)) / static_cast<int64_t>(ib));
  }
  float fb = reqFloat(vm, b, "idiv:");
  if (fb == 0.0f) vm.toss("Integer division by zero");
  int64_t q = static_cast<int64_t>(toFloatUnchecked(rcvr)) / static_cast<int64_t>(fb);
  return makeInt64OrFloat(q);
}

static Oop primNumImod(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    int32_t ib = asInt30(b);
    if (ib == 0) vm.toss("Integer modulo by zero");
    return makeInt64OrFloat(static_cast<int64_t>(asInt30(rcvr)) % static_cast<int64_t>(ib));
  }
  float fb = reqFloat(vm, b, "imod:");
  if (fb == 0.0f) vm.toss("Integer modulo by zero");
  int64_t r = static_cast<int64_t>(toFloatUnchecked(rcvr)) % static_cast<int64_t>(fb);
  return makeInt64OrFloat(r);
}

static Oop primNumPow(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) {
    int32_t exp = asInt30(b);
    if (exp >= 0 && exp <= 30) {
      int64_t base = asInt30(rcvr);
      int64_t acc  = 1;
      bool    ovf  = false;
      for (int32_t i = 0; i < exp; i++) {
        acc *= base;
        if (acc < kMinInt30 || acc > kMaxInt30) { ovf = true; break; }
      }
      if (!ovf) return makeInt30(static_cast<int32_t>(acc));
    }
  }
  float fb = reqFloat(vm, b, "pow:");
  return vm.makeFloatChecked(std::pow(toFloatUnchecked(rcvr), fb));
}

static Oop primNumBitAnd(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeInt64OrFloat(reqInt(vm, rcvr, "&") & reqInt(vm, args[0], "&"));
}
static Oop primNumBitOr(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeInt64OrFloat(reqInt(vm, rcvr, "|") | reqInt(vm, args[0], "|"));
}
static Oop primNumBitXor(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return makeInt64OrFloat(reqInt(vm, rcvr, "^") ^ reqInt(vm, args[0], "^"));
}
static Oop primNumShl(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  int32_t s = reqInt(vm, args[0], "<<");
  if (s < 0 || s >= 62) return makeInt30(0);
  return makeInt64OrFloat(static_cast<int64_t>(reqInt(vm, rcvr, "<<")) << s);
}
static Oop primNumShr(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  int32_t s = reqInt(vm, args[0], ">>");
  if (s < 0 || s >= 31) return makeInt30(reqInt(vm, rcvr, ">>") < 0 ? -1 : 0);
  return makeInt64OrFloat(reqInt(vm, rcvr, ">>") >> s);
}

static Oop primNumNeg(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) return makeInt64OrFloat(-static_cast<int64_t>(asInt30(rcvr)));
  return vm.makeFloatChecked(-asFloat(rcvr));
}
static Oop primNumAbs(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) {
    int64_t v = asInt30(rcvr);
    return makeInt64OrFloat(v < 0 ? -v : v);
  }
  return vm.makeFloatChecked(std::fabs(asFloat(rcvr)));
}
static Oop primNumSgn(VM&, Oop rcvr, const Oop*, uint8_t) {
  float f = toFloatUnchecked(rcvr);
  return makeInt30(f > 0.0f ? 1 : (f < 0.0f ? -1 : 0));
}
static Oop primNumSq(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) {
    int64_t v = asInt30(rcvr);
    return makeInt64OrFloat(v * v);
  }
  float f = asFloat(rcvr);
  return vm.makeFloatChecked(f * f);
}
static Oop primNumSqrt(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.makeFloatChecked(std::sqrt(toFloatUnchecked(rcvr)));
}
static Oop primNumInt(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) return rcvr;
  return makeInt64OrFloat(static_cast<int64_t>(asFloat(rcvr)));
}
static Oop primNumFloor(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) return rcvr;
  return makeInt64OrFloat(static_cast<int64_t>(std::floor(asFloat(rcvr))));
}
static Oop primNumCeil(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) return rcvr;
  return makeInt64OrFloat(static_cast<int64_t>(std::ceil(asFloat(rcvr))));
}
static Oop primNumRound(VM&, Oop rcvr, const Oop*, uint8_t) {
  if (isInt(rcvr)) return rcvr;
  return makeInt64OrFloat(static_cast<int64_t>(std::floor(asFloat(rcvr) + 0.5f)));
}
static Oop primNumFloat(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.makeFloatChecked(toFloatUnchecked(rcvr));
}
static Oop primNumSelf(VM&, Oop rcvr, const Oop*, uint8_t) { return rcvr; }

static Oop primNumSin(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(std::sin(toFloatUnchecked(rcvr))); }
static Oop primNumCos(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(std::cos(toFloatUnchecked(rcvr))); }
static Oop primNumTan(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(std::tan(toFloatUnchecked(rcvr))); }
static Oop primNumAsin(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::asin(toFloatUnchecked(rcvr))); }
static Oop primNumAcos(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::acos(toFloatUnchecked(rcvr))); }
static Oop primNumAtan(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::atan(toFloatUnchecked(rcvr))); }
static Oop primNumAtan2(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return vm.makeFloatChecked(std::atan2(toFloatUnchecked(rcvr), reqFloat(vm, args[0], "atan2:")));
}
static Oop primNumSinh(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::sinh(toFloatUnchecked(rcvr))); }
static Oop primNumCosh(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::cosh(toFloatUnchecked(rcvr))); }
static Oop primNumTanh(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::tanh(toFloatUnchecked(rcvr))); }
static Oop primNumLn(VM& vm, Oop rcvr, const Oop*, uint8_t)    { return vm.makeFloatChecked(std::log(toFloatUnchecked(rcvr))); }
static Oop primNumLog10(VM& vm, Oop rcvr, const Oop*, uint8_t) { return vm.makeFloatChecked(std::log10(toFloatUnchecked(rcvr))); }
static Oop primNumLog2(VM& vm, Oop rcvr, const Oop*, uint8_t)  { return vm.makeFloatChecked(std::log2(toFloatUnchecked(rcvr))); }
static Oop primNumExp(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(std::exp(toFloatUnchecked(rcvr))); }
static Oop primNumDeg(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(toFloatUnchecked(rcvr) * 57.2957795f); }
static Oop primNumRad(VM& vm, Oop rcvr, const Oop*, uint8_t)   { return vm.makeFloatChecked(toFloatUnchecked(rcvr) * 0.0174532925f); }

static Oop primNumMin(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) return (asInt30(rcvr) <= asInt30(b)) ? rcvr : b;
  float fa = toFloatUnchecked(rcvr), fb = reqFloat(vm, b, "min:");
  return (fa <= fb) ? rcvr : b;
}
static Oop primNumMax(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  Oop b = args[0];
  if (isInt(rcvr) && isInt(b)) return (asInt30(rcvr) >= asInt30(b)) ? rcvr : b;
  float fa = toFloatUnchecked(rcvr), fb = reqFloat(vm, b, "max:");
  return (fa >= fb) ? rcvr : b;
}
static Oop primNumClamp(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  float x  = toFloatUnchecked(rcvr);
  float lo = reqFloat(vm, args[0], "clamp:to:");
  float hi = reqFloat(vm, args[1], "clamp:to:");
  if (x < lo) return args[0];
  if (x > hi) return args[1];
  return rcvr;
}
static Oop primNumLerp(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  float a = toFloatUnchecked(rcvr);
  float b = reqFloat(vm, args[0], "lerp:t:");
  float t = reqFloat(vm, args[1], "lerp:t:");
  return vm.makeFloatChecked(a + (b - a) * t);
}

static Oop primNumChr(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  int32_t cp = reqInt(vm, rcvr, "chr");
  char c = static_cast<char>(cp & 0xFF);
  return vm.newStr(&c, 1);
}
static Oop primNumChar(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return makeChar(static_cast<uint32_t>(reqInt(vm, rcvr, "char")) & 0x1FFFFFu);
}

static Oop primNumFmt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  char buf[128];
  if (isNum(args[0])) {
    int prec = reqInt(vm, args[0], "fmt:");
    if (prec < 0) prec = 0;
    if (prec > 12) prec = 12;
    std::snprintf(buf, sizeof(buf), "%.*f", prec, static_cast<double>(toFloatUnchecked(rcvr)));
    return vm.newStr(buf);
  }
  const char* fmt = reqCStr(vm, args[0], "fmt:");
  // Detect whether format conversion specifier expects integer vs float
  bool wantInt = false;
  for (const char* p = fmt; *p; p++) {
    if (*p == '%' && p[1] != '%') {
      while (*++p && (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' ||
                      std::isdigit(static_cast<unsigned char>(*p)) || *p == '.')) {}
      if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'X' || *p == 'o' || *p == 'c') {
        wantInt = true;
      }
      break;
    }
  }
  if (wantInt) {
    std::snprintf(buf, sizeof(buf), fmt, static_cast<int>(reqInt(vm, rcvr, "fmt:")));
  } else {
    std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(toFloatUnchecked(rcvr)));
  }
  return vm.newStr(buf);
}

static Oop primNumRange(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  int32_t n = reqInt(vm, rcvr, "range");
  if (n <= 0) return vm.newList(0);
  if (n > 65536) vm.toss("range too large (%d)", static_cast<int>(n));
  Oop res = vm.newList(static_cast<uint32_t>(n));
  auto* l = reinterpret_cast<ObjList*>(objFromOop(res));
  for (int32_t i = 0; i < n; i++) l->items[i] = makeInt30(i);
  l->len = static_cast<uint32_t>(n);
  return res;
}

static Oop primNumTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  int32_t a = reqInt(vm, rcvr, "to:");
  int32_t b = reqInt(vm, args[0], "to:");
  if (b < a) return vm.newList(0);
  int32_t n = b - a + 1;
  if (n > 65536) vm.toss("to: range too large (%d)", static_cast<int>(n));
  Oop res = vm.newList(static_cast<uint32_t>(n));
  auto* l = reinterpret_cast<ObjList*>(objFromOop(res));
  for (int32_t i = 0; i < n; i++) l->items[i] = makeInt30(a + i);
  l->len = static_cast<uint32_t>(n);
  return res;
}

static Oop primNumClsPi(VM& vm, Oop, const Oop*, uint8_t)  { return vm.makeFloatChecked(3.14159265358979323846f); }
static Oop primNumClsTau(VM& vm, Oop, const Oop*, uint8_t) { return vm.makeFloatChecked(6.28318530717958647692f); }
static Oop primNumClsE(VM& vm, Oop, const Oop*, uint8_t)   { return vm.makeFloatChecked(2.71828182845904523536f); }
static Oop primNumClsRand(VM& vm, Oop, const Oop*, uint8_t) {
  float f = static_cast<float>(nextRandU32() >> 8) * (1.0f / 16777216.0f);
  return vm.makeFloatChecked(f);
}
static Oop primNumClsRand1(VM& vm, Oop, const Oop* args, uint8_t) {
  int32_t n = reqInt(vm, args[0], "Num rand:");
  if (n <= 0) return makeInt30(0);
  return makeInt30(static_cast<int32_t>(nextRandU32() % static_cast<uint32_t>(n)));
}
static Oop primNumClsSeed(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  gRngState = static_cast<uint32_t>(reqInt(vm, args[0], "Num seed:"));
  if (gRngState == 0) gRngState = 1u;
  return rcvr;
}

// ---------------------------------------------------------------------------
// 3. Char & CharCls Primitives
// ---------------------------------------------------------------------------
static Oop primCharClsNew(VM& vm, Oop, const Oop* args, uint8_t) {
  if (isChar(args[0])) return args[0];
  if (isNum(args[0])) return makeChar(static_cast<uint32_t>(reqInt(vm, args[0], "Char new:")));
  if (isObj(args[0]) && objFromOop(args[0])->format == OBJ_STR) {
    auto* s = reinterpret_cast<ObjStr*>(objFromOop(args[0]));
    if (s->hdr.size == 0) vm.toss("Char new: got empty Str");
    return makeChar(static_cast<unsigned char>(s->chars[0]));
  }
  vm.toss("Char new: expected Num or Str");
}
static Oop primCharOrd(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(asChar(rcvr)));
}
static Oop primCharStr(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  char c = static_cast<char>(asChar(rcvr) & 0xFFu);
  return vm.newStr(&c, 1);
}
static Oop primCharIsAlpha(VM&, Oop rcvr, const Oop*, uint8_t) {
  uint32_t cp = asChar(rcvr);
  return makeBool(cp < 128 && std::isalpha(static_cast<int>(cp)) != 0);
}
static Oop primCharIsDigit(VM&, Oop rcvr, const Oop*, uint8_t) {
  uint32_t cp = asChar(rcvr);
  return makeBool(cp < 128 && std::isdigit(static_cast<int>(cp)) != 0);
}
static Oop primCharIsSpace(VM&, Oop rcvr, const Oop*, uint8_t) {
  uint32_t cp = asChar(rcvr);
  return makeBool(cp < 128 && std::isspace(static_cast<int>(cp)) != 0);
}
static Oop primCharUpper(VM&, Oop rcvr, const Oop*, uint8_t) {
  uint32_t cp = asChar(rcvr);
  return (cp < 128) ? makeChar(static_cast<uint32_t>(std::toupper(static_cast<int>(cp)))) : rcvr;
}
static Oop primCharLower(VM&, Oop rcvr, const Oop*, uint8_t) {
  uint32_t cp = asChar(rcvr);
  return (cp < 128) ? makeChar(static_cast<uint32_t>(std::tolower(static_cast<int>(cp)))) : rcvr;
}

// ---------------------------------------------------------------------------
// 4. Str, Buf, Bytes, Rex Primitives
// ---------------------------------------------------------------------------
static Oop primStrClsNew(VM& vm, Oop, const Oop*, uint8_t) { return vm.newStr("", 0); }

static Oop primStrLen(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(objFromOop(rcvr)->size));
}

static Oop primStrAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(s->hdr.size);
  int32_t idx = reqInt(vm, args[0], "Str at:");
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) vm.toss("Str index %d out of range (len %d)", idx, len);
  return vm.newStr(s->chars + idx, 1);
}

static Oop primStrCharAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(s->hdr.size);
  int32_t idx = reqInt(vm, args[0], "Str charAt:");
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) vm.toss("Str index %d out of range (len %d)", idx, len);
  return makeChar(static_cast<unsigned char>(s->chars[idx]));
}

static Oop primStrHead(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  if (s->hdr.size == 0) vm.toss("Str head on empty string");
  return vm.newStr(s->chars, 1);
}

static Oop primStrLast(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  if (s->hdr.size == 0) vm.toss("Str last on empty string");
  return vm.newStr(s->chars + s->hdr.size - 1, 1);
}

static Oop primStrTail(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  if (s->hdr.size == 0) vm.toss("Str tail on empty string");
  return vm.newStr(s->chars + 1, s->hdr.size - 1);
}

static Oop primStrHeadN(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t n = reqInt(vm, args[0], "Str head:");
  if (n <= 0) return vm.newStr("", 0);
  uint32_t take = (static_cast<uint32_t>(n) < s->hdr.size) ? static_cast<uint32_t>(n) : s->hdr.size;
  return vm.newStr(s->chars, take);
}

static Oop primStrTailN(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t n = reqInt(vm, args[0], "Str tail:");
  if (n <= 0) return vm.newStr("", 0);
  uint32_t take = (static_cast<uint32_t>(n) < s->hdr.size) ? static_cast<uint32_t>(n) : s->hdr.size;
  return vm.newStr(s->chars + (s->hdr.size - take), take);
}

static Oop primStrCutTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(s->hdr.size);
  int32_t p = reqInt(vm, args[0], "Str cut:to:");
  int32_t q = reqInt(vm, args[1], "Str cut:to:");
  if (p < 0) p += len;
  if (q < 0) q += len;
  if (p < 0) p = 0;
  if (q > len) q = len;
  if (q <= p) return vm.newStr("", 0);
  return vm.newStr(s->chars + p, static_cast<size_t>(q - p));
}

static Oop primStrCutLen(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(s->hdr.size);
  int32_t p = reqInt(vm, args[0], "Str cut:len:");
  int32_t n = reqInt(vm, args[1], "Str cut:len:");
  if (p < 0) p += len;
  if (p < 0) p = 0;
  if (n <= 0 || p >= len) return vm.newStr("", 0);
  if (p + n > len) n = len - p;
  return vm.newStr(s->chars + p, static_cast<size_t>(n));
}

static Oop primStrAppend(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string rhs = reqStdStr(vm, args[0], "Str append:");
  std::string out;
  out.reserve(s->hdr.size + rhs.size());
  out.append(s->chars, s->hdr.size);
  out.append(rhs);
  return vm.newStr(out.data(), out.size());
}

static Oop primStrRepeat(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  int32_t n = reqInt(vm, args[0], "Str *");
  if (n <= 0 || s->hdr.size == 0) return vm.newStr("", 0);
  std::string out;
  out.reserve(s->hdr.size * static_cast<size_t>(n));
  for (int32_t i = 0; i < n; i++) out.append(s->chars, s->hdr.size);
  return vm.newStr(out.data(), out.size());
}

static Oop primStrSplit(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  size_t dlen = 0;
  const char* delim = reqCStr(vm, args[0], "Str split:", &dlen);
  Oop res = vm.newList(4);
  TempRootGuard rg(vm, res);

  // Re-fetch `s` after allocations or copy to std::string first (GC safety!)
  std::string str(s->chars, s->hdr.size);
  std::string del(delim, dlen);

  if (del.empty()) {
    for (size_t i = 0; i < str.size(); i++) {
      vm.listAppend(res, vm.newStr(str.data() + i, 1));
    }
    return res;
  }

  // TTA splitNonEmpty on first delimiter char (or substring if multi-char):
  if (del.size() == 1) {
    char dc = del[0];
    size_t i = 0;
    while (i < str.size()) {
      while (i < str.size() && str[i] == dc) i++;
      if (i >= str.size()) break;
      size_t j = i;
      while (j < str.size() && str[j] != dc) j++;
      vm.listAppend(res, vm.newStr(str.data() + i, j - i));
      i = j;
    }
  } else {
    size_t pos = 0;
    while (pos <= str.size()) {
      size_t found = str.find(del, pos);
      size_t end = (found == std::string::npos) ? str.size() : found;
      if (end > pos) {
        vm.listAppend(res, vm.newStr(str.data() + pos, end - pos));
      }
      if (found == std::string::npos) break;
      pos = found + del.size();
    }
  }
  return res;
}

static Oop primStrLines(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string str(s->chars, s->hdr.size);
  Oop res = vm.newList(4);
  TempRootGuard rg(vm, res);
  size_t i = 0;
  while (i <= str.size()) {
    size_t nl = str.find('\n', i);
    size_t end = (nl == std::string::npos) ? str.size() : nl;
    size_t trimEnd = (end > i && str[end - 1] == '\r') ? (end - 1) : end;
    vm.listAppend(res, vm.newStr(str.data() + i, trimEnd - i));
    if (nl == std::string::npos) break;
    i = nl + 1;
    if (i == str.size()) break;  // don't add extra empty line after trailing \n
  }
  return res;
}

static Oop primStrWords(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string str(s->chars, s->hdr.size);
  Oop res = vm.newList(4);
  TempRootGuard rg(vm, res);
  size_t i = 0;
  while (i < str.size()) {
    while (i < str.size() && std::isspace(static_cast<unsigned char>(str[i]))) i++;
    if (i >= str.size()) break;
    size_t j = i;
    while (j < str.size() && !std::isspace(static_cast<unsigned char>(str[j]))) j++;
    vm.listAppend(res, vm.newStr(str.data() + i, j - i));
    i = j;
  }
  return res;
}

static Oop primStrTrim(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  size_t a = 0, b = s->hdr.size;
  while (a < b && std::isspace(static_cast<unsigned char>(s->chars[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s->chars[b - 1]))) b--;
  return vm.newStr(s->chars + a, b - a);
}

static Oop primStrLower(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string out(s->chars, s->hdr.size);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return vm.newStr(out.data(), out.size());
}

static Oop primStrUpper(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string out(s->chars, s->hdr.size);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return vm.newStr(out.data(), out.size());
}

static Oop primStrToNum(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  const char* p = s->chars;
  while (*p && std::isspace(static_cast<unsigned char>(*p))) p++;
  if (*p == '\0') vm.toss("Str num on blank string");
  char* endi = nullptr;
  int64_t iv = std::strtoll(p, &endi, 10);
  if (endi && endi > p) {
    const char* q = endi;
    while (*q && std::isspace(static_cast<unsigned char>(*q))) q++;
    if (*q == '\0' && iv >= kMinInt30 && iv <= kMaxInt30) {
      return makeInt30(static_cast<int32_t>(iv));
    }
  }
  char* endf = nullptr;
  float fv = std::strtof(p, &endf);
  if (!endf || endf == p) vm.toss("Cannot parse Str <%s> as Num", s->chars);
  while (*endf && std::isspace(static_cast<unsigned char>(*endf))) endf++;
  if (*endf != '\0') vm.toss("Cannot parse Str <%s> as Num", s->chars);
  return vm.makeFloatChecked(fv);
}

static Oop primStrOrd(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  if (s->hdr.size == 0) return makeInt30(-1);
  return makeInt30(static_cast<unsigned char>(s->chars[0]));
}

static Oop primStrChar(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  if (s->hdr.size == 0) vm.toss("Str char on empty string");
  return makeChar(static_cast<unsigned char>(s->chars[0]));
}

static Oop primStrChars(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string str(s->chars, s->hdr.size);
  Oop res = vm.newList(static_cast<uint32_t>(str.size()));
  auto* l = reinterpret_cast<ObjList*>(objFromOop(res));
  for (size_t i = 0; i < str.size(); i++) {
    l->items[i] = makeChar(static_cast<unsigned char>(str[i]));
  }
  l->len = static_cast<uint32_t>(str.size());
  return res;
}

static Oop primStrExplode(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string str(s->chars, s->hdr.size);
  Oop res = vm.newList(static_cast<uint32_t>(str.size()));
  auto* l = reinterpret_cast<ObjList*>(objFromOop(res));
  for (size_t i = 0; i < str.size(); i++) {
    l->items[i] = makeInt30(static_cast<unsigned char>(str[i]));
  }
  l->len = static_cast<uint32_t>(str.size());
  return res;
}

static Oop primStrBytes(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string str(s->chars, s->hdr.size);
  return vm.newBytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

static Oop primStrStarts(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  size_t plen = 0;
  const char* pfx = reqCStr(vm, args[0], "starts:", &plen);
  if (plen > s->hdr.size) return kOopFalse;
  return makeBool(std::memcmp(s->chars, pfx, plen) == 0);
}

static Oop primStrEnds(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  size_t slen = 0;
  const char* sfx = reqCStr(vm, args[0], "ends:", &slen);
  if (slen > s->hdr.size) return kOopFalse;
  return makeBool(std::memcmp(s->chars + (s->hdr.size - slen), sfx, slen) == 0);
}

static Oop primStrHas(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string sub = reqStdStr(vm, args[0], "Str has:");
  std::string hay(s->chars, s->hdr.size);
  return makeBool(hay.find(sub) != std::string::npos);
}

static Oop primStrIdx(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string sub = reqStdStr(vm, args[0], "Str idx:");
  std::string hay(s->chars, s->hdr.size);
  size_t pos = hay.find(sub);
  return (pos == std::string::npos) ? makeInt30(-1) : makeInt30(static_cast<int32_t>(pos));
}

static Oop primStrReplace(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string hay(s->chars, s->hdr.size);
  std::string from = reqStdStr(vm, args[0], "replace:with:");
  std::string to   = reqStdStr(vm, args[1], "replace:with:");
  if (from.empty()) return rcvr;
  std::string out;
  size_t pos = 0;
  while (true) {
    size_t f = hay.find(from, pos);
    if (f == std::string::npos) {
      out.append(hay, pos, std::string::npos);
      break;
    }
    out.append(hay, pos, f - pos);
    out.append(to);
    pos = f + from.size();
  }
  return vm.newStr(out.data(), out.size());
}

static Oop primStrMatchP(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string text(s->chars, s->hdr.size);
  std::string pat = reqStdStr(vm, args[0], "matchp:");
  RxCtx ctx{};
  ctx.text    = text.data();
  ctx.textLen = static_cast<int>(text.size());
  int mEnd    = 0;
  return makeBool(rxMatchAlt(ctx, pat.data(), 0, static_cast<int>(pat.size()), 0, &mEnd, true));
}

static Oop primStrMatch(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(rcvr));
  std::string text(s->chars, s->hdr.size);
  std::string pat;
  if (isObj(args[0]) && objFromOop(args[0])->format == OBJ_REX) {
    auto* rx = reinterpret_cast<ObjRex*>(objFromOop(args[0]));
    auto* ps = reinterpret_cast<ObjStr*>(objFromOop(rx->patternStr));
    pat.assign(ps->chars, ps->hdr.size);
  } else {
    pat = reqStdStr(vm, args[0], "match:");
  }
  return runRegexMatch(vm, pat.data(), static_cast<int>(pat.size()),
                       text.data(), static_cast<int>(text.size()), false);
}

// Buf
static Oop primBufClsNew(VM& vm, Oop, const Oop*, uint8_t) { return vm.newBuf(); }
static Oop primBufClsAp(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string s = vm.toStdString(args[0]);
  return vm.newBuf(s.data(), s.size());
}
static Oop primBufAp(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string s = vm.toStdString(args[0]);
  auto* b = reinterpret_cast<ObjBuf*>(objFromOop(rcvr));
  if (b->len + s.size() + 1u > b->cap) {
    uint32_t newCap = b->cap * 2u;
    while (newCap < b->len + s.size() + 1u) newCap *= 2u;
    b->data = static_cast<char*>(vm.reallocExternal(b->data, b->cap, newCap));
    b->cap  = newCap;
  }
  if (!s.empty()) std::memcpy(b->data + b->len, s.data(), s.size());
  b->len += static_cast<uint32_t>(s.size());
  b->data[b->len] = '\0';
  return rcvr;
}
static Oop primBufLen(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(reinterpret_cast<ObjBuf*>(objFromOop(rcvr))->len));
}
static Oop primBufClear(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* b = reinterpret_cast<ObjBuf*>(objFromOop(rcvr));
  b->len = 0;
  if (b->data) b->data[0] = '\0';
  return rcvr;
}

// Bytes
static Oop primBytesClsImplode(VM& vm, Oop, const Oop* args, uint8_t) {
  ObjList* l = reqList(vm, args[0], "Bytes implode:");
  std::vector<uint8_t> buf(l->len);
  for (uint32_t i = 0; i < l->len; i++) {
    buf[i] = static_cast<uint8_t>(reqInt(vm, l->items[i], "Bytes implode:") & 0xFF);
  }
  return vm.newBytes(buf.data(), buf.size());
}
static Oop primBytesLen(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(objFromOop(rcvr)->size));
}
static Oop primBytesAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(b->hdr.size);
  int32_t idx = reqInt(vm, args[0], "Bytes at:");
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) vm.toss("Bytes index %d out of range", idx);
  return makeInt30(b->bytes[idx]);
}
static Oop primBytesHeadN(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  int32_t n = reqInt(vm, args[0], "Bytes head:");
  if (n <= 0) return vm.newBytes(nullptr, 0);
  uint32_t take = (static_cast<uint32_t>(n) < b->hdr.size) ? static_cast<uint32_t>(n) : b->hdr.size;
  std::vector<uint8_t> tmp(b->bytes, b->bytes + take);
  return vm.newBytes(tmp.data(), tmp.size());
}
static Oop primBytesTailN(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  int32_t n = reqInt(vm, args[0], "Bytes tail:");
  if (n <= 0) return vm.newBytes(nullptr, 0);
  uint32_t take = (static_cast<uint32_t>(n) < b->hdr.size) ? static_cast<uint32_t>(n) : b->hdr.size;
  std::vector<uint8_t> tmp(b->bytes + (b->hdr.size - take), b->bytes + b->hdr.size);
  return vm.newBytes(tmp.data(), tmp.size());
}
static Oop primBytesCutTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(b->hdr.size);
  int32_t p = reqInt(vm, args[0], "Bytes cut:to:");
  int32_t q = reqInt(vm, args[1], "Bytes cut:to:");
  if (p < 0) p += len;
  if (q < 0) q += len;
  if (p < 0) p = 0;
  if (q > len) q = len;
  if (q <= p) return vm.newBytes(nullptr, 0);
  std::vector<uint8_t> tmp(b->bytes + p, b->bytes + q);
  return vm.newBytes(tmp.data(), tmp.size());
}
static Oop primBytesCutLen(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(b->hdr.size);
  int32_t p = reqInt(vm, args[0], "Bytes cut:len:");
  int32_t n = reqInt(vm, args[1], "Bytes cut:len:");
  if (p < 0) p += len;
  if (p < 0) p = 0;
  if (n <= 0 || p >= len) return vm.newBytes(nullptr, 0);
  if (p + n > len) n = len - p;
  std::vector<uint8_t> tmp(b->bytes + p, b->bytes + p + n);
  return vm.newBytes(tmp.data(), tmp.size());
}
static Oop primBytesExplode(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* b = reinterpret_cast<ObjBytes*>(objFromOop(rcvr));
  std::vector<uint8_t> tmp(b->bytes, b->bytes + b->hdr.size);
  Oop res = vm.newList(static_cast<uint32_t>(tmp.size()));
  auto* l = reinterpret_cast<ObjList*>(objFromOop(res));
  for (size_t i = 0; i < tmp.size(); i++) {
    l->items[i] = makeInt30(static_cast<int8_t>(tmp[i]));  // TTA signed -128..127
  }
  l->len = static_cast<uint32_t>(tmp.size());
  return res;
}

// Rex
static Oop primRexClsNew(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string p = reqStdStr(vm, args[0], "Rex new:");
  Oop ps = vm.newStr(p.data(), p.size());
  TempRootGuard rg(vm, ps);
  auto* rx = reinterpret_cast<ObjRex*>(vm.allocObj(CLS_REX, OBJ_REX, 0, sizeof(ObjRex)));
  rx->patternStr = ps;
  return oopFromObj(&rx->hdr);
}
static Oop primRexMatch(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* rx = reinterpret_cast<ObjRex*>(objFromOop(rcvr));
  auto* ps = reinterpret_cast<ObjStr*>(objFromOop(rx->patternStr));
  std::string pat(ps->chars, ps->hdr.size);
  std::string txt = reqStdStr(vm, args[0], "Rex match:");
  return runRegexMatch(vm, pat.data(), static_cast<int>(pat.size()),
                       txt.data(), static_cast<int>(txt.size()), false);
}

// ---------------------------------------------------------------------------
// 5. Pythonic List (aliased Vec) Primitives
// ---------------------------------------------------------------------------
static Oop primListClsNew(VM& vm, Oop, const Oop*, uint8_t) { return vm.newList(4); }
static Oop primListClsAp(VM& vm, Oop, const Oop* args, uint8_t) { return vm.newListFrom(args, 1); }

static Oop primListLen(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(reinterpret_cast<ObjList*>(objFromOop(rcvr))->len));
}

static int32_t checkListIndex(VM& vm, ObjList* l, Oop idxOop, const char* ctx) {
  int32_t len = static_cast<int32_t>(l->len);
  int32_t idx = reqInt(vm, idxOop, ctx);
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) {
    vm.toss("%s index %d out of range (len %d)", ctx, idx, len);
  }
  return idx;
}

static int32_t wrapListIndex(VM& vm, ObjList* l, Oop idxOop, const char* ctx) {
  int32_t len = static_cast<int32_t>(l->len);
  if (len == 0) vm.toss("%s on empty List", ctx);
  int32_t idx = reqInt(vm, idxOop, ctx) % len;
  if (idx < 0) idx += len;
  return idx;
}

static Oop primListAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  return l->items[checkListIndex(vm, l, args[0], "List at:")];
}

static Oop primListAtPut(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  l->items[checkListIndex(vm, l, args[0], "List at:put:")] = args[1];
  return rcvr;
}

static Oop primListWrapAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  return l->items[wrapListIndex(vm, l, args[0], "List wrapAt:")];
}

static Oop primListWrapAtPut(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  l->items[wrapListIndex(vm, l, args[0], "List wrapAt:put:")] = args[1];
  return rcvr;
}

static Oop primListAp(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.listAppend(rcvr, args[0]);
  return rcvr;
}

static Oop primListPop(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  if (l->len == 0) vm.toss("List pop on empty List");
  return l->items[--l->len];
}

static Oop primListPopAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  int32_t idx = checkListIndex(vm, l, args[0], "List pop:");
  Oop val = l->items[idx];
  for (uint32_t i = static_cast<uint32_t>(idx) + 1u; i < l->len; i++) {
    l->items[i - 1u] = l->items[i];
  }
  l->len--;
  return val;
}

static Oop primListInsAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(l->len);
  int32_t idx = reqInt(vm, args[1], "List ins:at:");
  if (idx < 0) idx += len;
  if (idx < 0) idx = 0;
  if (idx > len) idx = len;
  vm.listAppend(rcvr, kOopNil);
  l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  for (int32_t i = len; i > idx; i--) {
    l->items[i] = l->items[i - 1];
  }
  l->items[idx] = args[0];
  return rcvr;
}

static Oop primListRem(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  for (uint32_t i = 0; i < l->len; i++) {
    if (vm.oopEquals(l->items[i], args[0])) {
      for (uint32_t j = i + 1u; j < l->len; j++) l->items[j - 1u] = l->items[j];
      l->len--;
      return rcvr;
    }
  }
  vm.toss("List rem: value <%s> not in List", vm.toStdString(args[0]).c_str());
}

static Oop primListExt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjList* rhs = reqList(vm, args[0], "List ext:");
  std::vector<Oop> copy(rhs->items, rhs->items + rhs->len);
  for (Oop v : copy) vm.listAppend(rcvr, v);
  return rcvr;
}

static Oop primListCat(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* a = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  ObjList* b = reqList(vm, args[0], "List cat:");
  std::vector<Oop> merged;
  merged.reserve(a->len + b->len);
  merged.insert(merged.end(), a->items, a->items + a->len);
  merged.insert(merged.end(), b->items, b->items + b->len);
  return vm.newListFrom(merged.data(), static_cast<uint32_t>(merged.size()));
}

static Oop primListCutTo(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(l->len);
  int32_t p = reqInt(vm, args[0], "List cut:to:");
  int32_t q = reqInt(vm, args[1], "List cut:to:");
  if (p < 0) p += len;
  if (q < 0) q += len;
  if (p < 0) p = 0;
  if (q > len) q = len;
  if (q <= p) return vm.newList(0);
  std::vector<Oop> sub(l->items + p, l->items + q);
  return vm.newListFrom(sub.data(), static_cast<uint32_t>(sub.size()));
}

static Oop primListCutLen(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  int32_t len = static_cast<int32_t>(l->len);
  int32_t p = reqInt(vm, args[0], "List cut:len:");
  int32_t n = reqInt(vm, args[1], "List cut:len:");
  if (p < 0) p += len;
  if (p < 0) p = 0;
  if (n <= 0 || p >= len) return vm.newList(0);
  if (p + n > len) n = len - p;
  std::vector<Oop> sub(l->items + p, l->items + p + n);
  return vm.newListFrom(sub.data(), static_cast<uint32_t>(sub.size()));
}

static Oop primListHead(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  if (l->len == 0) vm.toss("List head on empty List");
  return l->items[0];
}
static Oop primListLast(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  if (l->len == 0) vm.toss("List last on empty List");
  return l->items[l->len - 1u];
}
static Oop primListTail(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  if (l->len == 0) vm.toss("List tail on empty List");
  std::vector<Oop> sub(l->items + 1, l->items + l->len);
  return vm.newListFrom(sub.data(), static_cast<uint32_t>(sub.size()));
}
static Oop primListX(VM& vm, Oop rcvr, const Oop*, uint8_t) { Oop idx = makeInt30(0); return primListAt(vm, rcvr, &idx, 1); }
static Oop primListY(VM& vm, Oop rcvr, const Oop*, uint8_t) { Oop idx = makeInt30(1); return primListAt(vm, rcvr, &idx, 1); }
static Oop primListZ(VM& vm, Oop rcvr, const Oop*, uint8_t) { Oop idx = makeInt30(2); return primListAt(vm, rcvr, &idx, 1); }
static Oop primListW(VM& vm, Oop rcvr, const Oop*, uint8_t) { Oop idx = makeInt30(3); return primListAt(vm, rcvr, &idx, 1); }

static Oop primListHas(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  for (uint32_t i = 0; i < l->len; i++) {
    if (vm.oopEquals(l->items[i], args[0])) return kOopTrue;
  }
  return kOopFalse;
}

static Oop primListIdx(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  for (uint32_t i = 0; i < l->len; i++) {
    if (vm.oopEquals(l->items[i], args[0])) return makeInt30(static_cast<int32_t>(i));
  }
  return makeInt30(-1);
}

static Oop primListCount(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  int32_t c = 0;
  for (uint32_t i = 0; i < l->len; i++) {
    if (vm.oopEquals(l->items[i], args[0])) c++;
  }
  return makeInt30(c);
}

static Oop primListClear(VM&, Oop rcvr, const Oop*, uint8_t) {
  reinterpret_cast<ObjList*>(objFromOop(rcvr))->len = 0;
  return rcvr;
}

static Oop primListCopy(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::vector<Oop> sub(l->items, l->items + l->len);
  return vm.newListFrom(sub.data(), static_cast<uint32_t>(sub.size()));
}

static Oop primListRev(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::reverse(l->items, l->items + l->len);
  return rcvr;
}

static Oop primListReversed(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::vector<Oop> sub(l->items, l->items + l->len);
  std::reverse(sub.begin(), sub.end());
  return vm.newListFrom(sub.data(), static_cast<uint32_t>(sub.size()));
}

static Oop primListSort(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::sort(l->items, l->items + l->len, [&](Oop a, Oop b) {
    return vm.oopCompare(a, b) < 0;
  });
  return rcvr;
}

static Oop primListSorted(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  Oop cp = primListCopy(vm, rcvr, nullptr, 0);
  return primListSort(vm, cp, nullptr, 0);
}

static void flattenRec(VM& vm, Oop v, Oop dest, int depth) {
  if (depth > 32) return;
  if (isObj(v) && objFromOop(v)->format == OBJ_LIST) {
    auto* l = reinterpret_cast<ObjList*>(objFromOop(v));
    std::vector<Oop> copy(l->items, l->items + l->len);
    for (Oop item : copy) flattenRec(vm, item, dest, depth + 1);
  } else {
    vm.listAppend(dest, v);
  }
}

static Oop primListFlat(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  Oop dest = vm.newList(8);
  TempRootGuard rg(vm, dest);
  flattenRec(vm, rcvr, dest, 0);
  return dest;
}

static Oop primListJoinSep(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string sep = reqStdStr(vm, args[0], "List join:");
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::vector<Oop> copy(l->items, l->items + l->len);
  std::string out;
  for (size_t i = 0; i < copy.size(); i++) {
    if (i > 0) out += sep;
    out += vm.toStdString(copy[i]);
  }
  return vm.newStr(out.data(), out.size());
}

static Oop primListJoin(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::vector<Oop> copy(l->items, l->items + l->len);
  std::string out;
  for (size_t i = 0; i < copy.size(); i++) {
    if (i > 0) out.push_back(' ');
    out += vm.toStdString(copy[i]);
  }
  return vm.newStr(out.data(), out.size());
}

static Oop primListJam(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  std::vector<Oop> copy(l->items, l->items + l->len);
  std::string out;
  for (Oop v : copy) out += vm.toStdString(v);
  return vm.newStr(out.data(), out.size());
}

static void implodeRec(VM& vm, Oop v, std::string& out, int depth) {
  if (depth > 32) return;
  if (isNum(v)) {
    out.push_back(static_cast<char>(reqInt(vm, v, "implode") & 0xFF));
  } else if (isChar(v)) {
    out.push_back(static_cast<char>(asChar(v) & 0xFF));
  } else if (isObj(v) && objFromOop(v)->format == OBJ_LIST) {
    auto* l = reinterpret_cast<ObjList*>(objFromOop(v));
    for (uint32_t i = 0; i < l->len; i++) implodeRec(vm, l->items[i], out, depth + 1);
  } else {
    out += vm.toStdString(v);
  }
}

static Oop primListImplode(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  std::string out;
  implodeRec(vm, rcvr, out, 0);
  return vm.newStr(out.data(), out.size());
}

// Vector Math on List: +, -, *, /, abs, unit, dot: (scalar!), mul: (elementwise!), cross:
static Oop listVecOp(VM& vm, Oop rcvr, Oop rhs, char op) {
  auto* a = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  if (isNum(rhs)) {
    return scalarBroadcastList(vm, toFloatUnchecked(rhs), a, op, false);
  }
  ObjList* b = reqList(vm, rhs, "List vector op");
  uint32_t n = (a->len < b->len) ? a->len : b->len;
  std::vector<Oop> ai(a->items, a->items + n);
  std::vector<Oop> bi(b->items, b->items + n);
  Oop res = vm.newList(n);
  TempRootGuard rg(vm, res);
  for (uint32_t i = 0; i < n; i++) {
    float va = reqFloat(vm, ai[i], "List vector op");
    float vb = reqFloat(vm, bi[i], "List vector op");
    float r  = 0.0f;
    switch (op) {
      case '+': r = va + vb; break;
      case '-': r = va - vb; break;
      case '*': r = va * vb; break;
      case '/': r = va / vb; break;
    }
    if (isInt(ai[i]) && isInt(bi[i]) && op != '/' && r == std::floor(r) &&
        r >= static_cast<float>(kMinInt30) && r <= static_cast<float>(kMaxInt30)) {
      vm.listAppend(res, makeInt30(static_cast<int32_t>(r)));
    } else {
      vm.listAppend(res, vm.makeFloatChecked(r));
    }
  }
  return res;
}

static Oop primListAdd(VM& vm, Oop rcvr, const Oop* args, uint8_t) { return listVecOp(vm, rcvr, args[0], '+'); }
static Oop primListSub(VM& vm, Oop rcvr, const Oop* args, uint8_t) { return listVecOp(vm, rcvr, args[0], '-'); }
static Oop primListMul(VM& vm, Oop rcvr, const Oop* args, uint8_t) { return listVecOp(vm, rcvr, args[0], '*'); }
static Oop primListDiv(VM& vm, Oop rcvr, const Oop* args, uint8_t) { return listVecOp(vm, rcvr, args[0], '/'); }

static Oop primListAbs(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  float sumSq = 0.0f;
  for (uint32_t i = 0; i < l->len; i++) {
    float v = reqFloat(vm, l->items[i], "List abs");
    sumSq += v * v;
  }
  return vm.makeFloatChecked(std::sqrt(sumSq));
}

static Oop primListUnit(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* l = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  float sumSq = 0.0f;
  for (uint32_t i = 0; i < l->len; i++) {
    float v = reqFloat(vm, l->items[i], "List unit");
    sumSq += v * v;
  }
  float norm = std::sqrt(sumSq);
  if (norm == 0.0f) vm.toss("List unit on zero-length vector");
  return scalarBroadcastList(vm, norm, l, '/', false);
}

// Scalar inner product (fixing TTA's elementwise `dot:` surprise!)
static Oop primListDot(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* a = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  ObjList* b = reqList(vm, args[0], "List dot:");
  uint32_t n = (a->len < b->len) ? a->len : b->len;
  bool allInt = true;
  int64_t isum = 0;
  float   fsum = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    if (allInt && isInt(a->items[i]) && isInt(b->items[i])) {
      isum += static_cast<int64_t>(asInt30(a->items[i])) * static_cast<int64_t>(asInt30(b->items[i]));
      if (isum < kMinInt30 || isum > kMaxInt30) {
        allInt = false;
        fsum = static_cast<float>(isum);
      }
    } else {
      if (allInt) {
        allInt = false;
        fsum = static_cast<float>(isum);
      }
      fsum += reqFloat(vm, a->items[i], "List dot:") * reqFloat(vm, b->items[i], "List dot:");
    }
  }
  return allInt ? makeInt30(static_cast<int32_t>(isum)) : vm.makeFloatChecked(fsum);
}

static Oop primListCross(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* a = reinterpret_cast<ObjList*>(objFromOop(rcvr));
  ObjList* b = reqList(vm, args[0], "List cross:");
  if (a->len != 3 || b->len != 3) vm.toss("List cross: requires two 3-element Lists");
  float ax = reqFloat(vm, a->items[0], "cross:"), ay = reqFloat(vm, a->items[1], "cross:"), az = reqFloat(vm, a->items[2], "cross:");
  float bx = reqFloat(vm, b->items[0], "cross:"), by = reqFloat(vm, b->items[1], "cross:"), bz = reqFloat(vm, b->items[2], "cross:");
  Oop out[3] = {
    vm.makeFloatChecked(ay * bz - az * by),
    vm.makeFloatChecked(az * bx - ax * bz),
    vm.makeFloatChecked(ax * by - ay * bx),
  };
  return vm.newListFrom(out, 3);
}

// ---------------------------------------------------------------------------
// 6. Pythonic Dict Primitives
// ---------------------------------------------------------------------------
static Oop primDictClsNew(VM& vm, Oop, const Oop*, uint8_t) { return vm.newDict(8); }

static Oop primDictLen(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(static_cast<int32_t>(reinterpret_cast<ObjDict*>(objFromOop(rcvr))->len));
}

static Oop primDictAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return vm.dictGet(rcvr, args[0]);
}

static Oop primDictAtPut(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.dictPut(rcvr, args[0], args[1]);
  return rcvr;
}

static Oop primDictGetOr(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(rcvr));
  if (d->len == 0) return args[1];
  uint32_t h = vm.oopHash(args[0]);
  uint32_t mask = d->cap - 1u;
  uint32_t idx = h & mask;
  while (true) {
    DictEntry& e = d->entries[idx];
    if (e.key == kOopNull) return args[1];
    if (e.key != kOopSuper && e.hash == h && vm.oopEquals(e.key, args[0])) return e.val;
    idx = (idx + 1u) & mask;
  }
}

static Oop primDictHas(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(rcvr));
  if (d->len == 0) return kOopFalse;
  uint32_t h = vm.oopHash(args[0]);
  uint32_t mask = d->cap - 1u;
  uint32_t idx = h & mask;
  while (true) {
    DictEntry& e = d->entries[idx];
    if (e.key == kOopNull) return kOopFalse;
    if (e.key != kOopSuper && e.hash == h && vm.oopEquals(e.key, args[0])) return kOopTrue;
    idx = (idx + 1u) & mask;
  }
}

static Oop primDictKeys(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  std::vector<Oop> keys;
  vm.dictSortedKeys(rcvr, keys);
  return vm.newListFrom(keys.data(), static_cast<uint32_t>(keys.size()));
}

static Oop primDictVals(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  std::vector<Oop> keys;
  vm.dictSortedKeys(rcvr, keys);
  Oop res = vm.newList(static_cast<uint32_t>(keys.size()));
  TempRootGuard rg(vm, res);
  for (Oop k : keys) vm.listAppend(res, vm.dictGet(rcvr, k));
  return res;
}

static Oop primDictItems(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  std::vector<Oop> keys;
  vm.dictSortedKeys(rcvr, keys);
  Oop res = vm.newList(static_cast<uint32_t>(keys.size()));
  TempRootGuard rg(vm, res);
  for (Oop k : keys) {
    Oop pair[2] = {k, vm.dictGet(rcvr, k)};
    vm.listAppend(res, vm.newListFrom(pair, 2));
  }
  return res;
}

static Oop primDictDel(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return vm.dictDel(rcvr, args[0]);
}

static Oop primDictUpdate(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  ObjDict* rhs = reqDict(vm, args[0], "Dict update:");
  std::vector<std::pair<Oop, Oop>> kvs;
  for (uint32_t i = 0; i < rhs->cap; i++) {
    Oop k = rhs->entries[i].key;
    if (k != kOopNull && k != kOopSuper) kvs.emplace_back(k, rhs->entries[i].val);
  }
  for (auto& kv : kvs) vm.dictPut(rcvr, kv.first, kv.second);
  return rcvr;
}

static Oop primDictCopy(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(rcvr));
  Oop cp = vm.newDict(d->cap);
  TempRootGuard rg(vm, cp);
  primDictUpdate(vm, cp, &rcvr, 1);
  return cp;
}

static Oop primDictClear(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* d = reinterpret_cast<ObjDict*>(objFromOop(rcvr));
  for (uint32_t i = 0; i < d->cap; i++) {
    d->entries[i].key  = kOopNull;
    d->entries[i].val  = kOopNil;
    d->entries[i].hash = 0;
  }
  d->len  = 0;
  d->used = 0;
  return rcvr;
}

// ---------------------------------------------------------------------------
// 7. Blk Primitives
// ---------------------------------------------------------------------------
static Oop primBlkValue(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.evalBlk(rcvr, nullptr, 0);
}
static Oop primBlkValue1(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return vm.evalBlk(rcvr, args, 1);
}
static Oop primBlkValue2(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  return vm.evalBlk(rcvr, args, 2);
}
static Oop primBlkStoreP0(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.blkStoreKV(rcvr, kOopNil, args[0]);
  return rcvr;
}
static Oop primBlkStoreKV(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  vm.blkStoreKV(rcvr, args[0], args[1]);
  return rcvr;
}

// ---------------------------------------------------------------------------
// 8. Cls, Meth, Frame, Usr Reflection Primitives
// ---------------------------------------------------------------------------
static Oop primClsClsAll(VM& vm, Oop, const Oop*, uint8_t) {
  Oop d = vm.newDict(vm.classCount * 2u + 8u);
  TempRootGuard rg(vm, d);
  for (uint16_t i = 1; i <= vm.classCount; i++) {
    Oop cOop = vm.classById[i];
    if (!isObj(cOop)) continue;
    auto* c = reinterpret_cast<ObjCls*>(objFromOop(cOop));
    vm.dictPut(d, vm.newStr(vm.symCStr(c->nameSym)), cOop);
  }
  return d;
}

static Oop primClsClsAt(VM& vm, Oop, const Oop* args, uint8_t) {
  size_t slen = 0;
  const char* s = reqCStr(vm, args[0], "Cls at:", &slen);
  Oop found = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(s, slen)));
  return (isObj(found) && objFromOop(found)->format == OBJ_CLS) ? found : kOopNil;
}

static Oop primClsName(VM&, Oop rcvr, const Oop*, uint8_t) {
  return reinterpret_cast<ObjCls*>(objFromOop(rcvr))->nameStr;
}

static Oop primClsSuper(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  return (c->superClsId == 0) ? kOopNil : vm.classById[c->superClsId];
}

static Oop primClsSubs(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  Oop cp = primListCopy(vm, c->subsList, nullptr, 0);
  return primListSort(vm, cp, nullptr, 0);
}

static Oop primClsDefSub(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* sup = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  std::string name = reqStdStr(vm, args[0], "defSub:");
  return vm.defineClass(name.c_str(), sup->clsId, sup->isUsrCls);
}

static Oop primClsVars(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  return primListCopy(vm, c->myVarsList, nullptr, 0);
}

static Oop primClsDefVars(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  std::string vs = reqStdStr(vm, args[0], "defVars:");
  vm.defineVars(c, vs.c_str(), true);
  return rcvr;
}

static Oop primClsMeths(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  auto* md = reinterpret_cast<ObjDict*>(objFromOop(c->methDict));
  Oop out = vm.newDict(md->len * 2u + 4u);
  TempRootGuard rg(vm, out);
  for (uint32_t i = 0; i < md->cap; i++) {
    Oop k = md->entries[i].key;
    if (k == kOopNull || k == kOopSuper) continue;
    Oop m = md->entries[i].val;
    if (isSym(k)) {
      vm.dictPut(out, vm.newStr(vm.symCStr(asSym(k))), m);
    } else {
      vm.dictPut(out, k, m);
    }
  }
  return out;
}

static Oop primClsAt(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  size_t slen = 0;
  const char* s = reqCStr(vm, args[0], "Cls>>at:", &slen);
  return vm.dictGet(c->methDict, makeSym(vm.internSym(s, slen)));
}

static Oop primClsDefC(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  std::string sel  = reqStdStr(vm, args[0], "def:c:");
  std::string code = reqStdStr(vm, args[1], "def:c:");
  vm.addUsrMeth(c, sel.c_str(), "", code.data(), code.size(), false, true);
  return rcvr;
}

static Oop primClsDefMeth4(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  std::string sel  = reqStdStr(vm, args[0], "defMeth:a:d:c:");
  std::string abbr = isNil(args[1]) ? "" : reqStdStr(vm, args[1], "defMeth:a:d:c:");
  std::string code = reqStdStr(vm, args[3], "defMeth:a:d:c:");
  vm.addUsrMeth(c, sel.c_str(), abbr.c_str(), code.data(), code.size(), false, true);
  return rcvr;
}

static Oop primClsTrace(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  reinterpret_cast<ObjCls*>(objFromOop(rcvr))->trace = vm.truth(args[0]);
  return rcvr;
}

// Meth
static Oop primMethOnCls(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.classById[reinterpret_cast<ObjMeth*>(objFromOop(rcvr))->onClsId];
}
static Oop primMethName(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.newStr(vm.symCStr(reinterpret_cast<ObjMeth*>(objFromOop(rcvr))->nameSym));
}
static Oop primMethAbbrev(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  return vm.newStr(vm.symCStr(reinterpret_cast<ObjMeth*>(objFromOop(rcvr))->abbrevSym));
}
static Oop primMethDoc(VM& vm, Oop, const Oop*, uint8_t) {
  return vm.newStr("", 0);
}
static Oop primUsrMethSrc(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* m = reinterpret_cast<ObjMeth*>(objFromOop(rcvr));
  return vm.newStr(m->source ? m->source : "", m->sourceLen);
}
static Oop primMethApply(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* m = reinterpret_cast<ObjMeth*>(objFromOop(rcvr));
  ObjList* av = reqList(vm, args[2], "applyFrame:receiver:args:");
  return vm.invokeMeth(m, args[1], av->items, static_cast<uint8_t>(av->len));
}

// Frame
static Oop primFramePrev(VM&, Oop rcvr, const Oop*, uint8_t) {
  return reinterpret_cast<ObjFrame*>(objFromOop(rcvr))->prevFrame;
}
static Oop primFrameRcvr(VM&, Oop rcvr, const Oop*, uint8_t) {
  return reinterpret_cast<ObjFrame*>(objFromOop(rcvr))->self;
}
static Oop primFrameLevel(VM&, Oop rcvr, const Oop*, uint8_t) {
  return makeInt30(reinterpret_cast<ObjFrame*>(objFromOop(rcvr))->level);
}

// Usr & UsrCls
static Oop primUsrClsNew(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  return vm.newUsr(c->clsId);
}

static Oop primUsrOid(VM& vm, Oop rcvr, const Oop*, uint8_t) {
  auto* u = reinterpret_cast<ObjSlots*>(objFromOop(rcvr));
  if (u->oid == 0) u->oid = vm.nextOid++;
  ObjCls* c = vm.clsById(u->hdr.clsId);
  if (c) c->lruCache[u->oid % kUsrLruSize] = rcvr;
  return makeInt30(static_cast<int32_t>(u->oid));
}

static Oop primUsrOname(VM& vm, Oop rcvr, const Oop* args, uint8_t na) {
  auto* u = reinterpret_cast<ObjSlots*>(objFromOop(rcvr));
  if (u->onameSym != 0) return vm.newStr(vm.symCStr(u->onameSym));
  Oop oid = primUsrOid(vm, rcvr, args, na);
  return primStr(vm, oid, nullptr, 0);
}

static Oop primUsrOpath(VM& vm, Oop rcvr, const Oop* args, uint8_t na) {
  Oop nm = primUsrOname(vm, rcvr, args, na);
  std::string s = vm.toStdString(nm) + "@" + vm.toStdString(oopFromObj(&vm.oopCls(rcvr)->hdr));
  return vm.newStr(s.data(), s.size());
}

static Oop primUsrClsFind(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  std::string key = reqStdStr(vm, args[0], "UsrCls find:");
  // 1. Check numeric oid in LRU cache
  char* endp = nullptr;
  long oid = std::strtol(key.c_str(), &endp, 10);
  if (endp && *endp == '\0' && oid > 0) {
    Oop cand = c->lruCache[oid % kUsrLruSize];
    if (isObj(cand) && objFromOop(cand)->format == OBJ_SLOTS) {
      if (reinterpret_cast<ObjSlots*>(objFromOop(cand))->oid == static_cast<uint32_t>(oid)) {
        return cand;
      }
    }
    return kOopNil;
  }
  // 2. Check saved `inst` dictionary (`name` or `name_<world>`)
  uint32_t ksym = vm.internSym(key.c_str(), key.size());
  Oop saved = vm.dictGet(c->savedInstsDict, makeSym(ksym));
  if (saved == kOopNil && key.find('_') == std::string::npos) {
    std::string wkey = key + "_" + vm.worldName;
    ksym  = vm.internSym(wkey.c_str(), wkey.size());
    saved = vm.dictGet(c->savedInstsDict, makeSym(ksym));
  }
  if (saved == kOopNil) return kOopNil;
  if (isObj(saved) && objFromOop(saved)->format == OBJ_SLOTS) return saved;
  if (isObj(saved) && objFromOop(saved)->format == OBJ_STR) {
    auto* codeStr = reinterpret_cast<ObjStr*>(objFromOop(saved));
    std::string code(codeStr->chars, codeStr->hdr.size);
    Oop inst = vm.newUsr(c->clsId);
    TempRootGuard rg(vm, inst);
    reinterpret_cast<ObjSlots*>(objFromOop(inst))->onameSym = ksym;
    Oop sd = vm.ensureSavedInstsDict(c);
    vm.dictPut(sd, makeSym(ksym), inst);
    vm.evalCode(inst, code.c_str());
    return inst;
  }
  return kOopNil;
}

// ---------------------------------------------------------------------------
// 9. Json & Hex Primitives
// ---------------------------------------------------------------------------
static void jsonAppendString(const std::string& s, std::string& out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", c);
          out += hex;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

static void jsonEncodeRec(VM& vm, Oop v, std::string& out, int depth) {
  if (depth > 32 || isNil(v)) {
    out += "null";
    return;
  }
  if (isInt(v)) {
    out += std::to_string(asInt30(v));
    return;
  }
  if (isFloat(v)) {
    float f = asFloat(v);
    if (!std::isfinite(f)) { out += "null"; return; }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.7g", static_cast<double>(f));
    out += buf;
    return;
  }
  if (isObj(v)) {
    ObjHeader* h = objFromOop(v);
    if (h->format == OBJ_LIST) {
      auto* l = reinterpret_cast<ObjList*>(h);
      out.push_back('[');
      for (uint32_t i = 0; i < l->len; i++) {
        if (i > 0) out.push_back(',');
        jsonEncodeRec(vm, l->items[i], out, depth + 1);
      }
      out.push_back(']');
      return;
    }
    if (h->format == OBJ_DICT) {
      std::vector<Oop> keys;
      vm.dictSortedKeys(v, keys);
      out.push_back('{');
      for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) out.push_back(',');
        jsonAppendString(vm.toStdString(keys[i]), out);
        out.push_back(':');
        jsonEncodeRec(vm, vm.dictGet(v, keys[i]), out, depth + 1);
      }
      out.push_back('}');
      return;
    }
  }
  jsonAppendString(vm.toStdString(v), out);
}

struct JsonParser {
  VM&         vm;
  const char* s;
  size_t      len;
  size_t      p = 0;

  void skipWs() {
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
  }

  Oop parseVal() {
    skipWs();
    if (p >= len) vm.toss("JSON unexpected EOF");
    char c = s[p];
    if (c == 'n' && p + 4 <= len && std::memcmp(s + p, "null", 4) == 0)  { p += 4; return kOopNil; }
    if (c == 't' && p + 4 <= len && std::memcmp(s + p, "true", 4) == 0)  { p += 4; return kOopTrue; }
    if (c == 'f' && p + 5 <= len && std::memcmp(s + p, "false", 5) == 0) { p += 5; return kOopFalse; }
    if (c == '"') return parseStr();
    if (c == '[') return parseArr();
    if (c == '{') return parseObj();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNum();
    vm.toss("JSON syntax error at offset %u", static_cast<unsigned>(p));
  }

  Oop parseStr() {
    p++;  // skip '"'
    std::string out;
    while (p < len && s[p] != '"') {
      if (s[p] == '\\' && p + 1 < len) {
        char e = s[p + 1];
        p += 2;
        switch (e) {
          case '"':  out.push_back('"');  break;
          case '\\': out.push_back('\\'); break;
          case '/':  out.push_back('/');  break;
          case 'b':  out.push_back('\b'); break;
          case 'f':  out.push_back('\f'); break;
          case 'n':  out.push_back('\n'); break;
          case 'r':  out.push_back('\r'); break;
          case 't':  out.push_back('\t'); break;
          case 'u':
            if (p + 4 <= len) {
              char hx[5] = {s[p], s[p+1], s[p+2], s[p+3], 0};
              unsigned cp = static_cast<unsigned>(std::strtoul(hx, nullptr, 16));
              out.push_back(static_cast<char>(cp & 0xFFu));
              p += 4;
            }
            break;
          default: out.push_back(e); break;
        }
      } else {
        out.push_back(s[p++]);
      }
    }
    if (p >= len || s[p] != '"') vm.toss("JSON unterminated string");
    p++;
    return vm.newStr(out.data(), out.size());
  }

  Oop parseNum() {
    size_t start = p;
    bool isFlt = false;
    if (s[p] == '-') p++;
    while (p < len && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
    if (p < len && s[p] == '.') {
      isFlt = true;
      p++;
      while (p < len && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
    }
    if (p < len && (s[p] == 'e' || s[p] == 'E')) {
      isFlt = true;
      p++;
      if (p < len && (s[p] == '+' || s[p] == '-')) p++;
      while (p < len && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
    }
    std::string numStr(s + start, p - start);
    if (!isFlt) {
      int64_t iv = std::strtoll(numStr.c_str(), nullptr, 10);
      if (iv >= kMinInt30 && iv <= kMaxInt30) return makeInt30(static_cast<int32_t>(iv));
    }
    return vm.makeFloatChecked(std::strtof(numStr.c_str(), nullptr));
  }

  Oop parseArr() {
    p++;  // skip '['
    Oop arr = vm.newList(4);
    TempRootGuard rg(vm, arr);
    skipWs();
    if (p < len && s[p] == ']') { p++; return arr; }
    while (true) {
      Oop item = parseVal();
      vm.listAppend(arr, item);
      skipWs();
      if (p < len && s[p] == ',') { p++; continue; }
      if (p < len && s[p] == ']') { p++; break; }
      vm.toss("JSON expected ',' or ']'");
    }
    return arr;
  }

  Oop parseObj() {
    p++;  // skip '{'
    Oop d = vm.newDict(8);
    TempRootGuard rg(vm, d);
    skipWs();
    if (p < len && s[p] == '}') { p++; return d; }
    while (true) {
      skipWs();
      if (p >= len || s[p] != '"') vm.toss("JSON expected string key");
      Oop k = parseStr();
      rg.push(k);
      skipWs();
      if (p >= len || s[p] != ':') vm.toss("JSON expected ':'");
      p++;
      Oop v = parseVal();
      vm.dictPut(d, k, v);
      skipWs();
      if (p < len && s[p] == ',') { p++; continue; }
      if (p < len && s[p] == '}') { p++; break; }
      vm.toss("JSON expected ',' or '}'");
    }
    return d;
  }
};

static Oop primJsonEn(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string out;
  jsonEncodeRec(vm, args[0], out, 0);
  return vm.newStr(out.data(), out.size());
}
static Oop primJsonDe(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string src = reqStdStr(vm, args[0], "Json de:");
  JsonParser jp{vm, src.data(), src.size(), 0};
  return jp.parseVal();
}

static Oop primHexEn(VM& vm, Oop, const Oop* args, uint8_t) {
  static const char kHex[] = "0123456789abcdef";
  const uint8_t* ptr = nullptr;
  size_t len = 0;
  std::string tmp;
  if (isObj(args[0]) && objFromOop(args[0])->format == OBJ_BYTES) {
    auto* b = reinterpret_cast<ObjBytes*>(objFromOop(args[0]));
    ptr = b->bytes;
    len = b->hdr.size;
  } else {
    tmp = reqStdStr(vm, args[0], "Hex en:");
    ptr = reinterpret_cast<const uint8_t*>(tmp.data());
    len = tmp.size();
  }
  std::string out(len * 2u, '\0');
  for (size_t i = 0; i < len; i++) {
    out[2 * i]     = kHex[ptr[i] >> 4];
    out[2 * i + 1] = kHex[ptr[i] & 0x0Fu];
  }
  return vm.newStr(out.data(), out.size());
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static Oop primHexDe(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string src = reqStdStr(vm, args[0], "Hex de:");
  std::vector<uint8_t> bytes;
  int hi = -1;
  for (char c : src) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    int v = hexNibble(c);
    if (v < 0) vm.toss("Hex de: invalid hex char '%c'", c);
    if (hi < 0) {
      hi = v;
    } else {
      bytes.push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  if (hi >= 0) vm.toss("Hex de: odd number of hex digits");
  return vm.newBytes(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// 10. Sys & File Primitives
// ---------------------------------------------------------------------------
static Oop primSysMillis(VM& vm, Oop, const Oop*, uint8_t) {
  return makeInt64OrFloat(vm.nowMillis());
}
static Oop primSysSecs(VM& vm, Oop, const Oop*, uint8_t) {
  if (vm.host.millisFn) {
    return vm.makeFloatChecked(static_cast<float>(vm.nowMillis()) * 0.001f);
  }
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  float s = static_cast<float>(ts.tv_sec % 1000000) + static_cast<float>(ts.tv_nsec) * 1e-9f;
  return vm.makeFloatChecked(s);
}
static Oop primSysSleep(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  float secs = reqFloat(vm, args[0], "Sys sleep:");
  if (secs > 0.0f) {
#ifdef ARDUINO
    delay(static_cast<uint32_t>(secs * 1000.0f));
#else
    struct timespec req{};
    req.tv_sec  = static_cast<time_t>(secs);
    req.tv_nsec = static_cast<long>((secs - static_cast<float>(req.tv_sec)) * 1e9f);
    nanosleep(&req, nullptr);
#endif
  }
  return rcvr;
}
static Oop primSysSaid(VM& vm, Oop, const Oop*, uint8_t) {
  Oop res = vm.newList(static_cast<uint32_t>(vm.logCount));
  TempRootGuard rg(vm, res);
  for (size_t i = 0; i < vm.logCount; i++) {
    size_t idx = (vm.logWriteIdx + kLogRingSize - 1u - i) % kLogRingSize;
    vm.listAppend(res, vm.newStr(vm.logRing[idx].data(), vm.logRing[idx].size()));
  }
  return res;
}
static Oop primSysFind(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string path = reqStdStr(vm, args[0], "Sys find:");
  size_t atPos = path.find('@');
  if (atPos == std::string::npos) return kOopNil;
  std::string idPart  = path.substr(0, atPos);
  std::string clsPart = path.substr(atPos + 1);
  Oop clsOop = vm.dictGet(vm.globalsDict, makeSym(vm.internSym(clsPart.c_str(), clsPart.size())));
  if (!isObj(clsOop) || objFromOop(clsOop)->format != OBJ_CLS) return kOopNil;
  if (idPart.empty()) return clsOop;
  Oop idStr = vm.newStr(idPart.data(), idPart.size());
  TempRootGuard rg(vm, idStr);
  return primUsrClsFind(vm, clsOop, &idStr, 1);
}
static Oop primSysWorldName(VM& vm, Oop, const Oop*, uint8_t) {
  return vm.newStr(vm.worldName.data(), vm.worldName.size());
}
static Oop primSysWorldFile(VM& vm, Oop, const Oop*, uint8_t) {
  return vm.newStr(vm.worldPath.data(), vm.worldPath.size());
}
static Oop primSysGc(VM& vm, Oop, const Oop*, uint8_t) {
  vm.runGC();
  return makeInt64OrFloat(static_cast<int64_t>(vm.heap.totalBytes - vm.heap.usedBytes));
}
static Oop primSysFreeMem(VM& vm, Oop, const Oop*, uint8_t) {
  return makeInt64OrFloat(static_cast<int64_t>(vm.heap.totalBytes - vm.heap.usedBytes));
}
static Oop primSysUsedMem(VM& vm, Oop, const Oop*, uint8_t) {
  return makeInt64OrFloat(static_cast<int64_t>(vm.heap.usedBytes));
}
static Oop primSysFail(VM& vm, Oop, const Oop*, uint8_t) {
  vm.toss("Sys fail");
}
static Oop primSysFail1(VM& vm, Oop, const Oop* args, uint8_t) {
  vm.toss("%s", vm.toStdString(args[0]).c_str());
}
static Oop primSysStateDiff(VM& vm, Oop, const Oop*, uint8_t) {
  std::string diff = serializeClassState(vm);
  return vm.newStr(diff.data(), diff.size());
}
static Oop primSysSaveState(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string nm = reqStdStr(vm, args[0], "Sys saveState:");
  std::string resolved, err;
  if (!saveClassState(vm, nm.c_str(), &resolved, &err)) {
    vm.toss("Sys saveState: %s", err.c_str());
  }
  return vm.newStr(resolved.data(), resolved.size());
}
static Oop primClsIsPrimordial(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* c = reinterpret_cast<ObjCls*>(objFromOop(rcvr));
  return makeBool(c->isPrimordial != 0);
}
static Oop primMethIsPrimordial(VM&, Oop rcvr, const Oop*, uint8_t) {
  auto* m = reinterpret_cast<ObjMeth*>(objFromOop(rcvr));
  return makeBool(m->isPrimordial);
}

static std::string normalizeFilePath(VM& vm, const std::string& p) {
  if (p.empty()) return vm.host.vfsRead ? "/sd" : ".";
  if (p[0] == '/' || p[0] == '.') return p;
  if (vm.host.vfsRead) return std::string("/sd/") + p;
  return p;
}

static Oop listDirHelper(VM& vm, const std::string& rawDir) {
  std::string dir = normalizeFilePath(vm, rawDir);
  std::vector<std::pair<std::string, uint32_t>> entries;
  vm.listVfsFiles(dir.c_str(), entries);
  std::sort(entries.begin(), entries.end());
  Oop res = vm.newList(static_cast<uint32_t>(entries.size()));
  TempRootGuard rg(vm, res);
  for (const auto& kv : entries) {
    Oop ns = vm.newStr(kv.first.data(), kv.first.size());
    TempRootGuard rg2(vm, ns);
    Oop tup[3] = {ns, makeInt30(0), makeInt64OrFloat(kv.second)};
    vm.listAppend(res, vm.newListFrom(tup, 3));
  }
  return res;
}

static Oop primFileDir(VM& vm, Oop, const Oop*, uint8_t) {
  return listDirHelper(vm, "");
}
static Oop primFileDir1(VM& vm, Oop, const Oop* args, uint8_t) {
  return listDirHelper(vm, reqStdStr(vm, args[0], "File dir:"));
}
static Oop primFileExists(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File exists:"));
  std::string tmp;
  return makeBool(vm.readVfsFile(p.c_str(), tmp));
}
static Oop primFileRead(VM& vm, Oop, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File read:"));
  std::string content;
  if (!vm.readVfsFile(p.c_str(), content)) {
    vm.toss("File read: cannot open <%s>", p.c_str());
  }
  // Normalize \r\n -> \n
  std::string norm;
  norm.reserve(content.size());
  for (size_t i = 0; i < content.size(); i++) {
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') continue;
    norm.push_back(content[i]);
  }
  return vm.newStr(norm.data(), norm.size());
}
static Oop primFileWrite(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File write:value:"));
  if (p.rfind("/flash/", 0) == 0 || p == "/flash") {
    vm.toss("File write: /flash is immutable; write to /sd instead");
  }
  std::string data = vm.toStdString(args[1]);
  if (!vm.writeVfsFile(p.c_str(), data.data(), data.size(), false)) {
    vm.toss("File write: failed writing <%s>", p.c_str());
  }
  return rcvr;
}
static Oop primFileAppend(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File append:value:"));
  if (p.rfind("/flash/", 0) == 0 || p == "/flash") {
    vm.toss("File append: /flash is immutable; write to /sd instead");
  }
  std::string data = vm.toStdString(args[1]);
  if (!vm.writeVfsFile(p.c_str(), data.data(), data.size(), true)) {
    vm.toss("File append: failed appending <%s>", p.c_str());
  }
  return rcvr;
}
static Oop primFileDelete(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File delete:"));
  if (p.rfind("/flash/", 0) == 0 || p == "/flash") {
    vm.toss("File delete: /flash is immutable");
  }
  vm.deleteVfsFile(p.c_str());
  return rcvr;
}
static Oop primFileLoad(VM& vm, Oop rcvr, const Oop* args, uint8_t) {
  std::string p = normalizeFilePath(vm, reqStdStr(vm, args[0], "File load:"));
  std::string err;
  if (!loadWorldFile(vm, vm.worldName.c_str(), p.c_str(), &err)) {
    vm.toss("File load <%s>: %s", p.c_str(), err.c_str());
  }
  return rcvr;
}
static Oop primFileRun(VM& vm, Oop, const Oop* args, uint8_t) {
  Oop srcStr = primFileRead(vm, kOopNil, args, 1);
  TempRootGuard rg(vm, srcStr);
  auto* s = reinterpret_cast<ObjStr*>(objFromOop(srcStr));
  std::string code(s->chars, s->hdr.size);
  Oop tmp = vm.newUsr(CLS_TMP);
  rg.push(tmp);
  return vm.evalCode(tmp, code.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Master Registration of Built-in Core Primitives
// ---------------------------------------------------------------------------
void installCorePrimitives(VM& vm) {
  // 1. Ur & Obj
  vm.addPrim(CLS_UR,  "cls",        "",         primCls);
  vm.addPrim(CLS_UR,  "str",        "",         primStr);
  vm.addPrim(CLS_UR,  "repr",       "",         primRepr);
  vm.addPrim(CLS_UR,  "truth",      "",         primTruth);
  vm.addPrim(CLS_UR,  "hash",       "",         primHash);
  vm.addPrim(CLS_UR,  "equals:",    "",         primEquals);
  vm.addPrim(CLS_UR,  "does:",      "respondsTo:", primDoes);
  vm.addPrim(CLS_UR,  "peekVars",   "",         primPeekVars);
  vm.addPrim(CLS_UR,  "pokeVars:",  "",         primPokeVars);

  vm.addPrim(CLS_OBJ, "is:",        "",         primIs);
  vm.addPrim(CLS_OBJ, "null",       "",         primNull);
  vm.addPrim(CLS_OBJ, "isa:",       "",         primIsa);
  vm.addPrim(CLS_OBJ, "sysHash",    "",         primSysHash);
  vm.addPrim(CLS_OBJ, "not",        "",         primNot);
  vm.addPrim(CLS_OBJ, "==",         "eq:",      primEquals);
  vm.addPrim(CLS_OBJ, "!=",         "ne:",      primNe);
  vm.addPrim(CLS_OBJ, "<",          "lt:",      primLt);
  vm.addPrim(CLS_OBJ, "<=",         "le:",      primLe);
  vm.addPrim(CLS_OBJ, ">",          "gt:",      primGt);
  vm.addPrim(CLS_OBJ, ">=",         "ge:",      primGe);
  vm.addPrim(CLS_OBJ, "nearestInt", "",         primNearestInt);
  vm.addPrim(CLS_OBJ, "must",       "",         primMust);
  vm.addPrim(CLS_OBJ, "must:",      "",         primMust1);
  vm.addPrim(CLS_OBJ, "cant",       "",         primCant);
  vm.addPrim(CLS_OBJ, "cant:",      "",         primCant1);
  vm.addPrim(CLS_OBJ, "err",        "",         primErr);
  vm.addPrim(CLS_OBJ, "err:",       "",         primErr1);
  vm.addPrim(CLS_OBJ, "say",        "",         primSay);
  vm.addPrim(CLS_OBJ, "say:",       "",         primSay1);
  vm.addPrim(CLS_OBJ, "eval:",      "",         primEval);
  vm.addPrim(CLS_OBJ, "eval:arg:",  "",         primEval1);
  vm.addPrim(CLS_OBJ, "eval:arg:arg:", "",      primEval2);
  vm.addPrim(CLS_OBJ, "send:",      "",         primSend);
  vm.addPrim(CLS_OBJ, "send:arg:",  "",         primSend1);
  vm.addPrim(CLS_OBJ, "apply:args:","send:args:", primApplyArgs);

  // 2. Num & NumCls
  vm.addPrim(CLS_NUM, "+",          "",         primNumAdd);
  vm.addPrim(CLS_NUM, "-",          "",         primNumSub);
  vm.addPrim(CLS_NUM, "*",          "",         primNumMul);
  vm.addPrim(CLS_NUM, "/",          "",         primNumDiv);
  vm.addPrim(CLS_NUM, "%",          "",         primNumMod);
  vm.addPrim(CLS_NUM, "idiv:",      "",         primNumIdiv);
  vm.addPrim(CLS_NUM, "imod:",      "",         primNumImod);
  vm.addPrim(CLS_NUM, "pow:",       "",         primNumPow);
  vm.addPrim(CLS_NUM, "&",          "",         primNumBitAnd);
  vm.addPrim(CLS_NUM, "|",          "",         primNumBitOr);
  vm.addPrim(CLS_NUM, "^",          "",         primNumBitXor);
  vm.addPrim(CLS_NUM, "<<",         "shl:",     primNumShl);
  vm.addPrim(CLS_NUM, ">>",         "shr:",     primNumShr);
  vm.addPrim(CLS_NUM, "neg",        "",         primNumNeg);
  vm.addPrim(CLS_NUM, "abs",        "",         primNumAbs);
  vm.addPrim(CLS_NUM, "sgn",        "",         primNumSgn);
  vm.addPrim(CLS_NUM, "sq",         "",         primNumSq);
  vm.addPrim(CLS_NUM, "sqrt",       "",         primNumSqrt);
  vm.addPrim(CLS_NUM, "int",        "trunc",    primNumInt);
  vm.addPrim(CLS_NUM, "floor",      "",         primNumFloor);
  vm.addPrim(CLS_NUM, "ceil",       "",         primNumCeil);
  vm.addPrim(CLS_NUM, "round",      "",         primNumRound);
  vm.addPrim(CLS_NUM, "float",      "",         primNumFloat);
  vm.addPrim(CLS_NUM, "num",        "",         primNumSelf);
  vm.addPrim(CLS_NUM, "sin",        "",         primNumSin);
  vm.addPrim(CLS_NUM, "cos",        "",         primNumCos);
  vm.addPrim(CLS_NUM, "tan",        "",         primNumTan);
  vm.addPrim(CLS_NUM, "asin",       "",         primNumAsin);
  vm.addPrim(CLS_NUM, "acos",       "",         primNumAcos);
  vm.addPrim(CLS_NUM, "atan",       "",         primNumAtan);
  vm.addPrim(CLS_NUM, "atan2:",     "",         primNumAtan2);
  vm.addPrim(CLS_NUM, "sinh",       "",         primNumSinh);
  vm.addPrim(CLS_NUM, "cosh",       "",         primNumCosh);
  vm.addPrim(CLS_NUM, "tanh",       "",         primNumTanh);
  vm.addPrim(CLS_NUM, "ln",         "log",      primNumLn);
  vm.addPrim(CLS_NUM, "log10",      "",         primNumLog10);
  vm.addPrim(CLS_NUM, "log2",       "",         primNumLog2);
  vm.addPrim(CLS_NUM, "exp",        "",         primNumExp);
  vm.addPrim(CLS_NUM, "deg",        "",         primNumDeg);
  vm.addPrim(CLS_NUM, "rad",        "",         primNumRad);
  vm.addPrim(CLS_NUM, "min:",       "",         primNumMin);
  vm.addPrim(CLS_NUM, "max:",       "",         primNumMax);
  vm.addPrim(CLS_NUM, "clamp:to:",  "",         primNumClamp);
  vm.addPrim(CLS_NUM, "lerp:t:",    "",         primNumLerp);
  vm.addPrim(CLS_NUM, "chr",        "",         primNumChr);
  vm.addPrim(CLS_NUM, "char",       "",         primNumChar);
  vm.addPrim(CLS_NUM, "fmt:",       "",         primNumFmt);
  vm.addPrim(CLS_NUM, "range",      "",         primNumRange);
  vm.addPrim(CLS_NUM, "to:",        "",         primNumTo);

  vm.addPrim(CLS_NUM_CLS, "pi",     "",         primNumClsPi);
  vm.addPrim(CLS_NUM_CLS, "tau",    "",         primNumClsTau);
  vm.addPrim(CLS_NUM_CLS, "e",      "",         primNumClsE);
  vm.addPrim(CLS_NUM_CLS, "rand",   "",         primNumClsRand);
  vm.addPrim(CLS_NUM_CLS, "rand:",  "",         primNumClsRand1);
  vm.addPrim(CLS_NUM_CLS, "seed:",  "",         primNumClsSeed);

  // 3. Char & CharCls
  vm.addPrim(CLS_CHAR_CLS, "new:",  "",         primCharClsNew);
  vm.addPrim(CLS_CHAR, "ord",       "int",      primCharOrd);
  vm.addPrim(CLS_CHAR, "chr",       "",         primCharStr);
  vm.addPrim(CLS_CHAR, "isAlpha",   "",         primCharIsAlpha);
  vm.addPrim(CLS_CHAR, "isDigit",   "",         primCharIsDigit);
  vm.addPrim(CLS_CHAR, "isSpace",   "",         primCharIsSpace);
  vm.addPrim(CLS_CHAR, "upper",     "upp",      primCharUpper);
  vm.addPrim(CLS_CHAR, "lower",     "low",      primCharLower);

  // 4. Str, Buf, Bytes, Rex
  vm.addPrim(CLS_STR_CLS, "new",    "",         primStrClsNew);
  vm.addPrim(CLS_STR, "length",     "len",      primStrLen);
  vm.addPrim(CLS_STR, "at:",        "",         primStrAt);
  vm.addPrim(CLS_STR, "charAt:",    "",         primStrCharAt);
  vm.addPrim(CLS_STR, "head",       "hd",       primStrHead);
  vm.addPrim(CLS_STR, "first",      "",         primStrHead);
  vm.addPrim(CLS_STR, "last",       "",         primStrLast);
  vm.addPrim(CLS_STR, "tail",       "tl",       primStrTail);
  vm.addPrim(CLS_STR, "rest",       "",         primStrTail);
  vm.addPrim(CLS_STR, "head:",      "",         primStrHeadN);
  vm.addPrim(CLS_STR, "tail:",      "",         primStrTailN);
  vm.addPrim(CLS_STR, "cut:to:",    "sub:to:",  primStrCutTo);
  vm.addPrim(CLS_STR, "slice:to:",  "",         primStrCutTo);
  vm.addPrim(CLS_STR, "cut:len:",   "sub:len:", primStrCutLen);
  vm.addPrim(CLS_STR, "append:",    "ap:",      primStrAppend);
  vm.addPrim(CLS_STR, "+",          "",         primStrAppend);
  vm.addPrim(CLS_STR, "*",          "",         primStrRepeat);
  vm.addPrim(CLS_STR, "split:",     "",         primStrSplit);
  vm.addPrim(CLS_STR, "lines",      "",         primStrLines);
  vm.addPrim(CLS_STR, "words",      "",         primStrWords);
  vm.addPrim(CLS_STR, "trimWhite",  "trimw",    primStrTrim);
  vm.addPrim(CLS_STR, "trim",       "strip",    primStrTrim);
  vm.addPrim(CLS_STR, "lower",      "low",      primStrLower);
  vm.addPrim(CLS_STR, "upper",      "upp",      primStrUpper);
  vm.addPrim(CLS_STR, "toNumber",   "num",      primStrToNum);
  vm.addPrim(CLS_STR, "ord",        "",         primStrOrd);
  vm.addPrim(CLS_STR, "char",       "",         primStrChar);
  vm.addPrim(CLS_STR, "chars",      "",         primStrChars);
  vm.addPrim(CLS_STR, "explode",    "",         primStrExplode);
  vm.addPrim(CLS_STR, "bytes",      "",         primStrBytes);
  vm.addPrim(CLS_STR, "starts:",    "",         primStrStarts);
  vm.addPrim(CLS_STR, "ends:",      "",         primStrEnds);
  vm.addPrim(CLS_STR, "has:",       "",         primStrHas);
  vm.addPrim(CLS_STR, "idx:",       "find:",    primStrIdx);
  vm.addPrim(CLS_STR, "replace:with:", "",      primStrReplace);
  vm.addPrim(CLS_STR, "matchp:",    "",         primStrMatchP);
  vm.addPrim(CLS_STR, "match:",     "",         primStrMatch);

  vm.addPrim(CLS_BUF_CLS, "new",    "",         primBufClsNew);
  vm.addPrim(CLS_BUF_CLS, "append:","ap:",      primBufClsAp);
  vm.addPrim(CLS_BUF, "append:",    "ap:",      primBufAp);
  vm.addPrim(CLS_BUF, "len",        "",         primBufLen);
  vm.addPrim(CLS_BUF, "clear",      "",         primBufClear);

  vm.addPrim(CLS_BYTES_CLS, "implode:", "imp:", primBytesClsImplode);
  vm.addPrim(CLS_BYTES, "len",      "length",   primBytesLen);
  vm.addPrim(CLS_BYTES, "at:",      "",         primBytesAt);
  vm.addPrim(CLS_BYTES, "head:",    "",         primBytesHeadN);
  vm.addPrim(CLS_BYTES, "tail:",    "",         primBytesTailN);
  vm.addPrim(CLS_BYTES, "cut:to:",  "",         primBytesCutTo);
  vm.addPrim(CLS_BYTES, "cut:len:", "",         primBytesCutLen);
  vm.addPrim(CLS_BYTES, "explode",  "",         primBytesExplode);

  vm.addPrim(CLS_REX_CLS, "new:",   "",         primRexClsNew);
  vm.addPrim(CLS_REX, "match:",     "",         primRexMatch);

  // 5. Pythonic List (aliased Vec)
  vm.addPrim(CLS_LIST_CLS, "new",   "",         primListClsNew);
  vm.addPrim(CLS_LIST_CLS, "append:", "ap:",    primListClsAp);
  vm.addPrim(CLS_LIST, "len",       "length",   primListLen);
  vm.addPrim(CLS_LIST, "at:",       "",         primListAt);
  vm.addPrim(CLS_LIST, "at:put:",   "at:p:",    primListAtPut);
  vm.addPrim(CLS_LIST, "wrapAt:",   "",         primListWrapAt);
  vm.addPrim(CLS_LIST, "wrapAt:put:", "wrapAt:p:", primListWrapAtPut);
  vm.addPrim(CLS_LIST, "append:",   "ap:",      primListAp);
  vm.addPrim(CLS_LIST, "push:",     "",         primListAp);
  vm.addPrim(CLS_LIST, "pop",       "",         primListPop);
  vm.addPrim(CLS_LIST, "pop:",      "",         primListPopAt);
  vm.addPrim(CLS_LIST, "ins:at:",   "insert:at:", primListInsAt);
  vm.addPrim(CLS_LIST, "rem:",      "remove:",  primListRem);
  vm.addPrim(CLS_LIST, "ext:",      "extend:",  primListExt);
  vm.addPrim(CLS_LIST, "cat:",      "",         primListCat);
  vm.addPrim(CLS_LIST, "cut:to:",   "slice:to:",primListCutTo);
  vm.addPrim(CLS_LIST, "cut:len:",  "",         primListCutLen);
  vm.addPrim(CLS_LIST, "head",      "first",    primListHead);
  vm.addPrim(CLS_LIST, "last",      "",         primListLast);
  vm.addPrim(CLS_LIST, "tail",      "rest",     primListTail);
  vm.addPrim(CLS_LIST, "x",         "",         primListX);
  vm.addPrim(CLS_LIST, "y",         "",         primListY);
  vm.addPrim(CLS_LIST, "z",         "",         primListZ);
  vm.addPrim(CLS_LIST, "w",         "",         primListW);
  vm.addPrim(CLS_LIST, "has:",      "",         primListHas);
  vm.addPrim(CLS_LIST, "idx:",      "index:",   primListIdx);
  vm.addPrim(CLS_LIST, "count:",    "",         primListCount);
  vm.addPrim(CLS_LIST, "clear",     "",         primListClear);
  vm.addPrim(CLS_LIST, "copy",      "clone",    primListCopy);
  vm.addPrim(CLS_LIST, "rev",       "",         primListRev);
  vm.addPrim(CLS_LIST, "reversed",  "",         primListReversed);
  vm.addPrim(CLS_LIST, "sort",      "",         primListSort);
  vm.addPrim(CLS_LIST, "sorted",    "",         primListSorted);
  vm.addPrim(CLS_LIST, "flat",      "",         primListFlat);
  vm.addPrim(CLS_LIST, "join:",     "",         primListJoinSep);
  vm.addPrim(CLS_LIST, "join",      "",         primListJoin);
  vm.addPrim(CLS_LIST, "jam",       "",         primListJam);
  vm.addPrim(CLS_LIST, "implode",   "imp",      primListImplode);
  vm.addPrim(CLS_LIST, "+",         "add:",     primListAdd);
  vm.addPrim(CLS_LIST, "-",         "sub:",     primListSub);
  vm.addPrim(CLS_LIST, "*",         "mul:",     primListMul);
  vm.addPrim(CLS_LIST, "scale:",    "",         primListMul);
  vm.addPrim(CLS_LIST, "/",         "",         primListDiv);
  vm.addPrim(CLS_LIST, "abs",       "",         primListAbs);
  vm.addPrim(CLS_LIST, "unit",      "",         primListUnit);
  vm.addPrim(CLS_LIST, "dot:",      "",         primListDot);
  vm.addPrim(CLS_LIST, "cross:",    "",         primListCross);

  // 6. Pythonic Dict
  vm.addPrim(CLS_DICT_CLS, "new",   "",         primDictClsNew);
  vm.addPrim(CLS_DICT, "len",       "length",   primDictLen);
  vm.addPrim(CLS_DICT, "at:",       "",         primDictAt);
  vm.addPrim(CLS_DICT, "at:put:",   "at:p:",    primDictAtPut);
  vm.addPrim(CLS_DICT, "get:or:",   "",         primDictGetOr);
  vm.addPrim(CLS_DICT, "has:",      "",         primDictHas);
  vm.addPrim(CLS_DICT, "keys",      "dir",      primDictKeys);
  vm.addPrim(CLS_DICT, "vals",      "values",   primDictVals);
  vm.addPrim(CLS_DICT, "items",     "pairs",    primDictItems);
  vm.addPrim(CLS_DICT, "del:",      "pop:",     primDictDel);
  vm.addPrim(CLS_DICT, "update:",   "",         primDictUpdate);
  vm.addPrim(CLS_DICT, "copy",      "clone",    primDictCopy);
  vm.addPrim(CLS_DICT, "clear",     "",         primDictClear);

  // 7. Blk
  vm.addPrim(CLS_BLK, "value",      "",         primBlkValue);
  vm.addPrim(CLS_BLK, "value:",     "",         primBlkValue1);
  vm.addPrim(CLS_BLK, "value:value:", "",       primBlkValue2);
  vm.addPrim(CLS_BLK, "storeAtParam0:", "",     primBlkStoreP0);
  vm.addPrim(CLS_BLK, "storeAtParam:value:", "", primBlkStoreKV);

  // 8. Cls, Meth, Frame, Usr
  vm.addPrim(CLS_CLS_CLS, "all",    "",         primClsClsAll);
  vm.addPrim(CLS_CLS_CLS, "at:",    "",         primClsClsAt);
  vm.addPrim(CLS_CLS, "name",       "",         primClsName);
  vm.addPrim(CLS_CLS, "superCls",   "sup",      primClsSuper);
  vm.addPrim(CLS_CLS, "subs",       "",         primClsSubs);
  vm.addPrim(CLS_CLS, "defSub:",    "",         primClsDefSub);
  vm.addPrim(CLS_CLS, "vars",       "",         primClsVars);
  vm.addPrim(CLS_CLS, "defVars:",   "vars:",    primClsDefVars);
  vm.addPrim(CLS_CLS, "meths",      "allMeths", primClsMeths);
  vm.addPrim(CLS_CLS, "at:",        "",         primClsAt);
  vm.addPrim(CLS_CLS, "def:c:",     "",         primClsDefC);
  vm.addPrim(CLS_CLS, "defineMethod:abbrev:doc:code:", "defMeth:a:d:c:", primClsDefMeth4);
  vm.addPrim(CLS_CLS, "trace:",     "",         primClsTrace);
  vm.addPrim(CLS_CLS, "isPrimordial","",        primClsIsPrimordial);

  vm.addPrim(CLS_METH, "onCls",     "",         primMethOnCls);
  vm.addPrim(CLS_METH, "name",      "",         primMethName);
  vm.addPrim(CLS_METH, "abbrev",    "",         primMethAbbrev);
  vm.addPrim(CLS_METH, "doc",       "",         primMethDoc);
  vm.addPrim(CLS_METH, "isPrimordial","",       primMethIsPrimordial);
  vm.addPrim(CLS_METH, "applyFrame:receiver:args:", "", primMethApply);
  vm.addPrim(CLS_USR_METH, "src",   "",         primUsrMethSrc);

  vm.addPrim(CLS_FRAME, "prevFrame","",         primFramePrev);
  vm.addPrim(CLS_FRAME, "receiver", "",         primFrameRcvr);
  vm.addPrim(CLS_FRAME, "level",    "",         primFrameLevel);

  vm.addPrim(CLS_USR_CLS, "new",    "",         primUsrClsNew);
  vm.addPrim(CLS_USR_CLS, "find:",  "",         primUsrClsFind);
  vm.addPrim(CLS_USR, "oid",        "omention", primUsrOid);
  vm.addPrim(CLS_USR, "oname",      "",         primUsrOname);
  vm.addPrim(CLS_USR, "opath",      "",         primUsrOpath);

  // 9. Json & Hex
  vm.addPrim(CLS_JSON_CLS, "en:",   "",         primJsonEn);
  vm.addPrim(CLS_JSON_CLS, "de:",   "",         primJsonDe);
  vm.addPrim(CLS_HEX_CLS,  "en:",   "",         primHexEn);
  vm.addPrim(CLS_HEX_CLS,  "de:",   "",         primHexDe);

  // 10. Sys & File
  vm.addPrim(CLS_SYS_CLS, "millis", "",         primSysMillis);
  vm.addPrim(CLS_SYS_CLS, "secs",   "",         primSysSecs);
  vm.addPrim(CLS_SYS_CLS, "sleep:", "",         primSysSleep);
  vm.addPrim(CLS_SYS_CLS, "said",   "",         primSysSaid);
  vm.addPrim(CLS_SYS_CLS, "find:",  "",         primSysFind);
  vm.addPrim(CLS_SYS_CLS, "worldName", "",      primSysWorldName);
  vm.addPrim(CLS_SYS_CLS, "worldFileName", "",  primSysWorldFile);
  vm.addPrim(CLS_SYS_CLS, "gc",     "",         primSysGc);
  vm.addPrim(CLS_SYS_CLS, "freeMem","",         primSysFreeMem);
  vm.addPrim(CLS_SYS_CLS, "usedMem","",         primSysUsedMem);
  vm.addPrim(CLS_SYS_CLS, "fail",   "",         primSysFail);
  vm.addPrim(CLS_SYS_CLS, "fail:",  "",         primSysFail1);
  vm.addPrim(CLS_SYS_CLS, "stateDiff", "",      primSysStateDiff);
  vm.addPrim(CLS_SYS_CLS, "saveState:", "",     primSysSaveState);

  vm.addPrim(CLS_FILE_CLS, "dir",   "",         primFileDir);
  vm.addPrim(CLS_FILE_CLS, "dir:",  "",         primFileDir1);
  vm.addPrim(CLS_FILE_CLS, "exists:","",        primFileExists);
  vm.addPrim(CLS_FILE_CLS, "read:", "",         primFileRead);
  vm.addPrim(CLS_FILE_CLS, "write:value:", "save:data:", primFileWrite);
  vm.addPrim(CLS_FILE_CLS, "append:value:","",  primFileAppend);
  vm.addPrim(CLS_FILE_CLS, "delete:","",        primFileDelete);
  vm.addPrim(CLS_FILE_CLS, "load:", "",         primFileLoad);
  vm.addPrim(CLS_FILE_CLS, "run:",  "",         primFileRun);
}

}  // namespace tt
