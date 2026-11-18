#include "vm/tt_vm.h"
#include "vm/tt_world.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tt;

static int gTestsPassed = 0;

#define CHECK(cond) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
    std::exit(1); \
  } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
  double _va = (a), _vb = (b); \
  if (std::fabs(_va - _vb) > (eps)) { \
    std::fprintf(stderr, "FAIL [%s:%d]: %g != %g (eps=%g)\n", __FILE__, __LINE__, _va, _vb, (double)(eps)); \
    std::exit(1); \
  } \
} while (0)

static Oop evalStr(VM& vm, const char* code) {
  Oop tmp = vm.newUsr(CLS_TMP);
  TempRootGuard rg(vm, tmp);
  vm.ticksRemaining = 2000000;
  return vm.evalCode(tmp, code);
}

static void checkEvalEq(VM& vm, const char* code, const char* expectedExpr) {
  try {
    Oop got = evalStr(vm, code);
    TempRootGuard rg(vm, got);
    Oop exp = evalStr(vm, expectedExpr);
    if (!vm.oopEquals(got, exp)) {
      std::fprintf(stderr, "FAIL eval:\n  code:     %s\n  got:      %s\n  expected: %s (%s)\n",
                   code, vm.toReprString(got).c_str(),
                   vm.toReprString(exp).c_str(), expectedExpr);
      std::exit(1);
    }
  } catch (const TerseError& e) {
    std::fprintf(stderr, "FAIL exception in eval:\n  code:  %s\n  error: %s\n",
                 code, e.message.c_str());
    std::exit(1);
  }
}

static bool evalThrows(VM& vm, const char* code) {
  try {
    evalStr(vm, code);
    return false;
  } catch (const TerseError&) {
    return true;
  }
}

// ---------------------------------------------------------------------------
// 1. 32-Bit 2-Bit-Tagged Oop Representation Tests
// ---------------------------------------------------------------------------
static void testOopRepresentation() {
  static_assert(sizeof(Oop) == 4, "Oop must be 32 bits!");
  CHECK(oopTag(kOopNil) == TAG_OTHER && isNil(kOopNil));
  CHECK(oopTag(kOopSuper) == TAG_OTHER && isSuper(kOopSuper));

  // Int30 boundaries: [-536870912 .. +536870911]
  Oop zero = makeInt30(0);
  Oop one  = makeInt30(1);
  Oop neg1 = makeInt30(-1);
  Oop minI = makeInt30(kMinInt30);
  Oop maxI = makeInt30(kMaxInt30);

  CHECK(isInt(zero) && asInt30(zero) == 0);
  CHECK(isInt(one)  && asInt30(one)  == 1);
  CHECK(isInt(neg1) && asInt30(neg1) == -1);
  CHECK(isInt(minI) && asInt30(minI) == -536870912);
  CHECK(isInt(maxI) && asInt30(maxI) ==  536870911);

  // Overflow promotion to 30-bit immediate Float
  Oop ovfPos = makeInt64OrFloat(static_cast<int64_t>(kMaxInt30) + 1);
  Oop ovfNeg = makeInt64OrFloat(static_cast<int64_t>(kMinInt30) - 1);
  CHECK(isFloat(ovfPos));
  CHECK(isFloat(ovfNeg));
  CHECK_NEAR(asFloat(ovfPos), 536870912.0f, 1.0f);
  CHECK_NEAR(asFloat(ovfNeg), -536870913.0f, 64.0f);

  // 30-bit immediate Float round-to-nearest-even & canonical +-0.0f
  Oop pz = makeFloatRaw(0.0f);
  Oop nz = makeFloatRaw(-0.0f);
  CHECK(pz == nz && isFloat(pz) && asFloat(pz) == 0.0f);

  Oop pi = makeFloatRaw(3.14159265f);
  CHECK(isFloat(pi));
  CHECK_NEAR(asFloat(pi), 3.14159265f, 1e-6f);

  Oop negPi = makeFloatRaw(-3.14159265f);
  CHECK(isFloat(negPi));
  CHECK_NEAR(asFloat(negPi), -3.14159265f, 1e-6f);

  // Char & Sym immediates
  Oop chA = makeChar('A');
  CHECK(isChar(chA) && !isSym(chA) && !isNil(chA) && asChar(chA) == 65);
  Oop sym7 = makeSym(7);
  CHECK(isSym(sym7) && !isChar(sym7) && !isNil(sym7) && asSym(sym7) == 7);

  gTestsPassed++;
  std::printf("[PASS] 32-bit 2-bit-tagged Oop representation\n");
}

