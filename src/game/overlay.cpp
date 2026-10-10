// Loader overlay drawn on top of the game's frame with plain GLES2: the
// controller highlight, notices, the debug menu and the frame-rate counter.
// Drawing calls are recorded on the game thread and drawn on the GL thread
// (see end()). Everything the game may rely on (program, buffers, attributes
// 0/1, texture binding, blending, tests, viewport, framebuffer, unpack
// alignment) is saved and restored around the overlay.
#include "game/overlay.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <unordered_map>
#include <vector>

#include "external/stb/stb_truetype.h"  // implementation is in hle/jni/text.cpp
#include "game/paths.h"
#include "hle/gl_queue.h"
#include "platform/platform.h"

namespace game::overlay {

namespace {

#define GLFN(ret, name, args) ret(*name) args = nullptr
struct Gl {
    GLFN(GLuint, CreateShader, (GLenum));
    GLFN(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
    GLFN(void, CompileShader, (GLuint));
    GLFN(void, GetShaderiv, (GLuint, GLenum, GLint*));
    GLFN(GLuint, CreateProgram, ());
    GLFN(void, AttachShader, (GLuint, GLuint));
    GLFN(void, BindAttribLocation, (GLuint, GLuint, const GLchar*));
    GLFN(void, LinkProgram, (GLuint));
    GLFN(void, GetProgramiv, (GLuint, GLenum, GLint*));
    GLFN(GLint, GetUniformLocation, (GLuint, const GLchar*));
    GLFN(void, UseProgram, (GLuint));
    GLFN(void, Uniform1i, (GLint, GLint));
    GLFN(void, Uniform2f, (GLint, GLfloat, GLfloat));
    GLFN(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat));
    GLFN(void, GetIntegerv, (GLenum, GLint*));
    GLFN(GLboolean, IsEnabled, (GLenum));
    GLFN(void, Enable, (GLenum));
    GLFN(void, Disable, (GLenum));
    GLFN(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum));
    GLFN(void, BlendEquationSeparate, (GLenum, GLenum));
    GLFN(void, BindBuffer, (GLenum, GLuint));
    GLFN(void, BindFramebuffer, (GLenum, GLuint));
    GLFN(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
    GLFN(void, Scissor, (GLint, GLint, GLsizei, GLsizei));
    GLFN(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
    GLFN(void, GetBooleanv, (GLenum, GLboolean*));
    GLFN(void, GetVertexAttribiv, (GLuint, GLenum, GLint*));
    GLFN(void, GetVertexAttribPointerv, (GLuint, GLenum, void**));
    GLFN(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*));
    GLFN(void, EnableVertexAttribArray, (GLuint));
    GLFN(void, DisableVertexAttribArray, (GLuint));
    GLFN(void, DrawArrays, (GLenum, GLint, GLsizei));
    GLFN(void, GenTextures, (GLsizei, GLuint*));
    GLFN(void, DeleteTextures, (GLsizei, const GLuint*));
    GLFN(void, BindTexture, (GLenum, GLuint));
    GLFN(void, ActiveTexture, (GLenum));
    GLFN(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
    GLFN(void, TexParameteri, (GLenum, GLenum, GLint));
    GLFN(void, PixelStorei, (GLenum, GLint));
} gl;
#undef GLFN

GLuint g_color_prog = 0, g_text_prog = 0;
GLint g_cu_size = -1, g_tu_size = -1, g_tu_color = -1, g_tu_tex = -1;
bool g_failed = false;
int g_sw = 0, g_sh = 0;

template <class F>
void load(F& f, const char* name) {
    f = reinterpret_cast<F>(platform::gl_get_proc(name));
}

GLuint build(const char* vs, const char* fs) {
    auto compile = [](GLenum type, const char* src) {
        GLuint s = gl.CreateShader(type);
        gl.ShaderSource(s, 1, &src, nullptr);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        return ok ? s : 0u;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, v);
    gl.AttachShader(p, f);
    gl.BindAttribLocation(p, 0, "a_pos");
    gl.BindAttribLocation(p, 1, "a_uv");
    gl.BindAttribLocation(p, 1, "a_color");
    gl.LinkProgram(p);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0u;
}

bool setup() {
    if (g_color_prog) return true;
    if (g_failed) return false;
    load(gl.CreateShader, "glCreateShader");
    load(gl.ShaderSource, "glShaderSource");
    load(gl.CompileShader, "glCompileShader");
    load(gl.GetShaderiv, "glGetShaderiv");
    load(gl.CreateProgram, "glCreateProgram");
    load(gl.AttachShader, "glAttachShader");
    load(gl.BindAttribLocation, "glBindAttribLocation");
    load(gl.LinkProgram, "glLinkProgram");
    load(gl.GetProgramiv, "glGetProgramiv");
    load(gl.GetUniformLocation, "glGetUniformLocation");
    load(gl.UseProgram, "glUseProgram");
    load(gl.Uniform1i, "glUniform1i");
    load(gl.Uniform2f, "glUniform2f");
    load(gl.Uniform4f, "glUniform4f");
    load(gl.GetIntegerv, "glGetIntegerv");
    load(gl.IsEnabled, "glIsEnabled");
    load(gl.Enable, "glEnable");
    load(gl.Disable, "glDisable");
    load(gl.BlendFuncSeparate, "glBlendFuncSeparate");
    load(gl.BlendEquationSeparate, "glBlendEquationSeparate");
    load(gl.BindBuffer, "glBindBuffer");
    load(gl.BindFramebuffer, "glBindFramebuffer");
    load(gl.Viewport, "glViewport");
    load(gl.Scissor, "glScissor");
    load(gl.ColorMask, "glColorMask");
    load(gl.GetBooleanv, "glGetBooleanv");
    load(gl.GetVertexAttribiv, "glGetVertexAttribiv");
    load(gl.GetVertexAttribPointerv, "glGetVertexAttribPointerv");
    load(gl.VertexAttribPointer, "glVertexAttribPointer");
    load(gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
    load(gl.DisableVertexAttribArray, "glDisableVertexAttribArray");
    load(gl.DrawArrays, "glDrawArrays");
    load(gl.GenTextures, "glGenTextures");
    load(gl.DeleteTextures, "glDeleteTextures");
    load(gl.BindTexture, "glBindTexture");
    load(gl.ActiveTexture, "glActiveTexture");
    load(gl.TexImage2D, "glTexImage2D");
    load(gl.TexParameteri, "glTexParameteri");
    load(gl.PixelStorei, "glPixelStorei");
    if (!gl.CreateShader || !gl.DrawArrays || !gl.GetVertexAttribPointerv || !gl.TexImage2D || !gl.Scissor) {
        g_failed = true;
        return false;
    }
    const char* vs =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "uniform vec2 u_size;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "  v_uv = a_uv;\n"
        "  gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);\n"
        "}\n";
    // Flat and gradient geometry: one colour per vertex, so a whole overlay
    // (highlight, panels, glows) goes out in very few draw calls.
    const char* color_vs =
        "attribute vec2 a_pos;\n"
        "attribute vec4 a_color;\n"
        "uniform vec2 u_size;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "  v_color = a_color;\n"
        "  gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);\n"
        "}\n";
    const char* color_fs =
        "precision mediump float;\n"
        "varying vec4 v_color;\n"
        "void main() { gl_FragColor = v_color; }\n";
    const char* text_fs =
        "precision mediump float;\n"
        "uniform vec4 u_color;\n"
        "uniform sampler2D u_tex;\n"
        "varying vec2 v_uv;\n"
        "void main() { gl_FragColor = vec4(u_color.rgb, u_color.a * texture2D(u_tex, v_uv).a); }\n";
    g_color_prog = build(color_vs, color_fs);
    g_text_prog = build(vs, text_fs);
    if (!g_color_prog || !g_text_prog) {
        LOGW("overlay: shader setup failed; loader overlay disabled");
        g_failed = true;
        g_color_prog = 0;
        return false;
    }
    g_cu_size = gl.GetUniformLocation(g_color_prog, "u_size");
    g_tu_size = gl.GetUniformLocation(g_text_prog, "u_size");
    g_tu_color = gl.GetUniformLocation(g_text_prog, "u_color");
    g_tu_tex = gl.GetUniformLocation(g_text_prog, "u_tex");
    return true;
}

// ---- saved GL state --------------------------------------------------------
struct Attrib {
    GLint enabled, size, type, norm, stride, buffer;
    void* ptr;
};
struct State {
    GLint program, array_buffer, framebuffer, viewport[4], scissor_box[4], texture, active_texture, unpack;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    GLboolean blend, depth, cull, scissor, stencil, color_mask[4];
    Attrib attr[2];
} g_saved;

void save_attrib(GLuint i, Attrib& a) {
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.norm);
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
    gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
    gl.GetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.ptr);
}

void restore_attrib(GLuint i, const Attrib& a) {
    gl.BindBuffer(GL_ARRAY_BUFFER, a.buffer);
    gl.VertexAttribPointer(i, a.size, a.type, a.norm, a.stride, a.ptr);
    if (a.enabled) gl.EnableVertexAttribArray(i);
    else gl.DisableVertexAttribArray(i);
}

void save(State& s) {
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array_buffer);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
    gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    gl.GetIntegerv(GL_SCISSOR_BOX, s.scissor_box);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.active_texture);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpack);
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &s.blend_src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &s.blend_dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend_src_a);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &s.blend_dst_a);
    gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &s.blend_eq_rgb);
    gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blend_eq_a);
    s.blend = gl.IsEnabled(GL_BLEND);
    s.depth = gl.IsEnabled(GL_DEPTH_TEST);
    s.cull = gl.IsEnabled(GL_CULL_FACE);
    s.scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    s.stencil = gl.IsEnabled(GL_STENCIL_TEST);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);
    save_attrib(0, s.attr[0]);
    save_attrib(1, s.attr[1]);
}

