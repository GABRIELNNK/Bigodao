/**
 * @file ppu_native.c
 * @brief Renderizador nativo por linha. Veja ppu_native.h.
 *
 * Desenha só os 160 px reais (e, com escala N > 1, o mesmo centro em N x N:
 * passo 4a). As margens do widescreen continuam vindo de
 * ppu_render_widescreen_scanline() em ppu.c (que tem a lógica de paredes de
 * sala, tabela de atributos do DX e atributos aprendidos); no VBlank este
 * módulo sobrescreve apenas o centro do framebuffer widescreen.
 *
 * Referência de comportamento: ppu_render_dot()/ppu_fetch_*_dot() em ppu.c.
 * As regras de prioridade, paleta e window foram copiadas de lá para que o
 * modo COMPARE possa exigir igualdade exata.
 */

#include "ppu_native.h"
#include "gbrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_W GB_SCREEN_WIDTH
#define NATIVE_H GB_SCREEN_HEIGHT
#define NATIVE_TILES 384u
#define TILE_DATA_BYTES 0x1800u /* 0x8000-0x97FF */
#define MAP0_OFFSET 0x1800u     /* 0x9800 */
#define MAP1_OFFSET 0x1C00u     /* 0x9C00 */
#define BANK1_OFFSET 0x2000u

/* ============================================================================
 * Estado do módulo
 * ========================================================================== */

static PPURendererMode g_mode = PPU_RENDERER_NATIVE; /* padrao desde o passo 2c */

/* Cache de tiles: índice de cor de 2 bits por pixel, linha a linha, x=0 é o
 * pixel mais à esquerda (bit 7). A paleta é aplicada só na composição. */
static uint8_t g_tile_px[2][NATIVE_TILES][64];
static bool g_tile_dirty[2][NATIVE_TILES];
/* Cópia dos bytes de cada tile no momento em que foi decodificado; serve para
 * detectar escritas na VRAM que não passaram pelos hooks. */
static uint8_t g_tile_shadow[2][TILE_DATA_BYTES];
static bool g_cache_ready = false;

static uint32_t g_frame[NATIVE_W * NATIVE_H];
static PPUNativeLineState g_lines[NATIVE_H];
static bool g_frame_open = false;

static uint32_t g_rgb_lut[0x8000];
static bool g_rgb_lut_ready = false;

static PPUNativeCompareStats g_stats;
static uint32_t g_cur_mismatch = 0;
static int16_t g_cur_first_x = -1;
static int16_t g_cur_first_y = -1;
static uint32_t g_cur_first_expected = 0;
static uint32_t g_cur_first_got = 0;

static const uint16_t k_dmg_rgb555[4] = {0x67DC, 0x32EE, 0x2A66, 0x0841};

/* Frame HD (passo 4a): só o centro, 160*N x 144*N. */
#define HD_MAX PPU_NATIVE_HD_MAX_SCALE
static uint32_t g_hd[(NATIVE_W * HD_MAX) * (NATIVE_H * HD_MAX)];
static int g_hd_scale_req = 1;     /* pedida pela plataforma */
static int g_hd_scale = 1;         /* usada no frame em andamento */
static int g_hd_valid_scale = 0;   /* escala do último frame completo (0 = nenhum) */
static bool g_hd_frame_full = false;
static bool g_hd_check = false;
static PPUNativeHdStats g_hd_stats;
static uint32_t g_hd_cur_mismatch = 0;
static int16_t g_hd_cur_x = -1;
static int16_t g_hd_cur_y = -1;

/* Passo 5a: identidade dos tiles e despejo (dump) para PNG.
 * O hash do tile vem dos 16 bytes dele na VRAM (igual em qualquer banco ou
 * posição); o hash da paleta vem das 4 cores RGB555 resolvidas na linha. */
static uint64_t g_tile_hash[2][NATIVE_TILES];
static bool g_dump_enabled = false;
#define DUMP_SEEN_CAP 65536u            /* potência de 2 */
#define DUMP_QUEUE_CAP 512u
static uint64_t g_dump_seen[DUMP_SEEN_CAP]; /* 0 = vazio */
static uint32_t g_dump_seen_count = 0;
static PPUNativeTileDump g_dump_queue[DUMP_QUEUE_CAP];
static uint32_t g_dump_head = 0;         /* próximo a retirar */
static uint32_t g_dump_size = 0;

/* Passo 5a.2: captura de cena (fundo, sprites e origem de cada pixel). */
enum { SCENE_IDLE = 0, SCENE_REQUESTED, SCENE_CAPTURING, SCENE_READY };
static int g_scene_state = SCENE_IDLE;
static PPUNativeScene g_scene;
#define SCENE_MAP_CAP 8192u   /* potência de 2, > PPU_NATIVE_SCENE_MAX_TILES */
static uint64_t g_scene_map_key[SCENE_MAP_CAP];   /* 0 = vazio */
static uint16_t g_scene_map_val[SCENE_MAP_CAP];

/* Passo 5b: tiles HD carregados da pasta de mods. */
typedef struct {
    uint64_t key;        /* dump_key(tipo, hash do tile, 0); 0 = vazio */
    uint32_t* px;        /* size x size, 0xAARRGGBB */
    uint16_t size;
    bool has_ref;
    uint32_t ref[4];     /* paleta em que o tile foi pintado (pack.json) */
} HdTile;
#define HD_TABLE_CAP 16384u   /* potência de 2 */
static HdTile g_hd_tiles[HD_TABLE_CAP];
static uint32_t g_hd_tile_count = 0;
static bool g_hd_tiles_enabled = true;
static uint32_t g_hd_generation = 1;               /* muda quando o pacote muda */
/* Cache por tile da VRAM: [tipo 0=b 1=o][banco*384 + tile] */
static const HdTile* g_hd_for[2][2 * NATIVE_TILES];

/* Passo 5d: laterais do widescreen em HD e na captura */
#define MARGIN_W PPU_NATIVE_MARGIN_PIXELS
static uint32_t g_hd_m[(MARGIN_W * HD_MAX) * (NATIVE_H * HD_MAX)];
static uint32_t g_margin_lines = 0;      /* linhas de lateral HD neste frame */
static int g_hd_m_valid_scale = 0;       /* escala das laterais do último frame completo */
static unsigned g_scene_w = NATIVE_W;    /* largura da cena em captura */
static unsigned g_scene_off = 0;         /* coluna da cena onde começa o centro */
static uint32_t g_hd_for_gen[2][2 * NATIVE_TILES];

/* ============================================================================
 * Utilitários
 * ========================================================================== */

