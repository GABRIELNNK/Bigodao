/**
 * @file video_gl.cpp
 * @brief Apresentação por OpenGL (passo 14a). Veja video_gl.h.
 */
#include "video_gl.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "video_gl_fns.h"

using namespace vglx;

namespace vglx {

Fns gl;

template <typename T>
static bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool load_fns(std::string& missing) {
    bool ok = true;
#define L(n) if (!load(gl.n, "gl" #n)) { ok = false; missing += " gl" #n; }
    L(GetError) L(GetString) L(Viewport) L(Scissor) L(Enable) L(Disable) L(BlendFuncSeparate)
    L(ClearColor) L(Clear) L(GenTextures) L(DeleteTextures) L(BindTexture) L(ActiveTexture)
    L(TexParameteri) L(TexImage2D) L(TexSubImage2D) L(PixelStorei) L(ReadPixels)
    L(CreateShader) L(DeleteShader) L(ShaderSource) L(CompileShader) L(GetShaderiv)
    L(GetShaderInfoLog) L(CreateProgram) L(DeleteProgram) L(AttachShader) L(BindAttribLocation)
    L(LinkProgram) L(GetProgramiv) L(GetProgramInfoLog) L(UseProgram) L(GetUniformLocation)
    L(Uniform1i) L(GenVertexArrays) L(DeleteVertexArrays) L(BindVertexArray) L(GenBuffers)
    L(DeleteBuffers) L(BindBuffer) L(BufferData) L(VertexAttribPointer)
    L(EnableVertexAttribArray) L(DrawArrays)
    L(GenFramebuffers) L(DeleteFramebuffers) L(BindFramebuffer) L(FramebufferTexture2D)
    L(CheckFramebufferStatus) L(TexImage3D) L(TexSubImage3D) L(GetIntegerv) L(Uniform2i) L(Uniform1f) L(Uniform4f) L(UniformMatrix4fv) L(Uniform2f)
    L(GenRenderbuffers) L(DeleteRenderbuffers) L(BindRenderbuffer) L(RenderbufferStorage)
    L(FramebufferRenderbuffer) L(DepthFunc) L(DepthMask) L(DrawArraysInstanced)
    L(Uniform3i) L(Uniform4i) L(Uniform4iv) L(Uniform3f) L(DrawBuffers)
#undef L
    return ok;
}

GLuint compile_shader(GLenum type, const std::string& src, std::string& err) {
    GLuint s = gl.CreateShader(type);
    const char* p = src.c_str();
    gl.ShaderSource(s, 1, &p, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        char log[2048] = "";
        gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        err = std::string("shader: ") + log;
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program(const std::string& vs_src, const std::string& fs_src,
                    const char* const* attrib_names, int attrib_count, std::string& err) {
    const GLuint vs = compile_shader(kVertexShader, vs_src, err);
    if (!vs) return 0;
    const GLuint fs = compile_shader(kFragmentShader, fs_src, err);
    if (!fs) { gl.DeleteShader(vs); return 0; }
    GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    for (int i = 0; i < attrib_count; ++i) gl.BindAttribLocation(prog, (GLuint)i, attrib_names[i]);
    gl.LinkProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint linked = 0;
    gl.GetProgramiv(prog, kLinkStatus, &linked);
    if (!linked) {
        char log[2048] = "";
        gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
        err = std::string("link: ") + log;
        gl.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

}  // namespace vglx

namespace {

SDL_GLContext g_ctx = nullptr;
bool g_ready = false;
bool g_es = false;
GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
GLint g_loc_rgba = -1;   /* passo 14c-2 */
std::string g_desc;
/* Por quadro: tamanhos da janela (coordenadas de destino) e do framebuffer. */
int g_win_w = 1, g_win_h = 1, g_fb_w = 1, g_fb_h = 1;
bool g_clip_on = false;

/* A textura guarda os bytes de 0xAARRGGBB na ordem da memória (B, G, R, A em
 * little-endian), enviados como RGBA: o shader devolve .bgra. */
const char* kVertexBody =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
const char* kFragmentBody =
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_rgba;\n"   /* passo 14c-2: textura já em RGBA (tela da GPU) */
    "void main() { vec4 c = texture(u_tex, v_uv); o_color = u_rgba != 0 ? c : c.bgra; }\n";

}  // namespace

const char* vglx::shader_header(void) {
    return g_es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                  "precision highp usampler2D;\nprecision highp sampler2D;\n"
                : "#version 330 core\n";
}

struct VglTex {
    GLuint id = 0;
    int w = 0, h = 0;
    bool blend = false;
    bool linear = false;
    bool owned = true;     /* passo 14c-2: false = textura de outro (vgl_tex_wrap) */
    bool rgba = false;     /* passo 14c-2: guarda RGBA de verdade (sem trocar canais) */
    std::vector<uint32_t> staging;
};

void vgl_set_context_attributes(void) {
#if defined(__ANDROID__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    const char* force_es = SDL_getenv("BIGODAO_GLES");   /* teste: o caminho do Android no PC */
    if (force_es && force_es[0] == '1') {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

/* ---- Passo 21a: medição por etapa (GPU por timer query, CPU pelo relógio) ---- */
namespace {
constexpr int kProfSlots = 4;      /* quadros em voo: o resultado é lido 3 quadros depois */
constexpr int kProfMarks = 40;     /* etapas por quadro (+1 marca de fim) */
constexpr int kProfNames = 40;
struct ProfSlot {
    GLuint q[kProfMarks + 1] = {};
    const char* name[kProfMarks] = {};
    double cpu[kProfMarks + 1] = {};
    int n = 0;
    bool pending = false;
};
bool g_prof_on = false, g_prof_gpu = false, g_prof_in_frame = false, g_prof_qmade = false;
ProfSlot g_prof[kProfSlots];
int g_prof_cur = 0;
unsigned g_prof_frame = 0;
struct ProfAcc { const char* name; double gpu, cpu; int gpu_n; double gmax, cmax; };
ProfAcc g_prof_acc[kProfNames];
int g_prof_acc_n = 0, g_prof_acc_frames = 0, g_prof_acc_gframes = 0;
double g_prof_acc_t0 = 0.0;
VglProfRow g_prof_rows[kProfNames];
int g_prof_rows_n = 0;
float g_prof_gtot = -1.0f, g_prof_ctot = 0.0f;

double prof_now_ms(void) {
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void prof_load(void) {
    g_prof_gpu = false;
    auto get = [](const char* a, const char* b) -> void* {
        void* f = SDL_GL_GetProcAddress(a);
        if (!f && b) f = SDL_GL_GetProcAddress(b);
        return f;
    };
    gl.GenQueries = reinterpret_cast<decltype(gl.GenQueries)>(get("glGenQueries", "glGenQueriesEXT"));
    gl.DeleteQueries = reinterpret_cast<decltype(gl.DeleteQueries)>(get("glDeleteQueries", "glDeleteQueriesEXT"));
    gl.GetQueryObjectuiv = reinterpret_cast<decltype(gl.GetQueryObjectuiv)>(get("glGetQueryObjectuiv", "glGetQueryObjectuivEXT"));
    if (g_es) {   /* no GLES o carimbo de tempo só existe com a extensão */
        const char* ext = (const char*)gl.GetString(kExtensions);
        gl.GetError();
        if (!ext || !std::strstr(ext, "GL_EXT_disjoint_timer_query")) return;
        gl.QueryCounter = reinterpret_cast<decltype(gl.QueryCounter)>(get("glQueryCounterEXT", nullptr));
        gl.GetQueryObjectui64v = reinterpret_cast<decltype(gl.GetQueryObjectui64v)>(get("glGetQueryObjectui64vEXT", nullptr));
    } else {
        gl.QueryCounter = reinterpret_cast<decltype(gl.QueryCounter)>(get("glQueryCounter", nullptr));
        gl.GetQueryObjectui64v = reinterpret_cast<decltype(gl.GetQueryObjectui64v)>(get("glGetQueryObjectui64v", nullptr));
    }
    g_prof_gpu = gl.GenQueries && gl.DeleteQueries && gl.QueryCounter && gl.GetQueryObjectuiv && gl.GetQueryObjectui64v;
}

ProfAcc* prof_acc(const char* name) {
    for (int i = 0; i < g_prof_acc_n; ++i)
        if (g_prof_acc[i].name == name || std::strcmp(g_prof_acc[i].name, name) == 0) return &g_prof_acc[i];
    if (g_prof_acc_n >= kProfNames) return nullptr;
    g_prof_acc[g_prof_acc_n] = {name, 0.0, 0.0, 0, 0.0, 0.0};
    return &g_prof_acc[g_prof_acc_n++];
}

/* lê um quadro antigo; sem o resultado pronto, o quadro só entra na CPU */
void prof_collect(ProfSlot& s) {
    if (!s.pending) return;
    s.pending = false;
    bool gpu_ok = g_prof_gpu && s.n > 0;
    if (gpu_ok) {
        GLuint avail = 0;
        gl.GetQueryObjectuiv(s.q[s.n], kQueryResultAvailable, &avail);
        if (!avail) gpu_ok = false;
        if (g_es) {
            GLint dis = 0;
            gl.GetIntegerv(kGpuDisjoint, &dis);
            if (dis) gpu_ok = false;
        }
    }
    unsigned long long t[kProfMarks + 1] = {};
    if (gpu_ok)
        for (int i = 0; i <= s.n; ++i) gl.GetQueryObjectui64v(s.q[i], kQueryResult, &t[i]);
    gl.GetError();
    for (int i = 0; i < s.n; ++i) {
        ProfAcc* a = prof_acc(s.name[i]);
        if (!a) continue;
        const double dc = s.cpu[i + 1] - s.cpu[i];
        a->cpu += dc;
        if (dc > a->cmax) a->cmax = dc;
        if (gpu_ok && t[i + 1] >= t[i]) {
            const double dg = (double)(t[i + 1] - t[i]) / 1.0e6;
            a->gpu += dg;
            a->gpu_n++;
            if (dg > a->gmax) a->gmax = dg;
        }
    }
    g_prof_acc_frames++;
    if (gpu_ok) g_prof_acc_gframes++;
    const double now = prof_now_ms();
    if (now - g_prof_acc_t0 >= 500.0 && g_prof_acc_frames > 0) {
        g_prof_rows_n = 0;
        double gt = 0.0, ct = 0.0;
        for (int i = 0; i < g_prof_acc_n; ++i) {
            VglProfRow& r = g_prof_rows[g_prof_rows_n++];
            r.name = g_prof_acc[i].name;
            r.cpu_ms = (float)(g_prof_acc[i].cpu / g_prof_acc_frames);
            r.gpu_ms = g_prof_acc_gframes > 0 ? (float)(g_prof_acc[i].gpu / g_prof_acc_gframes) : -1.0f;
            r.gpu_max = g_prof_acc_gframes > 0 ? (float)g_prof_acc[i].gmax : -1.0f;
            r.cpu_max = (float)g_prof_acc[i].cmax;
            ct += r.cpu_ms;
            if (r.gpu_ms > 0.0f) gt += r.gpu_ms;
        }
        g_prof_ctot = (float)ct;
        g_prof_gtot = g_prof_acc_gframes > 0 ? (float)gt : -1.0f;
        g_prof_acc_n = 0;
        g_prof_acc_frames = g_prof_acc_gframes = 0;
        g_prof_acc_t0 = now;
    }
}
}   /* namespace */

void vgl_prof_enable(bool on) {
    if (on == g_prof_on) return;
    g_prof_on = on;
    g_prof_in_frame = false;
    for (ProfSlot& s : g_prof) s.pending = false;
    g_prof_acc_n = g_prof_rows_n = 0;
    g_prof_acc_frames = g_prof_acc_gframes = 0;
    g_prof_acc_t0 = prof_now_ms();
}
bool vgl_prof_enabled(void) { return g_prof_on; }
bool vgl_prof_gpu_available(void) { return g_prof_gpu; }

void vgl_prof_begin(void) {
    if (!g_prof_on || !g_ready) return;
    if (g_prof_gpu && !g_prof_qmade) {
        for (ProfSlot& s : g_prof) gl.GenQueries(kProfMarks + 1, s.q);
        g_prof_qmade = true;
    }
    g_prof_cur = (int)(g_prof_frame++ % kProfSlots);
    prof_collect(g_prof[(g_prof_cur + 1) % kProfSlots]);   /* o mais antigo em voo */
    ProfSlot& s = g_prof[g_prof_cur];
    s.pending = false;
    s.n = 0;
    g_prof_in_frame = true;
}

void vgl_prof_mark(const char* name) {
    if (!g_prof_in_frame) return;
    ProfSlot& s = g_prof[g_prof_cur];
    if (s.n >= kProfMarks) return;
    if (g_prof_gpu) gl.QueryCounter(s.q[s.n], kTimestamp);
    s.cpu[s.n] = prof_now_ms();
    s.name[s.n++] = name;
}

void vgl_prof_end(void) {
    if (!g_prof_in_frame) return;
    ProfSlot& s = g_prof[g_prof_cur];
    if (g_prof_gpu) gl.QueryCounter(s.q[s.n], kTimestamp);
    s.cpu[s.n] = prof_now_ms();
    s.pending = s.n > 0;
    g_prof_in_frame = false;
}

int vgl_prof_rows(VglProfRow* out, int max, float* gpu_total_ms, float* cpu_total_ms) {
    const int n = std::min(max, g_prof_rows_n);
    for (int i = 0; i < n; ++i) out[i] = g_prof_rows[i];
    if (gpu_total_ms) *gpu_total_ms = g_prof_gtot;
    if (cpu_total_ms) *cpu_total_ms = g_prof_ctot;
    return n;
}

bool vgl_init(SDL_Window* window, char* err, size_t err_size) {
    auto fail = [&](const std::string& why) {
        if (err && err_size) SDL_snprintf(err, err_size, "%s", why.c_str());
        vgl_shutdown();
        return false;
    };
    int profile = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
    g_es = (profile == SDL_GL_CONTEXT_PROFILE_ES);
    g_ctx = SDL_GL_CreateContext(window);
    if (!g_ctx) return fail(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
    SDL_GL_MakeCurrent(window, g_ctx);
    std::string missing;
    if (!load_fns(missing)) return fail("funcoes do OpenGL faltando:" + missing);

    std::string e;
    static const char* const kAttribs[] = {"a_pos", "a_uv"};
    g_prog = link_program(std::string(shader_header()) + kVertexBody,
                          std::string(shader_header()) + kFragmentBody, kAttribs, 2, e);
    if (!g_prog) return fail(e);
    gl.UseProgram(g_prog);
    gl.Uniform1i(gl.GetUniformLocation(g_prog, "u_tex"), 0);
    g_loc_rgba = gl.GetUniformLocation(g_prog, "u_rgba");
    gl.Uniform1i(g_loc_rgba, 0);
    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.GenBuffers(1, &g_vbo);
    gl.BindBuffer(kArrayBuffer, g_vbo);
    gl.BufferData(kArrayBuffer, 16 * sizeof(float), nullptr, kStreamDraw);
    gl.VertexAttribPointer(0, 2, kFloat, 0, 4 * sizeof(float), (const void*)0);
    gl.VertexAttribPointer(1, 2, kFloat, 0, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    if (gl.GetError() != 0) return fail("erro do OpenGL ao preparar o desenho");

    const char* ver = (const char*)gl.GetString(kVersion);
    const char* ren = (const char*)gl.GetString(kRenderer);
    g_desc = std::string(ver ? ver : "?") + " (" + (ren ? ren : "?") + ")";
    prof_load();   /* passo 21a */
    g_ready = true;
    fprintf(stderr, "[GL] %s\n", g_desc.c_str());
    return true;
}

void vgl_shutdown(void) {
    if (g_ctx) {
        if (gl.DeleteProgram && g_prog) gl.DeleteProgram(g_prog);
        if (gl.DeleteBuffers && g_vbo) gl.DeleteBuffers(1, &g_vbo);
        if (gl.DeleteVertexArrays && g_vao) gl.DeleteVertexArrays(1, &g_vao);
        if (g_prof_qmade && gl.DeleteQueries)   /* passo 21a */
            for (ProfSlot& ps : g_prof) gl.DeleteQueries(kProfMarks + 1, ps.q);
        g_prof_qmade = false;
        g_prof_in_frame = false;
        SDL_GL_DeleteContext(g_ctx);
    }
    g_prog = g_vao = g_vbo = 0;
    g_ctx = nullptr;
    g_ready = false;
}

bool vgl_ready(void) { return g_ready; }
SDL_GLContext vgl_context(void) { return g_ctx; }
const char* vgl_glsl_version(void) { return g_es ? "#version 300 es" : "#version 330 core"; }
const char* vgl_description(void) { return g_desc.c_str(); }

void vgl_set_vsync(bool enabled) {
    if (g_ready) SDL_GL_SetSwapInterval(enabled ? 1 : 0);
}

VglTex* vgl_tex_create(int w, int h) {
    if (!g_ready || w <= 0 || h <= 0) return nullptr;
    VglTex* t = new VglTex();
    t->w = w;
    t->h = h;
    t->staging.assign((size_t)w * h, 0u);   /* transparente (camada de sprites do 120 fps) */
    gl.GenTextures(1, &t->id);
    gl.BindTexture(kTexture2D, t->id);
    gl.TexParameteri(kTexture2D, kTexMinFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2D, kTexMagFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2D, kTexWrapS, (GLint)kClampToEdge);
    gl.TexParameteri(kTexture2D, kTexWrapT, (GLint)kClampToEdge);
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.PixelStorei(kUnpackRowLength, 0);
    gl.TexImage2D(kTexture2D, 0, (GLint)kRGBA8, w, h, 0, kRGBA, kUnsignedByte, t->staging.data());
    return t;
}

void vgl_tex_destroy(VglTex* tex) {
    if (!tex) return;
    if (g_ready && tex->id && tex->owned) gl.DeleteTextures(1, &tex->id);
    delete tex;
}

void vgl_tex_set_linear(VglTex* tex, bool linear) {
    if (!tex || !g_ready || tex->linear == linear) return;
    tex->linear = linear;
    gl.BindTexture(kTexture2D, tex->id);
    gl.TexParameteri(kTexture2D, kTexMinFilter, (GLint)(linear ? kLinear : kNearest));
    gl.TexParameteri(kTexture2D, kTexMagFilter, (GLint)(linear ? kLinear : kNearest));
}

VglTex* vgl_tex_wrap(unsigned int id, int w, int h) {
    if (!g_ready || !id || w <= 0 || h <= 0) return nullptr;
    VglTex* t = new VglTex();
    t->id = id;
    t->w = w;
    t->h = h;
    t->owned = false;
    t->rgba = true;
    gl.BindTexture(kTexture2D, id);   /* o filtro fica conhecido (vgl_tex_set_linear) */
    gl.TexParameteri(kTexture2D, kTexMinFilter, (GLint)kNearest);
    gl.TexParameteri(kTexture2D, kTexMagFilter, (GLint)kNearest);
    return t;
}

void vgl_tex_set_blend(VglTex* tex, bool blend) {
    if (tex) tex->blend = blend;
}

void* vgl_tex_lock(VglTex* tex, int* pitch) {
    if (!tex) return nullptr;
    if (pitch) *pitch = tex->w * 4;
    return tex->staging.data();
}

void vgl_tex_unlock(VglTex* tex) {
    if (!tex || !g_ready) return;
    gl.BindTexture(kTexture2D, tex->id);
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.PixelStorei(kUnpackRowLength, 0);
    gl.TexSubImage2D(kTexture2D, 0, 0, 0, tex->w, tex->h, kRGBA, kUnsignedByte, tex->staging.data());
}

void vgl_tex_update(VglTex* tex, const SDL_Rect* rect, const void* pixels, int pitch) {
    if (!tex || !g_ready || !pixels) return;
    SDL_Rect r = rect ? *rect : SDL_Rect{0, 0, tex->w, tex->h};
    if (r.w <= 0 || r.h <= 0) return;
    gl.BindTexture(kTexture2D, tex->id);
    gl.PixelStorei(kUnpackAlignment, 4);
    gl.PixelStorei(kUnpackRowLength, pitch / 4);
    gl.TexSubImage2D(kTexture2D, 0, r.x, r.y, r.w, r.h, kRGBA, kUnsignedByte, pixels);
    gl.PixelStorei(kUnpackRowLength, 0);
}

void vgl_begin_frame(SDL_Window* window) {
    if (!g_ready) return;
    SDL_GetWindowSize(window, &g_win_w, &g_win_h);
    SDL_GL_GetDrawableSize(window, &g_fb_w, &g_fb_h);
    if (g_win_w <= 0) g_win_w = 1;
    if (g_win_h <= 0) g_win_h = 1;
    if (g_fb_w <= 0) g_fb_w = g_win_w;
    if (g_fb_h <= 0) g_fb_h = g_win_h;
    gl.Disable(kScissorTest);
    g_clip_on = false;
    gl.Disable(kDepthTest);
    gl.Disable(kCullFace);
    gl.Viewport(0, 0, g_fb_w, g_fb_h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(kColorBufferBit);
}

void vgl_copy_f(VglTex* tex, const SDL_Rect* src, const SDL_FRect* dst) {
    if (!tex || !g_ready || !dst) return;
    /* O ImGui troca programa, VAO, viewport e blend: tudo é refeito aqui. */
    gl.Viewport(0, 0, g_fb_w, g_fb_h);
    gl.UseProgram(g_prog);
    gl.BindVertexArray(g_vao);
    gl.BindBuffer(kArrayBuffer, g_vbo);
    gl.ActiveTexture(kTexture0);
    gl.BindTexture(kTexture2D, tex->id);
    gl.Uniform1i(g_loc_rgba, tex->rgba ? 1 : 0);   /* passo 14c-2 */
    if (tex->blend) {
        gl.Enable(kBlend);
        gl.BlendFuncSeparate(kSrcAlpha, kOneMinusSrcAlpha, kOne, kOneMinusSrcAlpha);
    } else {
        gl.Disable(kBlend);
    }
    const SDL_Rect s = src ? *src : SDL_Rect{0, 0, tex->w, tex->h};
    const float u0 = (float)s.x / (float)tex->w, u1 = (float)(s.x + s.w) / (float)tex->w;
    const float v0 = (float)s.y / (float)tex->h, v1 = (float)(s.y + s.h) / (float)tex->h;
    const float x0 = dst->x / (float)g_win_w * 2.0f - 1.0f;
    const float x1 = (dst->x + dst->w) / (float)g_win_w * 2.0f - 1.0f;
    const float y0 = 1.0f - dst->y / (float)g_win_h * 2.0f;
    const float y1 = 1.0f - (dst->y + dst->h) / (float)g_win_h * 2.0f;
    const float v[16] = {x0, y0, u0, v0,  x1, y0, u1, v0,  x0, y1, u0, v1,  x1, y1, u1, v1};
    gl.BufferData(kArrayBuffer, sizeof(v), v, kStreamDraw);
    gl.DrawArrays(kTriangleStrip, 0, 4);
}

void vgl_copy(VglTex* tex, const SDL_Rect* src, const SDL_Rect* dst) {
    if (!tex) return;
    SDL_FRect d = dst ? SDL_FRect{(float)dst->x, (float)dst->y, (float)dst->w, (float)dst->h}
                      : SDL_FRect{0.0f, 0.0f, (float)g_win_w, (float)g_win_h};
    vgl_copy_f(tex, src, &d);
}

/* Passo 3D-1: retângulo da janela (coordenadas de vgl_copy) em pixels do
 * framebuffer, origem embaixo à esquerda (para glViewport). */
void vgl_rect_to_fb(const SDL_Rect* r, int* x, int* y, int* w, int* h) {
    const float sx = (float)g_fb_w / (float)g_win_w, sy = (float)g_fb_h / (float)g_win_h;
    const int fx = (int)(r->x * sx + 0.5f), fw = (int)(r->w * sx + 0.5f), fh = (int)(r->h * sy + 0.5f);
    const int fy = g_fb_h - (int)((r->y + r->h) * sy + 0.5f);
    if (x) *x = fx;
    if (y) *y = fy;
    if (w) *w = fw;
    if (h) *h = fh;
}

void vgl_set_clip(const SDL_Rect* clip) {
    if (!g_ready) return;
    if (!clip) {
        gl.Disable(kScissorTest);
        g_clip_on = false;
        return;
    }
    const float sx = (float)g_fb_w / (float)g_win_w, sy = (float)g_fb_h / (float)g_win_h;
    const int x = (int)(clip->x * sx + 0.5f);
    const int w = (int)(clip->w * sx + 0.5f);
    const int h = (int)(clip->h * sy + 0.5f);
    const int y = g_fb_h - (int)((clip->y + clip->h) * sy + 0.5f);
    gl.Enable(kScissorTest);
    gl.Scissor(x, y, w < 0 ? 0 : w, h < 0 ? 0 : h);
    g_clip_on = true;
}

void vgl_present(SDL_Window* window) {
    if (!g_ready) return;
    SDL_GL_SwapWindow(window);
}

bool vgl_read_pixels(int x, int y, int w, int h, unsigned char* rgba) {
    if (!g_ready || !rgba) return false;
    gl.PixelStorei(kPackAlignment, 1);
    std::vector<unsigned char> tmp((size_t)w * h * 4);
    gl.ReadPixels(x, g_fb_h - y - h, w, h, kRGBA, kUnsignedByte, tmp.data());
    for (int r = 0; r < h; ++r) {   /* GL lê de baixo para cima */
        std::memcpy(rgba + (size_t)r * w * 4, tmp.data() + (size_t)(h - 1 - r) * w * 4, (size_t)w * 4);
    }
    return gl.GetError() == 0;
}
