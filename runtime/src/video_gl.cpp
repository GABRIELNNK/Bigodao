/**
 * @file video_gl.cpp
 * @brief Apresentação por OpenGL (passo 14a). Veja video_gl.h.
 */
#include "video_gl.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* Tipos e constantes do OpenGL definidos aqui (valores do padrão), para não
 * depender de qual cabeçalho de GL cada plataforma tem. As funções vêm do
 * SDL_GL_GetProcAddress com nomes próprios (vgl::), sem colidir com nada. */
#if defined(_WIN32)
#define VGL_APIENTRY __stdcall
#else
#define VGL_APIENTRY
#endif

namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;
typedef std::ptrdiff_t GLsizeiptr;

constexpr GLenum kTexture2D = 0x0DE1, kTexMinFilter = 0x2801, kTexMagFilter = 0x2800;
constexpr GLenum kNearest = 0x2600, kLinear = 0x2601;
constexpr GLenum kTexWrapS = 0x2802, kTexWrapT = 0x2803, kClampToEdge = 0x812F;
constexpr GLenum kRGBA = 0x1908, kRGBA8 = 0x8058, kUnsignedByte = 0x1401;
constexpr GLenum kUnpackAlignment = 0x0CF5, kUnpackRowLength = 0x0CF2, kPackAlignment = 0x0D05;
constexpr GLenum kColorBufferBit = 0x4000, kBlend = 0x0BE2, kScissorTest = 0x0C11;
constexpr GLenum kDepthTest = 0x0B71, kCullFace = 0x0B44;
constexpr GLenum kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303, kOne = 1;
constexpr GLenum kTriangleStrip = 0x0005, kFloat = 0x1406;
constexpr GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0;
constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84;
constexpr GLenum kTexture0 = 0x84C0, kVersion = 0x1F02, kRenderer = 0x1F01;

