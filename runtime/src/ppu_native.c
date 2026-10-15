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
} ObjLine;

/* Rasteriza todos os sprites da linha num buffer, com a mesma regra de
 * prioridade entre sprites de ppu_fetch_object_dot(). Sem limite de 10 por
 * linha, igual ao caminho emulado atual. */
static void rasterize_objects(const GBPPU* ppu, const GBContext* ctx, bool cgb,
                              const PPUNativeLineState* st, int y, ObjLine* out) {
    memset(out->raw, 0, sizeof(out->raw));
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

static void render_line(const GBPPU* ppu, const GBContext* ctx, int y) {
    static ObjLine objects;
    const PPUNativeLineState* st = &g_lines[y];
    const bool cgb = is_cgb(ctx);
    const bool compat = is_compat(ctx);
    uint32_t* row = g_frame + (size_t)y * NATIVE_W;

    rasterize_objects(ppu, ctx, cgb, st, y, &objects);

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

        if (g_hd_scale > 1) {
            const unsigned n = (unsigned)g_hd_scale;
            const size_t stride = (size_t)NATIVE_W * n;
            uint32_t* hd = g_hd + (size_t)y * n * stride + (size_t)x * n;
            if (!g_hd_check) {
                /* Sem tiles HD: bloco N x N com a cor do pixel. */
                for (unsigned sy = 0; sy < n; ++sy) {
                    uint32_t* d = hd + sy * stride;
                    for (unsigned sx = 0; sx < n; ++sx) d[sx] = row[x];
                }
            } else {
                /* Caminho de amostragem por subpixel (o dos PNGs), com as
                 * mesmas regras de prioridade, conferido contra row[x]. */
                const uint8_t obp = objects.palette[x] ? st->obp1 : st->obp0;
                for (unsigned sy = 0; sy < n; ++sy) {
                    uint32_t* d = hd + sy * stride;
                    for (unsigned sx = 0; sx < n; ++sx) {
                        const uint8_t bg_raw = bg.tile
                            ? sample_tile_sub(bg.tile, bg.u, bg.v, bg.flags, sx, sy, n)
                            : 0u;
                        const uint8_t obj_raw = objects.raw[x]
                            ? sample_tile_sub(objects.tile[x], objects.u[x], objects.v[x],
                                              objects.flags[x], sx, sy, n)
                            : 0u;
                        bool obj_wins = obj_raw != 0;
                        if (obj_wins && bg_raw != 0) {
                            if (cgb) {
                                if ((st->lcdc & LCDC_BG_ENABLE) && (bg.priority || objects.behind[x])) {
                                    obj_wins = false;
                                }
                            } else if (objects.behind[x]) {
                                obj_wins = false;
                            }
                        }
                        const uint32_t c = obj_wins
                            ? obj_color(ppu, cgb, compat, objects.palette[x], obj_raw, obp)
                            : bg_color(ppu, cgb, compat, bg.palette, bg_raw, st->bgp);
                        d[sx] = c;
                        if (c != row[x]) {
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
    g_hd_cur_mismatch = 0;
    g_hd_cur_x = -1;
    g_hd_cur_y = -1;
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
