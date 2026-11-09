#include "game/icon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb/stb_image_write.h"

#include "core/common.h"
#include "game/paths.h"
#include "game/setup.h"

namespace game::icon {

namespace {

constexpr const char* kTitle = "NFS Most Wanted 2012";
constexpr int kSize = 256;  // what the Homebrew Menu and forwarders show

// ---- a little image code: premultiplied RGBA, 0..1 -----------------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // w * h * 4
    float* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};

bool decode(const std::vector<u8>& png, Image& out) {
    std::vector<u8> rgba;
    if (png.empty() || !setup::decode_png(png.data(), png.size(), rgba, out.w, out.h) || out.w <= 0 || out.h <= 0)
        return false;
    out.px.resize((size_t)out.w * out.h * 4);
    for (size_t i = 0; i < (size_t)out.w * out.h; i++) {
        const float a = rgba[i * 4 + 3] / 255.f;
        for (int c = 0; c < 3; c++) out.px[i * 4 + c] = rgba[i * 4 + c] / 255.f * a;
        out.px[i * 4 + 3] = a;
    }
    return true;
}

// Weights of the source pixels for each output pixel along one axis: a box
// filter when shrinking, linear interpolation when enlarging.
std::vector<std::vector<std::pair<int, float>>> axis_weights(int src, int dst) {
    std::vector<std::vector<std::pair<int, float>>> w(dst);
    const double scale = (double)src / dst;
    for (int o = 0; o < dst; o++) {
        if (scale >= 1) {
            const double a = o * scale, b = (o + 1) * scale;
            for (int i = (int)std::floor(a); i < (int)std::ceil(b) && i < src; i++) {
                const double cover = std::min<double>(b, i + 1) - std::max<double>(a, i);
                if (cover > 0) w[o].push_back({i, (float)(cover / scale)});
            }
        } else {
            const double c = (o + 0.5) * scale - 0.5;
            const int i0 = (int)std::floor(c);
            const float f = (float)(c - i0);
            w[o].push_back({std::clamp(i0, 0, src - 1), 1 - f});
            w[o].push_back({std::clamp(i0 + 1, 0, src - 1), f});
        }
    }
    return w;
}

Image resize(const Image& in, int w, int h) {
    const auto wx = axis_weights(in.w, w), wy = axis_weights(in.h, h);
    Image tmp{w, in.h, std::vector<float>((size_t)w * in.h * 4)};
    for (int y = 0; y < in.h; y++)
        for (int x = 0; x < w; x++) {
            float* d = tmp.at(x, y);
            for (auto [i, k] : wx[x])
                for (int c = 0; c < 4; c++) d[c] += in.at(i, y)[c] * k;
        }
    Image out{w, h, std::vector<float>((size_t)w * h * 4)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float* d = out.at(x, y);
            for (auto [i, k] : wy[y])
                for (int c = 0; c < 4; c++) d[c] += tmp.at(x, i)[c] * k;
        }
    return out;
}

Image crop(const Image& in, int x0, int y0, int x1, int y1) {
    Image out{x1 - x0, y1 - y0, {}};
    out.px.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < out.h; y++) std::memcpy(out.at(0, y), in.at(x0, y0 + y), (size_t)out.w * 16);
    return out;
}

void over(Image& dst, const Image& src) {  // same size
    for (size_t i = 0; i < dst.px.size(); i += 4) {
        const float a = src.px[i + 3];
        for (int c = 0; c < 4; c++) dst.px[i + c] = src.px[i + c] + dst.px[i + c] * (1 - a);
    }
}

