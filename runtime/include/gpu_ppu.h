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
    bool black_fill;         /* passo 3D-1: laterais sem cenário (vão para longe) */
} GpuScreenInput;
bool gpu_ppu_compose_screen(const GpuScreenInput* in);
bool gpu_ppu_read_screen(uint32_t* argb, int* w, int* h);
/* Passo 14c-2: a textura da tela (RGBA, linha 0 = topo) para desenhar na
 * janela; 0 se ainda não existe. Muda de id quando o tamanho muda. */
unsigned int gpu_ppu_screen_texture(int* w, int* h);
/* Passo 14e-1: filtro de pós-processamento (GBPostFilterMode; 0 = nenhum)
 * sobre a última tela, com as mesmas contas de post_processing.cpp. Depois
 * dele, gpu_ppu_screen_texture e as camadas do 120 fps usam a tela filtrada. */
bool gpu_ppu_apply_filter(int mode);
bool gpu_ppu_read_final(uint32_t* argb);

/* Passo 3D-1: modo 3D em camadas. gpu_ppu_compose_3d monta a tela em três
 * imagens (cor, só o fundo, classe por ponto: 0 longe, 1 fundo, 2 HUD,
 * 3 sprite) a partir do quadro capturado e da base 1x (in->hd_margins = usar
 * as laterais do ppu.c; in->scale = escala da textura). gpu_ppu_draw_3d
 * desenha na janela, no retângulo do jogo (pixels do framebuffer, origem
 * embaixo), com a câmera inclinada; com inclinação 0 fica igual ao 2D. */
typedef struct {
    int vp_x, vp_y, vp_w, vp_h;
    float tilt_deg;   /* olhando de cima para baixo */
    float yaw_deg;
    float depth;      /* 1 = profundidade padrão */
    bool linear;
    bool debug;       /* 3D-3f: pinta por cima as classes (diagnóstico) */
    int hide;         /* 3D-4f: diagnóstico, esconde partes (bit = camada; 11 lados, 12 chão da superfície) */
} Gpu3DView;
bool gpu_ppu_compose_3d(const PPUNativeGpuFrame* frame, const GpuScreenInput* in);
bool gpu_ppu_draw_3d(const Gpu3DView* view);
/* Passo 3D-3: o 3D é desenhado fora da tela (com profundidade) no tamanho do
 * retângulo do jogo; esta é a imagem pronta para ir à janela (RGBA, linha 0 = topo). */
unsigned int gpu_ppu_3d_texture(int* w, int* h);
bool gpu_ppu_read_3d(int which, uint32_t* argb, int* w, int* h);

/* Passo 14d-1: camadas do 120 fps a partir da última tela da GPU (mesmo
 * tamanho). runs: 256x144 pares (início + 1, fim) do trecho de pontos de
 * sprite que contém cada ponto da resolução base (0 = cenário), na largura
 * base (256 ou 160; o resto da linha é ignorado). Cenário: o trecho é
 * preenchido metade com o ponto à esquerda, metade com o da direita; sprites:
 * só os pontos de sprite, opacos, o resto transparente. */
bool gpu_ppu_compose_ip_layers(const uint16_t* runs, int scale);
unsigned int gpu_ppu_ip_texture(bool obj, int* w, int* h);
bool gpu_ppu_read_ip_layer(bool obj, uint32_t* argb);
/* Texto curto sobre o atlas de tiles HD (para o menu). */
const char* gpu_ppu_atlas_info(void);

#endif /* GPU_PPU_H */