static void ensure_rgb_lut(void) {
    if (g_rgb_lut_ready) {
        return;
    }
    /* Mesma conversão de rgb555_to_rgba() em ppu.c. */
    for (uint32_t c = 0; c < 0x8000u; ++c) {
        const uint32_t r = ((c >> 0) & 0x1Fu) * 255u / 31u;
        const uint32_t g = ((c >> 5) & 0x1Fu) * 255u / 31u;
        const uint32_t b = ((c >> 10) & 0x1Fu) * 255u / 31u;
        g_rgb_lut[c] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    g_rgb_lut_ready = true;
}

static inline uint32_t rgb(uint16_t c) {
    return g_rgb_lut[c & 0x7FFFu];
}

static inline bool is_cgb(const GBContext* ctx) {
    return ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
}

static inline bool is_compat(const GBContext* ctx) {
    return ctx->config.model == GB_MODEL_CGB && ctx->config.cgb_compatibility_mode;
}

static inline uint16_t palette_word(const uint8_t* ram, uint8_t palette, uint8_t index) {
    const size_t i = (size_t)(palette & 7u) * 8u + (size_t)(index & 3u) * 2u;
    return (uint16_t)(ram[i] | (ram[i + 1] << 8));
}

static inline uint8_t dmg_shade(uint8_t raw, uint8_t reg) {
    return (uint8_t)((reg >> (raw * 2u)) & 3u);
}

static uint32_t bg_color(const GBPPU* ppu, bool cgb, bool compat,
                         uint8_t palette, uint8_t raw, uint8_t bgp) {
    if (cgb) {
        return rgb(palette_word(ppu->bg_palette_ram, palette, raw));
    }
    if (compat) {
        return rgb(palette_word(ppu->bg_palette_ram, 0, dmg_shade(raw, bgp)));
    }
    return rgb(k_dmg_rgb555[dmg_shade(raw, bgp)]);
}

static uint32_t obj_color(const GBPPU* ppu, bool cgb, bool compat,
                          uint8_t palette, uint8_t raw, uint8_t obp) {
    if (cgb) {
        return rgb(palette_word(ppu->obj_palette_ram, palette, raw));
    }
    if (compat) {
        return rgb(palette_word(ppu->obj_palette_ram, palette, dmg_shade(raw, obp)));
    }
    return rgb(k_dmg_rgb555[dmg_shade(raw, obp)]);
}

/* ============================================================================
 * Cache de tiles
 * ========================================================================== */

static void decode_tile(const GBContext* ctx, unsigned bank, unsigned tile) {
    const uint8_t* src = ctx->vram + bank * BANK1_OFFSET + tile * 16u;
    uint8_t* dst = g_tile_px[bank][tile];
    for (unsigned y = 0; y < 8; ++y) {
        const uint8_t lo = src[y * 2u];
        const uint8_t hi = src[y * 2u + 1u];
        for (unsigned x = 0; x < 8; ++x) {
            const unsigned bit = 7u - x;
            dst[y * 8u + x] = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
        }
    }
    memcpy(&g_tile_shadow[bank][tile * 16u], src, 16);
    {
        uint64_t h = 1469598103934665603ull;          /* FNV-1a 64 */
        for (unsigned i = 0; i < 16u; ++i) {
            h ^= src[i];
            h *= 1099511628211ull;
        }
        g_tile_hash[bank][tile] = h;
    }
    g_hd_for_gen[0][bank * NATIVE_TILES + tile] = 0;
    g_hd_for_gen[1][bank * NATIVE_TILES + tile] = 0;
    g_tile_dirty[bank][tile] = false;
}

static inline const uint8_t* tile_pixels(const GBContext* ctx, unsigned bank, unsigned tile) {
    if (g_tile_dirty[bank][tile]) {
        decode_tile(ctx, bank, tile);
    }
    return g_tile_px[bank][tile];
}

/* Índice no cache para tiles de BG/window (LCDC bit 4). */
static inline unsigned bg_tile_index(uint8_t lcdc, uint8_t number) {
    if (lcdc & LCDC_TILE_DATA) {
        return number;                         /* 0x8000 + n*16 */
    }
    return (unsigned)(256 + (int8_t)number);   /* 0x9000 + (int8)n*16 */
}

/* Rede de segurança: acha tiles alterados por caminhos sem hook. */
static void verify_shadow(const GBContext* ctx) {
    for (unsigned bank = 0; bank < 2; ++bank) {
        const uint8_t* vram = ctx->vram + bank * BANK1_OFFSET;
        for (unsigned tile = 0; tile < NATIVE_TILES; ++tile) {
            if (g_tile_dirty[bank][tile]) {
                continue;
            }
            if (memcmp(&g_tile_shadow[bank][tile * 16u], vram + tile * 16u, 16) != 0) {
                g_tile_dirty[bank][tile] = true;
                g_stats.unhooked_tile_writes++;
            }
        }
    }
}

/* Passo 3b: conta escritas no mapa de tiles (0x9800-0x9FFF, dois bancos). */
static uint32_t g_vram_map_generation = 0;

uint32_t ppu_native_vram_map_generation(void) {
    return g_vram_map_generation;
}

void ppu_native_on_vram_write(uint8_t bank, uint16_t offset) {
    if (offset < TILE_DATA_BYTES) {
        g_tile_dirty[bank & 1u][offset >> 4] = true;
    } else {
        g_vram_map_generation++;
    }
}

void ppu_native_invalidate_all(void) {
    memset(g_tile_dirty, 1, sizeof(g_tile_dirty));
    g_vram_map_generation++;
    g_cache_ready = true;
}

/* ============================================================================
 * Amostragem de BG / window
 * ========================================================================== */

typedef struct {
    uint8_t raw;
    uint8_t palette;
    bool priority;
    /* Origem do pixel, para a amostragem em alta resolução */
    const uint8_t* tile;  /* 64 índices de cor do tile (cache) */
    uint8_t u;            /* coluna dentro do tile, no espaço da tela (antes do flip) */
    uint8_t v;            /* linha dentro do tile, no espaço da tela (antes do flip) */
    uint8_t flags;        /* OAM_FLIP_X / OAM_FLIP_Y */
} BgPixel;

static BgPixel sample_map(const GBContext* ctx, bool cgb, uint8_t lcdc,
                          uint16_t map_offset, int source_x, int source_y) {
    BgPixel p = {0, 0, false, NULL, 0, 0, 0};
    const unsigned sx = (unsigned)source_x;
    const unsigned sy = (unsigned)source_y;
    const uint16_t entry = (uint16_t)(map_offset + ((sy >> 3) & 31u) * 32u + ((sx >> 3) & 31u));
    const uint8_t number = ctx->vram[entry];
    const uint8_t attr = cgb ? ctx->vram[BANK1_OFFSET + entry] : 0u;
    unsigned px = sx & 7u;
    unsigned py = sy & 7u;
    p.u = (uint8_t)px;
    p.v = (uint8_t)py;
    p.flags = attr & (OAM_FLIP_X | OAM_FLIP_Y);
    if (attr & OAM_FLIP_X) px = 7u - px;
    if (attr & OAM_FLIP_Y) py = 7u - py;
    p.palette = attr & OAM_CGB_PALETTE;
    p.priority = (attr & OAM_PRIORITY) != 0;
    p.tile = tile_pixels(ctx, (attr & OAM_CGB_BANK) ? 1u : 0u, bg_tile_index(lcdc, number));
    p.raw = p.tile[py * 8u + px];
    return p;
}

/* Amostra um subpixel (cs, rs) no espaço HD da tela, com escala n, de um tile
 * cujo pixel de tela está em (u, v). Com tiles 8x8 isso devolve o mesmo
 * índice de cor do pixel original; com PNG (passo 5) vai amostrar a imagem. */
static inline uint8_t sample_tile_sub(const uint8_t* tile, unsigned u, unsigned v,
                                      uint8_t flags, unsigned sx, unsigned sy,
                                      unsigned n) {
    const unsigned span = 8u * n;
    unsigned cs = u * n + sx;
    unsigned rs = v * n + sy;
    if (flags & OAM_FLIP_X) cs = span - 1u - cs;
    if (flags & OAM_FLIP_Y) rs = span - 1u - rs;
    return tile[(rs / n) * 8u + (cs / n)];
}

/* ============================================================================
 * Sprites da linha
 * ========================================================================== */

typedef struct {
    uint8_t raw[NATIVE_W];
    uint8_t palette[NATIVE_W];
    uint8_t behind[NATIVE_W];
    int16_t screen_x[NATIVE_W];
    uint8_t oam_index[NATIVE_W];
    /* Origem do pixel vencedor, para a amostragem em alta resolução */
    const uint8_t* tile[NATIVE_W];
    uint8_t u[NATIVE_W];      /* coluna no sprite, espaço da tela */
    uint8_t v[NATIVE_W];      /* linha no tile de 8, espaço da tela */
    uint8_t flags[NATIVE_W];  /* OAM_FLIP_X / OAM_FLIP_Y */
    /* Passo 5b: sprite cuja caixa 8x8 cobre o pixel onde nenhum sprite tem
     * cor (o desenho HD pode ocupar essas áreas). */
    const uint8_t* box_tile[NATIVE_W];
    uint8_t box_u[NATIVE_W];
    uint8_t box_v[NATIVE_W];
    uint8_t box_flags[NATIVE_W];
    uint8_t box_palette[NATIVE_W];
    uint8_t box_behind[NATIVE_W];
} ObjLine;

/* Rasteriza todos os sprites da linha num buffer, com a mesma regra de
 * prioridade entre sprites de ppu_fetch_object_dot(). Sem limite de 10 por
 * linha, igual ao caminho emulado atual. */
static void rasterize_objects(const GBPPU* ppu, const GBContext* ctx, bool cgb,
                              const PPUNativeLineState* st, int y, ObjLine* out) {
    memset(out->raw, 0, sizeof(out->raw));
    memset(out->box_tile, 0, sizeof(out->box_tile));
    if (!(st->lcdc & LCDC_OBJ_ENABLE)) {
        return;
    }
    const int height = (st->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    const bool dmg_priority = !cgb || ppu->opri != 0;

    for (int index = 0; index < 40; ++index) {
        const uint8_t* oam = ctx->oam + index * 4;
        const int sprite_y = (int)oam[0] - 16;
        if (y < sprite_y || y >= sprite_y + height) {
            continue;
        }
        const int sprite_x = (int)oam[1] - 8;
        if (sprite_x <= -8 || sprite_x >= GB_SCREEN_WIDTH) {
            continue;
        }
        const uint8_t flags = oam[3];

        int line = y - sprite_y;
        if (flags & OAM_FLIP_Y) {
            line = height - 1 - line;
        }
        const unsigned tile = (height == 16)
            ? (unsigned)(oam[2] & 0xFEu) + (unsigned)(line >> 3)
            : oam[2];
        const unsigned bank = (cgb && (flags & OAM_CGB_BANK)) ? 1u : 0u;
        const uint8_t* tile_px = tile_pixels(ctx, bank, tile);
        const uint8_t* row = tile_px + (line & 7) * 8;
        const uint8_t screen_v = (uint8_t)((y - sprite_y) & 7);
        const uint8_t palette = cgb ? (flags & OAM_CGB_PALETTE) : ((flags & OAM_PALETTE) ? 1u : 0u);

        for (int p = 0; p < 8; ++p) {
            const int x = sprite_x + p;
            if (x < 0 || x >= GB_SCREEN_WIDTH) {
                continue;
            }
            const uint8_t raw = row[(flags & OAM_FLIP_X) ? 7 - p : p];
            if (raw == 0) {
                if (!out->box_tile[x]) {
                    out->box_tile[x] = tile_px;
                    out->box_u[x] = (uint8_t)p;
                    out->box_v[x] = screen_v;
                    out->box_flags[x] = flags & (OAM_FLIP_X | OAM_FLIP_Y);
                    out->box_palette[x] = palette;
                    out->box_behind[x] = (flags & OAM_PRIORITY) ? 1u : 0u;
                }
                continue;
            }
            if (out->raw[x]) {
                const bool wins = dmg_priority
                    ? (sprite_x < out->screen_x[x] ||
                       (sprite_x == out->screen_x[x] && index < out->oam_index[x]))
                    : (index < out->oam_index[x]);
                if (!wins) {
                    continue;
                }
            }
            out->raw[x] = raw;
            out->palette[x] = palette;
            out->behind[x] = (flags & OAM_PRIORITY) ? 1u : 0u;
            out->screen_x[x] = (int16_t)sprite_x;
            out->oam_index[x] = (uint8_t)index;
            out->tile[x] = tile_px;
            out->u[x] = (uint8_t)p;
            out->v[x] = screen_v;
            out->flags[x] = flags & (OAM_FLIP_X | OAM_FLIP_Y);
        }
    }
}

/* ============================================================================
 * Linha completa (regras de ppu_render_dot)
 * ========================================================================== */

/* ============================================================================
 * Despejo de tiles (passo 5a)
 * ========================================================================== */

static inline unsigned tile_global_index(const uint8_t* tile_px) {
    return (unsigned)((tile_px - &g_tile_px[0][0][0]) / 64);   /* 0..767 */
}

static uint32_t palette_hash(const uint16_t colors[4]) {
    uint32_t h = 2166136261u;                         /* FNV-1a 32 */
    for (unsigned i = 0; i < 4u; ++i) {
        h ^= (uint32_t)(colors[i] & 0xFFu);
        h *= 16777619u;
        h ^= (uint32_t)(colors[i] >> 8);
        h *= 16777619u;
    }
    return h;
}

static inline uint64_t dump_key(char kind, uint64_t tile_hash, uint32_t pal_hash) {
    uint64_t k = tile_hash ^ ((uint64_t)pal_hash * 0x9E3779B97F4A7C15ull) ^
                 (kind == 'o' ? 0xD1B54A32D192ED03ull : 0u);
    return k ? k : 1u;
}

/* Marca como visto; devolve true se for novo. */
static bool dump_mark_seen(uint64_t key) {
    if (g_dump_seen_count >= DUMP_SEEN_CAP / 2u) {
        return false;   /* tabela cheia: para de despejar */
    }
    uint32_t i = (uint32_t)(key ^ (key >> 29)) & (DUMP_SEEN_CAP - 1u);
    while (g_dump_seen[i] != 0) {
        if (g_dump_seen[i] == key) {
            return false;
        }
        i = (i + 1u) & (DUMP_SEEN_CAP - 1u);
    }
    g_dump_seen[i] = key;
    g_dump_seen_count++;
    return true;
}

static void dump_tile(char kind, const uint8_t* tile_px, const uint16_t colors[4],
                      uint32_t pal_hash) {
    const unsigned gi = tile_global_index(tile_px);
    const uint64_t th = g_tile_hash[gi / NATIVE_TILES][gi % NATIVE_TILES];
    if (g_dump_size >= DUMP_QUEUE_CAP) {
        return;         /* fila cheia: tenta de novo num próximo frame */
    }
    if (!dump_mark_seen(dump_key(kind, th, 0u))) {
        return;
    }
    PPUNativeTileDump* d = &g_dump_queue[(g_dump_head + g_dump_size) % DUMP_QUEUE_CAP];
    g_dump_size++;
    d->kind = kind;
    d->tile_hash = th;
    d->pal_hash = pal_hash;
    memcpy(d->index, tile_px, 64);
    for (unsigned c = 0; c < 4u; ++c) {
        d->rgba[c] = rgb(colors[c]);
    }
    if (kind == 'o') {
        d->rgba[0] &= 0x00FFFFFFu;  /* cor 0 do sprite é transparente */
    }
}

/* Cores RGB555 de uma paleta, como a linha as resolve. */
static void line_palette_colors(const GBPPU* ppu, bool cgb, bool compat, bool obj,
                                uint8_t palette, uint8_t dmg_reg, uint16_t out[4]) {
    for (uint8_t raw = 0; raw < 4u; ++raw) {
        if (cgb) {
            out[raw] = palette_word(obj ? ppu->obj_palette_ram : ppu->bg_palette_ram, palette, raw);
        } else if (compat) {
            out[raw] = palette_word(obj ? ppu->obj_palette_ram : ppu->bg_palette_ram,
                                    obj ? palette : 0u, dmg_shade(raw, dmg_reg));
        } else {
            out[raw] = k_dmg_rgb555[dmg_shade(raw, dmg_reg)];
        }
    }
}

/* Índice do tile na tabela da cena (mesmo tile = mesma entrada, qualquer
 * paleta; a paleta guardada é a da primeira vez que ele apareceu). */
static uint16_t scene_tile_id(char kind, const uint8_t* tile_px, const uint16_t colors[4]) {
    const unsigned gi = tile_global_index(tile_px);
    const uint64_t th = g_tile_hash[gi / NATIVE_TILES][gi % NATIVE_TILES];
    const uint64_t key = dump_key(kind, th, 0u);
    uint32_t i = (uint32_t)(key ^ (key >> 31)) & (SCENE_MAP_CAP - 1u);
    while (g_scene_map_key[i] != 0) {
        if (g_scene_map_key[i] == key) {
            return g_scene_map_val[i];
        }
        i = (i + 1u) & (SCENE_MAP_CAP - 1u);
    }
    if (g_scene.tile_count >= PPU_NATIVE_SCENE_MAX_TILES) {
        return PPU_NATIVE_SCENE_NO_TILE;
    }
    const uint16_t id = (uint16_t)g_scene.tile_count++;
    g_scene_map_key[i] = key;
    g_scene_map_val[i] = id;
    PPUNativeTileDump* t = &g_scene.tiles[id];
    t->kind = kind;
    t->tile_hash = th;
    t->pal_hash = palette_hash(colors);
    memcpy(t->index, tile_px, 64);
    for (unsigned c = 0; c < 4u; ++c) {
        t->rgba[c] = rgb(colors[c]);
    }
    if (kind == 'o') {
        t->rgba[0] &= 0x00FFFFFFu;
    }
    return id;
}

static inline uint8_t scene_uvf(uint8_t u, uint8_t v, uint8_t flags) {
    return (uint8_t)((u & 7u) | ((v & 7u) << 3) |
                     ((flags & OAM_FLIP_X) ? 0x40u : 0u) |
                     ((flags & OAM_FLIP_Y) ? 0x80u : 0u));
}

/* Na captura, pixels transparentes dentro da caixa 8x8 de um sprite também
 * ganham a origem (tile e posição), para o desenho HD poder ocupar áreas que
 * eram cor 0. A cor desses pixels na camada de sprites continua vazia. */
static void scene_fill_sprite_boxes(const GBContext* ctx, bool cgb,
                                    const PPUNativeLineState* st, int y,
                                    const uint16_t obj_cols[8][4]) {
    if (!(st->lcdc & LCDC_OBJ_ENABLE)) {
        return;
    }
    const int height = (st->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    for (int index = 0; index < 40; ++index) {
        const uint8_t* oam = ctx->oam + index * 4;
        const int sprite_y = (int)oam[0] - 16;
        if (y < sprite_y || y >= sprite_y + height) {
            continue;
        }
        const int sprite_x = (int)oam[1] - 8;
        if (sprite_x <= -8 || sprite_x >= GB_SCREEN_WIDTH) {
            continue;
        }
        const uint8_t flags = oam[3];
        int line = y - sprite_y;
        if (flags & OAM_FLIP_Y) {
            line = height - 1 - line;
        }
        const unsigned tile = (height == 16)
            ? (unsigned)(oam[2] & 0xFEu) + (unsigned)(line >> 3)
            : oam[2];
        const unsigned bank = (cgb && (flags & OAM_CGB_BANK)) ? 1u : 0u;
        const uint8_t palette = cgb ? (flags & OAM_CGB_PALETTE) : ((flags & OAM_PALETTE) ? 1u : 0u);
        const uint8_t* tile_px = tile_pixels(ctx, bank, tile);
        const uint8_t screen_v = (uint8_t)((y - sprite_y) & 7);
        for (int p = 0; p < 8; ++p) {
            const int x = sprite_x + p;
            if (x < 0 || x >= GB_SCREEN_WIDTH) {
                continue;
            }
            const size_t i = (size_t)y * g_scene_w + g_scene_off + (size_t)x;
            if (g_scene.obj_tile[i] != PPU_NATIVE_SCENE_NO_TILE) {
                continue;
            }
            g_scene.obj_tile[i] = scene_tile_id('o', tile_px, obj_cols[palette & 7u]);
            g_scene.obj_uvf[i] = scene_uvf((uint8_t)p, screen_v, flags);
        }
    }
}

/* ============================================================================
 * Tiles HD (passo 5b)
 * ========================================================================== */

static const HdTile* hd_find(uint64_t key) {
    uint32_t i = (uint32_t)(key ^ (key >> 33)) & (HD_TABLE_CAP - 1u);
    while (g_hd_tiles[i].key != 0) {
        if (g_hd_tiles[i].key == key) {
            return &g_hd_tiles[i];
        }
        i = (i + 1u) & (HD_TABLE_CAP - 1u);
    }
    return NULL;
}

/* Tile HD de um tile do cache (NULL se não houver), com cache por tile. */
static inline const HdTile* hd_lookup(char kind, const uint8_t* tile_px) {
    const unsigned gi = tile_global_index(tile_px);
    const unsigned k = (kind == 'o') ? 1u : 0u;
    if (g_hd_for_gen[k][gi] != g_hd_generation) {
        const uint64_t th = g_tile_hash[gi / NATIVE_TILES][gi % NATIVE_TILES];
        g_hd_for[k][gi] = hd_find(dump_key(kind, th, 0u));
        g_hd_for_gen[k][gi] = g_hd_generation;
    }
    return g_hd_for[k][gi];
}

/* Mesmo mapeamento de sample_tile_sub(), lendo a imagem HD (qualquer tamanho). */
static inline uint32_t sample_hd_sub(const HdTile* t, unsigned u, unsigned v, uint8_t flags,
                                     unsigned sx, unsigned sy, unsigned n) {
    const unsigned span = 8u * n;
    unsigned cs = u * n + sx;
    unsigned rs = v * n + sy;
    if (flags & OAM_FLIP_X) cs = span - 1u - cs;
    if (flags & OAM_FLIP_Y) rs = span - 1u - rs;
    if (t->size == span) {
        return t->px[rs * span + cs];
    }
    const unsigned hx = cs * t->size / span;
    const unsigned hy = rs * t->size / span;
    return t->px[hy * t->size + hx];
}

/* Recoloração proporcional: se a paleta atual difere da paleta em que o tile
 * foi pintado (fades, piscadas), cada canal é escalado pela razão entre as
 * duas cores do mesmo índice do tile original. Calculado uma vez por pixel. */
typedef struct {
    bool active;
    uint32_t mul[3];   /* 16.16, arredondado para cima: ref * mul >> 16 == atual */
    uint32_t add[3];   /* usado quando o canal de referência é 0 */
} HdRecolor;

static inline void hd_recolor_setup(HdRecolor* r, const HdTile* t, uint8_t idx, uint32_t cur) {
    r->active = false;
    if (!t->has_ref) {
        return;
    }
    const uint32_t ref = t->ref[idx & 3u];
    if (((ref ^ cur) & 0x00FFFFFFu) == 0) {
        return;
    }
    r->active = true;
    for (unsigned ch = 0; ch < 3u; ++ch) {
        const uint32_t rc = (ref >> (ch * 8u)) & 0xFFu;
        const uint32_t kc = (cur >> (ch * 8u)) & 0xFFu;
        if (rc) {
            r->mul[ch] = ((kc << 16) + rc - 1u) / rc;
            r->add[ch] = 0;
        } else {
            r->mul[ch] = 1u << 16;
            r->add[ch] = kc;
        }
    }
}

static inline uint32_t hd_recolor_apply(const HdRecolor* r, uint32_t c) {
    if (!r->active) {
        return c | 0xFF000000u;
    }
    uint32_t out = 0xFF000000u;
    for (unsigned ch = 0; ch < 3u; ++ch) {
        const uint32_t hc = (c >> (ch * 8u)) & 0xFFu;
        uint32_t v = ((hc * r->mul[ch]) >> 16) + r->add[ch];
        if (v > 255u) v = 255u;
        out |= v << (ch * 8u);
    }
    return out;
}

static void render_line(const GBPPU* ppu, const GBContext* ctx, int y) {
    static ObjLine objects;
    const PPUNativeLineState* st = &g_lines[y];
    const bool cgb = is_cgb(ctx);
    const bool compat = is_compat(ctx);
    uint32_t* row = g_frame + (size_t)y * NATIVE_W;

    rasterize_objects(ppu, ctx, cgb, st, y, &objects);

    /* Passo 5a: cores e hash de cada paleta nesta linha (só no despejo). */
    uint16_t bg_cols[8][4];
    uint16_t obj_cols[8][4];
    uint32_t bg_hash[8];
    uint32_t obj_hash[8];
    const uint8_t* last_bg_tile = NULL;
    const uint8_t* last_obj_tile = NULL;
    uint8_t last_bg_pal = 0xFFu;
    uint8_t last_obj_pal = 0xFFu;
    const bool capturing = (g_scene_state == SCENE_CAPTURING);
    const bool hd_tiles = g_hd_scale > 1 && g_hd_tiles_enabled && g_hd_tile_count > 0;
    if (g_dump_enabled || capturing || hd_tiles) {
        const unsigned count = cgb ? 8u : 2u;
        for (unsigned p = 0; p < count; ++p) {
            line_palette_colors(ppu, cgb, compat, false, (uint8_t)p, st->bgp, bg_cols[p]);
            bg_hash[p] = palette_hash(bg_cols[p]);
            line_palette_colors(ppu, cgb, compat, true, (uint8_t)p,
                                p ? st->obp1 : st->obp0, obj_cols[p]);
            obj_hash[p] = palette_hash(obj_cols[p]);
        }
    }

    const bool bg_on = cgb || (st->lcdc & LCDC_BG_ENABLE) != 0;
    const bool window_on = (st->flags & PPU_NATIVE_LINE_WINDOW) != 0;
    const int window_start = (int)st->wx - 7;
    const uint16_t bg_map = (st->lcdc & LCDC_BG_TILEMAP) ? MAP1_OFFSET : MAP0_OFFSET;
    const uint16_t window_map = (st->lcdc & LCDC_WINDOW_TILEMAP) ? MAP1_OFFSET : MAP0_OFFSET;

    for (int x = 0; x < GB_SCREEN_WIDTH; ++x) {
        BgPixel bg = {0, 0, false, NULL, 0, 0, 0};
        if (bg_on) {
            if (window_on && x >= window_start) {
                bg = sample_map(ctx, cgb, st->lcdc, window_map, x - window_start, st->window_line);
            } else {
                bg = sample_map(ctx, cgb, st->lcdc, bg_map, x + st->scx, y + st->scy);
            }
        }

        bool use_object = objects.raw[x] != 0;
        if (use_object && bg.raw != 0) {
            if (cgb) {
                if ((st->lcdc & LCDC_BG_ENABLE) && (bg.priority || objects.behind[x])) {
                    use_object = false;
                }
            } else if (objects.behind[x]) {
                use_object = false;
            }
        }

        if (use_object) {
            const uint8_t obp = objects.palette[x] ? st->obp1 : st->obp0;
            row[x] = obj_color(ppu, cgb, compat, objects.palette[x], objects.raw[x], obp);
        } else {
            row[x] = bg_color(ppu, cgb, compat, bg.palette, bg.raw, st->bgp);
        }

        if (g_dump_enabled) {
            if (bg.tile && (bg.tile != last_bg_tile || bg.palette != last_bg_pal)) {
                last_bg_tile = bg.tile;
                last_bg_pal = bg.palette;
                const uint8_t pi = cgb ? (bg.palette & 7u) : 0u;
                dump_tile('b', bg.tile, bg_cols[pi], bg_hash[pi]);
            }
            if (use_object &&
                (objects.tile[x] != last_obj_tile || objects.palette[x] != last_obj_pal)) {
                last_obj_tile = objects.tile[x];
                last_obj_pal = objects.palette[x];
                const uint8_t pi = objects.palette[x] & 7u;
                dump_tile('o', objects.tile[x], obj_cols[pi], obj_hash[pi]);
            }
        }

        if (capturing) {
            const size_t i = (size_t)y * g_scene_w + g_scene_off + (size_t)x;
            g_scene.bg_rgba[i] = bg_color(ppu, cgb, compat, bg.palette, bg.raw, st->bgp);
            if (bg.tile) {
                const uint8_t pi = cgb ? (bg.palette & 7u) : 0u;
                g_scene.bg_tile[i] = scene_tile_id('b', bg.tile, bg_cols[pi]);
                g_scene.bg_uvf[i] = scene_uvf(bg.u, bg.v, bg.flags);
            } else {
                g_scene.bg_tile[i] = PPU_NATIVE_SCENE_NO_TILE;
                g_scene.bg_uvf[i] = 0;
            }
            if (objects.raw[x]) {
                const uint8_t obp = objects.palette[x] ? st->obp1 : st->obp0;
                const uint8_t pi = objects.palette[x] & 7u;
                g_scene.obj_rgba[i] = obj_color(ppu, cgb, compat, objects.palette[x],
                                                objects.raw[x], obp);
                g_scene.obj_tile[i] = scene_tile_id('o', objects.tile[x], obj_cols[pi]);
                g_scene.obj_uvf[i] = scene_uvf(objects.u[x], objects.v[x], objects.flags[x]);
            } else {
                g_scene.obj_rgba[i] = 0u;
                g_scene.obj_tile[i] = PPU_NATIVE_SCENE_NO_TILE;
                g_scene.obj_uvf[i] = 0;
            }
        }

        if (g_hd_scale > 1) {
            const unsigned n = (unsigned)g_hd_scale;
            const size_t stride = (size_t)NATIVE_W * n;
            uint32_t* hd = g_hd + (size_t)y * n * stride + (size_t)x * n;
            /* Sprite candidato: o vencedor do pixel ou, se nenhum sprite tem
             * cor aqui, um sprite cuja caixa cobre o pixel e que tenha HD. */
            const uint8_t* ot = NULL;
            uint8_t ou = 0, ov = 0, of = 0, opal = 0, obehind = 0;
            const HdTile* obj_hd = NULL;
            if (objects.raw[x]) {
                ot = objects.tile[x];
                ou = objects.u[x];
                ov = objects.v[x];
                of = objects.flags[x];
                opal = objects.palette[x];
                obehind = objects.behind[x];
                obj_hd = hd_tiles ? hd_lookup('o', ot) : NULL;
            } else if (hd_tiles && objects.box_tile[x]) {
                obj_hd = hd_lookup('o', objects.box_tile[x]);
                if (obj_hd) {
                    ot = objects.box_tile[x];
                    ou = objects.box_u[x];
                    ov = objects.box_v[x];
                    of = objects.box_flags[x];
                    opal = objects.box_palette[x];
                    obehind = objects.box_behind[x];
                }
            }
            const HdTile* bg_hd = (hd_tiles && bg.tile) ? hd_lookup('b', bg.tile) : NULL;

            if (!bg_hd && !obj_hd && !g_hd_check) {
                /* Sem tile HD neste pixel: bloco N x N com a cor do pixel. */
                for (unsigned sy = 0; sy < n; ++sy) {
                    uint32_t* d = hd + sy * stride;
                    for (unsigned sx = 0; sx < n; ++sx) d[sx] = row[x];
                }
            } else {
                /* Todos os subpixels de um pixel caem no mesmo pixel do tile
                 * original, então índice de cor, prioridade e cores originais
                 * são calculados uma vez por pixel. Por subpixel só se lê o PNG.
                 * Fundo: prioridade pelo tile original, alfa 0 no PNG = cor
                 * original. Sprite: forma pelo alfa do PNG (>= 128). */
                const uint8_t obp = opal ? st->obp1 : st->obp0;
                const uint32_t bg_orig = bg_color(ppu, cgb, compat, bg.palette, bg.raw, st->bgp);
                const uint8_t obj_raw = objects.raw[x];      /* 0 quando veio da caixa */
                const uint32_t obj_orig = obj_raw
                    ? obj_color(ppu, cgb, compat, opal, obj_raw, obp) : 0u;
                bool obj_blocked = false;
                if (bg.raw != 0) {
                    obj_blocked = cgb
                        ? ((st->lcdc & LCDC_BG_ENABLE) && (bg.priority || obehind))
                        : (obehind != 0);
                }
                const bool obj_can_win = ot && !obj_blocked;
                HdRecolor bg_rc = {false, {0, 0, 0}, {0, 0, 0}};
                HdRecolor obj_rc = {false, {0, 0, 0}, {0, 0, 0}};
                if (bg_hd) {
                    hd_recolor_setup(&bg_rc, bg_hd, bg.raw,
                                     rgb(bg_cols[cgb ? (bg.palette & 7u) : 0u][bg.raw]));
                }
                if (obj_hd && obj_can_win) {
                    hd_recolor_setup(&obj_rc, obj_hd, obj_raw, rgb(obj_cols[opal & 7u][obj_raw]));
                }
                const bool verify = g_hd_check && !bg_hd && !obj_hd;
                for (unsigned sy = 0; sy < n; ++sy) {
                    uint32_t* d = hd + sy * stride;
                    for (unsigned sx = 0; sx < n; ++sx) {
                        uint32_t c = bg_orig;
                        if (bg_hd) {
                            const uint32_t h = sample_hd_sub(bg_hd, bg.u, bg.v, bg.flags, sx, sy, n);
                            if (h >> 24) {
                                c = hd_recolor_apply(&bg_rc, h);
                            }
                        }
                        if (obj_can_win) {
                            if (obj_hd) {
                                const uint32_t h = sample_hd_sub(obj_hd, ou, ov, of, sx, sy, n);
                                if ((h >> 24) >= 128u) {
                                    c = hd_recolor_apply(&obj_rc, h);
                                }
                            } else if (obj_raw) {
                                c = obj_orig;
                            }
                        }
                        d[sx] = c;
                        if (verify && c != row[x]) {
                            if (g_hd_cur_mismatch == 0) {
                                g_hd_cur_x = (int16_t)(x * (int)n + (int)sx);
                                g_hd_cur_y = (int16_t)(y * (int)n + (int)sy);
                            }
                            g_hd_cur_mismatch++;
                        }
                    }
                }
            }
        }
    }
    if (capturing) {
        scene_fill_sprite_boxes(ctx, cgb, st, y, (const uint16_t (*)[4])obj_cols);
    }
}

/* ============================================================================
 * API
 * ========================================================================== */

void ppu_native_set_mode(PPURendererMode mode) {
    if ((int)mode < 0 || mode >= PPU_RENDERER_COUNT) {
        mode = PPU_RENDERER_EMULATED;
    }
    if (mode != g_mode) {
        g_mode = mode;
        g_frame_open = false;
        g_cache_ready = false; /* força decodificar tudo de novo */
    }
}

PPURendererMode ppu_native_get_mode(void) {
    return g_mode;
}

const char* ppu_native_mode_name(PPURendererMode mode) {
    switch (mode) {
        case PPU_RENDERER_EMULATED: return "Emulado (referencia)";
        case PPU_RENDERER_NATIVE: return "Nativo";
        case PPU_RENDERER_COMPARE: return "Comparar (nativo x emulado)";
        default: return "?";
    }
}

static void begin_frame(const GBContext* ctx, uint8_t first_line) {
    g_frame_open = true;
    g_hd_scale = g_hd_scale_req;
    g_hd_frame_full = (first_line == 0);
    if (g_scene_state == SCENE_REQUESTED && first_line == 0) {
        g_scene_state = SCENE_CAPTURING;
        g_scene.tile_count = 0;
        memset(g_scene_map_key, 0, sizeof(g_scene_map_key));
    } else if (g_scene_state == SCENE_CAPTURING) {
        g_scene_state = SCENE_REQUESTED;   /* frame interrompido: tenta de novo */
    }
    g_hd_cur_mismatch = 0;
    g_hd_cur_x = -1;
    g_hd_cur_y = -1;
    g_margin_lines = 0;
    g_cur_mismatch = 0;
    g_cur_first_x = -1;
    g_cur_first_y = -1;
    verify_shadow(ctx);
}

void ppu_native_begin_line(GBPPU* ppu, const GBContext* ctx) {
    if (g_mode == PPU_RENDERER_EMULATED || !ppu || !ctx || !ctx->vram || !ctx->oam) {
        return;
    }
    const uint8_t y = ppu->ly;
    if (y >= GB_SCREEN_HEIGHT) {
        return;
    }

    ensure_rgb_lut();
    if (!g_cache_ready) {
        ppu_native_invalidate_all();
    }
    if (y == 0 || !g_frame_open) {
        begin_frame(ctx, y);
        if (y == 0 && g_scene_state == SCENE_CAPTURING) {
            /* Passo 5d: com widescreen a cena inclui as laterais. */
            const bool wide = ppu->widescreen_enabled && ppu->widescreen_framebuffer;
            g_scene_w = wide ? (unsigned)GB_WIDESCREEN_WIDTH : (unsigned)NATIVE_W;
            g_scene_off = wide ? (unsigned)GB_WIDESCREEN_MARGIN : 0u;
            g_scene.width = (uint16_t)g_scene_w;
            g_scene.height = (uint16_t)NATIVE_H;
            const size_t count = (size_t)g_scene_w * NATIVE_H;
            memset(g_scene.bg_rgba, 0, count * sizeof(g_scene.bg_rgba[0]));
            memset(g_scene.obj_rgba, 0, count * sizeof(g_scene.obj_rgba[0]));
            memset(g_scene.bg_tile, 0xFF, count * sizeof(g_scene.bg_tile[0]));
            memset(g_scene.obj_tile, 0xFF, count * sizeof(g_scene.obj_tile[0]));
            memset(g_scene.bg_uvf, 0, count);
            memset(g_scene.obj_uvf, 0, count);
        }
    }

    const bool cgb = is_cgb(ctx);
    PPUNativeLineState* st = &g_lines[y];
    st->lcdc = ppu->lcdc;
    st->scx = ppu->scx;
    st->scy = ppu->scy;
    st->wx = ppu->wx;
    st->wy = ppu->wy;
    st->bgp = ppu->bgp;
    st->obp0 = ppu->obp0;
    st->obp1 = ppu->obp1;
    st->window_line = ppu->window_line;

    /* Mesmas condições de ppu_try_start_window() */
    const bool bg_visible = cgb || (ppu->lcdc & LCDC_BG_ENABLE) != 0;
    const bool window = (ppu->lcdc & LCDC_WINDOW_ENABLE) != 0 &&
                        ppu->window_y_triggered && ppu->wx <= 166 && bg_visible;
    st->flags = window ? PPU_NATIVE_LINE_WINDOW : 0u;

    st->world_scroll_x = 0;
    st->world_scroll_y = 0;
    if (ctx->eram && ctx->eram_size >= 0x0904u) {
        st->world_scroll_x = (uint16_t)((ctx->eram[0x0902] << 8) | ctx->eram[0x0903]);
        st->world_scroll_y = (uint16_t)((ctx->eram[0x0900] << 8) | ctx->eram[0x0901]);
    }

    render_line(ppu, ctx, y);
}

void ppu_native_end_line(GBPPU* ppu, const GBContext* ctx) {
    (void)ctx;
    if (g_mode != PPU_RENDERER_COMPARE || !ppu || !g_frame_open) {
        return;
    }
    const uint8_t y = ppu->ly;
    if (y >= GB_SCREEN_HEIGHT) {
        return;
    }
    const uint32_t* row = g_frame + (size_t)y * NATIVE_W;
    const uint16_t* reference = ppu->color_framebuffer + (size_t)y * GB_SCREEN_WIDTH;
    for (int x = 0; x < GB_SCREEN_WIDTH; ++x) {
        const uint32_t expected = rgb(reference[x]);
        if (row[x] != expected) {
            if (g_cur_mismatch == 0) {
                g_cur_first_x = (int16_t)x;
                g_cur_first_y = (int16_t)y;
                g_cur_first_expected = expected;
                g_cur_first_got = row[x];
            }
            g_cur_mismatch++;
        }
    }
}

void ppu_native_end_frame(GBPPU* ppu, const GBContext* ctx) {
    if (g_mode == PPU_RENDERER_EMULATED || !ppu) {
        return;
    }

    if (g_mode == PPU_RENDERER_COMPARE && g_frame_open) {
        g_stats.frames_compared++;
        g_stats.last_frame_mismatched_pixels = g_cur_mismatch;
        if (g_cur_mismatch > 0) {
            g_stats.frames_with_mismatch++;
            g_stats.first_mismatch_frame = ctx ? ctx->completed_frames + 1u : 0u;
            g_stats.first_mismatch_x = g_cur_first_x;
            g_stats.first_mismatch_y = g_cur_first_y;
            g_stats.first_mismatch_expected = g_cur_first_expected;
            g_stats.first_mismatch_got = g_cur_first_got;

            const uint64_t n = g_stats.frames_with_mismatch;
            if (n <= 30u || (n % 300u) == 0u) {
                const PPUNativeLineState* st = &g_lines[g_cur_first_y];
                fprintf(stderr,
                        "[NATIVE-CMP] frame=%llu px_diferentes=%u primeira=(%d,%d) "
                        "esperado=%08X obtido=%08X | lcdc=%02X scx=%u scy=%u wx=%u wy=%u "
                        "win_line=%u win=%u\n",
                        (unsigned long long)g_stats.first_mismatch_frame,
                        g_cur_mismatch, g_cur_first_x, g_cur_first_y,
                        g_cur_first_expected, g_cur_first_got,
                        st->lcdc, st->scx, st->scy, st->wx, st->wy,
                        st->window_line, (st->flags & PPU_NATIVE_LINE_WINDOW) ? 1u : 0u);
            }
        }
    }
    if (g_frame_open && g_hd_scale > 1 && g_hd_check) {
        g_hd_stats.frames_checked++;
        if (g_hd_cur_mismatch > 0) {
            g_hd_stats.frames_with_mismatch++;
            g_hd_stats.pixels_mismatched += g_hd_cur_mismatch;
            g_hd_stats.last_x = g_hd_cur_x;
            g_hd_stats.last_y = g_hd_cur_y;
            if (g_hd_stats.frames_with_mismatch <= 20u) {
                fprintf(stderr, "[HD-CMP] escala=%d subpixels_diferentes=%u primeiro=(%d,%d)\n",
                        g_hd_scale, g_hd_cur_mismatch, g_hd_cur_x, g_hd_cur_y);
            }
        }
    }
    g_hd_valid_scale = (g_frame_open && g_hd_frame_full && g_hd_scale > 1) ? g_hd_scale : 0;
    g_hd_m_valid_scale = (g_hd_valid_scale > 1 && g_margin_lines == (uint32_t)NATIVE_H)
        ? g_hd_valid_scale : 0;
    if (g_scene_state == SCENE_CAPTURING) {
        g_scene_state = (g_frame_open && g_hd_frame_full) ? SCENE_READY : SCENE_REQUESTED;
    }
    g_frame_open = false;

    /* Publica o centro no framebuffer normal e no meio do widescreen.
     * As margens do widescreen já foram escritas linha a linha por ppu.c. */
    memcpy(ppu->rgb_framebuffer, g_frame, sizeof(g_frame));
    if (ppu->widescreen_enabled && ppu->widescreen_framebuffer) {
        for (int y = 0; y < GB_SCREEN_HEIGHT; ++y) {
            memcpy(ppu->widescreen_framebuffer + (size_t)y * GB_WIDESCREEN_WIDTH + GB_WIDESCREEN_MARGIN,
                   g_frame + (size_t)y * NATIVE_W,
                   GB_SCREEN_WIDTH * sizeof(uint32_t));
        }
    }
}

const PPUNativeCompareStats* ppu_native_compare_stats(void) {
    return &g_stats;
}

void ppu_native_reset_compare_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.first_mismatch_x = -1;
    g_stats.first_mismatch_y = -1;
}

const PPUNativeLineState* ppu_native_line_states(void) {
    return g_lines;
}

/* ============================================================================
 * Frame HD (passo 4a)
 * ========================================================================== */

void ppu_native_set_hd_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > HD_MAX) scale = HD_MAX;
    g_hd_scale_req = scale;
}

