#include "tt_compiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace tt {

namespace {

enum TokType : uint8_t {
  TOK_EOF = 0,
  TOK_NAME,
  TOK_NUMBER,
  TOK_STRING,
  TOK_OTHER,
};

struct LexState {
  size_t      pos   = 0;
  size_t      front = 0;
  TokType     t     = TOK_EOF;
  std::string w;
  bool        numIsFloat = false;
};

struct TLex {
  VM&         vm;
  const char* src;
  size_t      len;
  LexState    cur;

  TLex(VM& v, const char* s, size_t l) : vm(v), src(s), len(l) {
    advance();
  }

  LexState save() const { return cur; }
  void restore(const LexState& st) { cur = st; }

  void skipWhiteAndComments() {
    while (cur.pos < len) {
      char c = src[cur.pos];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
        cur.pos++;
      } else if (c == '"') {
        // Smalltalk-style comment "..."
        cur.pos++;
        while (cur.pos < len && src[cur.pos] != '"') {
          cur.pos++;
        }
        if (cur.pos < len && src[cur.pos] == '"') {
          cur.pos++;
        }
      } else {
        break;
      }
    }
  }

  static bool isNameStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }
  static bool isNameCont(char c) {
    return isNameStart(c) || (c >= '0' && c <= '9') || c == '_';
  }
  static bool isDigit(char c) {
    return c >= '0' && c <= '9';
  }

  void advance() {
    skipWhiteAndComments();
    cur.front      = cur.pos;
    cur.w.clear();
    cur.numIsFloat = false;

    if (cur.pos >= len) {
      cur.t = TOK_EOF;
      return;
    }

    char c = src[cur.pos];

    // 1. Identifier NAME
    if (isNameStart(c)) {
      size_t start = cur.pos++;
      while (cur.pos < len && isNameCont(src[cur.pos])) cur.pos++;
      cur.w.assign(src + start, cur.pos - start);
      cur.t = TOK_NAME;
      return;
    }

    // 2. NUMBER: -?[0-9]+(\.[0-9]+)?([Ee][+-]?[0-9]+)?
    if (isDigit(c) || (c == '-' && cur.pos + 1 < len && isDigit(src[cur.pos + 1]))) {
      size_t start = cur.pos++;
      while (cur.pos < len && isDigit(src[cur.pos])) cur.pos++;
      if (cur.pos + 1 < len && src[cur.pos] == '.' && isDigit(src[cur.pos + 1])) {
        cur.numIsFloat = true;
        cur.pos += 2;
        while (cur.pos < len && isDigit(src[cur.pos])) cur.pos++;
      }
      if (cur.pos < len && (src[cur.pos] == 'e' || src[cur.pos] == 'E')) {
        size_t ePos = cur.pos + 1;
        if (ePos < len && (src[ePos] == '+' || src[ePos] == '-')) ePos++;
        if (ePos < len && isDigit(src[ePos])) {
          cur.numIsFloat = true;
          cur.pos = ePos + 1;
          while (cur.pos < len && isDigit(src[cur.pos])) cur.pos++;
        }
      }
      cur.w.assign(src + start, cur.pos - start);
      cur.t = TOK_NUMBER;
      return;
    }

    // 3. Single-quoted STRING literal ('foo''bar' -> foo'bar)
    if (c == '\'') {
      cur.t = TOK_STRING;
      while (cur.pos < len && src[cur.pos] == '\'') {
        cur.pos++;  // skip opening '
        size_t start = cur.pos;
        while (cur.pos < len && src[cur.pos] != '\'') {
          cur.pos++;
        }
        if (cur.pos >= len) {
          vm.toss("Unterminated string literal at offset %u", static_cast<unsigned>(cur.front));
        }
        cur.w.append(src + start, cur.pos - start);
        cur.pos++;  // skip closing '
        if (cur.pos < len && src[cur.pos] == '\'') {
          // Doubled '' inside string literal -> literal '
          cur.w.push_back('\'');
        } else {
          break;
        }
      }
      return;
    }

    // 4. OTHER (multi-char operators & single-char punctuation)
    cur.t = TOK_OTHER;
    if (cur.pos + 1 < len) {
      char c2 = src[cur.pos + 1];
      if ((c == '=' && c2 == '=') || (c == '!' && c2 == '=') ||
          (c == '<' && c2 == '=') || (c == '>' && c2 == '=') ||
          (c == '<' && c2 == '<') || (c == '>' && c2 == '>') ||
          (c == ',' && c2 == ',') || (c == '.' && c2 == ',') ||
          (c == ',' && c2 == '.')) {
        cur.w.assign(src + cur.pos, 2);
        cur.pos += 2;
        return;
      }
    }
    cur.w.assign(1, c);
    cur.pos++;
  }

  bool isEOF() const { return cur.t == TOK_EOF; }
  bool endsStmt() const { return cur.t == TOK_OTHER && cur.w == "."; }
  bool isAssign() const { return cur.t == TOK_OTHER && cur.w == "="; }
  bool isTupler() const { return cur.t == TOK_OTHER && cur.w == ","; }
  bool isLister() const {
    return cur.t == TOK_OTHER && (cur.w == ";" || cur.w == ".," || cur.w == ",.");
  }
  bool isChain() const {
    return cur.t == TOK_OTHER && (cur.w == "$" || cur.w == ",,");
  }
  bool opensParen() const {
    if (cur.t != TOK_OTHER || cur.w.size() != 1) return false;
    char c = cur.w[0];
    return c == '(' || c == '[' || c == '{' || c == '!';
  }
  bool closesParen() const {
    if (cur.t != TOK_OTHER || cur.w.size() != 1) return false;
    char c = cur.w[0];
    return c == ')' || c == ']' || c == '}' || c == '?';
  }
  bool isBinop1() const {
    return cur.t == TOK_OTHER &&
           (cur.w == "*" || cur.w == "/" || cur.w == "%" || cur.w == "&" ||
            cur.w == "<<" || cur.w == ">>");
  }
  bool isBinop2() const {
    return cur.t == TOK_OTHER &&
           (cur.w == "+" || cur.w == "-" || cur.w == "|" || cur.w == "^");
  }
  bool isBinop3() const {
    return cur.t == TOK_OTHER &&
           (cur.w == "==" || cur.w == "!=" || cur.w == "<" || cur.w == "<=" ||
            cur.w == ">" || cur.w == ">=");
  }
  bool isBinop() const { return isBinop1() || isBinop2() || isBinop3(); }

  bool isKeyword() {
    if (cur.t != TOK_NAME) return false;
    LexState st = save();
    advance();
    bool ok = (cur.t == TOK_OTHER && cur.w == ":");
    restore(st);
    return ok;
  }

  bool isMacro() {
    if (cur.t != TOK_NAME) return false;
    LexState st = save();
    advance();
    bool ok = opensParen();
    restore(st);
    return ok;
  }
};

