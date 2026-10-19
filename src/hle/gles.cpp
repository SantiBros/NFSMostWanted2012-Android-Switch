// OpenGL ES 2.0 bridge. Almost every entry point only needs guest pointers
// turned into host pointers (generated in gles_gen.inc); the handful below need
// real work: client-side vertex arrays, shader source arrays, buffer mapping
// and strings returned to the guest.
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "game/controller_art.h"
#include "hle/gl_queue.h"
#include "hle/hle.h"
#include "platform/platform.h"

namespace hle {

namespace {

std::atomic<bool> g_trace{false};
std::atomic<u64> g_calls{0};
std::atomic<u64> g_gl_ns{0};  // time spent inside the host GL driver
// Per-call statistics. GL calls come from the game's main thread only, so
// plain loads/stores are enough (no atomic read-modify-write per call). With
// the GL thread the calls only queue work, so they are not timed.
struct GlScope {
    bool timed;
    std::chrono::steady_clock::time_point t0;
    explicit GlScope(const char* name) : timed(!glq::active()) {
        g_calls.store(g_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (timed) t0 = std::chrono::steady_clock::now();
        if (g_trace.load(std::memory_order_relaxed)) LOGV("gl: %s", name);
    }
    ~GlScope() {
        if (!timed) return;
        u64 ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        g_gl_ns.store(g_gl_ns.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
    }
};
#define GL_TRACE(name) GlScope gl_scope_(name)

void* gl_proc(const char* name) {
    void* p = platform::gl_get_proc(name);
    if (!p) fatal("OpenGL ES function %s not available on host", name);
    return p;
}

// Bytes of client image data glTexImage2D/glTexSubImage2D read for a w x h
// image (honouring GL_UNPACK_ALIGNMENT). Unknown formats return a size larger
// than the queue takes, so those uploads run synchronously instead.
s32 g_unpack_alignment = 4;
size_t image_bytes(s32 w, s32 h, u32 format, u32 type) {
    if (w <= 0 || h <= 0) return 0;
    int comps = 0;
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: comps = 1; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    case GL_RGB: comps = 3; break;
    case GL_RGBA: case GL_BGRA_EXT: comps = 4; break;
    case GL_DEPTH_STENCIL_OES: comps = 1; break;
    default: return SIZE_MAX;
    }
    size_t bpp = 0;
    switch (type) {
    case GL_UNSIGNED_BYTE: bpp = comps; break;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
    case GL_UNSIGNED_SHORT: bpp = 2 * comps; break;
    case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_24_8_OES: bpp = 4 * comps; break;
    case GL_FLOAT: bpp = 4 * comps; break;
    case GL_HALF_FLOAT_OES: bpp = 2 * comps; break;
    default: return SIZE_MAX;
    }
    size_t a = (size_t)std::max(1, g_unpack_alignment);
    size_t row = ((size_t)w * bpp + a - 1) / a * a;
    return row * (size_t)(h - 1) + (size_t)w * bpp;
}

#include "hle/gles_gen.inc"

#define LOAD(fn) static auto p = (decltype(&::fn))gl_proc(#fn)

GLuint g_array_buffer = 0, g_element_buffer = 0;
u64 g_client_draws = 0, g_client_attribs = 0, g_client_index_draws = 0;
u32 g_attrib_client_mask = 0;  // vertex attributes whose pointer is client memory

// ---- redundant state filter --------------------------------------------------
// The game sets a lot of GL state that is already set: it re-binds the same
// textures and buffers, re-sends unchanged uniform values, and switches its
// vertex arrays off after every draw and on again before the next one. Those
// calls are dropped here, before they reach the queue or the driver. The
// filter tracks the state the game asked for; the GL state always matches it
// at draw time. NFSMW_NO_GL_FILTER=1 turns it off (for comparison).
const bool g_filter = std::getenv("NFSMW_NO_GL_FILTER") == nullptr;
u64 g_filtered = 0;  // calls dropped
#define FILTERED()     \
    do {               \
        g_filtered++;  \
        return;        \
    } while (0)

u32 g_unit_tracked = 0;      // active texture unit (index)
u32 g_bound_cube[32] = {};   // GL_TEXTURE_CUBE_MAP binding per unit
s64 g_program = -1;          // current program (-1: unknown)
// Vertex arrays: enabled state the game asked for vs. what GL has; applied at draws.
u32 g_attrib_want = 0, g_attrib_have = 0;
struct AttribPtr {
    bool set = false;
    GLuint buffer;
    GLint size;
    GLenum type;
    GLboolean norm;
    GLsizei stride;
    const void* ptr;
};
AttribPtr g_attrib_ptr[32];
// glEnable/glDisable for the usual capabilities (-1: unknown)
int g_caps[16];
int cap_index(u32 cap) {
    switch (cap) {
    case GL_BLEND: return 0;
    case GL_CULL_FACE: return 1;
    case GL_DEPTH_TEST: return 2;
    case GL_SCISSOR_TEST: return 3;
    case GL_STENCIL_TEST: return 4;
    case GL_POLYGON_OFFSET_FILL: return 5;
    case GL_DITHER: return 6;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: return 7;
    case GL_SAMPLE_COVERAGE: return 8;
    default: return -1;
    }
}
struct SmallState {
    bool known = false;
    u32 v[4] = {};
    bool same(u32 a, u32 b = 0, u32 c = 0, u32 d = 0) const {
        return known && v[0] == a && v[1] == b && v[2] == c && v[3] == d;
    }
    void set(u32 a, u32 b = 0, u32 c = 0, u32 d = 0) { known = true, v[0] = a, v[1] = b, v[2] = c, v[3] = d; }
};
SmallState g_blend_func, g_depth_mask, g_depth_func, g_color_mask, g_cull_face, g_front_face;
// Uniform values of each program (location -> value), for single-element uploads.
struct UniformVal {
    u8 n = 0;  // words
    u32 v[16];
};
std::unordered_map<u32, std::unordered_map<s32, UniformVal>> g_uniforms;
std::unordered_map<s32, UniformVal>* g_cur_uniforms = nullptr;
struct FilterInit {
    FilterInit() {
        for (int& c : g_caps) c = -1;
    }
} g_filter_init;

// True if the value is already set (and remembers it otherwise).
bool uniform_same(s32 loc, const u32* v, int n) {
    if (!g_filter || !g_cur_uniforms || loc < 0) return false;
    UniformVal& u = (*g_cur_uniforms)[loc];
    if (u.n == n && std::memcmp(u.v, v, (size_t)n * 4) == 0) return true;
    u.n = (u8)n;
    std::memcpy(u.v, v, (size_t)n * 4);
    return false;
}
void flush_attribs() {
    u32 diff = g_attrib_want ^ g_attrib_have;
    if (!diff) return;
    static auto en = (void (*)(GLuint))gl_proc("glEnableVertexAttribArray");
    static auto dis = (void (*)(GLuint))gl_proc("glDisableVertexAttribArray");
    for (u32 i = 0; i < 32; i++)
        if (diff & (1u << i)) {
            if (g_attrib_want & (1u << i)) glq::call(en, i);
            else glq::call(dis, i);
        }
    g_attrib_have = g_attrib_want;
}

// glDelete* with a list of names: the list is copied into the queue.
template <void (**P)(GLsizei, const GLuint*)>
void delete_names(s32 n, u32 ids) {
    struct Args {
        GLsizei n;
    };
    glq::call_data<Args>([](const Args& a, const void* d) { (*P)(a.n, static_cast<const GLuint*>(d)); }, Args{n},
                         n > 0 ? mem::ptr<void>(ids) : nullptr, n > 0 ? (size_t)n * 4 : 0);
}
void (*p_delete_buffers)(GLsizei, const GLuint*) = nullptr;
void (*p_delete_textures)(GLsizei, const GLuint*) = nullptr;
void (*p_delete_framebuffers)(GLsizei, const GLuint*) = nullptr;
void (*p_delete_renderbuffers)(GLsizei, const GLuint*) = nullptr;

void m_glBindBuffer(u32 target, u32 buf) {
    LOAD(glBindBuffer);
    GL_TRACE("glBindBuffer");
    if (target == GL_ARRAY_BUFFER) {
        if (g_filter && g_array_buffer == buf) FILTERED();
        g_array_buffer = buf;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        if (g_filter && g_element_buffer == buf) FILTERED();
        g_element_buffer = buf;
    }
    glq::call(p, target, buf);
}
void m_glDeleteBuffers(s32 n, u32 ids) {
    if (!p_delete_buffers) p_delete_buffers = (decltype(p_delete_buffers))gl_proc("glDeleteBuffers");
    GL_TRACE("glDeleteBuffers");
    for (s32 i = 0; i < n; i++) {
        u32 id = mem::r32(ids + i * 4);
        if (id == g_array_buffer) g_array_buffer = 0;
        if (id == g_element_buffer) g_element_buffer = 0;
        for (AttribPtr& a : g_attrib_ptr)
            if (a.buffer == id) a.set = false;
    }
    delete_names<&p_delete_buffers>(n, ids);
}
void m_glVertexAttribPointer(u32 index, s32 size, u32 type, u32 norm, s32 stride, u32 ptr) {
    LOAD(glVertexAttribPointer);
    GL_TRACE("glVertexAttribPointer");
    // NFSMW_ATTRIB_LOG=1 (debugging): each kind of vertex attribute the game uses.
    static const bool attrib_log = std::getenv("NFSMW_ATTRIB_LOG") != nullptr;
    if (attrib_log) {
        static std::vector<u64> seen;
        const u64 key = (u64)size | (u64)type << 8 | (u64)(norm != 0) << 24 | (u64)(stride & 63) << 32 |
                        (u64)(g_array_buffer ? (ptr & 3) : 9) << 40;
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            LOGI("attrib: size %d type 0x%X normalized %u stride %d offset %u (mod 4: %u) %s", size, type, norm, stride,
                 ptr, ptr & 3, g_array_buffer ? "buffer" : "client memory");
        }
    }
    // With a VBO bound the "pointer" is an offset; otherwise it is client memory.
    const void* hp = g_array_buffer ? reinterpret_cast<const void*>((uintptr_t)ptr) : mem::ptr<void>(ptr);
    if (index < 32) {
        if (g_array_buffer) g_attrib_client_mask &= ~(1u << index);
        else g_attrib_client_mask |= 1u << index, g_client_attribs++;
        AttribPtr& a = g_attrib_ptr[index];
        if (g_filter && g_array_buffer && a.set && a.buffer == g_array_buffer && a.size == size && a.type == type &&
            a.norm == (GLboolean)norm && a.stride == stride && a.ptr == hp)
            FILTERED();
        a = {g_array_buffer != 0, g_array_buffer, size, type, (GLboolean)norm, stride, hp};
    }
    glq::call(p, index, size, type, (GLboolean)norm, stride, hp);
}
void m_glDrawElements(u32 mode, s32 count, u32 type, u32 indices) {
    LOAD(glDrawElements);
    GL_TRACE("glDrawElements");
    if (g_attrib_client_mask) g_client_draws++;
    if (!g_element_buffer) g_client_index_draws++;
    const void* hp = g_element_buffer ? reinterpret_cast<const void*>((uintptr_t)indices) : mem::ptr<void>(indices);
    flush_attribs();
    // Client-memory arrays are read during the draw: that memory may change
    // as soon as we return, so such draws wait for the GL thread. (The game
    // keeps all of its vertex and index data in buffer objects.)
    if (g_attrib_client_mask || !g_element_buffer) glq::call_sync_void(p, mode, count, type, hp);
    else glq::call(p, mode, count, type, hp);
}
void m_glDrawArrays(u32 mode, s32 first, s32 count) {
    LOAD(glDrawArrays);
    GL_TRACE("glDrawArrays");
    flush_attribs();
    if (g_attrib_client_mask) glq::call_sync_void(p, mode, first, count);
    else glq::call(p, mode, first, count);
}
void m_glPixelStorei(u32 pname, s32 v) {
    LOAD(glPixelStorei);
    GL_TRACE("glPixelStorei");
    if (pname == GL_UNPACK_ALIGNMENT) g_unpack_alignment = v;
    glq::call(p, pname, v);
}
// NFSMW_DUMP_SHADERS=<folder> (debugging): write every shader the game
// compiles to that folder.
void dump_shader(u32 shader, s32 count, u32 strings, u32 lengths) {
    static const char* dir = std::getenv("NFSMW_DUMP_SHADERS");
    if (!dir) return;
    static std::atomic<int> n{0};
    char path[512];
    std::snprintf(path, sizeof path, "%s/shader_%04d_%u.glsl", dir, n.fetch_add(1), shader);
    if (FILE* f = std::fopen(path, "wb")) {
        for (s32 i = 0; i < count; i++) {
            const char* src = mem::str(mem::r32(strings + i * 4));
            const s32 len = lengths ? (s32)mem::r32(lengths + i * 4) : -1;
            std::fwrite(src, 1, len >= 0 ? (size_t)len : std::strlen(src), f);
        }
        std::fclose(f);
    }
}
// The game's shader source (all strings joined).
std::string shader_text(s32 count, u32 strings, u32 lengths) {
    std::string out;
    for (s32 i = 0; i < count; i++) {
        const char* src = mem::str(mem::r32(strings + i * 4));
        const s32 len = lengths ? (s32)mem::r32(lengths + i * 4) : -1;
        out.append(src, len >= 0 ? (size_t)len : std::strlen(src));
    }
    return out;
}
// Changes to the game's shaders (see patch_shader below); empty when none apply.
std::string patch_shader(const std::string& src);
void m_glShaderSource(u32 shader, s32 count, u32 strings, u32 lengths) {
    LOAD(glShaderSource);
    GL_TRACE("glShaderSource");
    dump_shader(shader, count, strings, lengths);
    std::string patched = patch_shader(shader_text(count, strings, lengths));
    glq::sync([&] {
        if (!patched.empty()) {
            const GLchar* one = patched.c_str();
            p(shader, 1, &one, nullptr);
            return;
        }
        std::vector<const GLchar*> s(count);
        for (s32 i = 0; i < count; i++) s[i] = mem::str(mem::r32(strings + i * 4));
        p(shader, count, s.data(), lengths ? mem::ptr<GLint>(lengths) : nullptr);
    });
}
std::string patch_shader(const std::string& src) {
    std::string out;
    // NFSMW_SHADER_NAN_DEBUG=1 (debugging): building windows' reflection
    // shader shows magenta where its reflection coordinate is not a number.
    static const bool nan_debug = std::getenv("NFSMW_SHADER_NAN_DEBUG") != nullptr;
    if (nan_debug && src.find("g_ReflectionMask") != std::string::npos && src.find("gl_FragColor") != std::string::npos) {
        const size_t end = src.rfind('}');
        if (end != std::string::npos) {
            out = src.substr(0, end) +
                  "  if (v_3.x != v_3.x || v_3.y != v_3.y) gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n" +
                  "  else if (v_3.x < 0.0 || v_3.x > 1.0 || v_3.y < 0.0 || v_3.y > 1.0) gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n}\n";
        }
    }
    return out;
}
// A shader or program the driver rejects is drawn wrongly or not at all, and
// drivers differ in what they accept (the game was written for phone GPUs).
// These checks run on the GL thread right after the compile / link, without
// making the game wait, and put the driver's message in log.txt.
void check_shader(GLuint shader) {
    static auto get = (void (*)(GLuint, GLenum, GLint*))gl_proc("glGetShaderiv");
    static auto info = (void (*)(GLuint, GLsizei, GLsizei*, GLchar*))gl_proc("glGetShaderInfoLog");
    static std::atomic<int> failures{0};
    GLint ok = 1;
    get(shader, GL_COMPILE_STATUS, &ok);
    if (ok || failures.fetch_add(1) >= 20) return;
    char log[1024] = "";
    info(shader, sizeof log, nullptr, log);
    LOGW("GL: shader %u did not compile: %s", shader, log);
}
void check_program(GLuint prog) {
    static auto get = (void (*)(GLuint, GLenum, GLint*))gl_proc("glGetProgramiv");
    static auto info = (void (*)(GLuint, GLsizei, GLsizei*, GLchar*))gl_proc("glGetProgramInfoLog");
    static std::atomic<int> failures{0};
    GLint ok = 1;
    get(prog, GL_LINK_STATUS, &ok);
    if (ok || failures.fetch_add(1) >= 20) return;
    char log[1024] = "";
    info(prog, sizeof log, nullptr, log);
    LOGW("GL: program %u did not link: %s", prog, log);
}
void f_glCompileShader(u32 shader) {
    g_glCompileShader(shader);
    glq::call(&check_shader, (GLuint)shader);
}
void m_glGetVertexAttribPointerv(u32 index, u32 pname, u32 out) {
    LOAD(glGetVertexAttribPointerv);
    void* hp = nullptr;
    glq::sync([&] { p(index, pname, &hp); });
    mem::w32(out, mem::is_guest(hp) ? mem::guest(hp) : (u32)(uintptr_t)hp);
}
// ---- 3D scene resolution, and the output resolution ------------------------
// In races the game draws the 3D scene into its own off-screen framebuffer,
// the size of the screen, then copies it to the screen and draws the HUD on
// top. The scene can be drawn at a lower resolution by making that
// framebuffer smaller: render targets the size of the screen are allocated
// at the scaled size, and viewport/scissor rectangles are scaled while one of
// them is bound. The copy to the screen samples the smaller texture with
// filtering, so the HUD and menus stay at full resolution. The game never
// notices: glGetIntegerv still reports the rectangles it set.
//
// The same scaling lets the screen itself change size while the game runs
// (docking / undocking, 0.3.0). The game keeps the size it started with (its
// "screen"): it reads it once and cannot change it. The real screen (the
// window, framebuffer 0) is re-created at the new size, and everything drawn
// to it is scaled by the output factor (real / game size); screen-size render
// targets by output factor x 3D scene scale. A game started handheld and then
// docked draws at 1920x1080 for real, not a 1280x720 picture blown up, and a
// game started docked and then undocked draws at 1280x720 again.
namespace scene {
int g_screen_w = 0, g_screen_h = 0;  // the game's screen (its size at start-up)
float g_scale = 1.f;                 // 3D scene resolution (debug menu)
float g_out = 1.f;                   // real screen / game screen
float g_rect_f = 1.f;                // factor for the bound framebuffer's rectangles
std::atomic<int> g_trace{0};  // debug command "fbtrace": log framebuffer use

struct TexInfo {
    s32 w = 0, h = 0;
    s32 ifmt = 0;
    u32 fmt = 0, type = 0;
    s32 mag_filter = -1;  // the game's magnification filter (-1: not known yet)
    bool target = false;  // screen-size render target (scaled)
};
std::unordered_map<u32, TexInfo> g_tex;  // texture id -> level-0 image of a 2D texture
struct RbInfo {
    s32 w = 0, h = 0;
    u32 fmt = 0;
    bool target = false;
};
std::unordered_map<u32, RbInfo> g_rb;
// framebuffer -> attachment points (color, depth, stencil) holding a scaled image
std::unordered_map<u32, unsigned> g_fbo_scaled;
u32 g_unit = 0;                             // active texture unit
u32 g_bound_tex[32] = {};                   // GL_TEXTURE_2D binding per unit
u32 g_bound_rb = 0, g_bound_fbo = 0;
bool g_scaled_now = false;  // the bound framebuffer is drawn at the scaled size
GLint g_vp[4] = {0, 0, 0, 0}, g_sc[4] = {0, 0, 0, 0};
bool g_have_vp = false, g_have_sc = false;
unsigned attach_bit(u32 att) {
    return att == GL_COLOR_ATTACHMENT0 ? 1u : att == GL_DEPTH_ATTACHMENT ? 2u : att == GL_STENCIL_ATTACHMENT ? 4u : 0u;
}
// framebuffer -> image attached at each attachment point (texture or renderbuffer id)
struct Attached {
    u32 id[3] = {0, 0, 0};
    bool rb[3] = {false, false, false};
};
std::unordered_map<u32, Attached> g_attached;
void note_attachment(u32 att, bool scaled, u32 id = 0, bool is_rb = false) {
    unsigned& m = g_fbo_scaled[g_bound_fbo];
    unsigned bit = attach_bit(att);
    if (scaled) m |= bit;
    else m &= ~bit;
    int k = bit == 1 ? 0 : bit == 2 ? 1 : bit == 4 ? 2 : -1;
    if (k >= 0) g_attached[g_bound_fbo].id[k] = id, g_attached[g_bound_fbo].rb[k] = is_rb;
}
void update_bound();
// A render target was re-specified at another size: framebuffers that hold it
// are no longer drawn at the scaled size.
void untarget(u32 id, bool is_rb) {
    for (auto& [fbo, a] : g_attached)
        for (int k = 0; k < 3; k++)
            if (a.id[k] == id && a.rb[k] == is_rb) g_fbo_scaled[fbo] &= ~(1u << k);
    update_bound();
}

#define FBLOG(...)                                                       \
    do {                                                                 \
        if (g_trace.load(std::memory_order_relaxed) > 0) LOGI(__VA_ARGS__); \
    } while (0)

bool screen_sized(s32 w, s32 h) { return g_screen_w > 0 && w == g_screen_w && h == g_screen_h; }
float target_factor() { return g_scale * g_out; }
s32 scl(s32 v, float f) { return (s32)std::lround(v * f); }
s32 sc(s32 v) { return scl(v, g_rect_f); }  // a coordinate in the bound framebuffer
s32 scaled_w() { return std::max(1, scl(g_screen_w, target_factor())); }
s32 scaled_h() { return std::max(1, scl(g_screen_h, target_factor())); }

using RectFn = void (*)(GLint, GLint, GLsizei, GLsizei);
void apply_rect(RectFn fn, const GLint* r) {
    if (g_rect_f == 1.f) return glq::call(fn, r[0], r[1], r[2], r[3]);
    s32 x0 = sc(r[0]), y0 = sc(r[1]);
    glq::call(fn, x0, y0, sc(r[0] + r[2]) - x0, sc(r[1] + r[3]) - y0);
}
void reapply_rects() {
    static auto vp = (RectFn)gl_proc("glViewport");
    static auto scis = (RectFn)gl_proc("glScissor");
    if (g_have_vp) apply_rect(vp, g_vp);
    if (g_have_sc) apply_rect(scis, g_sc);
}
void update_bound() {
    auto it = g_fbo_scaled.find(g_bound_fbo);
    g_scaled_now = g_bound_fbo != 0 && it != g_fbo_scaled.end() && it->second != 0;
    const float f = g_bound_fbo == 0 ? g_out : g_scaled_now ? target_factor() : 1.f;
    if (f != g_rect_f) {
        g_rect_f = f;
        reapply_rects();
    }
}

// (Re)allocate a render target's storage at the current scale.
void alloc_tex(u32 id, const TexInfo& t) {
    static auto bind = (void (*)(GLenum, GLuint))gl_proc("glBindTexture");
    static auto image = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))gl_proc(
        "glTexImage2D");
    static auto param = (void (*)(GLenum, GLenum, GLint))gl_proc("glTexParameteri");
    glq::call(bind, GL_TEXTURE_2D, id);
    glq::call(image, GL_TEXTURE_2D, 0, t.ifmt, scaled_w(), scaled_h(), 0, t.fmt, t.type, (const void*)nullptr);
    // A smaller scene is stretched back to the screen: magnify with filtering.
    // (Only the magnification filter is touched; it cannot make the texture
    // incomplete the way a mipmap minification filter would.)
    if (g_scale < 1.f) glq::call(param, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    else if (t.mag_filter >= 0) glq::call(param, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t.mag_filter);
    glq::call(bind, GL_TEXTURE_2D, g_bound_tex[g_unit]);
}
void alloc_rb(u32 id, const RbInfo& r) {
    static auto bind = (void (*)(GLenum, GLuint))gl_proc("glBindRenderbuffer");
    static auto storage = (void (*)(GLenum, GLenum, GLsizei, GLsizei))gl_proc("glRenderbufferStorage");
    glq::call(bind, GL_RENDERBUFFER, id);
    glq::call(storage, GL_RENDERBUFFER, r.fmt, scaled_w(), scaled_h());
    glq::call(bind, GL_RENDERBUFFER, g_bound_rb);
}

// After a change of scale: the render targets at their new size, and the
// rectangles of the bound framebuffer again.
int resize_targets() {
    int n = 0;
    for (auto& [id, t] : g_tex)
        if (t.target) alloc_tex(id, t), n++;
    for (auto& [id, r] : g_rb)
        if (r.target) alloc_rb(id, r), n++;
    g_rect_f = 0.f;  // makes update_bound re-apply them
    update_bound();
    return n;
}

void set_scale(float s) {
    s = std::clamp(s, 0.25f, 1.f);
    if (s == g_scale) return;
    g_scale = s;
    const int n = resize_targets();
    LOGI("3D scene resolution: %d%% (%dx%d), %d render targets resized", (int)std::lround(s * 100), scaled_w(),
         scaled_h(), n);
}

void set_output(float o) {
    if (o <= 0.f || o == g_out) return;
    g_out = o;
    const int n = resize_targets();
    LOGI("output: the game's %dx%d screen is drawn at %dx%d (x%.3f), 3D scene %dx%d, %d render targets resized",
         g_screen_w, g_screen_h, scl(g_screen_w, o), scl(g_screen_h, o), o, scaled_w(), scaled_h(), n);
}
}  // namespace scene

void m_glBindFramebuffer(u32 target, u32 fb) {
    LOAD(glBindFramebuffer);
    GL_TRACE("glBindFramebuffer");
    glq::call(p, target, fb);
    scene::g_bound_fbo = fb;
    scene::update_bound();
    if (scene::g_trace.load(std::memory_order_relaxed) > 0) {
        static auto status = (GLenum(*)(GLenum))gl_proc("glCheckFramebufferStatus");
        LOGI("fb: bind %u%s status 0x%X", fb, scene::g_scaled_now ? " (scaled)" : "",
             (unsigned)glq::call_sync(status, GL_FRAMEBUFFER));
    }
}
void m_glDeleteFramebuffers(s32 n, u32 ids) {
    if (!p_delete_framebuffers) p_delete_framebuffers = (decltype(p_delete_framebuffers))gl_proc("glDeleteFramebuffers");
    GL_TRACE("glDeleteFramebuffers");
    for (s32 i = 0; i < n; i++) {
        u32 id = mem::r32(ids + i * 4);
        scene::g_fbo_scaled.erase(id);
        scene::g_attached.erase(id);
        if (id == scene::g_bound_fbo) scene::g_bound_fbo = 0;
    }
    delete_names<&p_delete_framebuffers>(n, ids);
    scene::update_bound();
}
void m_glActiveTexture(u32 unit) {
    LOAD(glActiveTexture);
    GL_TRACE("glActiveTexture");
    u32 u = std::min<u32>(unit - GL_TEXTURE0, 31);
    if (g_filter && u == scene::g_unit && g_unit_tracked == u + 1) FILTERED();
    scene::g_unit = u;
    g_unit_tracked = u + 1;
    glq::call(p, unit);
}
void m_glBindTexture(u32 target, u32 tex) {
    LOAD(glBindTexture);
    GL_TRACE("glBindTexture");
    if (target == GL_TEXTURE_2D) {
        if (g_filter && scene::g_bound_tex[scene::g_unit] == tex) FILTERED();
        scene::g_bound_tex[scene::g_unit] = tex;
    } else if (target == GL_TEXTURE_CUBE_MAP) {
        if (g_filter && g_bound_cube[scene::g_unit] == tex) FILTERED();
        g_bound_cube[scene::g_unit] = tex;
    }
    glq::call(p, target, tex);
}
void m_glDeleteTextures(s32 n, u32 ids) {
    if (!p_delete_textures) p_delete_textures = (decltype(p_delete_textures))gl_proc("glDeleteTextures");
    GL_TRACE("glDeleteTextures");
    for (s32 i = 0; i < n; i++) {
        u32 id = mem::r32(ids + i * 4);
        scene::g_tex.erase(id);
        for (u32& b : scene::g_bound_tex)
            if (b == id) b = 0;
        for (u32& b : g_bound_cube)
            if (b == id) b = 0;
    }
    delete_names<&p_delete_textures>(n, ids);
}
// The first texture of each format, for the log (which path the game took).
void note_texture_format(u32 fmt, u32 type, s32 w, s32 h, bool compressed) {
    static std::mutex mu;
    static std::vector<u64> seen;
    const u64 key = (u64)fmt << 32 | type | (compressed ? 1ull << 31 : 0);
    std::lock_guard lk(mu);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    if (compressed) LOGI("GL: first compressed texture in format 0x%X (%dx%d)", fmt, w, h);
    else LOGI("GL: first texture in format 0x%X, type 0x%X (%dx%d)", fmt, type, w, h);
}
// NFSMW_ETC1_SCAN=1 (debugging): count ETC1 blocks whose "differential" colours
// leave the 0..31 range (invalid ETC1; an ETC2 decoder reads them as other modes).
void etc1_scan(u32 w, u32 h, u32 size, u32 data) {
    static const bool on = std::getenv("NFSMW_ETC1_SCAN") != nullptr;
    if (!on || !data) return;
    static std::atomic<u64> total{0}, bad_total{0}, textures{0}, bad_textures{0};
    const u8* p = mem::host(data);
    u64 bad = 0, n = size / 8;
    for (u64 i = 0; i < n; i++) {
        const u8* b = p + i * 8;
        if (!(b[3] & 2)) continue;  // individual mode
        for (int c = 0; c < 3; c++) {
            const int base = b[c] >> 3, d = (b[c] & 7) >= 4 ? (b[c] & 7) - 8 : (b[c] & 7);
            if (base + d < 0 || base + d > 31) {
                bad++;
                break;
            }
        }
    }
    total += n;
    bad_total += bad;
    textures++;
    if (bad) bad_textures++;
    if (bad) LOGI("ETC1 scan: %ux%u texture: %llu of %llu blocks out of range (so far %llu of %llu textures, %llu of %llu blocks)", w, h,
                  (unsigned long long)bad, (unsigned long long)n, (unsigned long long)bad_textures.load(),
                  (unsigned long long)textures.load(), (unsigned long long)bad_total.load(), (unsigned long long)total.load());
}
void f_glCompressedTexImage2D(u32 target, u32 level, u32 ifmt, u32 w, u32 h, u32 border, u32 size, u32 data) {
    if (level == 0) note_texture_format(ifmt, 0, (s32)w, (s32)h, true);
    if (ifmt == 0x8D64) etc1_scan(w, h, size, data);
    g_glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
}
using TexImageFn = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
TexImageFn p_tex_image = nullptr;
void m_glTexImage2D(u32 target, s32 level, s32 ifmt, s32 w, s32 h, s32 border, u32 fmt, u32 type, u32 px) {
    if (!p_tex_image) p_tex_image = (TexImageFn)gl_proc("glTexImage2D");
    GL_TRACE("glTexImage2D");
    if (level == 0 && px) note_texture_format(fmt, type, w, h, false);
    if (target == GL_TEXTURE_2D && level == 0) {
        u32 id = scene::g_bound_tex[scene::g_unit];
        if (scene::screen_sized(w, h))
            LOGD("glTexImage2D: texture %u %dx%d fmt 0x%X type 0x%X %s", id, w, h, fmt, type, px ? "data" : "empty");
        auto& t = scene::g_tex[id];
        t.w = w, t.h = h, t.ifmt = ifmt, t.fmt = fmt, t.type = type;
        if (t.target && scene::screen_sized(w, h) && !px) {
            // The game re-creates a render target: keep it at the scaled size.
            glq::call(p_tex_image, target, level, ifmt, scene::scaled_w(), scene::scaled_h(), border, fmt, type,
                      (const void*)nullptr);
            return;
        }
        if (t.target) {
            t.target = false;
            scene::untarget(id, false);
        }
    }
    // The controls page's picture of the controller (game/controller_art.h).
    if (target == GL_TEXTURE_2D && level == 0 && px && fmt == GL_RGBA && type == GL_UNSIGNED_BYTE && w >= 1000)
        game::controller_art::patch_texture(mem::ptr<u8>(px), w, h);
    struct Args {
        GLenum target;
        GLint level, ifmt;
        GLsizei w, h;
        GLint border;
        GLenum fmt, type;
    };
    glq::call_data<Args>(
        [](const Args& a, const void* d) {
            p_tex_image(a.target, a.level, a.ifmt, a.w, a.h, a.border, a.fmt, a.type, d);
        },
        Args{target, level, ifmt, w, h, border, fmt, type}, px ? mem::ptr<void>(px) : nullptr,
        px ? image_bytes(w, h, fmt, type) : 0);
}
void m_glTexParameteri(u32 target, u32 pname, s32 v) {
    LOAD(glTexParameteri);
    GL_TRACE("glTexParameteri");
    if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MAG_FILTER) {
        auto it = scene::g_tex.find(scene::g_bound_tex[scene::g_unit]);
        if (it != scene::g_tex.end() && it->second.target) {
            it->second.mag_filter = v;
            if (scene::g_scale < 1.f) v = GL_LINEAR;
        }
    }
    glq::call(p, target, pname, v);
}
void m_glBindRenderbuffer(u32 target, u32 rb) {
    LOAD(glBindRenderbuffer);
    GL_TRACE("glBindRenderbuffer");
    scene::g_bound_rb = rb;
    glq::call(p, target, rb);
}
void m_glDeleteRenderbuffers(s32 n, u32 ids) {
    if (!p_delete_renderbuffers)
        p_delete_renderbuffers = (decltype(p_delete_renderbuffers))gl_proc("glDeleteRenderbuffers");
    GL_TRACE("glDeleteRenderbuffers");
    for (s32 i = 0; i < n; i++) {
        u32 id = mem::r32(ids + i * 4);
        scene::g_rb.erase(id);
        if (id == scene::g_bound_rb) scene::g_bound_rb = 0;
    }
    delete_names<&p_delete_renderbuffers>(n, ids);
}
void m_glRenderbufferStorage(u32 target, u32 fmt, s32 w, s32 h) {
    LOAD(glRenderbufferStorage);
    GL_TRACE("glRenderbufferStorage");
    auto& r = scene::g_rb[scene::g_bound_rb];
    r.w = w, r.h = h, r.fmt = fmt;
    LOGD("renderbuffer %u: %dx%d format 0x%X", scene::g_bound_rb, w, h, fmt);
    if (r.target && scene::screen_sized(w, h)) {
        glq::call(p, target, fmt, scene::scaled_w(), scene::scaled_h());
        return;
    }
    if (r.target) {
        r.target = false;
        scene::untarget(scene::g_bound_rb, true);
    }
    glq::call(p, target, fmt, w, h);
}
// Attaching a screen-size image to a framebuffer makes it a scaled render target.
void m_glFramebufferTexture2D(u32 target, u32 att, u32 textarget, u32 tex, s32 level) {
    LOAD(glFramebufferTexture2D);
    GL_TRACE("glFramebufferTexture2D");
    auto it = scene::g_tex.find(tex);
    bool sized = tex && textarget == GL_TEXTURE_2D && level == 0 && it != scene::g_tex.end() &&
                 scene::screen_sized(it->second.w, it->second.h);
    if (sized && !it->second.target) {
        it->second.target = true;
        {
            static auto bind = (void (*)(GLenum, GLuint))gl_proc("glBindTexture");
            static auto get = (void (*)(GLenum, GLenum, GLint*))gl_proc("glGetTexParameteriv");
            GLint mag = GL_LINEAR;
            u32 restore = scene::g_bound_tex[scene::g_unit];
            glq::sync([&] {
                bind(GL_TEXTURE_2D, tex);
                get(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag);
                bind(GL_TEXTURE_2D, restore);
            });
            it->second.mag_filter = mag;
        }
        if (scene::target_factor() != 1.f) scene::alloc_tex(tex, it->second);
        LOGI("3D scene: texture %u (%dx%d) is a screen-size render target of framebuffer %u", tex, it->second.w,
             it->second.h, scene::g_bound_fbo);
    }
    scene::note_attachment(att, sized, tex, false);
    glq::call(p, target, att, textarget, tex, level);
    scene::update_bound();
}
void m_glFramebufferRenderbuffer(u32 target, u32 att, u32 rbtarget, u32 rb) {
    LOAD(glFramebufferRenderbuffer);
    GL_TRACE("glFramebufferRenderbuffer");
    auto it = scene::g_rb.find(rb);
    bool sized = rb && it != scene::g_rb.end() && scene::screen_sized(it->second.w, it->second.h);
    if (sized && !it->second.target) {
        it->second.target = true;
        if (scene::target_factor() != 1.f) scene::alloc_rb(rb, it->second);
        LOGI("3D scene: renderbuffer %u (%dx%d) is a screen-size render target of framebuffer %u", rb, it->second.w,
             it->second.h, scene::g_bound_fbo);
    }
    scene::note_attachment(att, sized, rb, true);
    glq::call(p, target, att, rbtarget, rb);
    scene::update_bound();
}
void m_glViewport(s32 x, s32 y, s32 w, s32 h) {
    LOAD(glViewport);
    GL_TRACE("glViewport");
    scene::g_vp[0] = x, scene::g_vp[1] = y, scene::g_vp[2] = w, scene::g_vp[3] = h;
    scene::g_have_vp = true;
    FBLOG("fb: viewport %d %d %d %d", x, y, w, h);
    scene::apply_rect(p, scene::g_vp);
}
void m_glScissor(s32 x, s32 y, s32 w, s32 h) {
    LOAD(glScissor);
    GL_TRACE("glScissor");
    scene::g_sc[0] = x, scene::g_sc[1] = y, scene::g_sc[2] = w, scene::g_sc[3] = h;
    scene::g_have_sc = true;
    FBLOG("fb: scissor %d %d %d %d", x, y, w, h);
    scene::apply_rect(p, scene::g_sc);
}
// Reads from a scaled framebuffer: scale the source rectangle.
void m_glCopyTexSubImage2D(u32 target, s32 level, s32 xo, s32 yo, s32 x, s32 y, s32 w, s32 h) {
    LOAD(glCopyTexSubImage2D);
    GL_TRACE("glCopyTexSubImage2D");
    FBLOG("fb: copytexsubimage %d %d %dx%d", x, y, w, h);
    // Copies from a scaled framebuffer into a render target scaled the same
    // way keep both at the scaled size. (Any other copy is passed on
    // unchanged; the game makes none.)
    if (scene::g_rect_f != 1.f && target == GL_TEXTURE_2D) {
        auto it = scene::g_tex.find(scene::g_bound_tex[scene::g_unit]);
        const float dst = it != scene::g_tex.end() && it->second.target ? scene::target_factor() : 1.f;
        if (dst == scene::g_rect_f)
            xo = scene::sc(xo), yo = scene::sc(yo), x = scene::sc(x), y = scene::sc(y), w = scene::sc(w), h = scene::sc(h);
        else {
            static std::atomic<bool> once{false};
            if (!once.exchange(true))
                LOGW("glCopyTexSubImage2D from a framebuffer drawn x%.3f into a texture at x%.3f: copied unscaled",
                     scene::g_rect_f, dst);
        }
    }
    glq::call(p, target, level, xo, yo, x, y, w, h);
}
void m_glReadPixels(s32 x, s32 y, s32 w, s32 h, u32 fmt, u32 type, u32 px) {
    LOAD(glReadPixels);
    GL_TRACE("glReadPixels");
    FBLOG("fb: readpixels %d %d %dx%d", x, y, w, h);
    if (scene::g_rect_f != 1.f) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) LOGW("glReadPixels from a framebuffer drawn x%.3f: read unscaled", scene::g_rect_f);
    }
    glq::call_sync_void(p, x, y, w, h, fmt, type, mem::ptr<void>(px));
}
void m_glGetIntegerv(u32 pname, u32 out) {
    LOAD(glGetIntegerv);
    GL_TRACE("glGetIntegerv");
    // The game sees the rectangles it asked for, not the scaled ones.
    if (pname == GL_VIEWPORT && scene::g_have_vp) {
        for (int i = 0; i < 4; i++) mem::w32(out + i * 4, (u32)scene::g_vp[i]);
        return;
    }
    if (pname == GL_SCISSOR_BOX && scene::g_have_sc) {
        for (int i = 0; i < 4; i++) mem::w32(out + i * 4, (u32)scene::g_sc[i]);
        return;
    }
    glq::call_sync_void(p, pname, mem::ptr<GLint>(out));
}