int ppu_native_get_hd_scale(void) {
    return g_hd_scale_req;
}

const uint32_t* ppu_native_hd_frame(int* out_scale) {
    if (out_scale) {
        *out_scale = g_hd_valid_scale;
    }
    return g_hd_valid_scale > 1 ? g_hd : NULL;
}

void ppu_native_set_hd_check(bool enabled) {
    g_hd_check = enabled;
}

bool ppu_native_get_hd_check(void) {
    return g_hd_check;
}

const PPUNativeHdStats* ppu_native_hd_stats(void) {
    return &g_hd_stats;
}

void ppu_native_reset_hd_stats(void) {
    memset(&g_hd_stats, 0, sizeof(g_hd_stats));
    g_hd_stats.last_x = -1;
    g_hd_stats.last_y = -1;
}

/* ============================================================================
 * Despejo de tiles (passo 5a): API
 * ========================================================================== */

void ppu_native_set_tile_dump(bool enabled) {
    g_dump_enabled = enabled;
}

bool ppu_native_get_tile_dump(void) {
    return g_dump_enabled;
}

int ppu_native_take_tile_dumps(PPUNativeTileDump* out, int max) {
    int n = 0;
    while (n < max && g_dump_size > 0) {
        out[n++] = g_dump_queue[g_dump_head];
        g_dump_head = (g_dump_head + 1u) % DUMP_QUEUE_CAP;
        g_dump_size--;
    }
    return n;
}