// Parsed LValue representation
struct LvNode {
  bool                isTuple = false;
  bool                isInst  = false;
  uint32_t            symId   = 0;
  uint16_t            slotIdx = 0;
  std::vector<LvNode> children;
};

// Shared Method-Scope State across Pass 1 & Pass 2 (and nested Blocks)
struct MethodScope {
  VM&                   vm;
  ObjCls*               onCls;
  std::vector<uint32_t> instVars;    // Indexed by instVar slot [0..allVarCount-1]
  std::vector<uint32_t> localVars;   // User locals (args + assigned vars)
  uint16_t              numArgs        = 0;
  uint16_t              tempLocalWater = 0;
  uint16_t              maxLocals      = 0;
  bool                  capturesFrame  = false;

  MethodScope(VM& v, ObjCls* cls) : vm(v), onCls(cls) {
    if (cls && cls->allVarCount > 0 && cls->allVarSyms) {
      instVars.assign(cls->allVarSyms, cls->allVarSyms + cls->allVarCount);
    }
  }

  int findInstVar(uint32_t sym) const {
    for (size_t i = 0; i < instVars.size(); i++) {
      if (instVars[i] == sym) return static_cast<int>(i);
    }
    return -1;
  }

  int findLocalVar(uint32_t sym) const {
    for (size_t i = 0; i < localVars.size(); i++) {
      if (localVars[i] == sym) return static_cast<int>(i);
    }
    return -1;
  }

  uint16_t ensureLocalOrInst(uint32_t sym, bool& outIsInst) {
    int iv = findInstVar(sym);
    if (iv >= 0) {
      outIsInst = true;
      return static_cast<uint16_t>(iv);
    }
    outIsInst = false;
    int lv = findLocalVar(sym);
    if (lv >= 0) return static_cast<uint16_t>(lv);
    uint16_t idx = static_cast<uint16_t>(localVars.size());
    localVars.push_back(sym);
    if (localVars.size() > maxLocals) maxLocals = static_cast<uint16_t>(localVars.size());
    return idx;
  }

  uint16_t allocTempLocals(uint16_t count) {
    if (tempLocalWater < localVars.size()) {
      tempLocalWater = static_cast<uint16_t>(localVars.size());
    }
    uint16_t base = tempLocalWater;
    tempLocalWater = static_cast<uint16_t>(tempLocalWater + count);
    if (tempLocalWater > maxLocals) maxLocals = tempLocalWater;
    return base;
  }

  void freeTempLocals(uint16_t savedBase) {
    tempLocalWater = savedBase;
  }
};

struct CodeBuilder {
  MethodScope&          scope;
  bool                  emit;  // false = Pass 1 (discover vars), true = Pass 2 (codegen)
  std::vector<uint32_t> code;
  std::vector<Oop>      consts;
  std::vector<SendSite> sendSites;
  std::vector<BlkParam> blkParams;

  CodeBuilder(MethodScope& sc, bool doEmit) : scope(sc), emit(doEmit) {}

  size_t pc() const { return code.size(); }

  void emitIns(Op op, uint8_t a = 0, uint16_t bc = 0) {
    if (emit) code.push_back(encodeIns(op, a, bc));
  }

  void emitInsSigned(Op op, uint8_t a, int16_t sbc) {
    if (emit) code.push_back(encodeInsSigned(op, a, sbc));
  }

  size_t emitJumpPlaceholder(Op op, uint8_t a = 0) {
    if (!emit) return 0;
    size_t idx = code.size();
    code.push_back(encodeInsSigned(op, a, 0));
    return idx;
  }

  void patchJump(size_t jumpIdx) {
    if (!emit) return;
    int32_t rel = static_cast<int32_t>(code.size()) - static_cast<int32_t>(jumpIdx + 1);
    if (rel < -32767 || rel > 32767) {
      scope.vm.toss("Jump offset overflow (%d)", static_cast<int>(rel));
    }
    Op      op = insOp(code[jumpIdx]);
    uint8_t a  = insA(code[jumpIdx]);
    code[jumpIdx] = encodeInsSigned(op, a, static_cast<int16_t>(rel));
  }

  void emitBackwardJump(Op op, uint8_t a, size_t targetPc) {
    if (!emit) return;
    int32_t rel = static_cast<int32_t>(targetPc) - static_cast<int32_t>(code.size() + 1);
    if (rel < -32767 || rel > 32767) {
      scope.vm.toss("Backward jump offset overflow (%d)", static_cast<int>(rel));
    }
    code.push_back(encodeInsSigned(op, a, static_cast<int16_t>(rel)));
  }

  uint16_t addConst(Oop v) {
    if (!emit) return 0;
    for (size_t i = 0; i < consts.size(); i++) {
      if (consts[i] == v) return static_cast<uint16_t>(i);
    }
    uint16_t idx = static_cast<uint16_t>(consts.size());
    consts.push_back(v);
    return idx;
  }

  uint16_t addSendSite(uint32_t selectorSym, size_t loc) {
    if (!emit) return 0;
    SendSite ss{};
    ss.selectorSym = selectorSym;
    ss.cachedClsId = 0;
    ss.cachedGen   = 0;
    ss.cachedMeth  = nullptr;
    ss.sourceLoc   = static_cast<uint16_t>(loc > 0xFFFFu ? 0xFFFFu : loc);
    uint16_t idx   = static_cast<uint16_t>(sendSites.size());
    sendSites.push_back(ss);
    return idx;
  }