// ---------------------------------------------------------------------------
// 2. Parser, Precedence, Chaining $, Cascading ;, Tuples ,, String Interpolation
// ---------------------------------------------------------------------------
static void testSyntaxAndPrecedence(VM& vm) {
  // Binary operator precedence:
  // Level 1 (* / % & | ^) > Level 2 (+ -) > Level 2.5 (@) > Level 3 (== != < <= > >=)
  checkEvalEq(vm, "2 + 3 * 4", "14");
  checkEvalEq(vm, "20 - 3 * 5", "5");
  checkEvalEq(vm, "x = 10. x-2", "8");       // Disambiguated negative literal!
  checkEvalEq(vm, "x = 10. x-2*3", "4");
  checkEvalEq(vm, "1 + 2 @ 3 * 4 @ 5 + 1", "3 @ 12 @ 6");
  checkEvalEq(vm, "2 + 3 == 5", "1");
  checkEvalEq(vm, "2 + 3 == 6", "0");

  // Unary > Binary > Keyword
  checkEvalEq(vm, "-3 abs + 4", "7");
  checkEvalEq(vm, "'hello world' cut: 0 + 1 to: 2 * 2", "'ell'");

  // Chaining `$` (including `$` with NO space before selector!)
  checkEvalEq(vm, "0 $cos", "1.0");
  checkEvalEq(vm, "1 $not", "0");
  checkEvalEq(vm, "0 $not", "1");
  checkEvalEq(vm, "'  AbC  ' $trim $low $ap: '!'", "'abc!'");

  // Fluent chaining `$` & 2D list-of-tuples `;`
  checkEvalEq(vm, "v = List new. v ap: 10 $ ap: 20 $ ap: 30 $ len", "3");
  checkEvalEq(vm, "(11, 22; 33, 44)", "(11 @ 22) @ (33 @ 44)");

  // Tuple literals `,` and destructuring assignment `p, q = ...`
  checkEvalEq(vm, "(10, 20, 30)", "10 @ 20 @ 30");
  checkEvalEq(vm, "a, b, c = 7, 8, 9. a * 100 + (b * 10) + c", "789");
  checkEvalEq(vm, "x, y = 1, 2. x, y = y, x. x @ y", "2 @ 1");

  // Comments `"..."` and string interpolation `'...[expr]...'` & `[[` `]]` escapes
  checkEvalEq(vm, "\"comment\" 3 + \"mid comment\" 4", "7");
  checkEvalEq(vm, "n = 5. 'sq([n])=[[ [n sq] ]]'", "'sq(5)=' + (91 chr) + ' 25 ' + (93 chr)");
  checkEvalEq(vm, "'a''b'", "('a' explode ap: 39 $ ap: 98) imp");

  // Case-insensitivity of identifiers & selectors
  checkEvalEq(vm, "FooBar = 123. foobar + FOOBAR", "246");
  checkEvalEq(vm, "list NEW AP: 9 $ LEN", "1");

  gTestsPassed++;
  std::printf("[PASS] Syntax, precedence, $, ;, destructuring & interpolation\n");
}

