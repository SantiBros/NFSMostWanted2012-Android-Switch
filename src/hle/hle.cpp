#include "hle/hle.h"

#include "hle/jni/jni.h"

namespace hle {

void register_all() {
    register_libc_string();
    register_libc_stdio();
    register_libc_io();
    register_libc_misc();
    register_net_block();
    register_libc_math();
    register_pthread();
    register_android();
    register_gles();
    jni::init();
}

}  // namespace hle
