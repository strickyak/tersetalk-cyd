#include "tt_world.h"

#include <algorithm>
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
      while (!more.empty() && more.back() == '\n') {
        more.pop_back();
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

std::string normalizeStatePath(const std::string& nameOrPath) {
  std::string s = nameOrPath;
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  if (s.empty()) return "";
  if (s.find('/') == std::string::npos) {
    s = "/sd/worlds/" + s;
  }
  if (s.size() < 3 || s.substr(s.size() - 3) != ".tt") {
    s += ".tt";
  }
  return s;
}

static void appendIndentedBody(std::string& out, const char* body, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    size_t nl = pos;
    while (nl < len && body[nl] != '\n') nl++;
    size_t lineLen = nl - pos;
    if (lineLen > 0 && body[pos + lineLen - 1] == '\r') lineLen--;
    out.push_back(' ');
    out.append(body + pos, lineLen);
    out.push_back('\n');
    pos = (nl < len) ? (nl + 1) : len;
  }
}

std::string serializeClassState(VM& vm) {
  std::string out;

  // Build topological superclass-first order over all classes `1 .. vm.classCount`
  std::vector<uint8_t> visited(vm.classCount + 1u, 0);
  std::vector<uint16_t> topoOrder;
  topoOrder.reserve(vm.classCount);

  auto visitCls = [&](auto& selfRef, uint16_t cid) -> void {
    if (cid == 0 || cid > vm.classCount || visited[cid]) return;
    visited[cid] = 1;
    ObjCls* c = vm.clsById(cid);
    if (!c) return;
    if (c->superClsId != 0 && c->superClsId <= vm.classCount) {
      selfRef(selfRef, c->superClsId);
    }
    topoOrder.push_back(cid);
  };
  for (uint16_t cid = 1; cid <= vm.classCount; cid++) {
    visitCls(visitCls, cid);
  }

  // 1. Non-primordial instance classes (`cls <Sub> <Super>`)
  for (uint16_t cid : topoOrder) {
    ObjCls* c = vm.clsById(cid);
    if (!c || c->isPrimordial) continue;
    // Skip metaclasses (`*Cls`), which `defineClass` creates automatically alongside `Foo`
    if (c->hdr.clsId == CLS_METACLS) continue;
    ObjCls* sup = vm.clsById(c->superClsId);
    std::string cname   = vm.toStdString(c->nameStr);
    std::string supName = sup ? vm.toStdString(sup->nameStr) : "Usr";
    out += "cls " + cname + " " + supName + "\n\n";
  }

  // 2. Modified/added instance variables (`vars <ClassName>`)
  for (uint16_t cid : topoOrder) {
    ObjCls* c = vm.clsById(cid);
    if (!c || !c->varsModified) continue;
    if (!c->isPrimordial && c->myVarCount == 0) continue;

    std::string cname = vm.toStdString(c->nameStr);
    std::string vline;
    if (isObj(c->myVarsList)) {
      auto* vl = reinterpret_cast<ObjList*>(objFromOop(c->myVarsList));
      for (uint32_t i = 0; i < vl->len; i++) {
        if (i > 0) vline.push_back(' ');
        vline += vm.toStdString(vl->items[i]);
      }
    }
    out += "vars " + cname + "\n";
    out += " " + vline + "\n\n";
  }

  // 3. Non-primordial user methods (`meth <ClassName> <selector>`) on both classes & metaclasses
  for (uint16_t cid = 1; cid <= vm.classCount; cid++) {
    ObjCls* c = vm.clsById(cid);
    if (!c || !isObj(c->methDict)) continue;

    std::vector<std::pair<std::string, ObjMeth*>> entries;
    auto* md = reinterpret_cast<ObjDict*>(objFromOop(c->methDict));
    for (uint32_t j = 0; j < md->cap; j++) {
      Oop k = md->entries[j].key;
      if (k == kOopNull || k == kOopSuper) continue;
      Oop v = md->entries[j].val;
      if (!isObj(v) || objFromOop(v)->format != OBJ_METH) continue;
      auto* m = reinterpret_cast<ObjMeth*>(objFromOop(v));
      if (m->isPrim || m->isPrimordial || m->isAutoAccessor) continue;
      entries.emplace_back(vm.toStdString(k), m);
    }
    if (entries.empty()) continue;

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string cname = vm.toStdString(c->nameStr);
    for (const auto& kv : entries) {
      out += "meth " + cname + " " + kv.first + "\n";
      if (kv.second->source && kv.second->sourceLen > 0) {
        appendIndentedBody(out, kv.second->source, kv.second->sourceLen);
      } else {
        out += " me\n";
      }
      out.push_back('\n');
    }
  }

  return out;
}

bool saveClassState(VM& vm, const char* nameOrPath,
                    std::string* outResolvedPath, std::string* outErr) {
  std::string path = normalizeStatePath(nameOrPath ? nameOrPath : "");
  if (path.empty()) {
    if (outErr) *outErr = "Class state name cannot be empty";
    return false;
  }
  if (path == vm.worldPath || path == "/sd/worlds/w_main.tt") {
    if (outErr) *outErr = "Choose a savepoint name other than w_main";
    return false;
  }
  if (path.rfind("/flash/", 0) == 0 || path == "/flash") {
    if (outErr) *outErr = "/flash is immutable; save to /sd/worlds";
    return false;
  }

  std::string body = "# TerseTalk-CYD Class State\n\n" + serializeClassState(vm);
  if (!vm.writeVfsFile(path.c_str(), body.data(), body.size(), false)) {
    if (outErr) *outErr = "Failed writing " + path;
    return false;
  }
  if (outResolvedPath) *outResolvedPath = path;
  return true;
}

}  // namespace tt
