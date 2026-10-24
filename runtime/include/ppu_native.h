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
/* Passo 14c-2: false = a CPU não desenha o HD (centro e laterais) porque a
 * GPU monta a tela; ppu_native_hd_frame/_margin_frame devolvem NULL. Vale a
 * partir do próximo quadro. Padrão true. */
void ppu_native_set_cpu_hd(bool enabled);
/* Passo 3D-4a: pede os tiles do mundo em volta da tela (só com o 3D ligado). */
void ppu_native_set_world_ext(bool enabled);
bool ppu_native_get_cpu_hd(void);
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
    uint8_t pal;          /* passo 11a: número da paleta (CGB 0..7; DMG 0/1) */
    uint16_t context;     /* passo 11a: contexto (fase/mapa) quando foi visto */
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
 * Grava um frame inteiro (160x144, ou 256x144 com widescreen) separado em duas camadas e, para
 * cada pixel, de qual tile e de qual ponto do tile ele veio. É isso que deixa
 * pintar a cena inteira em alta resolução e depois fatiar em tiles.
 *   *_uvf: bits 0-2 = coluna no tile, 3-5 = linha (espaço da tela, antes do
 *          flip), bit 6 = flip X, bit 7 = flip Y.
 * Os tiles da tabela não dependem da paleta (o nome do arquivo HD será
 * b_<tile>.png / o_<tile>.png); a paleta guardada é a da primeira aparição.
 * -------------------------------------------------------------------------- */
#define PPU_NATIVE_SCENE_MAX_TILES 4096
#define PPU_NATIVE_SCENE_NO_TILE 0xFFFFu

/* Passo 5d: com o widescreen ligado a cena tem 256 px de largura (laterais
 * incluídas, sem o escurecimento da borda); sem ele, 160. Os arrays têm
 * width * height entradas válidas, linha a linha. */
#define PPU_NATIVE_SCENE_MAX_W GB_WIDESCREEN_WIDTH

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t bg_rgba[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];   /* só fundo/window */
    uint32_t obj_rgba[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];  /* só sprites, 0 = vazio */
    uint16_t bg_tile[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];   /* índice em tiles[] */
    uint16_t obj_tile[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];
    uint8_t bg_uvf[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];
    uint8_t obj_uvf[PPU_NATIVE_SCENE_MAX_W * GB_SCREEN_HEIGHT];
    uint32_t tile_count;
    uint16_t context;     /* passo 11a: contexto do frame capturado */
    PPUNativeTileDump tiles[PPU_NATIVE_SCENE_MAX_TILES];
} PPUNativeScene;

/* Pede a captura do próximo frame completo. */
void ppu_native_request_scene_capture(void);
bool ppu_native_scene_capture_pending(void);
/* Devolve a cena pronta uma única vez (NULL se não houver). */
const PPUNativeScene* ppu_native_take_scene(void);

/* ----------------------------------------------------------------------------
 * Tiles HD (passo 5b)
 *
 * A plataforma carrega mods/hd/b_<tile>.png e o_<tile>.png e entrega aqui.
 * Com escala N > 1, todo pixel cujo tile tem versão HD é desenhado a partir
 * dela:
 *  - fundo: o PNG só troca a cor; a prioridade vem do tile original. Alfa 0
 *    no PNG = usa a cor original naquele ponto;
 *  - sprite: o alfa do PNG define a forma (>= 128 = opaco) e pode ocupar a
 *    caixa 8x8 inteira de cada parte do sprite;
 *  - com paleta de referência (pack.json), fades e piscadas recolorem o tile
 *    na proporção da paleta atual.
 * -------------------------------------------------------------------------- */
/* size x size pixels (múltiplo de 8), 0xAARRGGBB. ref_palette: 4 cores ou NULL.
 * Copia os dados. Substitui um tile com o mesmo tipo e hash. */
bool ppu_native_hd_add_tile(char kind, uint64_t tile_hash, int size,
                            const uint32_t* argb, const uint32_t* ref_palette);

/* ----------------------------------------------------------------------------
 * Passo 14b: entrada para a GPU montar o centro (160x144) e conferir com a CPU
 *
 * Com a captura ligada, cada linha guarda o que o desenho dela usou
 * (registradores, cores já convertidas, OAM) e o fim do quadro guarda a VRAM
 * inteira e o quadro que a CPU desenhou. A VRAM é a do fim do quadro: se ela
 * foi escrita enquanto as linhas eram desenhadas, o quadro é marcado e não dá
 * para conferir (a GPU veria a VRAM nova em todas as linhas).
 * -------------------------------------------------------------------------- */
