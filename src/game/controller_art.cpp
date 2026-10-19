#include "game/controller_art.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "game/setup.h"
#include "hle/hle.h"
#include "platform/thread.h"

namespace game::controller_art {

namespace {

#include "game/controller_art_images.inc"

// Where the picture (628x431) is in each atlas that has it, from the packs'
// Box records: the atlas size, the picture's left and top edge, and whether
// the pack stores premultiplied alpha. The packs store their rows bottom-up
// (as OpenGL reads them), so the picture's row y is the atlas's row
// h - 1 - top - y.
struct Atlas {
    int w, h, left, top;
    bool premultiplied;
};
constexpr Atlas kAtlases[] = {
    {1022, 1022, 2, 2, false},      // published.1x in_game
    {1022, 1012, 2, 456, true},     // published.1x menu
    {1278, 1918, 2, 261, false},    // published.2x in_game
    {2046, 2046, 806, 1010, true},  // published.2x menu
    {2046, 2686, 14, 436, false},   // published.4x in_game
    {4094, 4094, 14, 1286, true},   // published.4x menu
};
constexpr int kW = 628, kH = 431;

// Parts of the original picture (inclusive, picture coordinates) and their
// FNV-1a checksum (the same in all six atlases): the MOGA logo, "SELECT" and
// "START". All opaque, so premultiplication does not change them.
struct Region {
    int x0, y0, x1, y1;
};
constexpr Region kRegions[] = {{234, 174, 387, 254}, {170, 214, 223, 225}, {172, 325, 222, 338}};
constexpr u32 kOriginal = 0x057A4B2Au;

u32 checksum(const u8* rgba, const Atlas& a) {
    u32 h = 0x811C9DC5u;
    for (const Region& r : kRegions)
        for (int y = r.y0; y <= r.y1; y++) {
            const u8* row = rgba + ((size_t)(a.h - 1 - a.top - y) * a.w + a.left + r.x0) * 4;
            for (int i = 0; i < (r.x1 - r.x0 + 1) * 4; i++) h = (h ^ row[i]) * 0x01000193u;
        }
    return h;
}

// The two drawings, decoded once: straight alpha, top row first.
struct Drawings {
    std::vector<u8> handheld, controller;
    bool ok = false;
};
const Drawings& drawings() {
    static Drawings d;
    static std::once_flag once;
    std::call_once(once, [] {
        int w = 0, h = 0, w2 = 0, h2 = 0;
        d.ok = setup::decode_png(k_handheld_png, sizeof k_handheld_png, d.handheld, w, h) &&
               setup::decode_png(k_controller_png, sizeof k_controller_png, d.controller, w2, h2) && w == kW &&
               h == kH && w2 == kW && h2 == kH;
        if (!d.ok) LOGW("controls picture: the port's drawings could not be decoded; the game's own picture stays");
    });
    return d;
}

// One row of a drawing as the atlas stores it.
void put_row(u8* dst, const u8* src, bool premultiplied) {
    if (!premultiplied) {
        std::memcpy(dst, src, kW * 4);
        return;
    }
    for (int x = 0; x < kW; x++, src += 4, dst += 4) {
        const unsigned a = src[3];
        dst[0] = (u8)((src[0] * a + 127) / 255);
        dst[1] = (u8)((src[1] * a + 127) / 255);
        dst[2] = (u8)((src[2] * a + 127) / 255);
        dst[3] = (u8)a;
    }
}

struct Placed {
    u32 texture;
    const Atlas* atlas;
    bool handheld;  // the drawing it holds now
};
std::mutex g_mu;
std::vector<Placed> g_placed;
bool g_handheld = true;

const std::vector<u8>& drawing(bool handheld) {
    const Drawings& d = drawings();
    return handheld ? d.handheld : d.controller;
}

}  // namespace

void prepare() {
    if (std::getenv("NFSMW_NO_ART_PATCH")) return;
    platform::detach_thread(platform::spawn_thread(
        [] {
            platform::set_thread_name("controls-art");
            drawings();
        },
        256 * 1024));
}

void patch_texture(u8* rgba, int width, int height, u32 texture) {
    if (!rgba) return;
    for (const Atlas& a : kAtlases) {
        if (a.w != width || a.h != height) continue;
        static const bool off = std::getenv("NFSMW_NO_ART_PATCH") != nullptr;
        if (off) return;
        if (checksum(rgba, a) != kOriginal) continue;  // another texture of the same size, or already changed
        if (!drawings().ok) return;
        std::lock_guard lk(g_mu);
        const std::vector<u8>& pic = drawing(g_handheld);
        for (int y = 0; y < kH; y++)
            put_row(rgba + ((size_t)(a.h - 1 - a.top - y) * a.w + a.left) * 4, pic.data() + (size_t)y * kW * 4,
                    a.premultiplied);
        bool known = false;
        for (Placed& p : g_placed)
            if (p.texture == texture) p = {texture, &a, g_handheld}, known = true;
        if (!known && texture) g_placed.push_back({texture, &a, g_handheld});
        static std::atomic<int> count{0};
        if (count++ < 6)
            LOGI("controls picture: the port's %s drawing in place of the MOGA's (%dx%d texture atlas, texture %u)",
                 g_handheld ? "handheld" : "controller", width, height, texture);
        return;
    }
}

void forget_texture(u32 texture) {
    std::lock_guard lk(g_mu);
    for (size_t i = 0; i < g_placed.size(); i++)
        if (g_placed[i].texture == texture) {
            g_placed.erase(g_placed.begin() + (long)i);
            return;
        }
}

void set_handheld(bool handheld) {
    std::lock_guard lk(g_mu);
    if (handheld == g_handheld) return;
    g_handheld = handheld;
    if (g_placed.empty() || !drawings().ok) return;
    const std::vector<u8>& pic = drawing(handheld);
    std::vector<u8> rows((size_t)kW * kH * 4);
    for (Placed& p : g_placed) {
        if (p.handheld == handheld) continue;
        // Bottom row first, as the texture holds it.
        for (int y = 0; y < kH; y++)
            put_row(rows.data() + (size_t)(kH - 1 - y) * kW * 4, pic.data() + (size_t)y * kW * 4,
                    p.atlas->premultiplied);
        hle::gl_update_texture(p.texture, p.atlas->left, p.atlas->h - p.atlas->top - kH, kW, kH, rows.data());
        p.handheld = handheld;
    }
    LOGI("controls picture: now the %s drawing (%zu texture atlas%s)", handheld ? "handheld" : "controller",
         g_placed.size(), g_placed.size() == 1 ? "" : "es");
}

}  // namespace game::controller_art
