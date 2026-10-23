/**
 * @file video_gl_fns.h
 * @brief Funções e constantes do OpenGL usadas pelo Bigodão (passo 14b).
 *
 * Tipos e constantes definidos aqui (valores do padrão), para não depender de
 * qual cabeçalho de GL cada plataforma tem. As funções vêm do
 * SDL_GL_GetProcAddress (vgl_load_fns, chamado pelo vgl_init) com nomes
 * próprios (vglx::gl.Nome), sem colidir com nada. Só para C++.
 */
#ifndef VIDEO_GL_FNS_H
#define VIDEO_GL_FNS_H

#include <cstddef>
#include <string>

#if defined(_WIN32)
#define VGL_APIENTRY __stdcall
#else
#define VGL_APIENTRY
#endif

namespace vglx {

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
constexpr GLenum kR8UI = 0x8232, kRedInteger = 0x8D94, kRGBA8UI = 0x8D7C, kRGBAInteger = 0x8D99;
constexpr GLenum kRG8UI = 0x8238, kRG16UI = 0x823A, kRGInteger = 0x8228, kUnsignedShort = 0x1403;
constexpr GLenum kR16UI = 0x8234, kRGBA16UI = 0x8D76, kTexture2DArray = 0x8C1A;
constexpr GLenum kRGBA32UI = 0x8D70, kUnsignedInt = 0x1405;
constexpr GLenum kRenderbuffer = 0x8D41, kDepthComponent24 = 0x81A6, kDepthAttachment = 0x8D00;   /* 3D-3 */
constexpr GLenum kDepthBufferBit = 0x0100, kLequal = 0x0203, kBack = 0x0405;
constexpr GLenum kMaxTextureSize = 0x0D33, kMaxArrayTextureLayers = 0x88FF;
constexpr GLenum kUnpackAlignment = 0x0CF5, kUnpackRowLength = 0x0CF2, kPackAlignment = 0x0D05;
constexpr GLenum kColorBufferBit = 0x4000, kBlend = 0x0BE2, kScissorTest = 0x0C11;
constexpr GLenum kDepthTest = 0x0B71, kCullFace = 0x0B44;
constexpr GLenum kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303, kOne = 1;
constexpr GLenum kTriangles = 0x0004, kTriangleStrip = 0x0005, kFloat = 0x1406;
constexpr GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0;
constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84;
constexpr GLenum kTexture0 = 0x84C0, kVersion = 0x1F02, kRenderer = 0x1F01;
constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;

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
    VGL_FN(void, Uniform2i, (GLint, GLint, GLint))   /* passo 14e-1 */
    VGL_FN(void, Uniform1f, (GLint, GLfloat))        /* passo 3D-1 */
    VGL_FN(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
    VGL_FN(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
    VGL_FN(void, Uniform2f, (GLint, GLfloat, GLfloat))   /* passo 3D-3 */
    VGL_FN(void, GenRenderbuffers, (GLsizei, GLuint*))
    VGL_FN(void, DeleteRenderbuffers, (GLsizei, const GLuint*))
    VGL_FN(void, BindRenderbuffer, (GLenum, GLuint))
    VGL_FN(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
    VGL_FN(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
    VGL_FN(void, DepthFunc, (GLenum))
    VGL_FN(void, DepthMask, (GLboolean))
    VGL_FN(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))
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
    VGL_FN(void, GenFramebuffers, (GLsizei, GLuint*))
    VGL_FN(void, DeleteFramebuffers, (GLsizei, const GLuint*))
    VGL_FN(void, BindFramebuffer, (GLenum, GLuint))
    VGL_FN(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
    VGL_FN(GLenum, CheckFramebufferStatus, (GLenum))
    VGL_FN(void, TexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
    VGL_FN(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*))
    VGL_FN(void, GetIntegerv, (GLenum, GLint*))
#undef VGL_FN
};

extern Fns gl;

/* Carrega as funções (chamado pelo vgl_init). */
bool load_fns(std::string& missing);
/* Shader: devolve 0 e o motivo em err se não compilar. */
GLuint compile_shader(GLenum type, const std::string& src, std::string& err);
/* Programa com os dois shaders; os atributos (nome, posição) são ligados
 * antes do link. Devolve 0 e o motivo em err. */
GLuint link_program(const std::string& vs, const std::string& fs,
                    const char* const* attrib_names, int attrib_count, std::string& err);
/* "#version 330 core\n" ou "#version 300 es\nprecision ...\n" */
const char* shader_header(void);

}  // namespace vglx

#endif /* VIDEO_GL_FNS_H */