// The game's icon as the launcher shows it, as a 256x256 JPEG: the adaptive
// icon's two layers (the visible middle 72/108 of each side), else the plain
// icon cropped to its shape on white. Same as tools/prepare_sd.py.
bool make_icon(std::vector<u8>& jpeg) {
    std::vector<u8> bg_png, fg_png, plain_png;
    if (!setup::game_icon_files(bg_png, fg_png, plain_png)) return false;
    Image img, bg, fg, plain;
    if (decode(bg_png, bg) && decode(fg_png, fg)) {
        if (fg.w != bg.w || fg.h != bg.h) fg = resize(fg, bg.w, bg.h);
        over(bg, fg);
        const int m = (int)std::lround(bg.w * 18.0 / 108.0);
        img = crop(bg, m, m, bg.w - m, bg.h - m);
    } else if (decode(plain_png, plain)) {
        int x0 = plain.w, y0 = plain.h, x1 = 0, y1 = 0;
        for (int y = 0; y < plain.h; y++)
            for (int x = 0; x < plain.w; x++)
                if (plain.at(x, y)[3] > 0) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
        if (x1 <= x0 || y1 <= y0) x0 = y0 = 0, x1 = plain.w, y1 = plain.h;
        Image icon = crop(plain, x0, y0, x1, y1);
        img = Image{icon.w, icon.h, std::vector<float>(icon.px.size(), 1.f)};  // white
        over(img, icon);
    } else {
        return false;
    }
    img = resize(img, kSize, kSize);
    std::vector<u8> rgb((size_t)kSize * kSize * 3);
    for (size_t i = 0; i < (size_t)kSize * kSize; i++)
        for (int c = 0; c < 3; c++) {
            const float a = img.px[i * 4 + 3];
            const float v = img.px[i * 4 + c] + (1 - a);  // anything see-through: on white
            rgb[i * 3 + c] = (u8)std::lround(std::clamp(v, 0.f, 1.f) * 255);
        }
    jpeg.clear();
    stbi_write_jpg_to_func([](void* ctx, void* data, int n) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + n);
    }, &jpeg, kSize, kSize, 3, rgb.data(), 92);
    return !jpeg.empty();
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    u8 buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}
bool write_file(const std::string& path, const std::vector<u8>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

u32 rd32(const std::vector<u8>& d, size_t o) { u32 v; std::memcpy(&v, &d[o], 4); return v; }
u64 rd64(const std::vector<u8>& d, size_t o) { u64 v; std::memcpy(&v, &d[o], 8); return v; }
void wr64(std::vector<u8>& d, size_t o, u64 v) { std::memcpy(&d[o], &v, 8); }

// The icon and the title (all 16 languages of the NACP) of an .nro, rewritten
// through a temporary file. `changed` is false when they were already so.
bool patch_nro(const std::string& path, const std::vector<u8>& jpeg, bool& changed) {
    changed = false;
    std::vector<u8> nro;
    if (!read_file(path, nro) || nro.size() < 0x80 || std::memcmp(&nro[0x10], "NRO0", 4) != 0) return false;
    const u32 size = rd32(nro, 0x18);
    if ((size_t)size + 56 > nro.size() || std::memcmp(&nro[size], "ASET", 4) != 0) return false;
    const size_t aset = size;
    std::vector<u8> blob[3];  // icon, NACP, RomFS
    for (int i = 0; i < 3; i++) {
        const u64 off = rd64(nro, aset + 8 + 16 * i), n = rd64(nro, aset + 16 + 16 * i);
        if (n && (aset + off + n > nro.size())) return false;
        if (n) blob[i].assign(nro.begin() + (ptrdiff_t)(aset + off), nro.begin() + (ptrdiff_t)(aset + off + n));
    }
    std::vector<u8> nacp = blob[1];
    if (nacp.size() >= 0x3000)
        for (int lang = 0; lang < 16; lang++) {
            char* name = reinterpret_cast<char*>(&nacp[lang * 0x300]);
            std::memset(name, 0, 0x200);
            std::memcpy(name, kTitle, std::strlen(kTitle));
        }
    if (blob[0] == jpeg && blob[1] == nacp) return true;
    blob[0] = jpeg;
    blob[1] = nacp;
    std::vector<u8> out(nro.begin(), nro.begin() + (ptrdiff_t)aset + 56);
    u64 pos = 56;
    for (int i = 0; i < 3; i++) {
        wr64(out, aset + 8 + 16 * i, blob[i].empty() ? 0 : pos);
        wr64(out, aset + 16 + 16 * i, blob[i].size());
        out.insert(out.end(), blob[i].begin(), blob[i].end());
        pos += blob[i].size();
    }
    const std::string tmp = path + ".tmp", old = path + ".old";
    std::remove(tmp.c_str());
    if (!write_file(tmp, out)) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(old.c_str());
    if (std::rename(path.c_str(), old.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::rename(old.c_str(), path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(old.c_str());
    changed = true;
    return true;
}

}  // namespace

void update() {
    if (std::getenv("NFSMW_NO_ICON")) return;
    // The icon from the player's APK, kept next to the game's files.
    const std::string saved = paths::game_dir() + "/icon.jpg";
    std::vector<u8> jpeg;
    if (!read_file(saved, jpeg) || jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        if (!make_icon(jpeg)) {
            LOGI("icon: no APK with the game's icon in the game folder; the port keeps its own icon");
            return;
        }
        if (write_file(saved, jpeg)) LOGI("icon: saved the game's icon from your APK as %s", paths::display(saved).c_str());
    }
    std::string nro = paths::self_nro();
    if (const char* t = std::getenv("NFSMW_TEST_NRO")) nro = t;
    if (nro.empty()) return;
#if defined(__SWITCH__)
    romfsExit();  // the .nro's RomFS keeps the file open
#endif
    bool changed = false;
    const bool ok = patch_nro(nro, jpeg, changed);
#if defined(__SWITCH__)
    romfsInit();
#endif
    if (!ok) LOGW("icon: could not put the game's icon on %s", paths::display(nro).c_str());
    else if (changed)
        LOGI("icon: %s now has the game's icon and the title \"%s\" (a forwarder installed from now on shows them)",
             paths::display(nro).c_str(), kTitle);
}

}  // namespace game::icon