// ---- filtered wrappers around generated thunks -------------------------------
void f_glUseProgram(u32 prog) {
    if (g_filter && g_program == (s64)prog) FILTERED();
    g_program = prog;
    g_cur_uniforms = &g_uniforms[prog];
    g_glUseProgram(prog);
}
void f_glLinkProgram(u32 prog) {
    g_uniforms.erase(prog);  // linking resets the program's uniforms
    if (g_program == (s64)prog) g_cur_uniforms = &g_uniforms[prog];
    g_glLinkProgram(prog);
    glq::call(&check_program, (GLuint)prog);
}
void f_glDeleteProgram(u32 prog) {
    g_uniforms.erase(prog);
    if (g_program == (s64)prog) g_program = -1, g_cur_uniforms = nullptr;
    g_glDeleteProgram(prog);
}
void f_glEnable(u32 cap) {
    int i = cap_index(cap);
    if (i >= 0) {
        if (g_filter && g_caps[i] == 1) FILTERED();
        g_caps[i] = 1;
    }
    g_glEnable(cap);
}
void f_glDisable(u32 cap) {
    int i = cap_index(cap);
    if (i >= 0) {
        if (g_filter && g_caps[i] == 0) FILTERED();
        g_caps[i] = 0;
    }
    g_glDisable(cap);
}
void f_glEnableVertexAttribArray(u32 i) {
    if (!g_filter || i >= 32) return g_glEnableVertexAttribArray(i);
    if (!(g_attrib_want & (1u << i))) g_attrib_want |= 1u << i;
    g_filtered++;  // applied at the next draw, if still wanted
}
void f_glDisableVertexAttribArray(u32 i) {
    if (!g_filter || i >= 32) return g_glDisableVertexAttribArray(i);
    g_attrib_want &= ~(1u << i);
    g_filtered++;
}
void f_glGetVertexAttribiv(u32 index, u32 pname, u32 out) {
    flush_attribs();
    g_glGetVertexAttribiv(index, pname, out);
}
void f_glGetVertexAttribfv(u32 index, u32 pname, u32 out) {
    flush_attribs();
    g_glGetVertexAttribfv(index, pname, out);
}
#define FILTER_STATE(var, ...)                    \
    do {                                          \
        if (g_filter && var.same(__VA_ARGS__)) FILTERED(); \
        var.set(__VA_ARGS__);                     \
    } while (0)