uint32_t ppu_native_tile_dump_seen(void) {
    return g_dump_seen_count;
}

void ppu_native_reset_tile_dump(void) {
    memset(g_dump_seen, 0, sizeof(g_dump_seen));
    g_dump_seen_count = 0;
    g_dump_head = 0;
    g_dump_size = 0;
}

/* ============================================================================
 * Captura de cena (passo 5a.2): API
 * ========================================================================== */

void ppu_native_request_scene_capture(void) {
    if (g_scene_state == SCENE_IDLE || g_scene_state == SCENE_READY) {
        g_scene_state = SCENE_REQUESTED;
    }
}

bool ppu_native_scene_capture_pending(void) {
    return g_scene_state == SCENE_REQUESTED || g_scene_state == SCENE_CAPTURING;
}

const PPUNativeScene* ppu_native_take_scene(void) {
    if (g_scene_state != SCENE_READY) {
        return NULL;
    }
    g_scene_state = SCENE_IDLE;
    return &g_scene;   /* válido até a próxima captura */
}

/* ============================================================================
 * Tiles HD (passo 5b): API
 * ========================================================================== */

bool ppu_native_hd_add_tile(char kind, uint64_t tile_hash, int size,
                            const uint32_t* argb, const uint32_t* ref_palette) {
    if ((kind != 'b' && kind != 'o') || !argb || size < 8 || size > 512 || (size % 8) != 0) {
        return false;
    }
    if (g_hd_tile_count >= HD_TABLE_CAP / 2u) {
        return false;
    }
    const uint64_t key = dump_key(kind, tile_hash, 0u);
    uint32_t i = (uint32_t)(key ^ (key >> 33)) & (HD_TABLE_CAP - 1u);
    while (g_hd_tiles[i].key != 0 && g_hd_tiles[i].key != key) {
        i = (i + 1u) & (HD_TABLE_CAP - 1u);
    }
    HdTile* t = &g_hd_tiles[i];
    uint32_t* px = (uint32_t*)malloc((size_t)size * (size_t)size * sizeof(uint32_t));
    if (!px) {
        return false;
    }
    memcpy(px, argb, (size_t)size * (size_t)size * sizeof(uint32_t));
    if (t->key == 0) {
        g_hd_tile_count++;
    } else {
        free(t->px);
    }
    t->key = key;
    t->px = px;
    t->size = (uint16_t)size;
    t->has_ref = ref_palette != NULL;
    for (unsigned c = 0; c < 4u; ++c) {
        t->ref[c] = ref_palette ? ref_palette[c] : 0u;
    }
    g_hd_generation++;
    return true;
}