/* Passo 14b-2: escritas na VRAM durante as linhas. A GPU parte da VRAM do
 * início do quadro e, para a linha y, aplica as escritas com line_plus1 <= y
 * (line_plus1 = última linha já desenhada + 1; 0 = antes da linha 0). */
#define PPU_NATIVE_GPU_LOG_MAX 8192
#define PPU_NATIVE_GPU_TL_MAX 2048   /* passo 14b-3: trocas de tile HD no meio do quadro */
#define PPU_NATIVE_GPU_MARGIN_W (2 * GB_WIDESCREEN_MARGIN)   /* passo 14b-4 */
/* Passo 3D-4a: o 3D desenha a fase além da tela: EXT_X pontos de cada lado,
 * EXT_Y em cima e embaixo; tiles do mundo pedidos à CPU (com folga). */
#define PPU_NATIVE_GPU_EXT_X 64
#define PPU_NATIVE_GPU_EXT_Y 48
#define PPU_NATIVE_GPU_EXT_TW 56
#define PPU_NATIVE_GPU_EXT_TH 40
typedef struct {
    uint16_t addr;        /* 0x0000-0x3FFF: banco * 0x2000 + (endereço - 0x8000) */
    uint8_t line_plus1;
    uint8_t value;        /* valor escrito */
} PPUNativeGpuWrite;

typedef struct {
    uint8_t vram[0x4000];                               /* 2 bancos de 8 KB, no INÍCIO do quadro (14b-2) */
    PPUNativeGpuWrite log[PPU_NATIVE_GPU_LOG_MAX];       /* escritas durante as linhas, na ordem */
    uint32_t log_count;
    bool log_overflow;                                  /* passou de PPU_NATIVE_GPU_LOG_MAX */
    uint8_t line[GB_SCREEN_HEIGHT][12];                 /* lcdc scx scy wx win_line win opri hud;
                                                         * passo 3D-2: mundo do ponto x = 0 (int16),
                                                         * mundo y da linha (int16) */
    uint32_t pal[GB_SCREEN_HEIGHT][64];                 /* 0xAARRGGBB: 0-31 fundo, 32-63 sprites */
    uint8_t oam[GB_SCREEN_HEIGHT][160];                 /* OAM como cada linha viu */
    uint32_t cpu[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];   /* quadro da CPU (centro) */
    bool cgb;                                           /* a GPU só monta CGB por enquanto */
    bool vram_changed_mid_frame;
    /* Passo 14b-2a: diagnóstico das escritas na VRAM durante as linhas. A
     * linha é a última já desenhada quando a escrita aconteceu (as linhas até
     * ela usaram o valor antigo). */
    uint32_t mid_writes;          /* total */
    uint32_t mid_tile_writes;     /* 0x8000-0x97FF (desenho dos tiles) */
    uint32_t mid_map_writes;      /* 0x9800-0x9FFF (mapas e atributos) */
    uint16_t mid_lines;           /* em quantas linhas diferentes */
    uint8_t mid_first_line, mid_last_line;
    /* Passo 14b-3: tiles HD. hd_slot[tipo * 8 + paleta][banco * 384 + tile] =
     * entrada da tabela de tiles HD + 1 (0 = sem HD) para o desenho do tile no
     * início do quadro (fase+paleta, paleta, fase, genérico). cpu_hd aponta
     * para o centro HD que a CPU desenhou (160*hd_scale x 144*hd_scale), válido
     * até o próximo quadro. */
    uint32_t serial;              /* muda a cada quadro publicado */
    bool hd_on;
    int hd_scale;
    const uint32_t* cpu_hd;
    uint16_t hd_slot[16][768];
    /* Tiles redesenhados no meio do quadro (animações): a partir da linha
     * hd_tl[i][0] - 1 o tile passa a usar hd_tl[i][1 + tipo * 8 + paleta].
     * hd_tl_dir[tile] = (início + 1, quantidade) em hd_tl, na ordem das linhas. */
    uint16_t hd_tl_dir[768][2];
    uint16_t hd_tl[PPU_NATIVE_GPU_TL_MAX][17];
    uint32_t hd_tl_count;
    bool hd_tl_overflow;
    /* Passo 14b-4: laterais do widescreen. margin_on = as 144 linhas vieram
     * de ppu.c neste quadro. Cada ponto m (48 da esquerda e depois 48 da
     * direita) é o PPUNativeMarginPixel compactado em margin[y][2m] e
     * margin[y][2m + 1]:
     *   [2m][0] rgba & 0xFFFFFF | bg_pal << 24 | obj_pal << 27 | fade << 30
     *   [2m][1] bg_rgba & 0xFFFFFF | bg_uvf << 24
     *   [2m][2] obj_rgba & 0xFFFFFF | obj_uvf << 24
     *   [2m][3] bg_raw | obj_raw << 2 | tem_sprite << 4
     *   [2m+1][0] tile HD do fundo (entrada + 1, 0 = sem HD)
     *   [2m+1][1] tile HD do sprite (idem)
     * O tile HD é o que a CPU usou na mesma hora (hd_lookup quando ppu.c
     * manda a linha), então não depende de quando a VRAM foi escrita.
     * fade_lut[coluna][v] = (uint8_t)(v * keep), como a CPU escurece a borda;
     * fade_conflict = a mesma coluna veio com dois keep no quadro (não
     * acontece hoje). cpu_hd_margin = laterais HD da CPU ou NULL. */
    /* Passo 3D-2: layout de blocos da fase (WRAM C000-DFFF, 256 x 32 blocos de
     * 16x16), quando a fase está na tela (level_layout). */
    bool level_layout;
    uint8_t layout[0x2000];
    /* Passo 3D-4a: tiles do mundo em volta da tela (ppu_world_tiles), para o 3D
     * mostrar a fase além da tela; ext_tx0/ty0 = tile do mundo do canto. */
    bool ext_on;
    int16_t ext_tx0, ext_ty0;
    uint16_t ext[PPU_NATIVE_GPU_EXT_TW * PPU_NATIVE_GPU_EXT_TH];
    bool margin_on;
    uint32_t margin[144][PPU_NATIVE_GPU_MARGIN_W * 2][4];
    uint8_t fade_lut[PPU_NATIVE_GPU_MARGIN_W][256];
    bool fade_conflict;
    const uint32_t* cpu_hd_margin;
} PPUNativeGpuFrame;