void f_glBlendFunc(u32 s, u32 d) {
    FILTER_STATE(g_blend_func, s, d, s, d);  // same as glBlendFuncSeparate(s, d, s, d)
    g_glBlendFunc(s, d);
}
void f_glBlendFuncSeparate(u32 s, u32 d, u32 sa, u32 da) {
    FILTER_STATE(g_blend_func, s, d, sa, da);
    g_glBlendFuncSeparate(s, d, sa, da);
}
void f_glDepthMask(u32 f) {
    FILTER_STATE(g_depth_mask, f & 0xFF);
    g_glDepthMask(f);
}
void f_glDepthFunc(u32 f) {
    FILTER_STATE(g_depth_func, f);
    g_glDepthFunc(f);
}
void f_glColorMask(u32 r, u32 g, u32 b, u32 a) {
    FILTER_STATE(g_color_mask, r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF);
    g_glColorMask(r, g, b, a);
}
void f_glCullFace(u32 m) {
    FILTER_STATE(g_cull_face, m);
    g_glCullFace(m);
}
void f_glFrontFace(u32 m) {
    FILTER_STATE(g_front_face, m);
    g_glFrontFace(m);
}
// Uniforms: single values and single-element arrays are compared with the
// value the current program already has.
u32 fbits(float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    return v;
}
void f_glUniform1i(u32 loc, u32 v) {
    if (uniform_same((s32)loc, &v, 1)) FILTERED();
    g_glUniform1i(loc, v);
}
void f_glUniform1f(u32 loc, float a) {
    u32 v[1] = {fbits(a)};
    if (uniform_same((s32)loc, v, 1)) FILTERED();
    g_glUniform1f(loc, a);
}
void f_glUniform2f(u32 loc, float a, float b) {
    u32 v[2] = {fbits(a), fbits(b)};
    if (uniform_same((s32)loc, v, 2)) FILTERED();
    g_glUniform2f(loc, a, b);
}
void f_glUniform3f(u32 loc, float a, float b, float c) {
    u32 v[3] = {fbits(a), fbits(b), fbits(c)};
    if (uniform_same((s32)loc, v, 3)) FILTERED();
    g_glUniform3f(loc, a, b, c);
}
void f_glUniform4f(u32 loc, float a, float b, float c, float d) {
    u32 v[4] = {fbits(a), fbits(b), fbits(c), fbits(d)};
    if (uniform_same((s32)loc, v, 4)) FILTERED();
    g_glUniform4f(loc, a, b, c, d);
}
// Uploads of several array elements are not tracked: forget the locations
// they cover (array element locations are consecutive in practice; to be
// safe, all values of the program are forgotten when an array is uploaded).
void forget_uniform_array() {
    if (g_cur_uniforms) g_cur_uniforms->clear();
}
template <int N, void (*G)(u32, u32, u32)>
void f_uniform_v(u32 loc, u32 count, u32 ptr) {
    if ((s32)count == 1 && ptr && uniform_same((s32)loc, mem::ptr<u32>(ptr), N)) FILTERED();
    if ((s32)count != 1) forget_uniform_array();
    G(loc, count, ptr);
}
void f_glUniformMatrix4fv(u32 loc, u32 count, u32 transpose, u32 ptr) {
    if ((s32)count == 1 && !transpose && ptr && uniform_same((s32)loc, mem::ptr<u32>(ptr), 16)) FILTERED();
    if ((s32)count != 1 || transpose) forget_uniform_array();
    g_glUniformMatrix4fv(loc, count, transpose, ptr);
}

