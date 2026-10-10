// Finding the game's C++ classes through its RTTI (type names such as
// "N2im11scene2d_new7layouts6ButtonE") instead of hard-coded addresses.
#pragma once

#include <string>
#include <vector>

#include "core/common.h"

namespace elf {
struct Module;
}

namespace game::rtti {

struct VTable {
    u32 vtable;       // address of the first virtual function slot
    u32 typeinfo;
    const char* name; // mangled class name
};

// Primary vtables of class `cls` and of every class derived from it.
std::vector<VTable> derived_vtables(elf::Module* m, const char* cls);

// Is `a` inside the module's executable segment?
bool is_code(elf::Module* m, u32 a);

// Mangled class name of a guest object ("?" if unknown).
std::string class_name(elf::Module* m, u32 obj);

}  // namespace game::rtti