void ppu_native_hd_clear_tiles(void) {
    for (uint32_t i = 0; i < HD_TABLE_CAP; ++i) {
        free(g_hd_tiles[i].px);
    }
    memset(g_hd_tiles, 0, sizeof(g_hd_tiles));
    g_hd_tile_count = 0;
    g_hd_generation++;
}

uint32_t ppu_native_hd_tile_count(void) {
    return g_hd_tile_count;
}

void ppu_native_hd_set_tiles_enabled(bool enabled) {
    g_hd_tiles_enabled = enabled;
}

bool ppu_native_hd_get_tiles_enabled(void) {
    return g_hd_tiles_enabled;
}

/* ============================================================================
 * Laterais do widescreen (passo 5d)
 * ========================================================================== */

static inline const uint8_t* tile_from_ref(const GBContext* ctx, uint16_t ref) {
    const unsigned gi = (unsigned)ref - 1u;
    return tile_pixels(ctx, gi / NATIVE_TILES, gi % NATIVE_TILES);
}

/* O escurecimento só depende da coluna (e da força escolhida no menu): uma
 * tabela de 256 valores por coluna, refeita quando o keep da coluna muda.
 * Dá exatamente (uint8_t)(canal * keep). */
static uint8_t g_fade_lut[MARGIN_W][256];
static double g_fade_key[MARGIN_W];
static bool g_fade_key_ready = false;