// Only advertise extensions whose entry points/semantics the bridge supports,
// roughly what a 2013-era mobile GPU offered. Anything else could make the
// game call functions that are not bridged (e.g. VAOs, queries).
const char* const kAllowedExtensions[] = {
    "GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_dxt1",
    "GL_ANGLE_texture_compression_dxt3", "GL_ANGLE_texture_compression_dxt5", "GL_OES_depth24",
    "GL_OES_depth_texture", "GL_OES_packed_depth_stencil", "GL_OES_element_index_uint", "GL_OES_mapbuffer",
    "GL_OES_rgb8_rgba8", "GL_OES_standard_derivatives", "GL_OES_texture_npot", "GL_OES_texture_float",
    "GL_OES_texture_half_float", "GL_OES_texture_float_linear", "GL_OES_texture_half_float_linear",
    "GL_OES_vertex_half_float", "GL_EXT_texture_filter_anisotropic", "GL_APPLE_texture_max_level",
    "GL_OES_stencil8", "GL_EXT_blend_minmax",
};
std::string filter_extensions(const std::string& all) {
    std::string out;
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(' ', i);
        if (j == std::string::npos) j = all.size();
        std::string e = all.substr(i, j - i);
        for (const char* a : kAllowedExtensions)
            if (e == a) {
                out += e + " ";
                break;
            }
        i = j + 1;
    }
    return out;
}