struct Fns {
#define VGL_FN(ret, name, args) ret(VGL_APIENTRY* name) args = nullptr;
    VGL_FN(GLenum, GetError, (void))
    VGL_FN(const GLubyte*, GetString, (GLenum))
    VGL_FN(void, Viewport, (GLint, GLint, GLsizei, GLsizei))
    VGL_FN(void, Scissor, (GLint, GLint, GLsizei, GLsizei))
    VGL_FN(void, Enable, (GLenum))
    VGL_FN(void, Disable, (GLenum))
    VGL_FN(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
    VGL_FN(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
    VGL_FN(void, Clear, (GLbitfield))
    VGL_FN(void, GenTextures, (GLsizei, GLuint*))
    VGL_FN(void, DeleteTextures, (GLsizei, const GLuint*))
    VGL_FN(void, BindTexture, (GLenum, GLuint))
    VGL_FN(void, ActiveTexture, (GLenum))
    VGL_FN(void, TexParameteri, (GLenum, GLenum, GLint))
    VGL_FN(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
    VGL_FN(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))
    VGL_FN(void, PixelStorei, (GLenum, GLint))
    VGL_FN(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))
    VGL_FN(GLuint, CreateShader, (GLenum))
    VGL_FN(void, DeleteShader, (GLuint))
    VGL_FN(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
    VGL_FN(void, CompileShader, (GLuint))
    VGL_FN(void, GetShaderiv, (GLuint, GLenum, GLint*))
    VGL_FN(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
    VGL_FN(GLuint, CreateProgram, (void))
    VGL_FN(void, DeleteProgram, (GLuint))
    VGL_FN(void, AttachShader, (GLuint, GLuint))
    VGL_FN(void, BindAttribLocation, (GLuint, GLuint, const GLchar*))
    VGL_FN(void, LinkProgram, (GLuint))
    VGL_FN(void, GetProgramiv, (GLuint, GLenum, GLint*))
    VGL_FN(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
    VGL_FN(void, UseProgram, (GLuint))
    VGL_FN(GLint, GetUniformLocation, (GLuint, const GLchar*))
    VGL_FN(void, Uniform1i, (GLint, GLint))
    VGL_FN(void, GenVertexArrays, (GLsizei, GLuint*))
    VGL_FN(void, DeleteVertexArrays, (GLsizei, const GLuint*))
    VGL_FN(void, BindVertexArray, (GLuint))
    VGL_FN(void, GenBuffers, (GLsizei, GLuint*))
    VGL_FN(void, DeleteBuffers, (GLsizei, const GLuint*))
    VGL_FN(void, BindBuffer, (GLenum, GLuint))
    VGL_FN(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))
    VGL_FN(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
    VGL_FN(void, EnableVertexAttribArray, (GLuint))
    VGL_FN(void, DrawArrays, (GLenum, GLint, GLsizei))
#undef VGL_FN
};

Fns gl;
SDL_GLContext g_ctx = nullptr;
bool g_ready = false;
bool g_es = false;
GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
std::string g_desc;
/* Por quadro: tamanhos da janela (coordenadas de destino) e do framebuffer. */
int g_win_w = 1, g_win_h = 1, g_fb_w = 1, g_fb_h = 1;
bool g_clip_on = false;

template <typename T>
bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool load_all(std::string& missing) {
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
#undef L
    return ok;
}

GLuint compile(GLenum type, const std::string& src, std::string& err) {
    GLuint s = gl.CreateShader(type);
    const char* p = src.c_str();
    gl.ShaderSource(s, 1, &p, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        char log[1024] = "";
        gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        err = std::string("shader: ") + log;
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

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
    "void main() { o_color = texture(u_tex, v_uv).bgra; }\n";

}  // namespace

struct VglTex {
    GLuint id = 0;
    int w = 0, h = 0;
    bool blend = false;
    bool linear = false;
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
    if (!load_all(missing)) return fail("funcoes do OpenGL faltando:" + missing);

    const std::string head = g_es ? "#version 300 es\nprecision mediump float;\n" : "#version 330 core\n";
    std::string e;
    const GLuint vs = compile(kVertexShader, head + kVertexBody, e);
    if (!vs) return fail(e);
    const GLuint fs = compile(kFragmentShader, head + kFragmentBody, e);
    if (!fs) { gl.DeleteShader(vs); return fail(e); }
    g_prog = gl.CreateProgram();
    gl.AttachShader(g_prog, vs);
    gl.AttachShader(g_prog, fs);
    gl.BindAttribLocation(g_prog, 0, "a_pos");
    gl.BindAttribLocation(g_prog, 1, "a_uv");
    gl.LinkProgram(g_prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint linked = 0;
    gl.GetProgramiv(g_prog, kLinkStatus, &linked);
    if (!linked) {
        char log[1024] = "";
        gl.GetProgramInfoLog(g_prog, sizeof(log), nullptr, log);
        return fail(std::string("link: ") + log);
    }
    gl.UseProgram(g_prog);
    gl.Uniform1i(gl.GetUniformLocation(g_prog, "u_tex"), 0);
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
    g_ready = true;
    fprintf(stderr, "[GL] %s\n", g_desc.c_str());
    return true;
}

void vgl_shutdown(void) {
    if (g_ctx) {
        if (gl.DeleteProgram && g_prog) gl.DeleteProgram(g_prog);
        if (gl.DeleteBuffers && g_vbo) gl.DeleteBuffers(1, &g_vbo);
        if (gl.DeleteVertexArrays && g_vao) gl.DeleteVertexArrays(1, &g_vao);
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
    if (g_ready && tex->id) gl.DeleteTextures(1, &tex->id);
    delete tex;
}

void vgl_tex_set_linear(VglTex* tex, bool linear) {
    if (!tex || !g_ready || tex->linear == linear) return;
    tex->linear = linear;
    gl.BindTexture(kTexture2D, tex->id);
    gl.TexParameteri(kTexture2D, kTexMinFilter, (GLint)(linear ? kLinear : kNearest));
    gl.TexParameteri(kTexture2D, kTexMagFilter, (GLint)(linear ? kLinear : kNearest));
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
