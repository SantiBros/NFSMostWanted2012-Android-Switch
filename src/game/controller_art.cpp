#include "game/controller_art.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace game::controller_art {

namespace {

// Where the picture (628x431) is in each atlas that has it, from the packs'
// Box records: the atlas size, and the picture's left and top edge. The packs
// store their rows bottom-up (as OpenGL reads them), so the picture's row y
// is the atlas's row h - 1 - top - y.
struct Atlas {
    int w, h, left, top;
};
constexpr Atlas kAtlases[] = {
    {1022, 1022, 2, 2},       // published.1x in_game
    {1022, 1012, 2, 456},     // published.1x menu
    {1278, 1918, 2, 261},     // published.2x in_game
    {2046, 2046, 806, 1010},  // published.2x menu
    {2046, 2686, 14, 436},    // published.4x in_game
    {4094, 4094, 14, 1286},   // published.4x menu
};
constexpr int kW = 628, kH = 431;

// The parts that change, in the picture's coordinates (inclusive), each with
// a clean row of the same columns to copy over it: the panel and its two
// dividers are plain vertical bands, so a row above or below the logo or a
// label is exactly what belongs behind it.
struct Region {
    int x0, y0, x1, y1, clean_row;
};
constexpr Region kLogo = {234, 174, 387, 254, 170};   // the MOGA logo (and its glow over the dividers)
constexpr Region kSelect = {170, 214, 223, 225, 226};  // "SELECT"
constexpr Region kStart = {172, 325, 222, 338, 324};   // "START"
constexpr Region kRegions[] = {kLogo, kSelect, kStart};
// FNV-1a over those regions in the original picture (the same in all six
// atlases), and after the change.
constexpr u32 kOriginal = 0x057A4B2Au;

struct Picture {
    u8* rgba;
    int w, h, left, top;
    u8* at(int x, int y) const { return rgba + ((size_t)(h - 1 - top - y) * w + left + x) * 4; }
};

u32 checksum(const Picture& p) {
    u32 h = 0x811C9DC5u;
    for (const Region& r : kRegions)
        for (int y = r.y0; y <= r.y1; y++) {
            const u8* row = p.at(r.x0, y);
            for (int i = 0; i < (r.x1 - r.x0 + 1) * 4; i++) h = (h ^ row[i]) * 0x01000193u;
        }
    return h;
}

void fill_white(const Picture& p, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) std::memset(p.at(x, y), 0xFF, 4);
}

void change(const Picture& p) {
    for (const Region& r : kRegions)
        for (int y = r.y0; y <= r.y1; y++)
            std::memcpy(p.at(r.x0, y), p.at(r.x0, r.clean_row), (size_t)(r.x1 - r.x0 + 1) * 4);
    // - and + in white, 13 pixels wide with 3-pixel strokes (the labels' own
    // weight), centred over the two buttons (x = 194).
    constexpr int cx = 194;
    fill_white(p, cx - 6, 218, cx + 6, 220);  // -
    fill_white(p, cx - 6, 330, cx + 6, 332);  // +
    fill_white(p, cx - 1, 325, cx + 1, 337);
}

}  // namespace

void patch_texture(u8* rgba, int width, int height) {
    if (!rgba) return;
    for (const Atlas& a : kAtlases) {
        if (a.w != width || a.h != height) continue;
        if (a.left + kW > width || a.top + kH > height) continue;
        static const bool off = std::getenv("NFSMW_NO_ART_PATCH") != nullptr;
        if (off) return;
        const Picture p{rgba, width, height, a.left, a.top};
        if (checksum(p) != kOriginal) continue;  // another texture of the same size, or already changed
        change(p);
        static std::atomic<int> count{0};
        if (count++ < 4)
            LOGI("controls picture: MOGA logo removed, SELECT / START -> - / + (%dx%d texture atlas)", width, height);
        return;
    }
}

}  // namespace game::controller_art