std::unordered_map<u32, u32> g_strings;
u32 m_glGetString(u32 name) {
    LOAD(glGetString);
    GL_TRACE("glGetString");
    auto it = g_strings.find(name);
    if (it != g_strings.end()) return it->second;
    std::string v;
    glq::sync([&] {
        const char* s = (const char*)p(name);
        v = s ? s : "";
    });
    if (name == GL_VERSION) v = "OpenGL ES 2.0 (" + v + ")";
    if (name == GL_SHADING_LANGUAGE_VERSION) v = "OpenGL ES GLSL ES 1.00";
    if (name == GL_EXTENSIONS) v = filter_extensions(v);
    LOGI("glGetString(0x%X) = %s", name, v.c_str());
    u32 g = intern_string(v);
    g_strings[name] = g;
    return g;
}

// GL_OES_mapbuffer, emulated with a guest-side shadow copy.
struct Mapping {
    u32 guest = 0;
    s32 size = 0;
};
std::unordered_map<u32, Mapping> g_maps;  // target -> mapping
u32 m_glMapBufferOES(u32 target, u32 access) {
    (void)access;
    LOAD(glGetBufferParameteriv);
    GL_TRACE("glMapBufferOES");
    GLint size = 0;
    glq::call_sync_void(p, target, (GLenum)GL_BUFFER_SIZE, &size);
    Mapping m{mem::heap_alloc(size > 0 ? size : 1), size};
    g_maps[target] = m;
    return m.guest;
}
void (*p_buffer_sub_data)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
u32 m_glUnmapBufferOES(u32 target) {
    if (!p_buffer_sub_data) p_buffer_sub_data = (decltype(p_buffer_sub_data))gl_proc("glBufferSubData");
    GL_TRACE("glUnmapBufferOES");
    auto it = g_maps.find(target);
    if (it == g_maps.end()) return 0;
    struct Args {
        GLenum target;
        GLsizeiptr size;
    };
    glq::call_data<Args>([](const Args& a, const void* d) { p_buffer_sub_data(a.target, 0, a.size, d); },
                         Args{target, it->second.size}, mem::host(it->second.guest), (size_t)std::max(0, it->second.size));
    mem::heap_free(it->second.guest);
    g_maps.erase(it);
    return 1;
}
void m_glGetBufferPointervOES(u32 target, u32, u32 out) {
    auto it = g_maps.find(target);
    mem::w32(out, it == g_maps.end() ? 0 : it->second.guest);
}