/* Passo 14b-3: tabela de tiles HD carregados, para a GPU montar o atlas. */
uint32_t ppu_native_hd_pack_generation(void);   /* muda quando entra ou sai tile HD */
/* Passo 14b-4: última linha desenhada no quadro atual (-1 = nenhuma); para testes. */
int ppu_native_gpu_last_line(void);
uint32_t ppu_native_hd_table_capacity(void);
/* Entrada i da tabela: false se vazia. ref = NULL sem paleta de referência. */
bool ppu_native_hd_table_entry(uint32_t i, int* size, const uint32_t** px, const uint32_t** ref);

void ppu_native_gpu_capture(bool enabled);
bool ppu_native_gpu_capture_enabled(void);
/* Devolve o quadro pronto uma vez (NULL se não houver). */
const PPUNativeGpuFrame* ppu_native_gpu_take_frame(void);

/* Passo 11a: tiles HD por contexto e paleta.
 *
 * O mesmo desenho de tile é reaproveitado em fases diferentes e com paletas
 * diferentes. Um tile HD pode valer só para uma fase (ou tela do mapa) e/ou
 * só para um número de paleta:
 *   b_<hash>.png             todo lugar
 *   b_<hash>_p3.png          paleta 3, qualquer fase
 *   b_<hash>_f10.png         fase $10, qualquer paleta
 *   b_<hash>_f10_p3.png      fase $10 com paleta 3
 *   (_m44 no lugar de _f10 = tela do mapa da página DX $44; _g00 = outra
 *    tela pelo modo do jogo, passo 11b)
 * Ao desenhar, vale o mais específico: fase+paleta, paleta, fase, genérico.
 *
 * Contexto (passo 11b): PPU_NATIVE_CTX_LEVEL | sLevelId ($A804) nos modos
 * ($A8C3) em que a fase está na tela: 2, 3, 4, 5, 6, 7, $0A e $0C;
 * PPU_NATIVE_CTX_MAP | (0x41 + $A800) no modo 1 (mapa); nos outros,
 * PPU_NATIVE_CTX_MODE | modo (_gXX: título/slots, tesouro, game over, final).
 * 0 = nenhum (sem RAM do cartucho). */