static inline const uint8_t* margin_fade_lut(int m, double keep) {
    if (!g_fade_key_ready) {
        for (int i = 0; i < MARGIN_W; ++i) g_fade_key[i] = -1.0;
        g_fade_key_ready = true;
    }
    if (g_fade_key[m] != keep) {
        for (unsigned v = 0; v < 256u; ++v) {
            g_fade_lut[m][v] = (uint8_t)(v * keep);
        }
        g_fade_key[m] = keep;
    }
    return g_fade_lut[m];
}

static inline uint32_t fade_with(const uint8_t* lut, uint32_t color) {
    if (!lut) {
        return color;
    }
    return (color & 0xFF000000u) |
           ((uint32_t)lut[(color >> 16) & 0xFFu] << 16) |
           ((uint32_t)lut[(color >> 8) & 0xFFu] << 8) |
           (uint32_t)lut[color & 0xFFu];
}

static inline bool margin_hd_active(void) {
    return g_hd_scale > 1 && ((g_hd_tiles_enabled && g_hd_tile_count > 0) || g_hd_check);
}

bool ppu_native_margins_wanted(void) {
    return g_mode != PPU_RENDERER_EMULATED && g_frame_open &&
           (margin_hd_active() ||
            (g_scene_state == SCENE_CAPTURING && g_scene_w == (unsigned)GB_WIDESCREEN_WIDTH));
}