u32 h_eglGetProcAddress(u32 name) {
    u32 a = thunks::lookup(mem::str(name));
    if (!a) LOGD("eglGetProcAddress(%s) -> not available", mem::str(name));
    return a;
}

}  // namespace

u64 gl_call_count() { return g_calls.load(); }
u64 gl_filtered_count() { return g_filtered; }
void gl_flush_state() { flush_attribs(); }
void gl_client_stats(u64& draws, u64& attribs, u64& index_draws) {
    draws = g_client_draws, attribs = g_client_attribs, index_draws = g_client_index_draws;
}
u64 gl_time_ns() { return g_gl_ns.load(); }
void gl_set_trace(bool on) { g_trace = on; }
void gl_fbtrace(int frames) { scene::g_trace = frames; }
int gl_fbtrace_left() { return scene::g_trace.load(); }
void gl_set_screen_size(int w, int h) {
    scene::g_screen_w = w;
    scene::g_screen_h = h;
}
void gl_set_scene_scale(float s) { scene::set_scale(s); }
void gl_set_output_size(int w, int h) {
    (void)h;
    if (scene::g_screen_w > 0) scene::set_output((float)w / (float)scene::g_screen_w);
}
float gl_output_scale() { return scene::g_out; }
float gl_scene_scale() { return scene::g_scale; }
bool gl_scene_scaled_targets() {
    for (auto& [id, t] : scene::g_tex)
        if (t.target) return true;
    return false;
}

