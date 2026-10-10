// Text rendering the game delegates to Android (Paint/Canvas/Bitmap), done
// here with stb_truetype using the fonts shipped in the APK.
#pragma once

#include "hle/jni/jni.h"

namespace jni::text {

void register_classes();

// For AndroidBitmap_* (libjnigraphics): bitmap object -> pixels/size.
bool bitmap_info(jref bitmap, u32& width, u32& height, u32& pixels);

}  // namespace jni::text