  CompiledCode* buildCompiledCode(uint32_t methNameSym, bool isBlock) {
    VM& vm = scope.vm;
    // Root all constants while allocating CompiledCode arrays
    TempRootGuard rg(vm);
    for (Oop c : consts) rg.push(c);

    auto* cc = static_cast<CompiledCode*>(vm.allocExternal(sizeof(CompiledCode)));
    std::memset(cc, 0, sizeof(CompiledCode));

    cc->codeLen       = static_cast<uint32_t>(code.size());
    cc->code          = cc->codeLen ? static_cast<uint32_t*>(vm.allocExternal(cc->codeLen * sizeof(uint32_t))) : nullptr;
    if (cc->codeLen) std::memcpy(cc->code, code.data(), cc->codeLen * sizeof(uint32_t));

    cc->constCount    = static_cast<uint16_t>(consts.size());
    cc->consts        = cc->constCount ? static_cast<Oop*>(vm.allocExternal(cc->constCount * sizeof(Oop))) : nullptr;
    if (cc->constCount) std::memcpy(cc->consts, consts.data(), cc->constCount * sizeof(Oop));

    cc->sendSiteCount = static_cast<uint16_t>(sendSites.size());
    cc->sendSites     = cc->sendSiteCount ? static_cast<SendSite*>(vm.allocExternal(cc->sendSiteCount * sizeof(SendSite))) : nullptr;
    if (cc->sendSiteCount) std::memcpy(cc->sendSites, sendSites.data(), cc->sendSiteCount * sizeof(SendSite));

    cc->numArgs       = isBlock ? static_cast<uint16_t>(blkParams.size()) : scope.numArgs;
    cc->numLocals     = scope.maxLocals;
    cc->onClsId       = scope.onCls ? scope.onCls->clsId : static_cast<uint16_t>(CLS_TMP);
    cc->methNameSym   = methNameSym;
    cc->capturesFrame = scope.capturesFrame;

    if (isBlock && !blkParams.empty()) {
      cc->blkParamCount = static_cast<uint8_t>(blkParams.size());
      cc->blkParams     = static_cast<BlkParam*>(vm.allocExternal(blkParams.size() * sizeof(BlkParam)));
      std::memcpy(cc->blkParams, blkParams.data(), blkParams.size() * sizeof(BlkParam));
    }
    return cc;
  }
};

struct Parser {
  TLex&        lex;
  CodeBuilder& cb;

  Parser(TLex& l, CodeBuilder& b) : lex(l), cb(b) {}

  VM& vm() { return cb.scope.vm; }

  bool skipStmtEnders() {
    bool missing = true;
    while (lex.endsStmt()) {
      lex.advance();
      missing = false;
    }
    return missing;
  }

  // Parses LValue or returns false (restoring lexer if caller saved it)
  bool parseLvName(LvNode& out) {
    if (lex.cur.t != TOK_NAME) return false;
    out = LvNode{};
    out.isTuple = false;
    out.symId   = vm().internSym(lex.cur.w.c_str(), lex.cur.w.size());
    lex.advance();
    return true;
  }

  bool parseLvTuple(LvNode first, LvNode& out) {
    out = LvNode{};
    out.isTuple = true;
    out.children.push_back(first);
    while (lex.cur.t == TOK_OTHER && lex.cur.w == ",") {
      lex.advance();
      LvNode next;
      if (!parseLvName(next)) return false;
      out.children.push_back(next);
    }
    return true;
  }

  bool parseLValue(LvNode& out) {
    LvNode a;
    if (!parseLvName(a)) return false;
    if (lex.cur.t == TOK_OTHER && lex.cur.w == ",") {
      if (!parseLvTuple(a, a)) return false;
    }
    if (lex.cur.t == TOK_OTHER && lex.cur.w == ";") {
      LvNode listNode;
      listNode.isTuple = true;
      listNode.children.push_back(a);
      while (lex.cur.t == TOK_OTHER && lex.cur.w == ";") {
        lex.advance();
        LvNode elem;
        if (!parseLvName(elem)) return false;
        if (lex.cur.t == TOK_OTHER && lex.cur.w == ",") {
          if (!parseLvTuple(elem, elem)) return false;
        }
        listNode.children.push_back(elem);
      }
      a = listNode;
    }
    out = a;
    return true;
  }

  void bindLvIndices(LvNode& lv) {
    if (!lv.isTuple) {
      lv.slotIdx = cb.scope.ensureLocalOrInst(lv.symId, lv.isInst);
    } else {
      for (auto& ch : lv.children) bindLvIndices(ch);
    }
  }

  // Emits store assuming value to assign is on top of stack, leaving it on top of stack!
  void emitStoreKeepTop(const LvNode& lv) {
    if (!lv.isTuple) {
      cb.emitIns(lv.isInst ? OP_SET_INST : OP_SET_LOCAL, 0, lv.slotIdx);
    } else {
      uint16_t n = static_cast<uint16_t>(lv.children.size());
      cb.emitIns(OP_UNPACK_LIST, 0, n);
      for (uint16_t i = 0; i < n; i++) {
        emitStorePopTop(lv.children[i]);
      }
      // Original List remains on top of stack!
    }
  }

  // Emits store consuming top of stack!
  void emitStorePopTop(const LvNode& lv) {
    emitStoreKeepTop(lv);
    cb.emitIns(OP_POP);
  }

  BlkParam lvToBlkParam(const LvNode& lv) {
    BlkParam bp{};
    if (!lv.isTuple) {
      bp.isTuple          = 0;
      bp.varCount         = 1;
      bp.vars[0].isInst   = lv.isInst ? 1 : 0;
      bp.vars[0].index    = lv.slotIdx;
    } else {
      bp.isTuple  = 1;
      bp.varCount = static_cast<uint8_t>(std::min<size_t>(lv.children.size(), 6));
      for (uint8_t i = 0; i < bp.varCount; i++) {
        if (lv.children[i].isTuple) {
          vm().toss("Nested 2D destructuring inside block param not supported");
        }
        bp.vars[i].isInst = lv.children[i].isInst ? 1 : 0;
        bp.vars[i].index  = lv.children[i].slotIdx;
      }
    }
    return bp;
  }

  // Parses zero or more `lvalue:` block parameters inside an open block
  void parseBlockParams(std::vector<LvNode>& outParams) {
    outParams.clear();
    while (true) {
      LexState st = lex.save();
      LvNode lv;
      if (parseLValue(lv) && lex.cur.t == TOK_OTHER && lex.cur.w == ":") {
        bindLvIndices(lv);
        outParams.push_back(lv);
        lex.advance();  // consume ':'
      } else {
        lex.restore(st);
        break;
      }
    }
  }

  void parseExpr() {
    skipStmtEnders();
    size_t count = 0;
    while (!lex.isEOF() && !lex.closesParen()) {
      if (count > 0) {
        cb.emitIns(OP_POP);
      }
      parseStmt();
      count++;
      if (skipStmtEnders()) break;
    }
    if (count == 0) {
      cb.emitIns(OP_PUSH_NIL);
    }
  }

  void parseStmt() {
    LexState st = lex.save();
    LvNode lv;
    if (parseLValue(lv) && lex.isAssign()) {
      bindLvIndices(lv);
      lex.advance();  // consume '='
      parseStmt();    // right-associative
      emitStoreKeepTop(lv);
      return;
    }
    lex.restore(st);
    parseList();
  }

  // Returns number of ';'-separated elements parsed (1 if not a ';' list)
  void parseList() {
    parseTuple();
    if (!lex.isLister()) return;

    uint16_t count = 1;
    while (lex.isLister()) {
      lex.advance();
      if (lex.endsStmt() || lex.closesParen() || lex.isEOF()) break;
      parseTuple();
      count++;
    }
    cb.emitIns(OP_MAKE_LIST, 0, count);
  }

