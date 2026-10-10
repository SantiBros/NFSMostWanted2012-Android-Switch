#include "core/elf.h"

#include <fstream>
#include <mutex>

#include "core/cpu.h"
#include "core/memory.h"
#include "core/thunks.h"

namespace elf {

namespace {

struct Ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Dyn {
    s32 tag;
    u32 val;
};
struct Sym {
    u32 name, value, size;
    u8 info, other;
    u16 shndx;
};
struct Rel {
    u32 offset, info;
};

constexpr u32 PT_LOAD = 1, PT_DYNAMIC = 2, PT_ARM_EXIDX = 0x70000001;
constexpr s32 DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_REL = 17,
              DT_RELSZ = 18, DT_INIT = 12, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26,
              DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_HASH = 4;
constexpr u32 R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_REL32 = 3, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22,
              R_ARM_RELATIVE = 23;
constexpr u8 STB_LOCAL = 0, STB_WEAK = 2;
constexpr u8 STT_OBJECT = 1, STT_FUNC = 2;

std::vector<Module*> g_modules;
u32 g_next_base = mem::MODULE_BASE;
std::mutex g_mutex;

const Sym* sym_at(const Module* m, u32 i) { return mem::ptr<Sym>(m->symtab + i * sizeof(Sym)); }
const char* sym_name(const Module* m, const Sym* s) { return mem::str(m->strtab + s->name); }

u32 resolve(Module* self, const Sym* s, const char* name) {
    u8 bind = s->info >> 4;
    if (bind == STB_LOCAL && s->shndx != 0) return self->base + s->value;
    // Android's linker: the requesting library first, then the rest of the
    // global group, then the system libraries (here: host thunks).
    if (u32 a = self->find_export(name)) return a;
    for (Module* m : g_modules)
        if (m != self)
            if (u32 a = m->find_export(name)) return a;
    if (u32 a = thunks::lookup(name)) return a;
    if (bind == STB_WEAK) return 0;
    u8 type = s->info & 0xF;
    if (type == STT_OBJECT) {
        u32 size = s->size ? s->size : 64;
        u32 a = mem::static_alloc(size < 64 ? 64 : size, 16);
        LOGW("%s: undefined data symbol %s -> zeroed placeholder", self->name.c_str(), name);
        thunks::add_data(name, a);
        return a;
    }
    LOGD("%s: unresolved import %s (stubbed)", self->name.c_str(), name);
    return thunks::make_missing(name);
}

void apply_rels(Module* m, u32 rels, u32 size) {
    for (u32 off = 0; off < size; off += sizeof(Rel)) {
        const Rel* r = mem::ptr<Rel>(rels + off);
        u32 type = r->info & 0xFF, symi = r->info >> 8;
        u32 P = m->base + r->offset;
        u32 S = 0;
        if (symi && type != R_ARM_RELATIVE) {
            const Sym* s = sym_at(m, symi);
            S = resolve(m, s, sym_name(m, s));
        }
        switch (type) {
        case R_ARM_NONE: break;
        case R_ARM_RELATIVE: mem::w32(P, mem::r32(P) + m->base); break;
        case R_ARM_ABS32: mem::w32(P, mem::r32(P) + S); break;
        case R_ARM_REL32: mem::w32(P, mem::r32(P) + S - P); break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT: mem::w32(P, S); break;
        default: LOGW("%s: unsupported relocation type %u at 0x%X", m->name.c_str(), type, r->offset);
        }
    }
}

}  // namespace

Module* load(const std::string& path, const std::string& name) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOGE("cannot open %s", path.c_str());
        return nullptr;
    }
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < sizeof(Ehdr)) return nullptr;
    Ehdr eh;
    std::memcpy(&eh, file.data(), sizeof eh);
    if (std::memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 1 || eh.machine != 40) {
        LOGE("%s is not a 32-bit ARM ELF", path.c_str());
        return nullptr;
    }
    std::vector<Phdr> ph(eh.phnum);
    std::memcpy(ph.data(), file.data() + eh.phoff, eh.phnum * sizeof(Phdr));

    u32 lo = 0xFFFFFFFF, hi = 0;
    for (auto& p : ph)
        if (p.type == PT_LOAD) {
            lo = std::min(lo, p.vaddr & ~0xFFFu);
            hi = std::max(hi, align_up(p.vaddr + p.memsz, 0x1000));
        }
    auto* m = new Module;
    m->name = name;
    m->size = hi - lo;
    {
        std::lock_guard lk(g_mutex);
        m->base = g_next_base - lo;
        g_next_base = align_up(g_next_base + m->size + 0x10000, 0x10000);
        if (g_next_base > mem::MODULE_END) fatal("module area exhausted");
    }
    for (auto& p : ph) {
        if (p.type == PT_LOAD) {
            std::memcpy(mem::host(m->base + p.vaddr), file.data() + p.offset, p.filesz);
            if (p.flags & 1) {  // PF_X
                m->exec_lo = m->base + p.vaddr;
                m->exec_hi = m->base + p.vaddr + p.memsz;
            } else if (p.flags & 2) {  // PF_W
                m->data_lo = m->base + p.vaddr;
                m->data_hi = m->base + p.vaddr + p.memsz;
            }
        } else if (p.type == PT_ARM_EXIDX) {
            m->exidx = m->base + p.vaddr;
            m->exidx_count = p.memsz / 8;
        }
    }
    u32 dyn = 0, hash = 0;
    for (auto& p : ph)
        if (p.type == PT_DYNAMIC) dyn = m->base + p.vaddr;
    if (!dyn) fatal("%s has no dynamic section", name.c_str());
    std::vector<u32> needed_off;
    for (u32 a = dyn;; a += sizeof(Dyn)) {
        const Dyn* d = mem::ptr<Dyn>(a);
        if (d->tag == DT_NULL) break;
        switch (d->tag) {
        case DT_NEEDED: needed_off.push_back(d->val); break;
        case DT_HASH: hash = m->base + d->val; break;
        case DT_STRTAB: m->strtab = m->base + d->val; break;
        case DT_SYMTAB: m->symtab = m->base + d->val; break;
        case DT_REL: m->rel = m->base + d->val; break;
        case DT_RELSZ: m->relsz = d->val; break;
        case DT_JMPREL: m->jmprel = m->base + d->val; break;
        case DT_PLTRELSZ: m->pltrelsz = d->val; break;
        case DT_INIT: m->init = m->base + d->val; break;
        case DT_INIT_ARRAY: m->init_array = m->base + d->val; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->val; break;
        case DT_FINI_ARRAY: m->fini_array = m->base + d->val; break;
        case DT_FINI_ARRAYSZ: m->fini_arraysz = d->val; break;
        default: break;
        }
    }
    for (u32 o : needed_off) m->needed.emplace_back(mem::str(m->strtab + o));
    if (!hash) fatal("%s has no DT_HASH", name.c_str());
    m->nsyms = mem::r32(hash + 4);  // nchain == number of symbols
    for (u32 i = 1; i < m->nsyms; i++) {
        const Sym* s = sym_at(m, i);
        if (s->shndx == 0 || (s->info >> 4) == STB_LOCAL) continue;
        const char* n = sym_name(m, s);
        u32 addr = m->base + s->value;
        m->exports.emplace(n, addr);
        u8 type = s->info & 0xF;
        if (type == STT_FUNC) m->by_addr[addr & ~1u] = n;
    }
    LOGI("loaded %s at 0x%08X (%u KiB, %zu exports)", name.c_str(), m->base, m->size / 1024, m->exports.size());
    std::lock_guard lk(g_mutex);
    g_modules.push_back(m);
    return m;
}