void register_gles() {
    register_generated();
    REG("glDeleteFramebuffers", m_glDeleteFramebuffers);
    REG("glActiveTexture", m_glActiveTexture);
    REG("glBindTexture", m_glBindTexture);
    REG("glDeleteTextures", m_glDeleteTextures);
    REG("glTexImage2D", m_glTexImage2D);
    REG("glCompressedTexImage2D", f_glCompressedTexImage2D);
    REG("glTexParameteri", m_glTexParameteri);
    REG("glBindRenderbuffer", m_glBindRenderbuffer);
    REG("glDeleteRenderbuffers", m_glDeleteRenderbuffers);
    REG("glRenderbufferStorage", m_glRenderbufferStorage);
    REG("glFramebufferTexture2D", m_glFramebufferTexture2D);
    REG("glFramebufferRenderbuffer", m_glFramebufferRenderbuffer);
    REG("glViewport", m_glViewport);
    REG("glScissor", m_glScissor);
    REG("glCopyTexSubImage2D", m_glCopyTexSubImage2D);
    REG("glReadPixels", m_glReadPixels);
    REG("glDrawArrays", m_glDrawArrays);
    REG("glUseProgram", f_glUseProgram);
    REG("glLinkProgram", f_glLinkProgram);
    REG("glCompileShader", f_glCompileShader);
    REG("glDeleteProgram", f_glDeleteProgram);
    REG("glEnable", f_glEnable);
    REG("glDisable", f_glDisable);
    REG("glEnableVertexAttribArray", f_glEnableVertexAttribArray);
    REG("glDisableVertexAttribArray", f_glDisableVertexAttribArray);
    REG("glGetVertexAttribiv", f_glGetVertexAttribiv);
    REG("glBlendFunc", f_glBlendFunc);
    REG("glBlendFuncSeparate", f_glBlendFuncSeparate);
    REG("glGetVertexAttribfv", f_glGetVertexAttribfv);
    REG("glDepthMask", f_glDepthMask);
    REG("glDepthFunc", f_glDepthFunc);
    REG("glColorMask", f_glColorMask);
    REG("glCullFace", f_glCullFace);
    REG("glFrontFace", f_glFrontFace);
    REG("glUniform1i", f_glUniform1i);
    REG("glUniform1f", f_glUniform1f);
    REG("glUniform2f", f_glUniform2f);
    REG("glUniform3f", f_glUniform3f);
    REG("glUniform4f", f_glUniform4f);
    REG("glUniform1fv", (f_uniform_v<1, g_glUniform1fv>));
    REG("glUniform2fv", (f_uniform_v<2, g_glUniform2fv>));
    REG("glUniform3fv", (f_uniform_v<3, g_glUniform3fv>));
    REG("glUniform4fv", (f_uniform_v<4, g_glUniform4fv>));
    REG("glUniform1iv", (f_uniform_v<1, g_glUniform1iv>));
    REG("glUniformMatrix4fv", f_glUniformMatrix4fv);
    REG("glPixelStorei", m_glPixelStorei);
    REG("glBindBuffer", m_glBindBuffer);
    REG("glDeleteBuffers", m_glDeleteBuffers);
    REG("glVertexAttribPointer", m_glVertexAttribPointer);
    REG("glDrawElements", m_glDrawElements);
    REG("glShaderSource", m_glShaderSource);
    REG("glGetVertexAttribPointerv", m_glGetVertexAttribPointerv);
    REG("glBindFramebuffer", m_glBindFramebuffer);
    REG("glGetIntegerv", m_glGetIntegerv);
    REG("glGetString", m_glGetString);
    REG("glMapBufferOES", m_glMapBufferOES);
    REG("glUnmapBufferOES", m_glUnmapBufferOES);
    REG("glGetBufferPointervOES", m_glGetBufferPointervOES);
    REG("eglGetProcAddress", h_eglGetProcAddress);
}

}  // namespace hle