#define PPU_NATIVE_HD_ANY_PAL 0xFFu
#define PPU_NATIVE_CTX_LEVEL 0x0100u
#define PPU_NATIVE_CTX_MAP   0x0200u
#define PPU_NATIVE_CTX_MODE  0x0400u
bool ppu_native_hd_add_tile_ex(char kind, uint64_t tile_hash, uint16_t context, uint8_t pal,
                               int size, const uint32_t* argb, const uint32_t* ref_palette);
/* Contexto do último frame. */
uint16_t ppu_native_hd_context(void);
/* "f10", "m44", "g00" ou "" (sem contexto). out com pelo menos 8 bytes. */
void ppu_native_hd_context_suffix(uint16_t context, char* out);
/* Quantos tiles HD carregados são específicos (com _f/_m/_p). */
uint32_t ppu_native_hd_variant_count(void);
void ppu_native_hd_clear_tiles(void);
uint32_t ppu_native_hd_tile_count(void);
void ppu_native_hd_set_tiles_enabled(bool enabled);
bool ppu_native_hd_get_tiles_enabled(void);

/* ----------------------------------------------------------------------------
 * Laterais do widescreen em HD (passo 5d)
 *
 * As laterais (48 px de cada lado) continuam sendo calculadas por ppu.c, com
 * a lógica das paredes de sala e dos atributos aprendidos. Para cada pixel
 * de lateral, ppu.c informa aqui de onde ele veio (tile, ponto no tile,
 * espelhamento) e as cores que usou; o ppu_native desenha a versão HD com os
 * mesmos tiles e a mesma recoloração do centro e grava as laterais na
 * captura de cena.
 *
 * Nas laterais o sprite sempre fica na frente do fundo (é assim em ppu.c).
 * -------------------------------------------------------------------------- */
#define PPU_NATIVE_MARGIN_PIXELS (2 * GB_WIDESCREEN_MARGIN)  /* 48 esq. + 48 dir. */

typedef struct {
    uint32_t rgba;      /* cor final 1x, a mesma que foi para o framebuffer */
    uint32_t bg_rgba;   /* cor do fundo sem o escurecimento da borda (preto nas
                         * áreas pretas das paredes de sala) */
    uint32_t obj_rgba;  /* cor do índice obj_raw do sprite (vencedor ou caixa) */
    double keep;        /* escurecimento da borda: canal * keep (se fade) */
    uint16_t bg_tile;   /* 1 + banco * 384 + tile; 0 = sem tile (preto, LCD off) */
    uint16_t obj_tile;  /* sprite com cor neste pixel (obj_raw != 0) ou, se não
                         * houver, sprite cuja caixa 8x8 cobre o pixel; 0 = nenhum */
    uint8_t bg_uvf;     /* bits 0-2 coluna, 3-5 linha, 0x40 flip X, 0x80 flip Y */
    uint8_t bg_raw;
    uint8_t bg_pal;
    uint8_t obj_uvf;
    uint8_t obj_raw;    /* 0 quando obj_tile veio da caixa */
    uint8_t obj_pal;
    bool fade;
    uint8_t bg_block;   /* passo 3D-2: bloco da fase + 1 (0 = fora do layout) */
    uint8_t bg_block_down;   /* passo 3D-2b: bloco logo abaixo + 1 */
} PPUNativeMarginPixel;

/* ppu.c pergunta antes de montar os registros (só gasta tempo quando há HD,
 * conferência ou captura). */
bool ppu_native_margins_wanted(void);
/* Uma linha (ppu->ly) de laterais: PPU_NATIVE_MARGIN_PIXELS registros, os 48
 * da esquerda e depois os 48 da direita. Chamado no fim do modo 3. */
void ppu_native_margin_line(const GBPPU* ppu, const GBContext* ctx,
                            const PPUNativeMarginPixel* px);
/* Laterais HD do último frame completo: (96*N) x (144*N), cada linha com os
 * 48*N pixels da esquerda e depois os 48*N da direita. NULL se não houver. */
const uint32_t* ppu_native_hd_margin_frame(int* out_scale);

#ifdef __cplusplus
}
#endif

#endif /* GB_PPU_NATIVE_H */
