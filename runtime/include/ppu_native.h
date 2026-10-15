/**
 * @file ppu_native.h
 * @brief Renderizador nativo por linha (substitui a geração de pixels da PPU emulada)
 *
 * A PPU emulada continua cuidando de tempo, STAT, interrupções e HDMA. Este
 * módulo só desenha: a cada início de modo 3 ele registra o estado da linha e
 * monta os pixels a partir de um cache de tiles decodificados.
 *
 * Modos:
 *   EMULATED - caminho antigo (referência), este módulo não faz nada.
 *   NATIVE   - a imagem exibida vem deste módulo.
 *   COMPARE  - os dois rodam; a imagem exibida é a nativa e cada linha é
 *              comparada pixel a pixel com a da PPU emulada (160 px centrais).
 *
 * O estado do módulo é global (uma instância) e fica fora do GBPPU de
 * propósito: o GBPPU é copiado inteiro nos savestates.
 */

#ifndef GB_PPU_NATIVE_H
#define GB_PPU_NATIVE_H

#include <stdint.h>
#include <stdbool.h>
#include "ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PPU_RENDERER_EMULATED = 0,
    PPU_RENDERER_NATIVE = 1,
    PPU_RENDERER_COMPARE = 2,
    PPU_RENDERER_COUNT
} PPURendererMode;

/* Estado registrado no início do modo 3 de cada linha visível. É o que um
 * compositor de GPU vai precisar mais tarde para montar o frame inteiro. */
#define PPU_NATIVE_LINE_WINDOW 0x01u

typedef struct {
    uint8_t lcdc;
    uint8_t scx;
    uint8_t scy;
    uint8_t wx;
    uint8_t wy;
    uint8_t bgp;
    uint8_t obp0;
    uint8_t obp1;
    uint8_t window_line;     /* contador interno da window nesta linha */
    uint8_t flags;           /* PPU_NATIVE_LINE_WINDOW */
    uint16_t world_scroll_x; /* câmera do Wario Land (ERAM $A902/$A903) */
    uint16_t world_scroll_y; /* câmera do Wario Land (ERAM $A900/$A901) */
} PPUNativeLineState;

typedef struct {
    uint64_t frames_compared;
    uint64_t frames_with_mismatch;
    uint32_t last_frame_mismatched_pixels;
    /* Primeira diferença do último frame que teve diferença */
    uint64_t first_mismatch_frame;
    int16_t first_mismatch_x;
    int16_t first_mismatch_y;
    uint32_t first_mismatch_expected; /* RGBA da PPU emulada */
    uint32_t first_mismatch_got;      /* RGBA do nativo */
    /* Tiles que mudaram na VRAM sem passar pelos hooks de escrita
     * (detectado por comparação com uma cópia sombra a cada frame). */
    uint64_t unhooked_tile_writes;
} PPUNativeCompareStats;

void ppu_native_set_mode(PPURendererMode mode);
PPURendererMode ppu_native_get_mode(void);
const char* ppu_native_mode_name(PPURendererMode mode);

/* Hooks de VRAM: offset relativo a 0x8000 (0x0000-0x1FFF). */
void ppu_native_on_vram_write(uint8_t bank, uint16_t offset);
void ppu_native_invalidate_all(void);

/* Chamados pela PPU emulada */
void ppu_native_begin_line(GBPPU* ppu, const GBContext* ctx); /* início do modo 3 */
void ppu_native_end_line(GBPPU* ppu, const GBContext* ctx);   /* fim do modo 3 */
void ppu_native_end_frame(GBPPU* ppu, const GBContext* ctx);  /* entrada no VBlank */

const PPUNativeCompareStats* ppu_native_compare_stats(void);
void ppu_native_reset_compare_stats(void);

/* GB_SCREEN_HEIGHT entradas, válidas para o último frame desenhado */
const PPUNativeLineState* ppu_native_line_states(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_PPU_NATIVE_H */