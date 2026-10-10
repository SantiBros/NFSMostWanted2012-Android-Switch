#define STB_TRUETYPE_IMPLEMENTATION
#include "hle/jni/text.h"

#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "external/stb/stb_truetype.h"
#include "hle/hle.h"
#include "game/paths.h"
#include "platform/platform.h"

namespace jni::text {

namespace {

struct Font {
    std::vector<u8> data;
    stbtt_fontinfo info{};
};

struct Paint {
    std::shared_ptr<Font> font;
    float size = 16.f;
};

std::mutex g_font_mutex;
std::map<std::string, std::shared_ptr<Font>> g_fonts;

std::shared_ptr<Font> load_font(const std::string& guest_path) {
    std::lock_guard lk(g_font_mutex);
    auto it = g_fonts.find(guest_path);
    if (it != g_fonts.end()) return it->second;
    std::string p = guest_path;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    std::vector<std::string> candidates = {game::paths::assets_dir() + "/" + p, hle::map_path(guest_path.c_str())};
    for (auto& c : candidates) {
        std::ifstream f(c, std::ios::binary);
        if (!f) continue;
        auto font = std::make_shared<Font>();
        font->data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (!stbtt_InitFont(&font->info, font->data.data(), stbtt_GetFontOffsetForIndex(font->data.data(), 0))) continue;
        LOGI("font loaded: %s", c.c_str());
        g_fonts[guest_path] = font;
        return font;
    }
    LOGW("font not found: %s", guest_path.c_str());
    g_fonts[guest_path] = nullptr;
    return nullptr;
}

std::shared_ptr<Font> default_font() {
    auto f = load_font("published/fonts/gothambook.ttf");
    return f;
}

std::vector<u32> utf8_decode(const std::string& s) {
    std::vector<u32> out;
    for (size_t i = 0; i < s.size();) {
        u8 c = (u8)s[i];
        u32 cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
        i += n;
        out.push_back(cp);
    }
    return out;
}

Paint* paint(jref p) {
    Object* o = get(p);
    return o ? static_cast<Paint*>(o->native.get()) : nullptr;
}

jref make_paint(std::shared_ptr<Font> f, float size) {
    jref o = new_object("android/graphics/Paint");
    auto p = std::make_shared<Paint>();
    p->font = f ? f : default_font();
    p->size = size;
    get(o)->native = p;
    return o;
}

float measure(Paint* p, const std::string& s) {
    if (!p || !p->font) return (float)s.size() * (p ? p->size : 16) * 0.5f;
    auto& info = p->font->info;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, p->size);
    float w = 0;
    auto cps = utf8_decode(s);
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, (int)cps[i], &adv, &lsb);
        w += adv * scale;
        if (i + 1 < cps.size()) w += scale * stbtt_GetCodepointKernAdvance(&info, (int)cps[i], (int)cps[i + 1]);
    }
    return w;
}

jref make_bitmap(u32 w, u32 h) {
    jref b = new_object("android/graphics/Bitmap");
    Object* o = get(b);
    o->data = mem::heap_calloc(w * h, 4);
    o->length = w * h * 4;
    o->fields["width"] = Value::I((s32)w);
    o->fields["height"] = Value::I((s32)h);
    return b;
}

void draw_string(jref bitmap, Paint* p, const std::string& s, int x, int y) {
    u32 bw, bh, px;
    if (!p || !p->font || !bitmap_info(bitmap, bw, bh, px)) return;
    auto& info = p->font->info;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, p->size);
    float pen = (float)x;
    auto cps = utf8_decode(s);
    u8* pixels = mem::host(px);
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, (int)cps[i], &adv, &lsb);
        int x0, y0, x1, y1;
        float sx = pen - std::floor(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&info, (int)cps[i], scale, scale, sx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<u8> g((size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&info, g.data(), gw, gh, gw, scale, scale, sx, 0, (int)cps[i]);
            int ox = (int)std::floor(pen) + x0, oy = y + y0;
            for (int yy = 0; yy < gh; yy++) {
                int ty = oy + yy;
                if (ty < 0 || ty >= (int)bh) continue;
                for (int xx = 0; xx < gw; xx++) {
                    int tx = ox + xx;
                    if (tx < 0 || tx >= (int)bw) continue;
                    u32 a = g[(size_t)yy * gw + xx];
                    if (!a) continue;
                    u8* d = pixels + ((size_t)ty * bw + tx) * 4;
                    // premultiplied white over destination
                    for (int k = 0; k < 4; k++) d[k] = (u8)(a + d[k] * (255 - a) / 255);
                }
            }
        }
        pen += adv * scale;
        if (i + 1 < cps.size()) pen += scale * stbtt_GetCodepointKernAdvance(&info, (int)cps[i], (int)cps[i + 1]);
    }
}

}  // namespace