// ---------------------------------------------------------------------------
// 3. Control Flow, Macros, Blocks & Lexical Closures
// ---------------------------------------------------------------------------
static void testMacrosAndClosures(VM& vm) {
  // IF / ELIF / ELSE
  checkEvalEq(vm, "IF(1) THEN('yes') ELSE('no')", "'yes'");
  checkEvalEq(vm, "IF(0) THEN('yes') ELSE('no')", "'no'");
  checkEvalEq(vm, "IF('') THEN(99)", "nil");
  checkEvalEq(vm, "x = 2. IF(x == 1) THEN('one') ELIF(x == 2) THEN('two') ELSE('other')", "'two'");

  // COND & CASE & AND / OR / MAYBE
  checkEvalEq(vm, "x = 15. COND(x < 10, 'small'; x < 20, 'med'; 1, 'big')", "'med'");
  checkEvalEq(vm, "k = 'b'. CASE(k) OF('a', 10; 'b', 20) ELSE(99)", "20");
  checkEvalEq(vm, "AND(1; 2; 3)", "1");
  checkEvalEq(vm, "c = 0. r = AND(1; 0; (c = 99. 1)). r @ c", "0 @ 0");
  checkEvalEq(vm, "c = 0. r = OR(0; 'ok'; (c = 99. 1)). r @ c", "1 @ 0");
  checkEvalEq(vm, "MAYBE(nil) NULL(MAYBE(0) NULL(99))", "0");  // 0 is non-nil!

  // WHILE
  checkEvalEq(vm, "i = 1. s = 0. WHILE(i <= 5) DO(s = s + i. i = i + 1). s", "15");

  // FOR over Num, List (1 var, 2 vars, destructured, MAP, IF..MAP, INIT..REDUCE), Dict, Str
  checkEvalEq(vm, "s = 0. FOR(i: 5) DO(s = s + i). s", "10");
  checkEvalEq(vm, "s = 0. FOR(i: v: 10 @ 20 @ 30) DO(s = s + (i * v)). s", "80");
  checkEvalEq(vm, "s = 0. FOR(x, y: (1, 2) @ (3, 4)) DO(s = s + (x * y)). s", "14");
  checkEvalEq(vm, "FOR(x: 1 to: 6) IF(x % 2 == 0) MAP(x sq)", "4 @ 16 @ 36");
  checkEvalEq(vm, "FOR(x: 1 to: 5) INIT(acc: 1) REDUCE(acc * x)", "120");
  checkEvalEq(vm, "d = DICT('b', 2; 'a', 1). out = Buf new. FOR(k: v: d) DO(out ap: '[k]=[v];'). out str", "'a=1;b=2;'");
  checkEvalEq(vm, "z = List new. FOR(ch: 'abc') DO(z ap: ch upp). z join: '-'", "'A-B-C'");

  // TRY / CATCH
  checkEvalEq(vm, "TRY(10 + 20) CATCH(e: 'bad')", "30");
  checkEvalEq(vm, "TRY('boom' err) CATCH(e: 'caught:' + e)", "'caught:boom'");

  // Lexical closures `FN(...)` mutating outer method locals & surviving return!
  const char* closureWorld =
    "cls CounterMaker Usr\n"
    "meth CounterMaker makeFrom:\n"
    " n = a.\n"
    " FN(step: n = n + step. n)\n"
    "meth CounterMaker customMacroTest\n"
    " REPEATTWICE(x = x + 3)\n"
    "meth CounterMaker macro:repeatTwice:\n"
    " b value. b value\n"
    "vars CounterMaker\n"
    " x\n";
  std::string err;
  CHECK(loadWorldBuffer(vm, "test", closureWorld, std::strlen(closureWorld), false, &err));
  checkEvalEq(vm, "cm = CounterMaker new. c1 = cm makeFrom: 10. c2 = cm makeFrom: 100. (c1 value: 5) @ (c1 value: 2) @ (c2 value: 1)",
              "15 @ 17 @ 101");
  checkEvalEq(vm, "cm = CounterMaker new. cm x: 10. cm customMacroTest. cm x", "16");

  gTestsPassed++;
  std::printf("[PASS] Control-flow macros, TRY/CATCH, user macros & closures\n");
}

