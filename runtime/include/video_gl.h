/**
 * @file video_gl.h
 * @brief Apresentação por OpenGL (passo 14a).
 *
 * O quadro continua sendo montado na CPU (ppu_native + laterais + HD + fundo
 * ambiente + pós-processamento) num buffer ARGB8888; este módulo só o leva à
 * tela com OpenGL, no lugar do SDL_Renderer. É a base para a GPU montar o
 * quadro nos próximos passos.
 *
 * PC: OpenGL 3.3 core. Android: OpenGL ES 3.0. As funções são carregadas com
 * SDL_GL_GetProcAddress (nada para ligar no PC; no Android o ImGui usa o
 * libGLESv3).
 *
 * As coordenadas de destino são as da janela (as mesmas do SDL_Renderer);
 * o módulo converte para os pixels do framebuffer.
 */
#ifndef VIDEO_GL_H
#define VIDEO_GL_H

#include <SDL.h>
#include <stdbool.h>

typedef struct VglTex VglTex;

/* Atributos do contexto: chamar antes de criar a janela (SDL_WINDOW_OPENGL). */
void vgl_set_context_attributes(void);
/* Cria o contexto na janela e prepara o shader. Em erro, err recebe o motivo
 * e nada fica criado (a plataforma volta para o SDL_Renderer). */
bool vgl_init(SDL_Window* window, char* err, size_t err_size);
void vgl_shutdown(void);
bool vgl_ready(void);
SDL_GLContext vgl_context(void);
/* "#version 330 core" ou "#version 300 es" (para o ImGui). */
const char* vgl_glsl_version(void);
/* "OpenGL 3.3 (Mesa llvmpipe ...)" para o menu. */
const char* vgl_description(void);
void vgl_set_vsync(bool enabled);

/* Textura ARGB8888 (0xAARRGGBB), como a SDL_PIXELFORMAT_ARGB8888. */
VglTex* vgl_tex_create(int w, int h);
void vgl_tex_destroy(VglTex* tex);
void vgl_tex_set_linear(VglTex* tex, bool linear);
void vgl_tex_set_blend(VglTex* tex, bool blend);   /* como SDL_BLENDMODE_BLEND */
/* Buffer da CPU para escrever o quadro inteiro (pitch em bytes). */
void* vgl_tex_lock(VglTex* tex, int* pitch);
void vgl_tex_unlock(VglTex* tex);                 /* envia o buffer inteiro */
void vgl_tex_update(VglTex* tex, const SDL_Rect* rect, const void* pixels, int pitch);

/* Quadro: limpa de preto, desenha, apresenta. */
void vgl_begin_frame(SDL_Window* window);
void vgl_copy(VglTex* tex, const SDL_Rect* src, const SDL_Rect* dst);
void vgl_copy_f(VglTex* tex, const SDL_Rect* src, const SDL_FRect* dst);
void vgl_set_clip(const SDL_Rect* clip);        /* NULL = sem recorte */
void vgl_present(SDL_Window* window);

/* Lê o framebuffer (RGBA, de cima para baixo) — para testes. */
bool vgl_read_pixels(int x, int y, int w, int h, unsigned char* rgba);

#endif /* VIDEO_GL_H */