void restore(const State& s) {
    restore_attrib(0, s.attr[0]);
    restore_attrib(1, s.attr[1]);
    gl.BindBuffer(GL_ARRAY_BUFFER, s.array_buffer);
    gl.BindTexture(GL_TEXTURE_2D, s.texture);
    gl.ActiveTexture(s.active_texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, s.unpack);
    gl.UseProgram(s.program);
    gl.BindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
    gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    gl.Scissor(s.scissor_box[0], s.scissor_box[1], s.scissor_box[2], s.scissor_box[3]);
    gl.BlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_a, s.blend_dst_a);
    gl.BlendEquationSeparate(s.blend_eq_rgb, s.blend_eq_a);
    auto set = [](GLenum cap, GLboolean on) { on ? gl.Enable(cap) : gl.Disable(cap); };
    set(GL_BLEND, s.blend);
    set(GL_DEPTH_TEST, s.depth);
    set(GL_CULL_FACE, s.cull);
    set(GL_SCISSOR_TEST, s.scissor);
    set(GL_STENCIL_TEST, s.stencil);
    gl.ColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
}

void add_quad(std::vector<float>& v, float x0, float y0, float x1, float y1, float u0 = 0, float v0 = 0,
              float u1 = 1, float v1 = 1) {
    const float q[] = {x0, y0, u0, v0, x1, y0, u1, v0, x1, y1, u1, v1,
                       x0, y0, u0, v0, x1, y1, u1, v1, x0, y1, u0, v1};
    v.insert(v.end(), q, q + 24);
}

