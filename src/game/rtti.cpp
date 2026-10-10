#include "game/rtti.h"

#include <cstring>
#include <set>

#include "core/elf.h"
#include "core/memory.h"

namespace game::rtti {

namespace {
const char* class_string(elf::Module* m, u32 a) {
    if (!m->contains(a)) return nullptr;
    const char* s = mem::str(a);
    // Mangled class names start with a digit or 'N'.
    if (!((s[0] >= '0' && s[0] <= '9') || s[0] == 'N')) return nullptr;
    return s;
}

u32 find_string(elf::Module* m, const char* str) {
    const u8* base = mem::host(m->base);
    const u8* end = base + m->size;
    size_t n = std::strlen(str) + 1;
    for (const u8* p = base; p + n <= end; p++)
        if (*p == str[0] && std::memcmp(p, str, n) == 0 && (p == base || p[-1] == 0)) return mem::guest(p);
    return 0;
}

// Every typeinfo object in [lo, hi) that derives from `base_ti`.
std::set<u32> derived_typeinfos(elf::Module* m, u32 base_ti, u32 lo, u32 hi) {
    std::set<u32> T{base_ti};
    for (bool grew = true; grew;) {
        grew = false;
        for (u32 a = lo; a + 4 <= hi; a += 4) {
            if (!T.count(mem::r32(a))) continue;
            // __si_class_type_info: [vptr][name][base]
            u32 ti = a - 8;
            if (class_string(m, mem::r32(ti + 4)) && mem::r32(ti) != 0) {
                if (T.insert(ti).second) grew = true;
                continue;
            }
            // __vmi_class_type_info: [vptr][name][flags][count][{base, offset_flags}...]
            for (u32 k = 0; k < 4; k++) {
                u32 vti = a - 16 - 8 * k;
                u32 count = mem::r32(vti + 12);
                if (count > k && count <= 8 && class_string(m, mem::r32(vti + 4)) && mem::r32(vti + 8) < 16) {
                    if (T.insert(vti).second) grew = true;
                    break;
                }
            }
        }
    }
    return T;
}
}  // namespace

bool is_code(elf::Module* m, u32 a) { return m && a >= m->exec_lo && a < m->exec_hi; }

std::vector<VTable> derived_vtables(elf::Module* m, const char* cls) {
    std::vector<VTable> out;
    if (!m) return out;
    u32 name = find_string(m, cls);
    if (!name) return out;
    u32 lo = m->data_lo, hi = m->data_hi;
    u32 base_ti = 0;
    for (u32 a = lo; a + 4 <= hi; a += 4)
        if (mem::r32(a) == name) {
            base_ti = a - 4;
            break;
        }
    if (!base_ti) return out;
    std::set<u32> tis = derived_typeinfos(m, base_ti, lo, hi);
    // Primary vtables: [offset-to-top = 0][typeinfo][functions...]
    for (u32 a = lo + 4; a + 4 <= hi; a += 4) {
        u32 ti = mem::r32(a);
        if (!tis.count(ti) || mem::r32(a - 4) != 0 || !is_code(m, mem::r32(a + 4))) continue;
        out.push_back({a + 4, ti, class_string(m, mem::r32(ti + 4))});
    }
    return out;
}

std::string class_name(elf::Module* m, u32 obj) {
    if (!obj || !m) return "null";
    u32 vt = mem::r32(obj);
    if (!m->contains(vt)) return "?";
    u32 ti = mem::r32(vt - 4);
    const char* n = m->contains(ti) ? class_string(m, mem::r32(ti + 4)) : nullptr;
    return n ? n : "?";
}

}  // namespace game::rtti
