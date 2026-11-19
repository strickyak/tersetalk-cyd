#include "tt_serial.h"
#include "vm/tt_world.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace tt {

namespace {

static std::string trimWs(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

}  // namespace

SerialRepl::SerialRepl(VM& v, DisplayHal& h, WindowManager& w)
    : vm(v), hal(h), wm(w) {}

void SerialRepl::printBanner() {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "\r\n=== TerseTalk-CYD (32-bit Tagged Oop, RAM Bytecode VM) ===\r\n"
                "Heap: %u KB | Flash: %s | SD: %s | Type :help for commands\r\n"
                "tt> ",
                static_cast<unsigned>(vm.heap.totalBytes / 1024u),
                hal.isFlashMounted() ? "OK" : "rodata",
                hal.isSdMounted() ? "OK" : "none");
  hal.serialPrint(buf);
}

void SerialRepl::poll() {
  std::string line;
  while (hal.pollSerialLine(line)) {
    processLine(line);
  }
}

void SerialRepl::processLine(const std::string& line) {
  if (puttingFile) {
    if (trimWs(line) == ":eof") {
      puttingFile = false;
      if (vm.writeVfsFile(putPath.c_str(), putBuffer.data(), putBuffer.size(), false)) {
        std::string msg = "Wrote " + std::to_string(putBuffer.size()) + " bytes to " + putPath + "\r\ntt> ";
        hal.serialPrint(msg.c_str());
      } else {
        hal.serialPrint("ERR: Failed writing file\r\ntt> ");
      }
      putBuffer.clear();
      wm.invalidateAll();
    } else {
      putBuffer += line + "\n";
    }
    return;
  }

  std::string cmd = trimWs(line);
  if (cmd.empty()) {
    hal.serialPrint("tt> ");
    return;
  }

  if (cmd[0] == ':') {
    size_t sp = cmd.find(' ');
    std::string verb = (sp == std::string::npos) ? cmd : cmd.substr(0, sp);
    std::string arg  = (sp == std::string::npos) ? ""  : trimWs(cmd.substr(sp + 1));

    if (verb == ":help") {
      hal.serialPrint(
        "Commands:\r\n"
        "  <expr>                  Evaluate TerseTalk expression\r\n"
        "  :ls [dir]               List /sd, /sd/worlds, or /flash\r\n"
        "  :cat <path>             Print file contents\r\n"
        "  :put <path>             Write lines into /sd/... until :eof\r\n"
        "  :states | :diff         List /sd/worlds/*.tt or print non-primordial diff\r\n"
        "  :save <name>            Save Class State savepoint (/sd/worlds/<name>.tt)\r\n"
        "  :loadstate <nm> [bak|-] Clean-reload savepoint (bak = rename w_main, - = clear)\r\n"
        "  :reset [bak|-]          Clean-reset to Primordial Prelude\r\n"
        "  :load <path>            Overlay .tt definitions without resetting\r\n"
        "  :docs | :doc <name>     List or open saved App documents (/sd/docs)\r\n"
        "  :open <AppCls>          Launch App window on CYD display\r\n"
        "  :max | :overlap         Switch display window layout\r\n"
        "  :gc  | :mem             Run GC and display memory stats\r\n"
        "tt> ");
      return;
    }
    if (verb == ":ls" || verb == ":docs" || verb == ":states") {
      std::string dir = (verb == ":docs") ? "/sd/docs"
                      : ((verb == ":states") ? "/sd/worlds" : (arg.empty() ? "/sd" : arg));
      std::vector<std::pair<std::string, uint32_t>> files;
      vm.listVfsFiles(dir.c_str(), files);
      std::sort(files.begin(), files.end());
      for (const auto& kv : files) {
        char row[128];
        std::snprintf(row, sizeof(row), "  %-24s %7u B\r\n",
                      kv.first.c_str(), static_cast<unsigned>(kv.second));
        hal.serialPrint(row);
      }
      hal.serialPrint("tt> ");
      return;
    }
    if (verb == ":doc") {
      Oop pStr = vm.newStr(arg.data(), arg.size());
      TempRootGuard rg(vm, pStr);
      Oop tmp = vm.newUsr(CLS_TMP);
      rg.push(tmp);
      try {
        vm.ticksRemaining = 2000000;
        vm.evalCode(tmp, "App fileIn: a", &pStr, 1);
        hal.serialPrint("OK opened doc.\r\ntt> ");
      } catch (const TerseError& e) {
        std::string m = "ERR: " + e.message + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      }
      return;
    }
    if (verb == ":cat") {
      std::string content;
      if (vm.readVfsFile(arg.c_str(), content)) {
        hal.serialPrint(content.c_str());
        if (content.empty() || content.back() != '\n') hal.serialPrint("\r\n");
      } else {
        hal.serialPrint("ERR: Cannot read file\r\n");
      }
      hal.serialPrint("tt> ");
      return;
    }
    if (verb == ":put") {
      if (arg.empty() || arg.rfind("/flash", 0) == 0) {
        hal.serialPrint("ERR: Usage :put /sd/filename.tt\r\ntt> ");
        return;
      }
      putPath     = (arg[0] == '/') ? arg : ("/sd/" + arg);
      putBuffer.clear();
      puttingFile = true;
      hal.serialPrint(".. send lines, end with :eof ..\r\n");
      return;
    }
    if (verb == ":diff") {
      std::string diff = serializeClassState(vm);
      if (diff.empty()) {
        hal.serialPrint("(all classes & methods match primordial prelude)\r\ntt> ");
      } else {
        hal.serialPrint(diff.c_str());
        if (diff.back() != '\n') hal.serialPrint("\r\n");
        hal.serialPrint("tt> ");
      }
      return;
    }
    if (verb == ":save") {
      std::string resolved, err;
      if (saveClassState(vm, arg.c_str(), &resolved, &err)) {
        std::string m = "OK saved " + resolved + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      } else {
        std::string m = "ERR: " + err + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      }
      return;
    }
    if (verb == ":loadstate" || verb == ":reset") {
      std::string targetArg, bakOpt;
      if (verb == ":reset") {
        bakOpt = arg;
      } else {
        size_t sp2 = arg.find(' ');
        targetArg  = (sp2 == std::string::npos) ? arg : arg.substr(0, sp2);
        bakOpt     = (sp2 == std::string::npos) ? ""  : trimWs(arg.substr(sp2 + 1));
        if (targetArg.empty()) {
          hal.serialPrint("ERR: Usage :loadstate <name> [backupName | -]\r\ntt> ");
          return;
        }
      }
      std::string vfsPath = targetArg.empty() ? "" : normalizeStatePath(targetArg);
      std::string oldLog;
      bool hasAutoLog = !vm.worldPath.empty() &&
                        vm.readVfsFile(vm.worldPath.c_str(), oldLog) &&
                        !oldLog.empty();
      if (hasAutoLog && bakOpt.empty()) {
        std::string sug = wm.nextAutoLogBackupName();
        std::string cmdEx = (verb == ":reset") ? ":reset" : (":loadstate " + targetArg);
        std::string ask = "Auto-log w_main.tt exists! Run '" + cmdEx + " " + sug +
                          "' to rename first, or '" + cmdEx + " -' to clear without backup.\r\ntt> ";
        hal.serialPrint(ask.c_str());
        return;
      }
      std::string renameTo = (bakOpt == "-") ? "" : bakOpt;
      std::string status;
      if (wm.performLoadClassState(vfsPath, renameTo, &status)) {
        std::string m = "OK: " + status + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      } else {
        std::string m = "ERR: " + status + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      }
      return;
    }
    if (verb == ":load") {
      std::string p = (arg.empty() || arg[0] == '/') ? arg : ("/sd/" + arg);
      std::string err;
      if (loadWorldFile(vm, vm.worldName.c_str(), p.c_str(), &err)) {
        hal.serialPrint("OK loaded.\r\ntt> ");
        wm.invalidateAll();
      } else {
        std::string m = "ERR: " + err + "\r\ntt> ";
        hal.serialPrint(m.c_str());
      }
      return;
    }
    if (verb == ":open") {
      if (wm.launchTerseAppByClassName(arg.c_str())) {
        hal.serialPrint("OK opened.\r\ntt> ");
      } else {
        hal.serialPrint("ERR: Could not open class.\r\ntt> ");
      }
      return;
    }
    if (verb == ":max") {
      wm.setMaximized(true);
      hal.serialPrint("Maximized.\r\ntt> ");
      return;
    }
    if (verb == ":overlap") {
      wm.setMaximized(false);
      hal.serialPrint("Overlapping windows.\r\ntt> ");
      return;
    }
    if (verb == ":gc" || verb == ":mem") {
      vm.runGC();
      char st[220];
#ifdef ARDUINO
      std::snprintf(st, sizeof(st),
                    "Arena: %u/%u B | Ext: %u B | ESP free: %u B (max blk: %u B) | GCs: %u\r\ntt> ",
                    static_cast<unsigned>(vm.heap.usedBytes),
                    static_cast<unsigned>(vm.heap.totalBytes),
                    static_cast<unsigned>(vm.heap.externalBytes),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(vm.heap.gcCount));
#else
      std::snprintf(st, sizeof(st),
                    "Arena: %u / %u B used (%u free) | Ext: %u B | GCs: %u | Cls: %u | Syms: %u\r\ntt> ",
                    static_cast<unsigned>(vm.heap.usedBytes),
                    static_cast<unsigned>(vm.heap.totalBytes),
                    static_cast<unsigned>(vm.heap.totalBytes - vm.heap.usedBytes),
                    static_cast<unsigned>(vm.heap.externalBytes),
                    static_cast<unsigned>(vm.heap.gcCount),
                    static_cast<unsigned>(vm.classCount),
                    static_cast<unsigned>(vm.symCount));
#endif
      hal.serialPrint(st);
      return;
    }
    hal.serialPrint("Unknown command (try :help)\r\ntt> ");
    return;
  }

  // Evaluate TerseTalk expression
  Oop tmp = vm.newUsr(CLS_TMP);
  TempRootGuard rg(vm, tmp);
  try {
    vm.ticksRemaining = 2000000;
    Oop res = vm.evalCode(tmp, cmd.c_str());
    std::string out = "=> " + vm.toReprString(res) + "\r\ntt> ";
    hal.serialPrint(out.c_str());
    wm.invalidateAll();
  } catch (const TerseError& e) {
    std::string out = "ERR: " + e.message + "\r\ntt> ";
    hal.serialPrint(out.c_str());
  }
}

}  // namespace tt