bool bitmap_info(jref bitmap, u32& width, u32& height, u32& pixels) {
    Object* o = get(bitmap);
    if (!o || !o->data) return false;
    width = (u32)o->fields["width"].i();
    height = (u32)o->fields["height"].i();
    pixels = o->data;
    return true;
}

void register_classes() {
    using V = Value;
    const char* BG = "com/ea/ironmonkey/BitmapGraphics";
    def(BG, "createPaintFromFamilyName", "(Ljava/lang/String;F)Landroid/graphics/Paint;", [](Call& c) {
        LOGD("createPaintFromFamilyName(%s, %.1f)", c.s(0).c_str(), c.f(1));
        return V::L(make_paint(nullptr, c.f(1)));
    });
    def(BG, "createPaintFromFile", "(Ljava/lang/String;F)Landroid/graphics/Paint;", [](Call& c) {
        LOGD("createPaintFromFile(%s, %.1f)", c.s(0).c_str(), c.f(1));
        return V::L(make_paint(load_font(c.s(0)), c.f(1)));
    });
    def(BG, "<init>", "(II)V", [](Call& c) {
        set_field(c.thiz, "bitmap", V::L(make_bitmap((u32)c.i(0), (u32)c.i(1))));
        return V{};
    });
    def(BG, "getBitmap", "()Landroid/graphics/Bitmap;", [](Call& c) {
        Object* o = get(c.thiz);
        return o ? o->fields["bitmap"] : V::L(0);
    });
    def(BG, "clear", "()V", [](Call& c) {
        u32 w, h, px;
        Object* o = get(c.thiz);
        if (o && bitmap_info(o->fields["bitmap"].l(), w, h, px)) std::memset(mem::host(px), 0, (size_t)w * h * 4);
        return V{};
    });
    def(BG, "drawString", "(Landroid/graphics/Paint;Ljava/lang/String;II)V", [](Call& c) {
        Object* o = get(c.thiz);
        if (o) draw_string(o->fields["bitmap"].l(), paint(c.l(0)), c.s(1), c.i(2), c.i(3));
        return V{};
    });
    const char* P = "android/graphics/Paint";
    def(P, "getTextSize", "()F", [](Call& c) { Paint* p = paint(c.thiz); return V::F(p ? p->size : 16.f); });
    def(P, "setTextSize", "(F)V", [](Call& c) {
        if (Paint* p = paint(c.thiz)) p->size = c.f(0);
        return V{};
    });
    def(P, "measureText", "(Ljava/lang/String;)F", [](Call& c) { return V::F(measure(paint(c.thiz), c.s(0))); });
    def(P, "getFontMetricsInt", "()Landroid/graphics/Paint$FontMetricsInt;", [](Call& c) {
        Paint* p = paint(c.thiz);
        jref m = new_object("android/graphics/Paint$FontMetricsInt");
        int asc = 0, desc = 0, gap = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float scale = 1;
        if (p && p->font) {
            stbtt_GetFontVMetrics(&p->font->info, &asc, &desc, &gap);
            stbtt_GetFontBoundingBox(&p->font->info, &x0, &y0, &x1, &y1);
            scale = stbtt_ScaleForMappingEmToPixels(&p->font->info, p->size);
        }
        set_field(m, "ascent", V::I(-(int)std::lround(asc * scale)));
        set_field(m, "descent", V::I((int)std::lround(-desc * scale)));
        set_field(m, "leading", V::I((int)std::lround(gap * scale)));
        set_field(m, "top", V::I(-(int)std::lround(y1 * scale)));
        set_field(m, "bottom", V::I((int)std::lround(-y0 * scale)));
        return V::L(m);
    });
    const char* B = "android/graphics/Bitmap";
    def(B, "getWidth", "()I", [](Call& c) { Object* o = get(c.thiz); return o ? o->fields["width"] : V::I(0); });
    def(B, "getHeight", "()I", [](Call& c) { Object* o = get(c.thiz); return o ? o->fields["height"] : V::I(0); });
}

}  // namespace jni::text