void relocate_all() {
    for (Module* m : g_modules) {
        if (m->relocated) continue;
        if (m->rel) apply_rels(m, m->rel, m->relsz);
        if (m->jmprel) apply_rels(m, m->jmprel, m->pltrelsz);
        m->relocated = true;
    }
}

static void init_module(Module* m) {
    if (m->initialized) return;
    m->initialized = true;
    for (auto& dep : m->needed)
        if (Module* d = find(dep)) init_module(d);
    auto& t = cpu::current();
    LOGI("running initializers of %s", m->name.c_str());
    if (m->init) t.call(m->init, {});
    for (u32 i = 0; i < m->init_arraysz / 4; i++) {
        u32 fn = mem::r32(m->init_array + i * 4);
        if (fn && fn != 0xFFFFFFFF) t.call(fn, {});
    }
}

void run_initializers() {
    for (Module* m : g_modules) init_module(m);
}

Module* find(const std::string& name) {
    for (Module* m : g_modules)
        if (m->name == name) return m;
    return nullptr;
}

Module* module_for(u32 addr) {
    for (Module* m : g_modules)
        if (m->contains(addr)) return m;
    return nullptr;
}

const std::vector<Module*>& all() { return g_modules; }

u32 lookup_global(const std::string& name) {
    for (Module* m : g_modules)
        if (u32 a = m->find_export(name)) return a;
    return thunks::lookup(name);
}

std::string describe(u32 addr) {
    char buf[256];
    if (const char* tn = thunks::name_at(addr & ~7u)) {
        std::snprintf(buf, sizeof buf, "0x%08X <host:%s>", addr, tn);
        return buf;
    }
    Module* m = module_for(addr);
    if (!m) {
        std::snprintf(buf, sizeof buf, "0x%08X", addr);
        return buf;
    }
    u32 a = addr & ~1u;
    auto it = m->by_addr.upper_bound(a);
    if (it != m->by_addr.begin()) {
        --it;
        std::snprintf(buf, sizeof buf, "0x%08X %s+0x%X <%s+0x%X>", addr, m->name.c_str(), a - m->base,
                      it->second.c_str(), a - it->first);
    } else {
        std::snprintf(buf, sizeof buf, "0x%08X %s+0x%X", addr, m->name.c_str(), a - m->base);
    }
    return buf;
}

}  // namespace elf