static void margin_capture(const GBPPU* ppu, const GBContext* ctx, int y,
                           const PPUNativeMarginPixel* px) {
    const bool cgb = is_cgb(ctx);
    const bool compat = is_compat(ctx);
    uint16_t bg_cols[8][4];
    uint16_t obj_cols[8][4];
    const unsigned count = cgb ? 8u : 2u;
    for (unsigned p = 0; p < count; ++p) {
        line_palette_colors(ppu, cgb, compat, false, (uint8_t)p, ppu->latched_bgp, bg_cols[p]);
        line_palette_colors(ppu, cgb, compat, true, (uint8_t)p,
                            p ? ppu->latched_obp1 : ppu->latched_obp0, obj_cols[p]);
    }
    for (int m = 0; m < MARGIN_W; ++m) {
        const PPUNativeMarginPixel* r = &px[m];
        const unsigned sx = (m < GB_WIDESCREEN_MARGIN) ? (unsigned)m : (unsigned)(m + GB_SCREEN_WIDTH);
        const size_t i = (size_t)y * g_scene_w + sx;
        g_scene.bg_rgba[i] = r->bg_rgba;
        if (r->bg_tile) {
            const uint8_t pi = cgb ? (r->bg_pal & 7u) : 0u;
            g_scene.bg_tile[i] = scene_tile_id('b', tile_from_ref(ctx, r->bg_tile), bg_cols[pi]);
            g_scene.bg_uvf[i] = r->bg_uvf;
        }
        if (r->obj_tile) {
            const uint8_t pi = r->obj_pal & 7u;
            g_scene.obj_tile[i] = scene_tile_id('o', tile_from_ref(ctx, r->obj_tile), obj_cols[pi]);
            g_scene.obj_uvf[i] = r->obj_uvf;
            g_scene.obj_rgba[i] = r->obj_raw ? r->obj_rgba : 0u;
        }
    }
}

