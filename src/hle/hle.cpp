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
    install_guest_fast();  // after the C library: replaces two of its functions
    jni::init();
}

}  // namespace hle
