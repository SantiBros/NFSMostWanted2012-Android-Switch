// Loader for 32-bit ARM ELF shared objects (the game's .so files).
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common.h"

namespace elf {

struct Module {
    std::string name;
    u32 base = 0, size = 0;
    u32 exec_lo = 0, exec_hi = 0;  // executable segment (guest addresses)
    u32 data_lo = 0, data_hi = 0;  // writable segment
    u32 symtab = 0, strtab = 0, nsyms = 0;
    u32 rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    u32 init = 0, init_array = 0, init_arraysz = 0;
    u32 fini_array = 0, fini_arraysz = 0;
    u32 exidx = 0, exidx_count = 0;
    std::vector<std::string> needed;
    std::unordered_map<std::string, u32> exports;  // defined dynamic symbols
    std::map<u32, std::string> by_addr;            // for crash reports
    bool relocated = false, initialized = false;

    bool contains(u32 a) const { return a >= base && a < base + size; }
    u32 find_export(const std::string& n) const {
        auto it = exports.find(n);
        return it == exports.end() ? 0 : it->second;
    }
};

// Map a library into guest memory (no relocation yet).
Module* load(const std::string& path, const std::string& name);
// Resolve relocations of every loaded module. Unresolved functions get a
// stub that logs and returns 0, so a missing import never crashes silently.
void relocate_all();
// Run DT_INIT / DT_INIT_ARRAY of every module, dependencies first.
void run_initializers();

Module* find(const std::string& name);
Module* module_for(u32 addr);
const std::vector<Module*>& all();

// Symbol lookup used by dlsym(): guest modules first, then host functions.
u32 lookup_global(const std::string& name);

// "libapp.so+0x1234 <symbol+0x10>" for logs.
std::string describe(u32 addr);

}  // namespace elf
