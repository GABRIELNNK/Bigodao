/**
 * @file gpu_ppu.h
 * @brief Centro do quadro (160x144, ou em HD) montado pela GPU (passos 14b-1 a 14b-3).
 *
 * Um shader faz, por pixel, o mesmo que o render_line do ppu_native: fundo e
 * janela pelo tilemap (atributos CGB, flips, banco, prioridade), sprites pelo
 * OAM de cada linha (sem o limite de 10, como na CPU) e a cor pela paleta da
 * linha. A entrada é o PPUNativeGpuFrame que o ppu_native guarda.
 *
 * Por enquanto só para conferir: o resultado fica numa textura fora da tela e
 * é lido de volta para comparar com o quadro da CPU. Precisa do vídeo OpenGL.
 */
#ifndef GPU_PPU_H
#define GPU_PPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ppu_native.h"

/* Prepara shader e texturas (chamar com o contexto OpenGL atual). */
bool gpu_ppu_init(char* err, size_t err_size);
void gpu_ppu_shutdown(void);
bool gpu_ppu_ready(void);
/* Monta o centro na textura da GPU. Só quadros CGB. */
bool gpu_ppu_compose(const PPUNativeGpuFrame* frame);
/* Lê o último centro montado: 160x144, 0xAARRGGBB, linha 0 = topo. */
bool gpu_ppu_read(uint32_t* argb);

/* Passo 14b-3: centro em alta resolução (160*N x 144*N, N = frame->hd_scale),
 * com os tiles HD do pacote (atlas montado quando o pacote muda). */
bool gpu_ppu_compose_hd(const PPUNativeGpuFrame* frame);
/* Lê o último centro HD; scale recebe o N. argb precisa de 160*N*144*N. */
bool gpu_ppu_read_hd(uint32_t* argb, int* scale);
/* Passo 14b-4: laterais do widescreen em HD (96*N x 144*N, 48 da esquerda e
 * 48 da direita lado a lado), dos registros que ppu.c manda por ponto. */
bool gpu_ppu_compose_margins_hd(const PPUNativeGpuFrame* frame);
bool gpu_ppu_read_margins_hd(uint32_t* argb, int* scale);
/* Passo 14c-1: a tela inteira (textura do jogo, (256 ou 160)*N x 144*N),
 * montada como em render_frame_internal: base 1x da CPU (já com o preto e o
 * fundo ambiente 1x das laterais) ampliada; por cima o centro HD e as
 * laterais HD da GPU (do último gpu_ppu_compose_hd / _margins_hd) e o fundo
 * ambiente HD. */
typedef struct {
    const uint32_t* base;    /* (wide ? 256 : 160) x 144, 0xAARRGGBB */
    bool wide;
    int scale;
    bool hd_center;          /* usar o centro HD da GPU */
    bool hd_margins;         /* usar as laterais HD da GPU */
    const uint16_t* amb_q;   /* 256x144x4 (fundo ambiente * 256) ou NULL */
} GpuScreenInput;
bool gpu_ppu_compose_screen(const GpuScreenInput* in);
bool gpu_ppu_read_screen(uint32_t* argb, int* w, int* h);
/* Passo 14c-2: a textura da tela (RGBA, linha 0 = topo) para desenhar na
 * janela; 0 se ainda não existe. Muda de id quando o tamanho muda. */
unsigned int gpu_ppu_screen_texture(int* w, int* h);
/* Texto curto sobre o atlas de tiles HD (para o menu). */
const char* gpu_ppu_atlas_info(void);

#endif /* GPU_PPU_H */