// ---------------------------------------------------------------------------
// 4. Math (including fixed ln/log10/exp/atan2/dot:) & Overflow Tests
// ---------------------------------------------------------------------------
static void testMathAndVectors(VM& vm) {
  // Int30 overflow auto-promotes cleanly in bytecode OP_ADD / OP_MUL
  checkEvalEq(vm, "400000000 + 400000000 > 799999000", "1");
  checkEvalEq(vm, "30000 * 30000", "900000000");

  // Fixed math bugs from TTA!
  Oop lnE   = evalStr(vm, "Num e ln");
  Oop log10 = evalStr(vm, "1000 log10");
  Oop exp2  = evalStr(vm, "2 exp");
  Oop atan2 = evalStr(vm, "(1 atan2: 1) deg");
  CHECK_NEAR(toFloatUnchecked(lnE),   1.0f, 1e-5f);
  CHECK_NEAR(toFloatUnchecked(log10), 3.0f, 1e-5f);
  CHECK_NEAR(toFloatUnchecked(exp2),  std::exp(2.0f), 1e-4f);
  CHECK_NEAR(toFloatUnchecked(atan2), 45.0f, 1e-3f);

  // Scalar `dot:`, elementwise `mul:`, 3D `cross:`, `abs`, `unit`, scalar broadcast
  checkEvalEq(vm, "(1 @ 2 @ 3) dot: (4 @ 5 @ 6)", "32");
  checkEvalEq(vm, "(1 @ 2 @ 3) mul: (4 @ 5 @ 6)", "4 @ 10 @ 18");
  checkEvalEq(vm, "3 * (10 @ 20)", "30 @ 60");
  checkEvalEq(vm, "(10 @ 20) + (1 @ 2)", "11 @ 22");
  checkEvalEq(vm, "(3 @ 4) abs", "5.0");
  checkEvalEq(vm, "(1 @ 0 @ 0) cross: (0 @ 1 @ 0)", "0.0 @ 0.0 @ 1.0");

  // Negative modulo uses non-negative residue
  checkEvalEq(vm, "-1 % 5", "4");
  checkEvalEq(vm, "-7 idiv: 3", "-2");

  gTestsPassed++;
  std::printf("[PASS] Fixed math functions, Int30 overflow & vector math\n");
}

// ---------------------------------------------------------------------------
// 5. Pythonic List & Dict, Regex, JSON, Hex, Classes & Super (`up`)
// ---------------------------------------------------------------------------
static void testCollectionsRegexJsonClasses(VM& vm) {
  // Pythonic List methods & bounds-checked negative indexing + wrapAt:
  checkEvalEq(vm, "l = 10 @ 20 @ 30. (l at: -1) @ (l at: -3)", "30 @ 10");
  CHECK(evalThrows(vm, "(10 @ 20) at: 2"));
  CHECK(evalThrows(vm, "(10 @ 20) at: -3"));
  checkEvalEq(vm, "(10 @ 20 @ 30) wrapAt: 4", "20");
  checkEvalEq(vm, "(10 @ 20 @ 30) wrapAt: -4", "30");

  checkEvalEq(vm, "l = 3 @ 1 @ 2. l push: 0 $ ins: 9 at: 1 $ rem: 3. l", "9 @ 1 @ 2 @ 0");
  checkEvalEq(vm, "l = 4 @ 1 @ 3 @ 2. p = l pop. l sort. p @ l", "2 @ (1 @ 3 @ 4)");
  checkEvalEq(vm, "(10 @ 20 @ 30 @ 40) cut: 1 to: -1", "20 @ 30");
  checkEvalEq(vm, "(10 @ 20 @ 10) count: 10", "2");
  checkEvalEq(vm, "(10 @ 20 @ 30) has: 20", "1");
  checkEvalEq(vm, "(10 @ 20 @ 30) idx: 30", "2");
  checkEvalEq(vm, "Vec is: List", "1");

  // Pythonic Dict methods
  checkEvalEq(vm, "d = DICT('x', 10; 'y', 20). (d at: 'x') @ (d at: 'z') @ (d get: 'z' or: 99)",
              "10 @ nil @ 99");
  checkEvalEq(vm, "d = DICT('b', 2; 'a', 1). (d keys) @ (d vals) @ (d has: 'a')",
              "('a' @ 'b') @ (1 @ 2) @ 1");
  checkEvalEq(vm, "d = DICT('a', 1; 'b', 2). v = d del: 'a'. v @ (d len) @ (d has: 'a')",
              "1 @ 1 @ 0");

  // Immediate Char
  checkEvalEq(vm, "c = 'a' char. (c isAlpha) @ (c upp str) @ (c ord)", "1 @ 'A' @ 97");

  // Regex (`Rex` & `matchp:`)
  checkEvalEq(vm, "'abc123' matchp: '^[[a-z]]+\\d+$'", "1");
  checkEvalEq(vm, "'abc123!' matchp: '^[[a-z]]+\\d+$'", "0");
  checkEvalEq(vm, "rx = Rex new: '^([[a-z]]+)=(\\d+)'. rx match: 'foo=42 bar'",
              "'foo=42' @ 'foo' @ '42'");

  // JSON & Hex round-trip
  checkEvalEq(vm, "d = DICT('k', 1 @ 'hi' @ nil). Json de: (Json en: d)", "DICT('k', 1 @ 'hi' @ nil)");
  checkEvalEq(vm, "(Hex de: (Hex en: 'Terse!')) str", "'Terse!'");

  // Inheritance, `up` super sends, and class-instance variables on `*Cls`
  const char* oopWorld =
    "cls BaseA Usr\n"
    "vars BaseA\n"
    " v\n"
    "meth BaseA greet\n"
    " 'A:' + (v str)\n"
    "cls SubB BaseA\n"
    "vars SubBCls\n"
    " count\n"
    "meth SubBCls reset\n"
    " count = 0. me\n"
    "meth SubBCls make:\n"
    " count = count + 1. (me new) v: a\n"
    "meth SubB greet\n"
    " up greet + '+B'\n";
  std::string err;
  CHECK(loadWorldBuffer(vm, "oop", oopWorld, std::strlen(oopWorld), false, &err));
  checkEvalEq(vm, "SubB reset. b1 = SubB make: 7. b2 = SubB make: 8. (b1 greet) @ (SubB count) @ (b2 isa: BaseA)",
              "'A:7+B' @ 2 @ 1");

  gTestsPassed++;
  std::printf("[PASS] Pythonic List/Dict, Char, Rex, Json, Hex, inheritance & class vars\n");
}

