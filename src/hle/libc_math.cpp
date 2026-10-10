// libm. Soft-float ABI: floats/doubles travel in core registers, which the
// marshalling layer already handles, so these are one-liners.
#include <cmath>

#include "hle/hle.h"

namespace hle {

namespace {
double h_frexp(double x, u32 e) {
    int ex = 0;
    double r = std::frexp(x, &ex);
    mem::w32(e, (u32)ex);
    return r;
}
double h_modf(double x, u32 ip) {
    double i = 0;
    double r = std::modf(x, &i);
    mem::w64(ip, bit_cast_<u64>(i));
    return r;
}
float h_modff(float x, u32 ip) {
    float i = 0;
    float r = std::modf(x, &i);
    mem::w32(ip, bit_cast_<u32>(i));
    return r;
}
s32 h_lrintf(float x) { return (s32)std::lrint(x); }
s32 h_lrint(double x) { return (s32)std::lrint(x); }
s32 h_lroundf(float x) { return (s32)std::lround(x); }
void h_sincosf(float x, u32 s, u32 c) {
    mem::w32(s, bit_cast_<u32>(std::sin(x)));
    mem::w32(c, bit_cast_<u32>(std::cos(x)));
}
}  // namespace

#define D1(n) REG(#n, +[](double a) -> double { return std::n(a); })
#define F1(n, h) REG(#n, +[](float a) -> float { return std::h(a); })
#define D2(n) REG(#n, +[](double a, double b) -> double { return std::n(a, b); })
#define F2(n, h) REG(#n, +[](float a, float b) -> float { return std::h(a, b); })

void register_libc_math() {
    D1(acos); D1(asin); D1(atan); D1(cos); D1(sin); D1(tan); D1(cosh); D1(sinh); D1(tanh);
    D1(exp); D1(log); D1(log10); D1(log2); D1(sqrt); D1(ceil); D1(floor); D1(rint); D1(round); D1(trunc);
    D1(fabs); D1(exp2); D1(cbrt);
    F1(acosf, acos); F1(asinf, asin); F1(atanf, atan); F1(cosf, cos); F1(sinf, sin); F1(tanf, tan);
    F1(expf, exp); F1(logf, log); F1(log10f, log10); F1(log2f, log2); F1(sqrtf, sqrt); F1(ceilf, ceil);
    F1(floorf, floor); F1(rintf, rint); F1(roundf, round); F1(truncf, trunc); F1(fabsf, fabs); F1(exp2f, exp2);
    D2(atan2); D2(pow); D2(fmod); D2(hypot); D2(fmax); D2(fmin);
    F2(atan2f, atan2); F2(powf, pow); F2(fmodf, fmod); F2(hypotf, hypot); F2(fmaxf, fmax); F2(fminf, fmin);
    REG("ldexp", +[](double x, int e) -> double { return std::ldexp(x, e); });
    REG("ldexpf", +[](float x, int e) -> float { return std::ldexp(x, e); });
    REG("frexp", h_frexp);
    REG("modf", h_modf);
    REG("modff", h_modff);
    REG("lrintf", h_lrintf);
    REG("lrint", h_lrint);
    REG("lroundf", h_lroundf);
    REG("sincosf", h_sincosf);
}

}  // namespace hle