  void parseTuple() {
    parseChain();
    if (!lex.isTupler()) return;

    uint16_t count = 1;
    while (lex.isTupler()) {
      lex.advance();
      if (lex.isLister() || lex.endsStmt() || lex.closesParen() || lex.isEOF()) break;
      parseChain();
      count++;
    }
    cb.emitIns(OP_MAKE_LIST, 0, count);
  }

  void parseChain() {
    parseUnary();
    parseKeyedTail();
    while (lex.isChain()) {
      lex.advance();
      // Unary messages at front of chain
      while (lex.cur.t == TOK_NAME && !lex.isKeyword() && !lex.isMacro()) {
        uint32_t sym = vm().internSym(lex.cur.w.c_str(), lex.cur.w.size());
        uint16_t cs  = cb.addSendSite(sym, lex.cur.front);
        cb.emitIns(OP_SEND, 0, cs);
        lex.advance();
      }
      if (lex.isKeyword() || lex.isBinop() || (lex.cur.t == TOK_OTHER && lex.cur.w == "@")) {
        parseKeyedTail();
      }
    }
  }

  void parseKeyedTail() {
    parseBinary3Tail();
    while (lex.isKeyword()) {
      std::string sel;
      uint8_t     nArgs    = 0;
      size_t      firstLoc = lex.cur.front;
      while (lex.isKeyword()) {
        for (char c : lex.cur.w) {
          sel.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        sel.push_back(':');
        lex.advance();  // consume keyword name
        if (lex.cur.t != TOK_OTHER || lex.cur.w != ":") {
          vm().toss("Expected ':' after keyword");
        }
        lex.advance();  // consume ':'
        parseUnary();
        parseBinary3Tail();
        nArgs++;
      }
      uint32_t sym = vm().internSym(sel.c_str(), sel.size());
      uint16_t cs  = cb.addSendSite(sym, firstLoc);
      cb.emitIns(OP_SEND, nArgs, cs);
    }
  }

  void emitBinOp(const std::string& opStr, size_t loc) {
    uint32_t sym = vm().internSym(opStr.c_str(), opStr.size());
    uint16_t cs  = cb.addSendSite(sym, loc);
    // Note: for fast-path binary opcodes, `bc` holds `cs` (sendSiteIdx) so non-Num
    // receivers can use the exact same inline call-site cache as OP_SEND!
    if      (opStr == "+")  cb.emitIns(OP_ADD, 1, cs);
    else if (opStr == "-")  cb.emitIns(OP_SUB, 1, cs);
    else if (opStr == "*")  cb.emitIns(OP_MUL, 1, cs);
    else if (opStr == "/")  cb.emitIns(OP_DIV, 1, cs);
    else if (opStr == "%")  cb.emitIns(OP_MOD, 1, cs);
    else if (opStr == "&")  cb.emitIns(OP_BIT_AND, 1, cs);
    else if (opStr == "|")  cb.emitIns(OP_BIT_OR, 1, cs);
    else if (opStr == "^")  cb.emitIns(OP_BIT_XOR, 1, cs);
    else if (opStr == "==") cb.emitIns(OP_EQ, 1, cs);
    else if (opStr == "!=") cb.emitIns(OP_NE, 1, cs);
    else if (opStr == "<")  cb.emitIns(OP_LT, 1, cs);
    else if (opStr == "<=") cb.emitIns(OP_LE, 1, cs);
    else if (opStr == ">")  cb.emitIns(OP_GT, 1, cs);
    else if (opStr == ">=") cb.emitIns(OP_GE, 1, cs);
    else                    cb.emitIns(OP_SEND, 1, cs);
  }

  void parseBinary3Tail() {
    parseBinary25Tail();
    while (lex.isBinop3()) {
      std::string op  = lex.cur.w;
      size_t      loc = lex.cur.front;
      lex.advance();
      parseUnary();
      parseBinary25Tail();
      emitBinOp(op, loc);
    }
  }

  void parseBinary25Tail() {
    parseBinary2Tail();
    if (lex.cur.t != TOK_OTHER || lex.cur.w != "@") return;
    uint16_t count = 1;
    while (lex.cur.t == TOK_OTHER && lex.cur.w == "@") {
      lex.advance();
      parseUnary();
      parseBinary2Tail();
      count++;
    }
    cb.emitIns(OP_MAKE_LIST, 0, count);
  }

  void parseBinary2Tail() {
    parseBinary1Tail();
    while (lex.isBinop2() || (lex.cur.t == TOK_NUMBER && !lex.cur.w.empty() && lex.cur.w[0] == '-')) {
      if (lex.cur.t == TOK_NUMBER && lex.cur.w[0] == '-') {
        // Disambiguate `x-2` where `-2` lexed as a negative NUMBER token!
        size_t      loc    = lex.cur.front;
        std::string posStr = lex.cur.w.substr(1);
        bool        isFlt  = lex.cur.numIsFloat;
        lex.advance();
        emitNumberLiteral(posStr, isFlt);
        // Any unary messages on the literal (`x-2 abs` -> `x - (2 abs)`)?
        // In TTA Parser.java line 437, `x-2` immediately formed `e - 2`; handle binary1 tail too:
        parseBinary1Tail();
        emitBinOp("-", loc);
      } else {
        std::string op  = lex.cur.w;
        size_t      loc = lex.cur.front;
        lex.advance();
        parseUnary();
        parseBinary1Tail();
        emitBinOp(op, loc);
      }
    }
  }

  void parseBinary1Tail() {
    while (lex.isBinop1()) {
      std::string op  = lex.cur.w;
      size_t      loc = lex.cur.front;
      lex.advance();
      parseUnary();
      emitBinOp(op, loc);
    }
  }

  void parseUnary() {
    parsePrim();
    while (lex.cur.t == TOK_NAME && !lex.isKeyword() && !lex.isMacro()) {
      uint32_t sym = vm().internSym(lex.cur.w.c_str(), lex.cur.w.size());
      uint16_t cs  = cb.addSendSite(sym, lex.cur.front);
      cb.emitIns(OP_SEND, 0, cs);
      lex.advance();
    }
  }

  void emitNumberLiteral(const std::string& s, bool isFlt) {
    if (!cb.emit) return;
    if (!isFlt) {
      char*   endp = nullptr;
      int64_t iv   = std::strtoll(s.c_str(), &endp, 10);
      if (endp && *endp == '\0' && iv >= kMinInt30 && iv <= kMaxInt30) {
        if (iv >= -32768 && iv <= 32767) {
          cb.emitInsSigned(OP_PUSH_INT16, 0, static_cast<int16_t>(iv));
        } else {
          uint16_t k = cb.addConst(makeInt30(static_cast<int32_t>(iv)));
          cb.emitIns(OP_PUSH_CONST, 0, k);
        }
        return;
      }
    }
    float fv = std::strtof(s.c_str(), nullptr);
    Oop   fo = vm().makeFloatChecked(fv);
    uint16_t k = cb.addConst(fo);
    cb.emitIns(OP_PUSH_CONST, 0, k);
  }

  void emitInterpolatedString(const std::string& raw) {
    const size_t n = raw.size();
    std::string  litBuf;
    uint16_t     partCount = 0;

    auto flushLit = [&]() {
      if (!litBuf.empty()) {
        if (cb.emit) {
          Oop s = vm().newStr(litBuf.data(), litBuf.size());
          TempRootGuard rg(vm(), s);
          uint16_t k = cb.addConst(s);
          cb.emitIns(OP_PUSH_CONST, 0, k);
        }
        litBuf.clear();
        partCount++;
      }
    };

    for (size_t i = 0; i < n; i++) {
      if (i + 1 < n && raw[i] == '[' && raw[i + 1] == '[') {
        litBuf.push_back('[');
        i++;
      } else if (i + 1 < n && raw[i] == ']' && raw[i + 1] == ']') {
        litBuf.push_back(']');
        i++;
      } else if (raw[i] == '[') {
        // Find matching ']'
        int    nested = 0;
        size_t j      = i + 1;
        bool   found  = false;
        for (; j < n; j++) {
          if (raw[j] == ']' && nested == 0) {
            found = true;
            break;
          } else if (raw[j] == ']') {
            nested--;
          } else if (raw[j] == '[') {
            nested++;
          }
        }
        if (found) {
          flushLit();
          std::string inner = raw.substr(i + 1, j - (i + 1));
          TLex subLex(vm(), inner.c_str(), inner.size());
          Parser subParser(subLex, cb);
          // Allow optional block params though typically 0-arg expression
          std::vector<LvNode> dummyParams;
          subParser.parseBlockParams(dummyParams);
          subParser.parseExpr();
          if (!subLex.isEOF()) {
            vm().toss("Syntax error inside interpolated string `[%s]`", inner.c_str());
          }
          partCount++;
          i = j;
        } else {
          litBuf.append(raw.substr(i));
          break;
        }
      } else {
        litBuf.push_back(raw[i]);
      }
    }
    flushLit();

    if (partCount == 0) {
      if (cb.emit) {
        Oop s = vm().newStr("", 0);
        TempRootGuard rg(vm(), s);
        uint16_t k = cb.addConst(s);
        cb.emitIns(OP_PUSH_CONST, 0, k);
      }
    } else if (partCount == 1) {
      // Ensure single `[expr]` interpolation still converts to Str via OP_STR_JAM if needed;
      // if it was a literal Str, OP_STR_JAM 1 returns it directly without copying!
      cb.emitIns(OP_STR_JAM, 0, 1);
    } else {
      cb.emitIns(OP_STR_JAM, 0, partCount);
    }
  }

  void emitVarRead(const std::string& name) {
    uint32_t sym = vm().internSym(name.c_str(), name.size());
    const char* low = vm().symCStr(sym);
    if (std::strcmp(low, "nil") == 0) {
      cb.emitIns(OP_PUSH_NIL);
      return;
    }
    if (std::strcmp(low, "me") == 0) {
      cb.emitIns(OP_PUSH_ME);
      return;
    }
    if (std::strcmp(low, "up") == 0) {
      cb.emitIns(OP_PUSH_UP);
      return;
    }
    if (std::strcmp(low, "true") == 0) {
      cb.emitInsSigned(OP_PUSH_INT16, 0, 1);
      return;
    }
    if (std::strcmp(low, "false") == 0) {
      cb.emitInsSigned(OP_PUSH_INT16, 0, 0);
      return;
    }
    int iv = cb.scope.findInstVar(sym);
    if (iv >= 0) {
      cb.emitIns(OP_GET_INST, 0, static_cast<uint16_t>(iv));
      return;
    }
    int lv = cb.scope.findLocalVar(sym);
    if (lv >= 0) {
      cb.emitIns(OP_GET_LOCAL, 0, static_cast<uint16_t>(lv));
      return;
    }
    cb.emitIns(OP_GET_GLOBAL, 0, static_cast<uint16_t>(sym));
  }

  void parseParen() {
    lex.advance();  // consume '('
    if (lex.closesParen()) {
      // Empty `()` creates a new empty mutable List!
      cb.emitIns(OP_MAKE_LIST, 0, 0);
      return;  // caller `parsePrim` advances past ')'
    }
    parseExpr();
    if (!lex.closesParen()) {
      vm().toss("Expected closing ')' but got '%s'", lex.cur.w.c_str());
    }
    // caller `parsePrim` advances past ')'
  }

  // Captures the raw source substring of each macro clause `NAME( ... )`!
  struct MacroClause {
    std::string nameLower;
    size_t      nameLoc;
    size_t      innerStart;  // offset right after '('
    size_t      innerLen;    // length up to matching ')'
  };

  void skipBalancedBlock(size_t& outInnerStart, size_t& outInnerLen) {
    if (!lex.opensParen()) {
      vm().toss("Expected '(' after macro word");
    }
    outInnerStart = lex.cur.pos;  // right after '('
    int depth = 1;
    size_t endPos = outInnerStart;
    while (depth > 0) {
      endPos = lex.cur.front;
      lex.advance();
      if (lex.isEOF()) {
        vm().toss("Unterminated macro block starting at offset %u", static_cast<unsigned>(outInnerStart));
      }
      if (lex.opensParen()) {
        depth++;
      } else if (lex.closesParen()) {
        depth--;
        if (depth == 0) {
          endPos = lex.cur.front;
          break;
        }
      }
    }
    outInnerLen = endPos - outInnerStart;
    lex.advance();  // consume matching closing ')'
  }

  void compileClauseAsSubparser(const MacroClause& mc, std::vector<LvNode>* outParams = nullptr) {
    TLex subLex(vm(), lex.src + mc.innerStart, mc.innerLen);
    Parser subP(subLex, cb);
    if (outParams) {
      subP.parseBlockParams(*outParams);
    }
    subP.parseExpr();
    if (!subLex.isEOF()) {
      vm().toss("Syntax error inside %s(...) macro block near '%s'",
                mc.nameLower.c_str(), subLex.cur.w.c_str());
    }
  }

  // Compiles a MacroClause into a standalone heap ObjMeth (for `FN(...)` or custom macros)
  uint16_t compileClauseToBlockConst(const MacroClause& mc) {
    cb.scope.capturesFrame = true;
    CodeBuilder blkCb(cb.scope, cb.emit);
    TLex subLex(vm(), lex.src + mc.innerStart, mc.innerLen);
    Parser subP(subLex, blkCb);
    std::vector<LvNode> params;
    subP.parseBlockParams(params);
    for (const auto& lv : params) {
      blkCb.blkParams.push_back(subP.lvToBlkParam(lv));
    }
    subP.parseExpr();
    if (!subLex.isEOF()) {
      vm().toss("Syntax error inside block near '%s'", subLex.cur.w.c_str());
    }
    blkCb.emitIns(OP_RET);

    if (!cb.emit) return 0;

    uint32_t blkSym = vm().internSym("__blk__");
    CompiledCode* cc = blkCb.buildCompiledCode(blkSym, true);
    auto* m = reinterpret_cast<ObjMeth*>(
        vm().allocObj(CLS_USR_METH, OBJ_METH, 0, sizeof(ObjMeth)));
    m->onClsId   = cb.scope.onCls ? cb.scope.onCls->clsId : static_cast<uint16_t>(CLS_TMP);
    m->isPrim    = false;
    m->nameSym   = blkSym;
    m->abbrevSym = blkSym;
    m->comp      = cc;
    // Copy block source snippet for clean formatting
    char* scopy = static_cast<char*>(vm().allocExternal(mc.innerLen + 1u));
    std::memcpy(scopy, lex.src + mc.innerStart, mc.innerLen);
    scopy[mc.innerLen] = '\0';
    m->source     = scopy;
    m->sourceLen  = static_cast<uint32_t>(mc.innerLen);
    m->ownsSource = true;

    Oop methOop = oopFromObj(&m->hdr);
    TempRootGuard rg(vm(), methOop);
    return cb.addConst(methOop);
  }

  // Helper to split top-level `;` or `,` items inside VEC(...), DICT(...), AND(...), OR(...), COND(...)
  // Calls `onItem(Parser& itemParser)` for each top-level element!
  template <typename Fn>
  size_t forEachTopVecElement(const MacroClause& mc, Fn onItem) {
    TLex subLex(vm(), lex.src + mc.innerStart, mc.innerLen);
    Parser subP(subLex, cb);
    subP.skipStmtEnders();
    if (subLex.isEOF()) return 0;

    // Determine whether top-level separator is ';' (if any top-level ';' exists) or ','
    bool hasTopSemicolon = false;
    {
      TLex scanLex(vm(), lex.src + mc.innerStart, mc.innerLen);
      int depth = 0;
      while (!scanLex.isEOF()) {
        if (scanLex.opensParen()) depth++;
        else if (scanLex.closesParen()) depth--;
        else if (depth == 0 && scanLex.isLister()) {
          hasTopSemicolon = true;
          break;
        }
        scanLex.advance();
      }
    }

    size_t count = 0;
    while (!subLex.isEOF()) {
      onItem(subP, hasTopSemicolon);
      count++;
      if (hasTopSemicolon) {
        if (subLex.isLister()) {
          subLex.advance();
          if (subLex.isEOF() || subLex.endsStmt()) break;
        } else {
          break;
        }
      } else {
        if (subLex.isTupler()) {
          subLex.advance();
          if (subLex.isEOF() || subLex.endsStmt()) break;
        } else {
          break;
        }
      }
    }
    subP.skipStmtEnders();
    if (!subLex.isEOF()) {
      vm().toss("Unexpected token '%s' in %s(...)", subLex.cur.w.c_str(), mc.nameLower.c_str());
    }
    return count;
  }

  // Splits a pair `k, v` or `k @ v` inside COND(...) or CASE(...)OF(...)
  bool compilePairSides(Parser& subP, bool parentUsesSemicolon,
                        std::function<void()> afterFirstBeforeSecond) {
    if (parentUsesSemicolon) {
      // Pair is `first , second` OR `first @ second`
      // Check if comma or '@' separates them at depth 0 before ';'
      LexState saved = subP.lex.save();
      int depth = 0;
      bool hasComma = false;
      while (!subP.lex.isEOF()) {
        if (subP.lex.opensParen()) depth++;
        else if (subP.lex.closesParen()) depth--;
        else if (depth == 0) {
          if (subP.lex.isLister() || subP.lex.endsStmt()) break;
          if (subP.lex.isTupler()) { hasComma = true; break; }
        }
        subP.lex.advance();
      }
      subP.lex.restore(saved);

      if (hasComma) {
        subP.parseChain();
        if (!subP.lex.isTupler()) return false;
        subP.lex.advance();
        afterFirstBeforeSecond();
        subP.parseChain();
        return true;
      }
    }
    // Otherwise pair must be `first @ second`!
    subP.parseUnary();
    subP.parseBinary2Tail();
    if (subP.lex.cur.t != TOK_OTHER || subP.lex.cur.w != "@") {
      vm().toss("Expected ',' or '@' pair inside COND/CASE");
    }
    subP.lex.advance();
    afterFirstBeforeSecond();
    subP.parseChain();
    return true;
  }

  void parseMacro() {
    std::vector<MacroClause> clauses;
    while (lex.cur.t == TOK_NAME && lex.isMacro()) {
      MacroClause mc;
      for (char c : lex.cur.w) {
        mc.nameLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      mc.nameLoc = lex.cur.front;
      lex.advance();  // consume macro NAME, now sitting on '('
      skipBalancedBlock(mc.innerStart, mc.innerLen);
      clauses.push_back(mc);
    }

    std::string sig;
    for (const auto& mc : clauses) {
      sig += mc.nameLower + ":";
    }

    // 1. FN(...)
    if (sig == "fn:") {
      uint16_t k = compileClauseToBlockConst(clauses[0]);
      cb.emitIns(OP_MAKE_BLK, 0, k);
      return;
    }

    // 2. IF(...) THEN(...) [ELIF(...) THEN(...)]* [ELSE(...)]
    if (clauses.size() >= 2 && clauses[0].nameLower == "if" && clauses[1].nameLower == "then") {
      bool validIf = true;
      size_t idx = 2;
      while (idx < clauses.size()) {
        if (clauses[idx].nameLower == "elif" && idx + 1 < clauses.size() && clauses[idx + 1].nameLower == "then") {
          idx += 2;
        } else if (clauses[idx].nameLower == "else" && idx + 1 == clauses.size()) {
          idx += 1;
        } else {
          validIf = false;
          break;
        }
      }
      if (validIf) {
        std::vector<size_t> endJumps;
        size_t i = 0;
        while (i + 1 < clauses.size() && (clauses[i].nameLower == "if" || clauses[i].nameLower == "elif")) {
          compileClauseAsSubparser(clauses[i]);
          size_t falseJmp = cb.emitJumpPlaceholder(OP_JMP_FALSE);
          compileClauseAsSubparser(clauses[i + 1]);
          endJumps.push_back(cb.emitJumpPlaceholder(OP_JMP));
          cb.patchJump(falseJmp);
          i += 2;
        }
        if (i < clauses.size() && clauses[i].nameLower == "else") {
          compileClauseAsSubparser(clauses[i]);
        } else {
          cb.emitIns(OP_PUSH_NIL);
        }
        for (size_t ej : endJumps) cb.patchJump(ej);
        return;
      }
    }

    // 3. WHILE(...) DO(...)
    if (sig == "while:do:") {
      size_t loopTop = cb.pc();
      compileClauseAsSubparser(clauses[0]);
      size_t exitJmp = cb.emitJumpPlaceholder(OP_JMP_FALSE);
      compileClauseAsSubparser(clauses[1]);
      cb.emitIns(OP_POP);
      cb.emitBackwardJump(OP_JMP, 0, loopTop);
      cb.patchJump(exitJmp);
      cb.emitIns(OP_PUSH_ME);
      return;
    }

    // 4. AND(...) / OR(...)
    if (sig == "and:" || sig == "or:") {
      bool isAnd = (sig == "and:");
      std::vector<size_t> shortJumps;
      size_t count = forEachTopVecElement(clauses[0], [&](Parser& p, bool semi) {
        if (semi) p.parseTuple();
        else p.parseChain();
        size_t sj = cb.emitJumpPlaceholder(isAnd ? OP_JMP_FALSE : OP_JMP_TRUE);
        shortJumps.push_back(sj);
      });
      (void)count;
      // Fell through all elements
      cb.emitInsSigned(OP_PUSH_INT16, 0, isAnd ? 1 : 0);
      size_t endJmp = cb.emitJumpPlaceholder(OP_JMP);
      for (size_t sj : shortJumps) cb.patchJump(sj);
      cb.emitInsSigned(OP_PUSH_INT16, 0, isAnd ? 0 : 1);
      cb.patchJump(endJmp);
      return;
    }

    // 5. MAYBE(...) NULL(...)
    if (sig == "maybe:null:") {
      compileClauseAsSubparser(clauses[0]);
      size_t keepJmp = cb.emitJumpPlaceholder(OP_JMP_NOT_NIL_PEEK);
      cb.emitIns(OP_POP);
      compileClauseAsSubparser(clauses[1]);
      cb.patchJump(keepJmp);
      return;
    }

    // 6. COND(p1, x1; p2, x2; ...)
    if (sig == "cond:") {
      std::vector<size_t> endJumps;
      forEachTopVecElement(clauses[0], [&](Parser& p, bool semi) {
        size_t nextJmp = 0;
        compilePairSides(p, semi, [&]() {
          nextJmp = cb.emitJumpPlaceholder(OP_JMP_FALSE);
        });
        endJumps.push_back(cb.emitJumpPlaceholder(OP_JMP));
        cb.patchJump(nextJmp);
      });
      cb.emitIns(OP_PUSH_NIL);
      for (size_t ej : endJumps) cb.patchJump(ej);
      return;
    }

    // 7. CASE(target) OF(k1, x1; ...) [ELSE(dflt)]
    if (sig == "case:of:" || sig == "case:of:else:") {
      compileClauseAsSubparser(clauses[0]);  // pushes target on stack
      std::vector<size_t> endJumps;
      forEachTopVecElement(clauses[1], [&](Parser& p, bool semi) {
        size_t skipBodyJmp = 0;
        compilePairSides(p, semi, [&]() {
          // Stack has [target, key]. OP_CASE_MATCH pops key; if == target, pops target & jumps +1!
          cb.emitInsSigned(OP_CASE_MATCH, 0, 1);
          skipBodyJmp = cb.emitJumpPlaceholder(OP_JMP);
        });
        endJumps.push_back(cb.emitJumpPlaceholder(OP_JMP));
        cb.patchJump(skipBodyJmp);
      });
      // No case matched: pop target and run ELSE or push nil
      cb.emitIns(OP_POP);
      if (sig == "case:of:else:") {
        compileClauseAsSubparser(clauses[2]);
      } else {
        cb.emitIns(OP_PUSH_NIL);
      }
      for (size_t ej : endJumps) cb.patchJump(ej);
      return;
    }

    // 8. LIST(...) / VEC(...)
    if (sig == "vec:" || sig == "list:") {
      size_t n = forEachTopVecElement(clauses[0], [&](Parser& p, bool semi) {
        if (semi) p.parseTuple();
        else p.parseChain();
      });
      cb.emitIns(OP_MAKE_LIST, 0, static_cast<uint16_t>(n));
      return;
    }

    // 9. DICT(...)
    if (sig == "dict:") {
      size_t n = forEachTopVecElement(clauses[0], [&](Parser& p, bool semi) {
        if (semi) p.parseTuple();
        else p.parseChain();
      });
      cb.emitIns(OP_MAKE_DICT, 0, static_cast<uint16_t>(n));
      return;
    }

    // 10. FOR(...) DO / MAP / IF..MAP / INIT..REDUCE
    if (sig == "for:do:" || sig == "for:map:" || sig == "for:if:map:" || sig == "for:init:reduce:") {
      bool isDo     = (sig == "for:do:");
      bool isMap    = (sig == "for:map:");
      bool isIfMap  = (sig == "for:if:map:");
      bool isReduce = (sig == "for:init:reduce:");

      // Allocate 3 loop-state hidden locals + 1 result/acc local if map/reduce
      uint16_t savedTemp = cb.scope.tempLocalWater;
      uint16_t tBase     = cb.scope.allocTempLocals((isMap || isIfMap || isReduce) ? 4 : 3);
      uint16_t accSlot   = static_cast<uint16_t>(tBase + 3);

      std::vector<LvNode> forParams;
      compileClauseAsSubparser(clauses[0], &forParams);  // pushes collection
      cb.emitIns(OP_FOR_PREP, static_cast<uint8_t>(tBase), 0);

      std::vector<LvNode> initParams;
      if (isMap || isIfMap) {
        cb.emitIns(OP_MAKE_LIST, 0, 0);
        cb.emitIns(OP_SET_LOCAL, 0, accSlot);
        cb.emitIns(OP_POP);
      } else if (isReduce) {
        compileClauseAsSubparser(clauses[1], &initParams);
        cb.emitIns(OP_SET_LOCAL, 0, accSlot);
        cb.emitIns(OP_POP);
      }

      size_t loopTop = cb.pc();
      Op stepOp = (forParams.empty()) ? OP_FOR_STEP_0
                : (forParams.size() == 1 ? OP_FOR_STEP_1 : OP_FOR_STEP_2);
      size_t exitJmp = cb.emitJumpPlaceholder(stepOp, static_cast<uint8_t>(tBase));

      // Bind FOR params (OP_FOR_STEP_2 pushed key then val, so val is on top!)
      if (forParams.size() == 1) {
        emitStorePopTop(forParams[0]);
      } else if (forParams.size() >= 2) {
        emitStorePopTop(forParams[1]);  // val
        emitStorePopTop(forParams[0]);  // key
      }

      if (isDo) {
        compileClauseAsSubparser(clauses[1]);
        cb.emitIns(OP_POP);
      } else if (isMap) {
        cb.emitIns(OP_GET_LOCAL, 0, accSlot);
        compileClauseAsSubparser(clauses[1]);
        uint16_t apCs = cb.addSendSite(vm().internSym("ap:"), clauses[1].nameLoc);
        cb.emitIns(OP_SEND, 1, apCs);
        cb.emitIns(OP_POP);
      } else if (isIfMap) {
        compileClauseAsSubparser(clauses[1]);  // cond
        size_t skipJmp = cb.emitJumpPlaceholder(OP_JMP_FALSE);
        cb.emitIns(OP_GET_LOCAL, 0, accSlot);
        compileClauseAsSubparser(clauses[2]);  // map expr
        uint16_t apCs = cb.addSendSite(vm().internSym("ap:"), clauses[2].nameLoc);
        cb.emitIns(OP_SEND, 1, apCs);
        cb.emitIns(OP_POP);
        cb.patchJump(skipJmp);
      } else if (isReduce) {
        if (!initParams.empty()) {
          cb.emitIns(OP_GET_LOCAL, 0, accSlot);
          emitStorePopTop(initParams[0]);
        }
        compileClauseAsSubparser(clauses[2]);
        cb.emitIns(OP_SET_LOCAL, 0, accSlot);
        cb.emitIns(OP_POP);
      }

      cb.emitBackwardJump(OP_JMP, 0, loopTop);
      cb.patchJump(exitJmp);

      if (isDo) {
        cb.emitIns(OP_PUSH_NIL);
      } else {
        cb.emitIns(OP_GET_LOCAL, 0, accSlot);
      }

      cb.scope.freeTempLocals(savedTemp);
      return;
    }

    // 11. TRY(...) CATCH(e: ...)
    if (sig == "try:catch:") {
      size_t catchJmp = cb.emitJumpPlaceholder(OP_TRY_PUSH);
      compileClauseAsSubparser(clauses[0]);
      cb.emitIns(OP_TRY_POP);
      size_t endJmp = cb.emitJumpPlaceholder(OP_JMP);

      cb.patchJump(catchJmp);
      // VM pushes caught error Str onto stack when jumping here!
      TLex subLex(vm(), lex.src + clauses[1].innerStart, clauses[1].innerLen);
      Parser subP(subLex, cb);
      std::vector<LvNode> catchParams;
      subP.parseBlockParams(catchParams);
      if (!catchParams.empty()) {
        emitStorePopTop(catchParams[0]);
      } else {
        cb.emitIns(OP_POP);
      }
      subP.parseExpr();
      if (!subLex.isEOF()) {
        vm().toss("Syntax error in CATCH(...) block");
      }
      cb.patchJump(endJmp);
      return;
    }

    // 12. Fallback: User-Defined Macro -> `me macro: frame <w1>: blk1 <w2>: blk2 ...`
    cb.scope.capturesFrame = true;
    cb.emitIns(OP_PUSH_ME);
    cb.emitIns(OP_PUSH_FRAME);
    for (const auto& mc : clauses) {
      uint16_t k = compileClauseToBlockConst(mc);
      cb.emitIns(OP_MAKE_BLK, 0, k);
    }
    std::string fullSel = "macro:" + sig;
    uint32_t selSym = vm().internSym(fullSel.c_str(), fullSel.size());
    uint16_t cs     = cb.addSendSite(selSym, clauses[0].nameLoc);
    cb.emitIns(OP_SEND, static_cast<uint8_t>(1 + clauses.size()), cs);
  }

  void parsePrim() {
    bool shouldAdvance = true;
    switch (lex.cur.t) {
      case TOK_NUMBER:
        emitNumberLiteral(lex.cur.w, lex.cur.numIsFloat);
        break;
      case TOK_STRING:
        emitInterpolatedString(lex.cur.w);
        break;
      case TOK_OTHER:
        if (lex.opensParen()) {
          parseParen();
        } else {
          vm().toss("Syntax error: unexpected '%s' at offset %u",
                    lex.cur.w.c_str(), static_cast<unsigned>(lex.cur.front));
        }
        break;
      case TOK_NAME:
        if (lex.isMacro()) {
          parseMacro();
          shouldAdvance = false;
        } else {
          emitVarRead(lex.cur.w);
        }
        break;
      case TOK_EOF:
        vm().toss("Unexpected end of code");
    }
    if (shouldAdvance) {
      lex.advance();
    }
  }
};

}  // namespace

CompiledCode* compileMethod(VM& vm, ObjCls* onCls, uint32_t methNameSym,
                            const char* src, size_t srcLen) {
  MethodScope scope(vm, onCls);
  const char* methName = vm.symCStr(methNameSym);

  // Determine implicit parameters a, b, c, ... from selector
  if (methName && methName[0] != '\0') {
    char c0 = methName[0];
    bool isAlpha = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_';
    if (isAlpha) {
      for (size_t i = 0; methName[i] != '\0'; i++) {
        if (methName[i] == ':') {
          char pName[2] = {static_cast<char>('a' + scope.numArgs), '\0'};
          bool dummyInst = false;
          scope.ensureLocalOrInst(vm.internSym(pName, 1), dummyInst);
          scope.numArgs++;
        }
      }
    } else {
      // Binary operator message takes 1 argument `a`
      bool dummyInst = false;
      scope.ensureLocalOrInst(vm.internSym("a", 1), dummyInst);
      scope.numArgs = 1;
    }
  }
  scope.maxLocals = static_cast<uint16_t>(scope.localVars.size());

  // Pass 1: Discover all local variables & whether method captures a Frame
  {
    CodeBuilder pass1(scope, false);
    TLex lex1(vm, src, srcLen);
    Parser p1(lex1, pass1);
    p1.parseExpr();
    if (!lex1.isEOF()) {
      vm.toss("Leftover token '%s' at offset %u while compiling <%s>",
              lex1.cur.w.c_str(), static_cast<unsigned>(lex1.cur.front), methName);
    }
  }

  // Pass 2: Emit aligned 32-bit RAM bytecodes
  CodeBuilder pass2(scope, true);
  TLex lex2(vm, src, srcLen);
  Parser p2(lex2, pass2);
  p2.parseExpr();
  pass2.emitIns(OP_RET);

  return pass2.buildCompiledCode(methNameSym, false);
}

}  // namespace tt