// ---------------------------------------------------------------------------
// 6. GC Coalescing Stress Test & World File Persistence Replay Test
// ---------------------------------------------------------------------------
static void testGcStressAndWorldPersistence() {
  VM vm2;
  // Small 64 KB arena to force many mark-sweep-coalesce GC cycles!
  CHECK(vm2.init(65536u, true));

  // Intercept world log writes in memory
  std::string memWorldLog;
  struct WLogCtx { std::string* log; } wctx{&memWorldLog};
  vm2.host.user = &wctx;
  vm2.host.vfsWrite = [](void* u, const char*, const char* data, size_t len, bool append) -> bool {
    auto* c = static_cast<WLogCtx*>(u);
    if (!append) c->log->clear();
    c->log->append(data, len);
    return true;
  };

  // Allocate thousands of temporary strings, lists, dicts, and closures
  checkEvalEq(vm2,
    "keep = DICT('title', 'survivor'). "
    "i = 0. s = 0. "
    "WHILE(i < 1200) DO("
    "  tmp = 'item_[i]' split: '_'. "
    "  d = DICT('k', tmp; 'n', i). "
    "  s = s + (d at: 'n'). "
    "  i = i + 1). "
    "(keep at: 'title') @ s",
    "'survivor' @ 719400");
  CHECK(vm2.heap.gcCount >= 3);  // Verified multiple GC sweeps ran cleanly!

  // Define a live class + vars + method at runtime and verify world log replay into fresh VM!
  evalStr(vm2,
    "c = Usr defSub: 'SavedPoint'. "
    "c defVars: 'px py'. "
    "c def: 'norm2' c: 'px sq + py sq'");
  CHECK(!memWorldLog.empty());

  VM vm3;
  CHECK(vm3.init(kDefaultArenaBytes, true));
  std::string err;
  CHECK(loadWorldBuffer(vm3, "main", memWorldLog.data(), memWorldLog.size(), false, &err));
  checkEvalEq(vm3, "p = SavedPoint new px: 3 $ py: 4. p norm2", "25");

  vm3.destroy();
  vm2.destroy();
  gTestsPassed++;
  std::printf("[PASS] Mark-sweep-coalesce GC stress & live World persistence replay\n");
}

int main() {
  try {
    std::printf("=== TerseTalk-CYD Host VM Test Suite ===\n");
    testOopRepresentation();

    VM vm;
    if (!vm.init(kDefaultArenaBytes, true)) {
      std::fprintf(stderr, "VM::init failed!\n");
      return 1;
    }

    testSyntaxAndPrecedence(vm);
    testMacrosAndClosures(vm);
    testMathAndVectors(vm);
    testCollectionsRegexJsonClasses(vm);

    vm.destroy();
    testGcStressAndWorldPersistence();

    std::printf("=== ALL %d VM TEST SUITES PASSED ===\n", gTestsPassed);
    return 0;
  } catch (const TerseError& e) {
    std::fprintf(stderr, "UNCAUGHT TerseError: %s\n", e.message.c_str());
    return 1;
  }
}
