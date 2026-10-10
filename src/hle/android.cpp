// libjnigraphics and other small Android-only APIs.
#include "hle/hle.h"
#include "hle/jni/text.h"

namespace hle {

namespace {
int h_bitmap_get_info(u32, u32 bitmap, u32 info) {
    u32 w, h, px;
    if (!jni::text::bitmap_info(bitmap, w, h, px)) return -1;
    mem::w32(info + 0, w);
    mem::w32(info + 4, h);
    mem::w32(info + 8, w * 4);
    mem::w32(info + 12, 1);  // ANDROID_BITMAP_FORMAT_RGBA_8888
    mem::w32(info + 16, 0);
    return 0;
}
int h_bitmap_lock(u32, u32 bitmap, u32 out) {
    u32 w, h, px;
    if (!jni::text::bitmap_info(bitmap, w, h, px)) return -1;
    if (out) mem::w32(out, px);
    return 0;
}
int h_bitmap_unlock(u32, u32) { return 0; }
}  // namespace

// OpenSL ES is not emulated yet: FMOD is told the library is missing.
bool opensl_available() { return false; }

void register_android() {
    REG("AndroidBitmap_getInfo", h_bitmap_get_info);
    REG("AndroidBitmap_lockPixels", h_bitmap_lock);
    REG("AndroidBitmap_unlockPixels", h_bitmap_unlock);
}

}  // namespace hle