// Text quads: x, y, u, v per vertex, one colour.
void draw_text_tris(const std::vector<float>& v, Color c) {
    gl.UseProgram(g_text_prog);
    gl.Uniform2f(g_tu_size, (float)g_sw, (float)g_sh);
    gl.Uniform4f(g_tu_color, c.r, c.g, c.b, c.a);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v.data());
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v.data() + 2);
    gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 4));
}

// Coloured triangles: x, y, r, g, b, a per vertex.
constexpr int kVtx = 6;
void draw_color_tris(const float* v, size_t vertices) {
    if (!vertices) return;
    gl.UseProgram(g_color_prog);
    gl.Uniform2f(g_cu_size, (float)g_sw, (float)g_sh);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kVtx * 4, v);
    gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, kVtx * 4, v + 2);
    gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)vertices);
}

// ---- text --------------------------------------------------------------------
struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};
Font& font() {
    static Font f;
    static bool tried = false;
    if (tried) return f;
    tried = true;
    std::string path = paths::assets_dir() + "/published/fonts/gothambold.ttf";
    if (FILE* fp = std::fopen(path.c_str(), "rb")) {
        std::fseek(fp, 0, SEEK_END);
        long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        f.data.resize(n > 0 ? (size_t)n : 0);
        if (n > 0 && std::fread(f.data.data(), 1, (size_t)n, fp) == (size_t)n)
            f.ok = stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)) != 0;
        std::fclose(fp);
    }
    if (!f.ok) LOGW("overlay: font %s not available; notices are shown without text", path.c_str());
    return f;
}

