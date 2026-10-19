// High-level emulation of the Android system libraries the game imports.
#pragma once

#include <string>

#include "core/abi.h"
#include "core/thunks.h"

namespace hle {

void register_all();

void register_libc_string();
void register_libc_stdio();
void register_libc_io();
void register_libc_misc();
void register_libc_math();
void register_pthread();
void register_android();
void register_gles();
void register_jni();
void register_net_block();  // online services: refused

// Printf/scanf over guest arguments (bionic semantics).
std::string format(const char* fmt, abi::VarArgs& va);
int scan(const char* input, const char* fmt, abi::VarArgs& va);

// A guest copy of a string that lives forever (deduplicated).
u32 intern_string(const std::string& s);

// Map an Android path to a host path (see libc_io.cpp).
std::string map_path(const char* guest_path);

// Run guest atexit handlers, then exit the process.
[[noreturn]] void guest_exit(int code);

// Implemented in libc_pthread.cpp / android.cpp.
void add_thread_dtor(u32 fn, u32 obj);
s32 futex_op(u32 uaddr, int op, u32 val, u32 timeout);
bool opensl_available();

// Number of network calls the game attempted (all refused; net_block.cpp).
u32 blocked_network_attempts();

// GL statistics/debugging (gles.cpp).
u64 gl_call_count();
u64 gl_filtered_count();  // redundant GL calls dropped by the state filter
// Apply state the filter holds back (vertex arrays), e.g. before the overlay.
void gl_flush_state();
void gl_client_stats(u64& draws, u64& attribs, u64& index_draws);  // client-side array use (debugging)
u64 gl_time_ns();  // cumulative time inside host GL calls
void gl_set_trace(bool on);
void gl_fbtrace(int frames);  // log framebuffer use for the next N frames
int gl_fbtrace_left();
// 3D scene resolution (gles.cpp): the game's screen-size render targets are
// allocated at scale * screen size (0.5-1).
void gl_set_screen_size(int w, int h);
void gl_set_scene_scale(float s);
// The real screen's size when it differs from the game's (docked / undocked
// while playing): everything the game draws is scaled to it.
void gl_set_output_size(int w, int h);
// Desktop recording aid (NFSMW_RECORD_FPS, see libc_misc.cpp): once per frame.
void record_clock_frame();
float gl_output_scale();  // real screen / game screen (1 = the size the game started with)
// Replace part of a texture with RGBA pixels (bottom row first, as GL reads
// them); the texture bound before stays bound. Copies the pixels; call from
// the thread that issues the game's GL calls.
void gl_update_texture(u32 texture, int x, int y, int w, int h, const u8* rgba);
float gl_scene_scale();
bool gl_scene_scaled_targets();  // the game has drawn into a scaled target

// errno helpers: translate the host errno into the guest's (Linux numbering).
int guest_errno_from_host(int e);
void set_errno_from_host();

}  // namespace hle
