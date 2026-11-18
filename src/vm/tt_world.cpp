#include "tt_world.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tt {

namespace {

static void splitWords(const std::string& line, std::vector<std::string>& out) {
  out.clear();
  size_t i = 0, n = line.size();
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= n) break;
    size_t start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
    out.emplace_back(line.substr(start, i - start));
  }
}

static ObjCls* findOrDefineOrphan(VM& vm, const std::string& className) {
  uint32_t sym = vm.internSym(className.c_str(), className.size());
  Oop cOop = vm.dictGet(vm.globalsDict, makeSym(sym));
  if (isObj(cOop) && objFromOop(cOop)->format == OBJ_CLS) {
    return reinterpret_cast<ObjCls*>(objFromOop(cOop));
  }
  Oop newC = vm.defineClass(className.c_str(), CLS_ORPHAN, true);
  return reinterpret_cast<ObjCls*>(objFromOop(newC));
}

}  // namespace

bool loadWorldBuffer(VM& vm, const char* worldTag, const char* buf, size_t len,
                     bool borrowStaticSource, std::string* outErr) {
  bool prevLoading = vm.loadingWorld;
  vm.loadingWorld  = true;

  size_t pos     = 0;
  int    lineNum = 0;

  auto nextLine = [&](size_t& lineStart, size_t& lineLen) -> bool {
    if (pos >= len) return false;
    lineStart = pos;
    while (pos < len && buf[pos] != '\n') pos++;
    lineLen = pos - lineStart;
    if (lineLen > 0 && buf[lineStart + lineLen - 1] == '\r') lineLen--;
    if (pos < len && buf[pos] == '\n') pos++;
    lineNum++;
    return true;
  };

  try {
    size_t lStart = 0, lLen = 0;
    bool hasLine = nextLine(lStart, lLen);
    while (hasLine) {
      std::string line(buf + lStart, lLen);

      // Trim check for blank or `#` comment lines
      size_t firstNonWs = 0;
      while (firstNonWs < line.size() && std::isspace(static_cast<unsigned char>(line[firstNonWs]))) {
        firstNonWs++;
      }
      if (firstNonWs == line.size() || line[0] == '#') {
        hasLine = nextLine(lStart, lLen);
        continue;
      }

      std::vector<std::string> words;
      splitWords(line, words);
      if (words.empty()) {
        hasLine = nextLine(lStart, lLen);
        continue;
      }

      // Timestamped block wrapper: `( 123456 meth Foo bar` ... `) 123456`
      if (words[0] == ")") {
        hasLine = nextLine(lStart, lLen);
        continue;
      }
      if (words[0] == "(" && words.size() > 2) {
        words.erase(words.begin(), words.begin() + 2);
      }

      std::string cmd = words[0];
      int cmdLineNum  = lineNum;

      // Slurp indented body lines (lines starting with whitespace or empty lines)
      // Note: if `borrowStaticSource` is false or lines have leading 1-space indent stripped,
      // we build `more` cleanly with 1 leading space stripped per line (matching Terp.java!).
      std::string more;
      while (true) {
        hasLine = nextLine(lStart, lLen);
        if (!hasLine) break;
        if (lLen == 0) {
          more.push_back('\n');
        } else if (std::isspace(static_cast<unsigned char>(buf[lStart]))) {
          more.append(buf + lStart + 1, lLen - 1);
          more.push_back('\n');
        } else {
          break;
        }
      }
      (void)borrowStaticSource;

      if (cmd == "stop") {
        break;
      } else if (cmd == "cls" || cmd == "class") {
        if (words.size() < 3) vm.toss("Line %d: 'cls' requires <Sub> <Super>", cmdLineNum);
        uint32_t subSym = vm.internSym(words[1].c_str(), words[1].size());
        uint32_t supSym = vm.internSym(words[2].c_str(), words[2].size());
        Oop supOop = vm.dictGet(vm.globalsDict, makeSym(supSym));
        if (!isObj(supOop) || objFromOop(supOop)->format != OBJ_CLS) {
          vm.toss("Line %d: Superclass <%s> does not exist for <%s>",
                  cmdLineNum, words[2].c_str(), words[1].c_str());
        }
        Oop existing = vm.dictGet(vm.globalsDict, makeSym(subSym));
        if (existing == kOopNil) {
          auto* supCls = reinterpret_cast<ObjCls*>(objFromOop(supOop));
          vm.defineClass(words[1].c_str(), supCls->clsId, supCls->isUsrCls);
        }
      } else if (cmd == "vars" || cmd == "instvars") {
        if (words.size() < 2) vm.toss("Line %d: 'vars' requires <ClassName>", cmdLineNum);
        ObjCls* cls = findOrDefineOrphan(vm, words[1]);
        vm.defineVars(cls, more.c_str(), true);
      } else if (cmd == "meth") {
        if (words.size() < 3) vm.toss("Line %d: 'meth' requires <ClassName> <selector>", cmdLineNum);
        ObjCls* cls = findOrDefineOrphan(vm, words[1]);
        vm.addUsrMeth(cls, words[2].c_str(), "", more.c_str(), more.size(), false, false);
      } else if (cmd == "inst") {
        if (words.size() < 3) vm.toss("Line %d: 'inst' requires <ClassName> <instName>", cmdLineNum);
        ObjCls* cls = findOrDefineOrphan(vm, words[1]);
        std::string iname = words[2];
        if (worldTag && worldTag[0] && iname.find('_') == std::string::npos) {
          iname = iname + "_" + worldTag;
        }
        uint32_t isym = vm.internSym(iname.c_str(), iname.size());
        Oop sVal = vm.newStr(more.c_str(), more.size());
        TempRootGuard rg(vm, sVal);
        Oop sd = vm.ensureSavedInstsDict(cls);
        vm.dictPut(sd, makeSym(isym), sVal);
      } else if (cmd == "equals") {
        std::string expectedExpr;
        for (size_t i = 1; i < words.size(); i++) {
          if (i > 1) expectedExpr.push_back(' ');
          expectedExpr += words[i];
        }
        Oop tmp1 = vm.newUsr(CLS_TMP);
        TempRootGuard rg(vm, tmp1);
        Oop p1 = vm.evalCode(tmp1, expectedExpr.c_str());
        rg.push(p1);
        Oop tmp2 = vm.newUsr(CLS_TMP);
        rg.push(tmp2);
        Oop p2 = vm.evalCode(tmp2, more.c_str());
        if (!vm.oopEquals(p1, p2)) {
          vm.toss("Line %d: equals failed: expected <%s> (%s), got <%s>",
                  cmdLineNum, expectedExpr.c_str(),
                  vm.toStdString(p1).c_str(), vm.toStdString(p2).c_str());
        }
      } else {
        vm.toss("Line %d: Unknown world directive <%s>", cmdLineNum, cmd.c_str());
      }
    }
  } catch (const TerseError& err) {
    vm.loadingWorld = prevLoading;
    if (outErr) *outErr = err.message;
    return false;
  }

  vm.loadingWorld = prevLoading;
  return true;
}

bool loadWorldFile(VM& vm, const char* worldTag, const char* vfsPath, std::string* outErr) {
  std::string content;
  if (!vm.readVfsFile(vfsPath, content)) {
    if (outErr) *outErr = std::string("Cannot read file: ") + vfsPath;
    return false;
  }
  return loadWorldBuffer(vm, worldTag, content.data(), content.size(), false, outErr);
}

}  // namespace tt