struct TextTex {
    GLuint tex = 0;
    int w = 0, h = 0;        // texture size
    float width = 0;         // text advance in pixels
    float ascent = 0;        // baseline offset from the top
};
std::unordered_map<std::string, TextTex> g_text_cache;
std::list<std::string> g_text_lru;

const TextTex* text_texture(const std::string& text, float px) {
    char key_prefix[16];
    std::snprintf(key_prefix, sizeof key_prefix, "%.1f|", px);
    std::string key = key_prefix + text;
    auto it = g_text_cache.find(key);
    if (it != g_text_cache.end()) return &it->second;
    Font& f = font();
    if (!f.ok) return nullptr;
    float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
    float ascent = asc * scale;
    int h = (int)std::ceil((asc - desc) * scale) + 2;
    float width = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)text[i], &adv, &lsb);
        width += adv * scale;
        if (i + 1 < text.size())
            width += scale * stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)text[i], (unsigned char)text[i + 1]);
    }
    int w = (int)std::ceil(width) + 2;
    std::vector<unsigned char> px_buf((size_t)w * h, 0);
    float pen = 1;
    for (size_t i = 0; i < text.size(); i++) {
        int cp = (unsigned char)text[i];
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        float sx = pen - std::floor(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cp, scale, scale, sx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> g((size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&f.info, g.data(), gw, gh, gw, scale, scale, sx, 0, cp);
            int ox = (int)std::floor(pen) + x0, oy = (int)std::round(ascent) + y0 + 1;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    int tx = ox + x, ty = oy + y;
                    if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                    unsigned char& d = px_buf[(size_t)ty * w + tx];
                    d = std::max(d, g[(size_t)y * gw + x]);
                }
        }
        pen += adv * scale;
        if (i + 1 < text.size()) pen += scale * stbtt_GetCodepointKernAdvance(&f.info, cp, (unsigned char)text[i + 1]);
    }
    TextTex t;
    t.w = w;
    t.h = h;
    t.width = width;
    t.ascent = ascent + 1;
    gl.GenTextures(1, &t.tex);
    gl.BindTexture(GL_TEXTURE_2D, t.tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px_buf.data());
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // keep the cache small
    if (g_text_lru.size() >= 48) {
        auto old = g_text_cache.find(g_text_lru.front());
        if (old != g_text_cache.end()) {
            gl.DeleteTextures(1, &old->second.tex);
            g_text_cache.erase(old);
        }
        g_text_lru.pop_front();
    }
    g_text_lru.push_back(key);
    return &(g_text_cache[key] = t);
}

// Text width from the font metrics alone (no GL; any thread).
float measure(const std::string& text, float px) {
    Font& f = font();
    if (!f.ok) return 0;
    float scale = stbtt_ScaleForPixelHeight(&f.info, px), width = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)text[i], &adv, &lsb);
        width += adv * scale;
        if (i + 1 < text.size())
            width += scale * stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)text[i], (unsigned char)text[i + 1]);
    }
    return width;
}

