// Soft-float AAPCS argument marshalling between guest registers/stack and
// ordinary C++ functions. A host thunk is written as a normal function such as
//
//     static u32 h_strlen(u32 s) { return strlen(mem::str(s)); }
//
// and registered with REG("strlen", h_strlen). Pointers are passed as u32 guest
// addresses; floats/doubles arrive as values (armeabi-v7a passes them in core
// registers). A trailing VarArgs parameter receives the variadic part.
#pragma once

#include <functional>
#include <tuple>
#include <type_traits>

#include "core/cpu.h"
#include "core/memory.h"

namespace abi {

// Reader over "..." arguments, either still in registers/stack (direct
// variadic call) or behind an ARM va_list pointer.
class VarArgs {
public:
    static VarArgs from_regs(const std::array<u32, 16>& r, int ncrn, u32 nsaa) {
        VarArgs v;
        for (int i = 0; i < 4; i++) v.r_[i] = r[i];
        v.sp_ = r[13];
        v.idx_ = ncrn < 4 ? (u32)ncrn : 4 + (nsaa - r[13]) / 4;
        v.mem_ = false;
        return v;
    }
    static VarArgs from_valist(u32 ap) {
        VarArgs v;
        v.ap_ = ap;
        v.mem_ = true;
        return v;
    }
    u32 next32() {
        if (mem_) {
            u32 x = mem::r32(ap_);
            ap_ += 4;
            return x;
        }
        u32 x = idx_ < 4 ? r_[idx_] : mem::r32(sp_ + (idx_ - 4) * 4);
        idx_++;
        return x;
    }
    u64 next64() {
        if (mem_) {
            ap_ = align_up(ap_, 8);
            u64 x = mem::r64(ap_);
            ap_ += 8;
            return x;
        }
        if (idx_ & 1) idx_++;
        u64 lo = next32();
        u64 hi = next32();
        return lo | (hi << 32);
    }
    double next_double() { return bit_cast_<double>(next64()); }

private:
    std::array<u32, 4> r_{};
    u32 sp_ = 0, idx_ = 0, ap_ = 0;
    bool mem_ = false;
};

class ArgReader {
public:
    explicit ArgReader(cpu::GuestThread& t) : r_(t.regs()), nsaa_(r_[13]) {}

    u32 word() {
        if (ncrn_ < 4) return r_[ncrn_++];
        u32 v = mem::r32(nsaa_);
        nsaa_ += 4;
        return v;
    }
    u64 dword() {
        if (ncrn_ & 1) ncrn_++;
        if (ncrn_ <= 2) {
            u64 v = r_[ncrn_] | ((u64)r_[ncrn_ + 1] << 32);
            ncrn_ += 2;
            return v;
        }
        ncrn_ = 4;
        nsaa_ = align_up(nsaa_, 8);
        u64 v = mem::r64(nsaa_);
        nsaa_ += 8;
        return v;
    }

    template <class T>
    T get() {
        if constexpr (std::is_same_v<T, VarArgs>) {
            return VarArgs::from_regs(r_, ncrn_, nsaa_);
        } else if constexpr (std::is_same_v<T, float>) {
            return bit_cast_<float>(word());
        } else if constexpr (std::is_same_v<T, double>) {
            return bit_cast_<double>(dword());
        } else if constexpr (sizeof(T) == 8) {
            return static_cast<T>(dword());
        } else if constexpr (std::is_same_v<T, bool>) {
            return (word() & 0xFF) != 0;
        } else {
            static_assert(sizeof(T) <= 4, "unsupported thunk argument type");
            return static_cast<T>(word());
        }
    }

private:
    std::array<u32, 16>& r_;
    int ncrn_ = 0;
    u32 nsaa_;
};

template <class R>
inline void set_ret(cpu::GuestThread& t, R v) {
    auto& r = t.regs();
    if constexpr (std::is_same_v<R, float>) {
        r[0] = bit_cast_<u32>(v);
    } else if constexpr (std::is_same_v<R, double>) {
        u64 x = bit_cast_<u64>(v);
        r[0] = (u32)x;
        r[1] = (u32)(x >> 32);
    } else if constexpr (sizeof(R) == 8) {
        u64 x = (u64)v;
        r[0] = (u32)x;
        r[1] = (u32)(x >> 32);
    } else if constexpr (std::is_same_v<R, bool>) {
        r[0] = v ? 1 : 0;
    } else {
        r[0] = (u32)(s32)v;
    }
}

template <class R, class... A>
std::function<void(cpu::GuestThread&)> wrap(R (*f)(A...)) {
    return [f](cpu::GuestThread& t) {
        ArgReader rd(t);
        // braced init guarantees left-to-right evaluation of the reads
        std::tuple<std::decay_t<A>...> args{rd.get<std::decay_t<A>>()...};
        if constexpr (std::is_void_v<R>) {
            std::apply(f, args);
        } else {
            R ret = std::apply(f, args);
            set_ret(t, ret);
        }
    };
}

}  // namespace abi
