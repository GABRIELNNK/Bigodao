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
/* Muda a cada escrita no mapa de tiles (0x9800-0x9FFF). */
uint32_t ppu_native_vram_map_generation(void);

/* Chamados pela PPU emulada */
void ppu_native_begin_line(GBPPU* ppu, const GBContext* ctx); /* início do modo 3 */
void ppu_native_end_line(GBPPU* ppu, const GBContext* ctx);   /* fim do modo 3 */
void ppu_native_end_frame(GBPPU* ppu, const GBContext* ctx);  /* entrada no VBlank */

const PPUNativeCompareStats* ppu_native_compare_stats(void);
void ppu_native_reset_compare_stats(void);

/* GB_SCREEN_HEIGHT entradas, válidas para o último frame desenhado */
const PPUNativeLineState* ppu_native_line_states(void);

/* ----------------------------------------------------------------------------
 * Frame em alta resolução (passo 4a)
 *
 * Com escala N > 1, cada linha também é desenhada em N linhas de 160*N pixels
 * (só o centro de 160 px; as margens do widescreen não entram aqui).
 * Sem tiles HD (passo 5) cada pixel vira um bloco N x N da mesma cor.
 * A "conferência" força o caminho que amostra cada subpixel dentro do tile
 * (o mesmo que os PNGs vão usar) e compara com o bloco da cor original.
 * -------------------------------------------------------------------------- */
#define PPU_NATIVE_HD_MAX_SCALE 4

typedef struct {
    uint64_t frames_checked;
    uint64_t frames_with_mismatch;
    uint64_t pixels_mismatched;   /* subpixels diferentes, acumulado */
    int16_t last_x;               /* coordenada HD do primeiro subpixel diferente */
    int16_t last_y;
} PPUNativeHdStats;

/* 1 desliga. Vale a partir do próximo frame. */
void ppu_native_set_hd_scale(int scale);
int ppu_native_get_hd_scale(void);
/* Último frame HD completo (160*N x 144*N, RGBA) ou NULL. */
const uint32_t* ppu_native_hd_frame(int* out_scale);

void ppu_native_set_hd_check(bool enabled);
bool ppu_native_get_hd_check(void);
const PPUNativeHdStats* ppu_native_hd_stats(void);
void ppu_native_reset_hd_stats(void);

/* ----------------------------------------------------------------------------
 * Despejo de tiles (passo 5a)
 *
 * Com o despejo ligado, cada tile novo que aparece na tela entra numa fila
 * (uma vez por tile, qualquer paleta; desde o passo 5a.2). A plataforma
 * retira da fila e grava como PNG 8x8:
 *   b_<hash do tile>.png  (fundo/window, opaco)
 *   o_<hash do tile>.png  (sprite, cor 0 transparente)
 * Esses mesmos nomes serão os dos PNGs HD na pasta de mods (passo 5b).
 * -------------------------------------------------------------------------- */
typedef struct {
    char kind;            /* 'b' fundo/window, 'o' sprite */
    uint64_t tile_hash;   /* FNV-1a 64 dos 16 bytes do tile */
    uint32_t pal_hash;    /* FNV-1a 32 das 4 cores RGB555 */
    uint8_t index[64];    /* índice de cor (0..3) de cada pixel, linha a linha */
    uint32_t rgba[4];     /* cor de cada índice, 0xAARRGGBB */
} PPUNativeTileDump;

void ppu_native_set_tile_dump(bool enabled);
bool ppu_native_get_tile_dump(void);
/* Retira até max itens da fila; devolve quantos retirou. */
int ppu_native_take_tile_dumps(PPUNativeTileDump* out, int max);
/* Quantas combinações tile + paleta já foram vistas desde o último reset. */
uint32_t ppu_native_tile_dump_seen(void);
void ppu_native_reset_tile_dump(void);

/* ----------------------------------------------------------------------------
 * Captura de cena (passo 5a.2)
 *
 * Grava um frame inteiro (centro de 160x144) separado em duas camadas e, para
 * cada pixel, de qual tile e de qual ponto do tile ele veio. É isso que deixa
 * pintar a cena inteira em alta resolução e depois fatiar em tiles.
 *   *_uvf: bits 0-2 = coluna no tile, 3-5 = linha (espaço da tela, antes do
 *          flip), bit 6 = flip X, bit 7 = flip Y.
 * Os tiles da tabela não dependem da paleta (o nome do arquivo HD será
 * b_<tile>.png / o_<tile>.png); a paleta guardada é a da primeira aparição.
 * -------------------------------------------------------------------------- */
#define PPU_NATIVE_SCENE_MAX_TILES 4096
#define PPU_NATIVE_SCENE_NO_TILE 0xFFFFu

typedef struct {
    uint32_t bg_rgba[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];   /* só fundo/window */
    uint32_t obj_rgba[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];  /* só sprites, 0 = vazio */
    uint16_t bg_tile[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];   /* índice em tiles[] */
    uint16_t obj_tile[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    uint8_t bg_uvf[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    uint8_t obj_uvf[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    uint32_t tile_count;
    PPUNativeTileDump tiles[PPU_NATIVE_SCENE_MAX_TILES];
} PPUNativeScene;

/* Pede a captura do próximo frame completo. */
void ppu_native_request_scene_capture(void);
bool ppu_native_scene_capture_pending(void);
/* Devolve a cena pronta uma única vez (NULL se não houver). */
const PPUNativeScene* ppu_native_take_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_PPU_NATIVE_H */