// ---- recorded drawing ------------------------------------------------------------
// The drawing calls only record what to draw. end() hands the list to the GL
// thread (hle/gl_queue), which draws it after the game's own GL calls of the
// frame, so the game thread never waits for the overlay.
struct Clip {
    bool on = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator==(const Clip& o) const {
        return on == o.on && (!on || (x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1));
    }
};
struct Op {
    enum Kind { Tris, Text } kind;
    size_t first = 0, count = 0;  // Tris: vertices in List::verts
    Clip clip;
    // Text
    float x = 0, y = 0, px = 0;
    Color c{};
    std::string text;
    Align align = Align::Left;
};
struct List {
    int w = 0, h = 0;
    std::vector<Op> ops;
    std::vector<float> verts;  // kVtx floats per vertex
    Clip clip;                 // current clip for new geometry
};
List* g_list = nullptr;

void vtx(float x, float y, Color c) {
    g_list->verts.insert(g_list->verts.end(), {x, y, c.r, c.g, c.b, c.a});
}
// Start or extend the current run of triangles (same clip).
void tris_begin() {
    if (g_list->ops.empty() || g_list->ops.back().kind != Op::Tris || !(g_list->ops.back().clip == g_list->clip)) {
        Op op;
        op.kind = Op::Tris;
        op.first = g_list->verts.size() / kVtx;
        op.clip = g_list->clip;
        g_list->ops.push_back(op);
    }
}
void tris_end() {
    Op& op = g_list->ops.back();
    op.count = g_list->verts.size() / kVtx - op.first;
}

void render(List* list) {  // on the GL thread
    if (setup()) {
        g_sw = list->w;
        g_sh = list->h;
        save(g_saved);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.Viewport(0, 0, list->w, list->h);
        gl.Disable(GL_DEPTH_TEST);
        gl.Disable(GL_CULL_FACE);
        gl.Disable(GL_SCISSOR_TEST);
        bool scissor_on = false;
        gl.Disable(GL_STENCIL_TEST);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.Enable(GL_BLEND);
        gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        gl.BindBuffer(GL_ARRAY_BUFFER, 0);
        gl.EnableVertexAttribArray(0);
        gl.EnableVertexAttribArray(1);
        gl.ActiveTexture(GL_TEXTURE0);
        for (const Op& op : list->ops) {
            if (op.clip.on != scissor_on) {
                op.clip.on ? gl.Enable(GL_SCISSOR_TEST) : gl.Disable(GL_SCISSOR_TEST);
                scissor_on = op.clip.on;
            }
            if (op.clip.on)  // GL's origin is bottom-left
                gl.Scissor(op.clip.x0, list->h - op.clip.y1, std::max(0, op.clip.x1 - op.clip.x0),
                           std::max(0, op.clip.y1 - op.clip.y0));
            if (op.kind == Op::Tris) {
                draw_color_tris(list->verts.data() + op.first * kVtx, op.count);
            } else {
                const TextTex* tt = text_texture(op.text, op.px);
                if (!tt) continue;
                float left = op.align == Align::Center ? op.x - tt->width / 2
                             : op.align == Align::Right ? op.x - tt->width
                                                        : op.x;
                float top = op.y - tt->ascent;  // y is the baseline
                left = std::round(left);
                top = std::round(top);
                gl.BindTexture(GL_TEXTURE_2D, tt->tex);
                gl.UseProgram(g_text_prog);
                gl.Uniform1i(g_tu_tex, 0);
                std::vector<float> v;
                add_quad(v, left - 1, top, left - 1 + tt->w, top + tt->h);
                draw_text_tris(v, op.c);
            }
        }
        restore(g_saved);
    }
    delete list;
}
}  // namespace

bool begin(int screen_w, int screen_h) {
    if (g_list || g_failed) return false;
    font();  // load it here, before the GL thread uses it
    g_list = new List;
    g_list->w = screen_w;
    g_list->h = screen_h;
    return true;
}

void end() {
    if (!g_list) return;
    List* l = g_list;
    g_list = nullptr;
    if (l->ops.empty()) {
        delete l;
        return;
    }
    hle::glq::call(&render, l);
}

void quad4(const float* xy, const Color* c) {
    if (!g_list) return;
    tris_begin();
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i : idx) vtx(xy[2 * i], xy[2 * i + 1], c[i]);
    tris_end();
}