static void margin_hd(const GBContext* ctx, int y, const PPUNativeMarginPixel* px) {
    const unsigned n = (unsigned)g_hd_scale;
    const size_t stride = (size_t)MARGIN_W * n;
    const bool hd_tiles = g_hd_tiles_enabled && g_hd_tile_count > 0;
    for (int m = 0; m < MARGIN_W; ++m) {
        const PPUNativeMarginPixel* r = &px[m];
        uint32_t* hd = g_hd_m + (size_t)y * n * stride + (size_t)m * n;
        const uint8_t* bt = r->bg_tile ? tile_from_ref(ctx, r->bg_tile) : NULL;
        const uint8_t* ot = r->obj_tile ? tile_from_ref(ctx, r->obj_tile) : NULL;
        const HdTile* bg_hd = (hd_tiles && bt) ? hd_lookup('b', bt) : NULL;
        const HdTile* obj_hd = (hd_tiles && ot) ? hd_lookup('o', ot) : NULL;
        if (!r->obj_raw && !obj_hd) {
            ot = NULL;            /* caixa sem HD: não muda nada */
        }
        if (!bg_hd && !obj_hd && !g_hd_check) {
            for (unsigned sy = 0; sy < n; ++sy) {
                uint32_t* d = hd + sy * stride;
                for (unsigned sx = 0; sx < n; ++sx) d[sx] = r->rgba;
            }
            continue;
        }
        const uint8_t bu = r->bg_uvf & 7u, bv = (r->bg_uvf >> 3) & 7u;
        const uint8_t bf = (uint8_t)(((r->bg_uvf & 0x40u) ? OAM_FLIP_X : 0u) |
                                     ((r->bg_uvf & 0x80u) ? OAM_FLIP_Y : 0u));
        const uint8_t ou = r->obj_uvf & 7u, ov = (r->obj_uvf >> 3) & 7u;
        const uint8_t of = (uint8_t)(((r->obj_uvf & 0x40u) ? OAM_FLIP_X : 0u) |
                                     ((r->obj_uvf & 0x80u) ? OAM_FLIP_Y : 0u));
        const uint8_t* lut = r->fade ? margin_fade_lut(m, r->keep) : NULL;
        const uint32_t bg_fin = fade_with(lut, r->bg_rgba);
        HdRecolor bg_rc = {false, {0, 0, 0}, {0, 0, 0}};
        HdRecolor obj_rc = {false, {0, 0, 0}, {0, 0, 0}};
        if (bg_hd) {
            hd_recolor_setup(&bg_rc, bg_hd, r->bg_raw, r->bg_rgba);
        }
        if (obj_hd) {
            hd_recolor_setup(&obj_rc, obj_hd, r->obj_raw, r->obj_rgba);
        }
        const bool verify = g_hd_check && !bg_hd && !obj_hd;
        for (unsigned sy = 0; sy < n; ++sy) {
            uint32_t* d = hd + sy * stride;
            for (unsigned sx = 0; sx < n; ++sx) {
                uint32_t c = bg_fin;
                if (bg_hd) {
                    const uint32_t h = sample_hd_sub(bg_hd, bu, bv, bf, sx, sy, n);
                    if (h >> 24) {
                        c = fade_with(lut, hd_recolor_apply(&bg_rc, h));
                    }
                }
                if (ot) {
                    if (obj_hd) {
                        const uint32_t h = sample_hd_sub(obj_hd, ou, ov, of, sx, sy, n);
                        if ((h >> 24) >= 128u) {
                            c = hd_recolor_apply(&obj_rc, h);
                        }
                    } else {
                        c = r->obj_rgba;
                    }
                }
                d[sx] = c;
                if (verify) {
                    /* Confere a origem: o subpixel tem que cair no mesmo índice
                     * de cor do pixel original, no tile informado por ppu.c. */
                    bool bad = (c != r->rgba);
                    if (bt && sample_tile_sub(bt, bu, bv, bf, sx, sy, n) != r->bg_raw) bad = true;
                    if (ot && r->obj_raw &&
                        sample_tile_sub(ot, ou, ov, of, sx, sy, n) != r->obj_raw) bad = true;
                    if (bad) {
                        if (g_hd_cur_mismatch == 0) {
                            const int ex = (m < GB_WIDESCREEN_MARGIN) ? m - GB_WIDESCREEN_MARGIN
                                                                      : m + GB_SCREEN_WIDTH - GB_WIDESCREEN_MARGIN;
                            g_hd_cur_x = (int16_t)(ex * (int)n + (int)sx);
                            g_hd_cur_y = (int16_t)(y * (int)n + (int)sy);
                        }
                        g_hd_cur_mismatch++;
                    }
                }
            }
        }
    }
}

void ppu_native_margin_line(const GBPPU* ppu, const GBContext* ctx,
                            const PPUNativeMarginPixel* px) {
    if (g_mode == PPU_RENDERER_EMULATED || !g_frame_open || !ppu || !ctx || !px) {
        return;
    }
    const int y = ppu->ly;
    if (y >= NATIVE_H) {
        return;
    }
    if (g_scene_state == SCENE_CAPTURING && g_scene_w == (unsigned)GB_WIDESCREEN_WIDTH) {
        margin_capture(ppu, ctx, y, px);
    }
    if (margin_hd_active()) {
        margin_hd(ctx, y, px);
        g_margin_lines++;
    }
}

const uint32_t* ppu_native_hd_margin_frame(int* out_scale) {
    if (out_scale) {
        *out_scale = g_hd_m_valid_scale;
    }
    return g_hd_m_valid_scale > 1 ? g_hd_m : NULL;
}