void quad(float x0, float y0, float x1, float y1, Color tl, Color tr, Color br, Color bl) {
    const float xy[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    const Color c[4] = {tl, tr, br, bl};
    quad4(xy, c);
}

void rect(float x0, float y0, float x1, float y1, Color c) {
    if (x1 > x0 && y1 > y0) quad(x0, y0, x1, y1, c, c, c, c);
}

void frame(float x0, float y0, float x1, float y1, float t, Color c) {
    rect(x0 - t, y0 - t, x1 + t, y0, c);  // top
    rect(x0 - t, y1, x1 + t, y1 + t, c);  // bottom
    rect(x0 - t, y0, x0, y1, c);          // left
    rect(x1, y0, x1 + t, y1, c);          // right
}

void glow(float x0, float y0, float x1, float y1, float w, Color c) {
    const Color o{c.r, c.g, c.b, 0.f};
    quad(x0, y0 - w, x1, y0, o, o, c, c);  // top
    quad(x0, y1, x1, y1 + w, c, c, o, o);  // bottom
    quad(x0 - w, y0, x0, y1, o, c, c, o);  // left
    quad(x1, y0, x1 + w, y1, c, o, o, c);  // right
    // Corners: a fan around the corner point, so the glow rounds off.
    auto corner = [&](float cx, float cy, float dx, float dy) {
        const int n = 4;
        for (int i = 0; i < n; i++) {
            const float a0 = 1.5707963f * i / n, a1 = 1.5707963f * (i + 1) / n;
            const float xy[8] = {cx, cy, cx + dx * w * std::cos(a0), cy + dy * w * std::sin(a0),
                                 cx + dx * w * std::cos(a1), cy + dy * w * std::sin(a1), cx, cy};
            const Color cc[4] = {c, o, o, c};
            quad4(xy, cc);
        }
    };
    corner(x0, y0, -1, -1);
    corner(x1, y0, +1, -1);
    corner(x1, y1, +1, +1);
    corner(x0, y1, -1, +1);
}

void clip(float x0, float y0, float x1, float y1) {
    if (!g_list) return;
    g_list->clip = {true, (int)std::floor(x0), (int)std::floor(y0), (int)std::ceil(x1), (int)std::ceil(y1)};
}

void clip_off() {
    if (g_list) g_list->clip = Clip{};
}

float text_width(const std::string& text, float px) { return measure(text, px); }

void text(float x, float y, float px, const std::string& s, Color c, Align align) {
    if (!g_list || s.empty()) return;
    Op op;
    op.kind = Op::Text;
    op.clip = g_list->clip;
    op.x = x;
    op.y = y;
    op.px = px;
    op.c = c;
    op.text = s;
    op.align = align;
    g_list->ops.push_back(std::move(op));
}

void selection(float x0, float y0, float x1, float y1, float u, const SelectionLook& look) {
    if (!g_list) return;
    const float a = std::clamp(look.alpha, 0.f, 1.f), pulse = std::clamp(look.pulse, 0.f, 1.f),
                flash = std::clamp(look.flash, 0.f, 1.f);
    if (a <= 0.f) return;
    // Whole pixels keep the thin lines crisp.
    x0 = std::round(x0), y0 = std::round(y0), x1 = std::round(x1), y1 = std::round(y1);
    const float w = x1 - x0, h = y1 - y0;
    if (w < 2 || h < 2) return;
    const float W = (float)g_list->w, H = (float)g_list->h;
    const Color red = kGameRed;
    auto with = [](Color c, float alpha) { return Color{c.r, c.g, c.b, alpha}; };
    const Color hot{std::min(1.f, red.r + 0.17f), red.g + 0.16f + 0.5f * flash, red.b + 0.14f + 0.5f * flash, 1.f};

    // The frame sits a little outside the button and closes in when A is pressed.
    const float line = std::max(1.f, std::round(2 * u));
    const float gap = std::max(0.f, std::round((3 + 1.5f * pulse - 3 * flash) * u));
    // Kept on screen for buttons at its edges (e.g. the pause menu's left column).
    const float fx0 = std::max(line, x0 - gap), fy0 = std::max(line, y0 - gap);
    const float fx1 = std::min(W - line, x1 + gap), fy1 = std::min(H - line, y1 + gap);

    // 1. Soft red glow around the frame, breathing slowly.
    glow(fx0 - line, fy0 - line, fx1 + line, fy1 + line, std::round(11 * u),
         with(red, (0.26f + 0.12f * pulse + 0.25f * flash) * a));

    // 2. A short white flash over the button when A is pressed.
    if (flash > 0) rect(x0, y0, x1, y1, {1.f, 1.f, 1.f, 0.30f * flash * a});

    // 3. A slanted light sweeping across the button (clipped to it), at the
    //    angle of the game's own slanted buttons.
    if (look.sheen >= 0 && look.sheen <= 1) {
        const float slant = h * 0.6f, band = std::max(18 * u, 0.22f * h + 10 * u);
        const float travel = w + 2 * band + slant;
        const float cx = x0 - band - slant + travel * look.sheen;  // band centre at the bottom edge
        const float peak = 0.22f * a;
        const Color c0{1.f, 1.f, 1.f, 0.f}, c1{1.f, 1.f, 1.f, peak};
        clip(x0, y0, x1, y1);
        // two halves, each fading from the centre line to its edge
        const float left[8] = {cx - band + slant, y0, cx + slant, y0, cx, y1, cx - band, y1};
        const Color lc[4] = {c0, c1, c1, c0};
        quad4(left, lc);
        const float right[8] = {cx + slant, y0, cx + band + slant, y0, cx + band, y1, cx, y1};
        const Color rc[4] = {c1, c0, c0, c1};
        quad4(right, rc);
        clip_off();
    }

    // 4. The thin red frame, with a faint light line just inside it.
    frame(fx0, fy0, fx1, fy1, line, with(red, 0.95f * a));
    frame(fx0 + 1, fy0 + 1, fx1 - 1, fy1 - 1, 1, {1.f, 1.f, 1.f, (0.10f + 0.10f * pulse) * a});

    // 5. Bright corner brackets over the frame's corners, with a dark edge so
    //    they stay visible on red and white parts of the game's UI.
    const float t = std::max(2.f, std::round(3 * u));
    const float len = std::round(std::clamp(std::min(fx1 - fx0, fy1 - fy0) * 0.3f, 10 * u, 24 * u));
    const float bx0 = fx0 - line, by0 = fy0 - line, bx1 = fx1 + line, by1 = fy1 + line;
    auto corner = [&](float cx, float cy, float dx, float dy, Color c, float grow) {
        // (cx, cy) is the outer corner; dx/dy point into the rectangle.
        const float L = len + grow, T = t + grow;
        const float ox = -dx * grow * 0.5f, oy = -dy * grow * 0.5f;
        rect(std::min(cx, cx + dx * L) + ox, std::min(cy, cy + dy * T) + oy, std::max(cx, cx + dx * L) + ox,
             std::max(cy, cy + dy * T) + oy, c);
        rect(std::min(cx, cx + dx * T) + ox, std::min(cy + dy * T, cy + dy * L) + oy, std::max(cx, cx + dx * T) + ox,
             std::max(cy + dy * T, cy + dy * L) + oy, c);
    };
    const Color edge{0.f, 0.f, 0.f, 0.45f * a};
    for (int pass = 0; pass < 2; pass++) {
        const Color c = pass == 0 ? edge : with(hot, a);
        const float grow = pass == 0 ? 2.f : 0.f;
        corner(bx0, by0, +1, +1, c, grow);
        corner(bx1, by0, -1, +1, c, grow);
        corner(bx0, by1, +1, -1, c, grow);
        corner(bx1, by1, -1, -1, c, grow);
    }
}

void draw_highlight(int screen_w, int screen_h, float x0, float y0, float x1, float y1, const SelectionLook& look) {
    if (!begin(screen_w, screen_h)) return;
    selection(x0, y0, x1, y1, screen_h / 720.f, look);  // sizes are designed for 720p
    end();
}

}  // namespace game::overlay
