/**
 * @file ppu.c
 * @brief GameBoy PPU (Pixel Processing Unit) implementation
 */

#include "ppu.h"
#include "gbrt.h"
#include "gbrt_debug.h"
#include "ppu_native.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Default DMG palette
 * ========================================================================== */

static const uint32_t dmg_palette_rgba[4] = {
    0xFFE0F8D0,
    0xFF88C070,
    0xFF346856,
    0xFF081820,
};

static const uint16_t dmg_palette_rgb555[4] = {
    0x67DC,
    0x32EE,
    0x2A66,
    0x0841,
};

/* ============================================================================
 * CGB compatibility palette tables (ported from SameBoy's cgb_boot.asm)
 * ========================================================================== */

static const uint8_t compat_title_checksums[] = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6,
    0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};

static const uint8_t compat_palette_per_checksum[] = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36,
    135, 37, 30, 44, 21, 32, 31, 20, 5, 33, 13, 14,
    5, 29, 5, 18, 9, 3, 2, 26, 25, 25, 41, 42,
    26, 45, 42, 45, 36, 38, 154, 42, 30, 41, 34, 34,
    5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16, 25,
    42, 42, 5, 0, 39, 36, 22, 25, 6, 32, 12, 36,
    11, 39, 18, 39, 24, 31, 50, 17, 46, 6, 27, 0,
    47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};

static const char compat_dup_fourth_letters[] = "BEFAARBEKEK R-URAR INAILICE R";
static const size_t compat_first_duplicate_index = 65;

/* Each value is a 16-bit-color offset into compat_palette_words. */
static const uint8_t compat_palette_combo_offsets[] = {
    16, 16, 116, 72, 72, 72, 80, 80, 80, 96, 96, 96, 36, 36, 36,
    0, 0, 0, 108, 108, 108, 20, 20, 20, 48, 48, 48, 104, 104, 104,
    64, 32, 32, 16, 112, 112, 16, 8, 8, 12, 16, 16, 16, 116, 116,
    112, 16, 112, 8, 68, 8, 64, 64, 32, 16, 16, 28, 16, 16,
    72, 16, 16, 80, 76, 76, 36, 15, 15, 44, 68, 68, 8, 16, 16,
    8, 16, 16, 12, 112, 112, 0, 12, 12, 0, 0, 0, 4, 72, 88,
    72, 80, 88, 80, 96, 88, 96, 64, 88, 32, 68, 16, 52, 111, 0,
    56, 111, 16, 60, 76, 91, 36, 64, 112, 40, 16, 92, 112, 68, 88,
    8, 16, 0, 8, 16, 112, 12, 112, 12, 0, 12, 112, 16, 84, 112,
    16, 12, 112, 0, 100, 12, 112, 0, 112, 32, 16, 12, 112, 112, 12,
    24, 16, 112, 116, 120, 120, 120, 124, 124, 124, 112, 16, 4, 0, 0, 8,
};

static const uint16_t compat_palette_words[] = {
    0x7FFF, 0x32BF, 0x00D0, 0x0000,
    0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000,
    0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000,
    0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000,
    0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B,
    0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000,
    0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000,
    0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000,
    0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000,
    0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000,
    0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00,
    0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000,
    0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000,
    0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000,
    0x7FFF, 0x1BEF, 0x6180, 0x0000,
    0x7FFF, 0x7FEA, 0x7D5F, 0x0000,
    0x4778, 0x3290, 0x1D87, 0x0861,
};

/* ============================================================================
 * Helpers
 * ========================================================================== */

static bool ppu_is_cgb_mode(const GBContext* ctx) {
    return ctx && ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
}

static bool ppu_is_cgb_compat_mode(const GBContext* ctx) {
    return ctx && ctx->config.model == GB_MODEL_CGB && ctx->config.cgb_compatibility_mode;
}

static bool ppu_is_cgb_hardware(const GBContext* ctx) {
    return ctx && ctx->config.model == GB_MODEL_CGB;
}

enum {
    LCD_STARTUP_NONE = 0,
    LCD_STARTUP_MODE0 = 1,
    LCD_STARTUP_DRAW = 2,
    LCD_STARTUP_HBLANK = 3,
};

/* DMG LCD-on first line: mode 0 until dot 79, mode 3 for the normal
 * 172-dot transfer, then a shortened line that exposes LY=1 at dot 452. */
#define LCD_STARTUP_DRAW_DOT 79u
#define LCD_STARTUP_LINE1_DOT 452u

static uint32_t rgb555_to_rgba(uint16_t color) {
    uint8_t r = (uint8_t)(((color >> 0) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((color >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((color >> 10) & 0x1F) * 255 / 31);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ============================================================================
 * Modo de cor: DX ou original (P&B pelos registradores BGP/OBP0/OBP1)
 * ========================================================================== */

static int g_color_mode = PPU_COLOR_DX;

/* Cinza: branco, 172, 82, preto (os valores RGB555 mais próximos de 4 tons
 * igualmente espaçados). O verde é a mesma paleta do modo DMG. */
static const uint16_t mono_gray_rgb555[4] = {0x7FFF, 0x56B5, 0x294A, 0x0000};

void ppu_set_color_mode(int mode) {
    g_color_mode = (mode >= 0 && mode < PPU_COLOR_MODE_COUNT) ? mode : PPU_COLOR_DX;
}

int ppu_get_color_mode(void) {
    return g_color_mode;
}

uint16_t ppu_mono_shade_rgb555(uint8_t shade) {
    return g_color_mode == PPU_COLOR_MONO_GREEN ? dmg_palette_rgb555[shade & 3u]
                                                : mono_gray_rgb555[shade & 3u];
}

void ppu_mono_ramp_ends(uint32_t* dark, uint32_t* light) {
    if (dark) *dark = rgb555_to_rgba(ppu_mono_shade_rgb555(3));
    if (light) *light = rgb555_to_rgba(ppu_mono_shade_rgb555(0));
}

uint32_t ppu_mono_tint_rgba(uint32_t argb) {
    /* Passo 31a: cai num dos 4 tons da rampa (as texturas originais usam
     * exatamente 255/170/85/0; no verde os tons do meio ficam os do DMG). */
    const uint32_t r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
    const uint32_t lum = (r * 77u + g * 150u + b * 29u) >> 8;   /* 0..255 */
    const uint8_t shade = lum >= 213u ? 0u : lum >= 128u ? 1u : lum >= 43u ? 2u : 3u;
    return rgb555_to_rgba(ppu_mono_shade_rgb555(shade));
}




static uint8_t apply_palette(uint8_t color, uint8_t palette) {
    return (uint8_t)((palette >> (color * 2)) & 0x03);
}

static uint16_t read_palette_color(const uint8_t* palette_ram, uint8_t palette_number, uint8_t color) {
    size_t index = (size_t)palette_number * 8u + (size_t)color * 2u;
    return (uint16_t)(palette_ram[index] | (palette_ram[index + 1] << 8));
}

static uint8_t vram_read_bank(const GBContext* ctx, uint8_t bank, uint16_t addr) {
    if (!ctx || !ctx->vram || addr < 0x8000 || addr > 0x9FFF) {
        return 0xFF;
    }
    return ctx->vram[(bank * VRAM_SIZE) + (addr - 0x8000)];
}

static uint16_t get_tile_data_addr(uint8_t lcdc, uint8_t tile_idx, bool is_obj) {
    if (is_obj || (lcdc & LCDC_TILE_DATA)) {
        return (uint16_t)(0x8000 + (tile_idx * 16));
    }
    return (uint16_t)(0x9000 + ((int8_t)tile_idx * 16));
}

static uint16_t get_bg_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
}

static uint16_t get_window_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;
}

static bool compat_license_is_nintendo(const GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size <= 0x145) {
        return false;
    }
    if (ctx->rom[0x14B] == 0x01) {
        return true;
    }
    return ctx->rom[0x14B] == 0x33 &&
           ctx->rom[0x144] == '0' &&
           ctx->rom[0x145] == '1';
}

static uint8_t compat_title_checksum(const GBContext* ctx) {
    uint8_t checksum = 0;

    if (!ctx || !ctx->rom || ctx->rom_size <= 0x143) {
        return 0;
    }

    for (size_t i = 0; i < 16; i++) {
        checksum = (uint8_t)(checksum + ctx->rom[0x134 + i]);
    }

    return checksum;
}

static uint8_t compat_palette_index(const GBContext* ctx) {
    uint8_t checksum;

    if (!compat_license_is_nintendo(ctx)) {
        return 0;
    }

    checksum = compat_title_checksum(ctx);
    for (size_t i = 0; i < sizeof(compat_title_checksums); i++) {
        if (compat_title_checksums[i] != checksum) {
            continue;
        }

        if (i >= compat_first_duplicate_index) {
            uint8_t fourth_letter = (ctx && ctx->rom && ctx->rom_size > 0x137) ? ctx->rom[0x137] : 0;
            if ((char)fourth_letter != compat_dup_fourth_letters[i - compat_first_duplicate_index]) {
                continue;
            }
        }

        return compat_palette_per_checksum[i];
    }

    return 0;
}

static void compat_copy_palette_bytes(uint8_t* dst, uint8_t word_offset) {
    for (size_t i = 0; i < 4; i++) {
        uint16_t color = compat_palette_words[word_offset + i];
        dst[i * 2] = (uint8_t)(color & 0xFF);
        dst[i * 2 + 1] = (uint8_t)(color >> 8);
    }
}

static void ppu_fill_palette_ram(uint8_t* palette_ram, uint16_t color) {
    for (size_t palette = 0; palette < 8; palette++) {
        for (size_t entry = 0; entry < 4; entry++) {
            size_t index = palette * 8 + entry * 2;
            palette_ram[index] = (uint8_t)(color & 0xFF);
            palette_ram[index + 1] = (uint8_t)(color >> 8);
        }
    }
}

static void ppu_load_compatibility_palettes(GBPPU* ppu, const GBContext* ctx) {
    uint8_t palette_index = compat_palette_index(ctx) & 0x7F;
    size_t combo_index = (size_t)palette_index * 3u;

    memset(ppu->bg_palette_ram, 0, sizeof(ppu->bg_palette_ram));
    memset(ppu->obj_palette_ram, 0, sizeof(ppu->obj_palette_ram));

    compat_copy_palette_bytes(&ppu->obj_palette_ram[0], compat_palette_combo_offsets[combo_index + 0]);
    compat_copy_palette_bytes(&ppu->obj_palette_ram[8], compat_palette_combo_offsets[combo_index + 1]);
    compat_copy_palette_bytes(&ppu->bg_palette_ram[0], compat_palette_combo_offsets[combo_index + 2]);
}

static void latch_scanline_registers(GBPPU* ppu) {
    ppu->latched_lcdc = ppu->lcdc;
    ppu->latched_scy = ppu->scy;
    ppu->latched_scx = ppu->scx;
    ppu->latched_bgp = ppu->bgp;
    ppu->latched_obp0 = ppu->obp0;
    ppu->latched_obp1 = ppu->obp1;
    ppu->latched_wy = ppu->wy;
    ppu->latched_wx = ppu->wx;
}

/* ============================================================================
 * PPU Initialization
 * ========================================================================== */

void ppu_init(GBPPU* ppu) {
    memset(ppu, 0, sizeof(*ppu));
    ppu->native_render_enabled = true; //renderizador nativo habilitado por padrão
    ppu->widescreen_fade_enabled = true;
    ppu->widescreen_fade_amount = 0.65f;
    ppu_reset(ppu, NULL);
    DBG_PPU("%s", "PPU initialized");
}

void ppu_reset(GBPPU* ppu, const GBContext* ctx) {
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool cgb_compat_mode = ppu_is_cgb_compat_mode(ctx);
    uint16_t default_color = cgb_mode ? 0x7FFF : dmg_palette_rgb555[0];

    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
    memset(ppu->native_line, 0, sizeof(ppu->native_line)); //renderizador nativo
    for (size_t i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->color_framebuffer[i] = default_color;
        ppu->rgb_framebuffer[i] = cgb_mode ? rgb555_to_rgba(default_color) : dmg_palette_rgba[0];
    }

    ppu->lcdc = 0x91;
    ppu->stat = 0x81;
    ppu->scy = 0x00;
    ppu->scx = 0x00;
    ppu->ly = 145;
    ppu->scanline = 145;
    ppu->lyc = 0x00;
    ppu->dma = (ctx && ctx->config.model == GB_MODEL_CGB) ? 0x00 : 0xFF;
    ppu->bgp = 0xFC;
    ppu->obp0 = 0xFF;
    ppu->obp1 = 0xFF;
    ppu->wy = 0x00;
    ppu->wx = 0x00;
    ppu->bgpi = 0xC0;
    ppu->obpi = 0xC0;
    ppu->opri = cgb_mode ? 0 : 1;

    latch_scanline_registers(ppu);

    ppu->stat_irq_state = false;
    ppu->mode = PPU_MODE_VBLANK;
    ppu->visible_mode = PPU_MODE_VBLANK;
    ppu->stat_irq_mode = PPU_MODE_VBLANK;
    ppu->vblank_oam_irq_source = false;
    ppu->lcd_startup_phase = LCD_STARTUP_NONE;
    ppu->mode_cycles = 4;
    ppu->mode3_length = CYCLES_PIXEL_DRAW;
    ppu->hblank_length = CYCLES_HBLANK;
    ppu->draw_x = 0;
    ppu->draw_startup = 0;
    ppu->draw_stall = 0;
    ppu->visible_sprite_count = 0;
    ppu->line_sprite_height = 8;
    ppu->fetched_sprite_mask = 0;
    ppu->considered_bg_tiles = 0;
    ppu->window_line = 0;
    ppu->window_triggered = false;
    ppu->window_y_triggered = false;
    ppu->window_active_line = false;
    ppu->window_rendered_line = false;
    ppu->window_pixel_x = 0;
    ppu->frame_ready = false;

    if (cgb_mode) {
        ppu_fill_palette_ram(ppu->bg_palette_ram, 0x7FFF);
        ppu_fill_palette_ram(ppu->obj_palette_ram, 0x7FFF);
    } else if (cgb_compat_mode) {
        ppu_load_compatibility_palettes(ppu, ctx);
    } else {
        memset(ppu->bg_palette_ram, 0, sizeof(ppu->bg_palette_ram));
        memset(ppu->obj_palette_ram, 0, sizeof(ppu->obj_palette_ram));
    }

    if (ctx) {
        GBContext* mutable_ctx = (GBContext*)ctx;
        mutable_ctx->io[0x40] = ppu->lcdc;
        mutable_ctx->io[0x41] = ppu->stat;
        mutable_ctx->io[0x42] = ppu->scy;
        mutable_ctx->io[0x43] = ppu->scx;
        mutable_ctx->io[0x44] = ppu->ly;
        mutable_ctx->io[0x45] = ppu->lyc;
        mutable_ctx->io[0x46] = ppu->dma;
        mutable_ctx->io[0x47] = ppu->bgp;
        mutable_ctx->io[0x48] = ppu->obp0;
        mutable_ctx->io[0x49] = ppu->obp1;
        mutable_ctx->io[0x4A] = ppu->wy;
        mutable_ctx->io[0x4B] = ppu->wx;
        mutable_ctx->io[0x68] = (uint8_t)(ppu->bgpi | 0x40);
        mutable_ctx->io[0x6A] = (uint8_t)(ppu->obpi | 0x40);
        mutable_ctx->io[0x6C] = (uint8_t)(0xFE | (ppu->opri & 0x01));
    }

    DBG_PPU("PPU reset - LCDC=0x%02X mode=%s cgb=%d compat=%d",
            ppu->lcdc, ppu_mode_name(ppu->mode), cgb_mode ? 1 : 0, cgb_compat_mode ? 1 : 0);
}

/* ============================================================================
 * Rendering
 * ========================================================================== */

static uint16_t resolve_bg_color(const GBPPU* ppu,
                                 const GBContext* ctx,
                                 uint8_t palette_number,
                                 uint8_t raw_color,
                                 uint8_t dmg_palette_reg) {
    if (ppu_is_cgb_mode(ctx)) {
        if (g_color_mode != PPU_COLOR_DX) {
            return ppu_mono_shade_rgb555(apply_palette(raw_color, dmg_palette_reg));
        }
        return read_palette_color(ppu->bg_palette_ram, palette_number, raw_color);
    }

    if (ppu_is_cgb_compat_mode(ctx)) {
        uint8_t shade = apply_palette(raw_color, dmg_palette_reg);
        return read_palette_color(ppu->bg_palette_ram, 0, shade);
    }

    return dmg_palette_rgb555[apply_palette(raw_color, dmg_palette_reg)];
}

static uint16_t resolve_obj_color(const GBPPU* ppu,
                                  const GBContext* ctx,
                                  uint8_t palette_number,
                                  uint8_t raw_color,
                                  uint8_t dmg_palette_reg) {
    if (ppu_is_cgb_mode(ctx)) {
        if (g_color_mode != PPU_COLOR_DX) {
            return ppu_mono_shade_rgb555(apply_palette(raw_color, dmg_palette_reg));
        }
        return read_palette_color(ppu->obj_palette_ram, palette_number, raw_color);
    }

    if (ppu_is_cgb_compat_mode(ctx)) {
        uint8_t shade = apply_palette(raw_color, dmg_palette_reg);
        return read_palette_color(ppu->obj_palette_ram, palette_number, shade);
    }

    return dmg_palette_rgb555[apply_palette(raw_color, dmg_palette_reg)];
}

static void render_bg_scanline(GBPPU* ppu,
                               GBContext* ctx,
                               uint8_t* bg_raw,
                               uint8_t* bg_priority) {
    uint8_t scanline = ppu->ly;
    uint8_t lcdc = ppu->latched_lcdc;
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool cgb_compat_mode = ppu_is_cgb_compat_mode(ctx);
    bool bg_visible = cgb_mode ? true : ((lcdc & LCDC_BG_ENABLE) != 0);
    bool window_enable = (lcdc & LCDC_WINDOW_ENABLE) &&
                         (ppu->latched_wx <= 166) &&
                         (ppu->latched_wy <= scanline) &&
                         (cgb_mode || bg_visible);

    if (!(lcdc & LCDC_LCD_ENABLE)) {
        memset(&ppu->framebuffer[scanline * GB_SCREEN_WIDTH], 0, GB_SCREEN_WIDTH);
        memset(bg_raw, 0, GB_SCREEN_WIDTH);
        memset(bg_priority, 0, GB_SCREEN_WIDTH);
        for (size_t x = 0; x < GB_SCREEN_WIDTH; x++) {
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] = dmg_palette_rgb555[0];
        }
        return;
    }

    if (window_enable && !ppu->window_triggered) {
        ppu->window_triggered = true;
    }

    if (!cgb_mode && !cgb_compat_mode) {
        const uint8_t* vram = ctx->vram;
        const size_t row_base = (size_t)scanline * GB_SCREEN_WIDTH;
        const uint16_t bg_tilemap_offset = (uint16_t)(get_bg_tilemap_addr(lcdc) - 0x8000);
        const uint16_t win_tilemap_offset = (uint16_t)(get_window_tilemap_addr(lcdc) - 0x8000);
        const bool unsigned_tile_data = (lcdc & LCDC_TILE_DATA) != 0;
        const uint8_t palette = ppu->latched_bgp;
        uint8_t shade_lut[4] = {
            (uint8_t)(palette & 0x03),
            (uint8_t)((palette >> 2) & 0x03),
            (uint8_t)((palette >> 4) & 0x03),
            (uint8_t)((palette >> 6) & 0x03),
        };
        uint16_t color_lut[4] = {
            dmg_palette_rgb555[shade_lut[0]],
            dmg_palette_rgb555[shade_lut[1]],
            dmg_palette_rgb555[shade_lut[2]],
            dmg_palette_rgb555[shade_lut[3]],
        };

        if (!bg_visible) {
            memset(&ppu->framebuffer[row_base], 0, GB_SCREEN_WIDTH);
            memset(bg_raw, 0, GB_SCREEN_WIDTH);
            memset(bg_priority, 0, GB_SCREEN_WIDTH);
            for (size_t x = 0; x < GB_SCREEN_WIDTH; x++) {
                ppu->color_framebuffer[row_base + x] = dmg_palette_rgb555[0];
            }
            return;
        }

        memset(bg_priority, 0, GB_SCREEN_WIDTH);
        for (int x = 0; x < GB_SCREEN_WIDTH;) {
            bool in_window = window_enable && (x >= (int)ppu->latched_wx - 7);
            int source_x;
            int source_y;
            int pixel_x;
            int pixel_y;
            int run;
            uint16_t tilemap_offset;
            uint8_t tile_x;
            uint8_t tile_y;
            uint8_t tile_idx;
            uint16_t tile_offset;
            uint8_t lo;
            uint8_t hi;

            if (in_window) {
                int win_x = x - ((int)ppu->latched_wx - 7);
                source_x = win_x;
                source_y = ppu->window_line;
                tilemap_offset = win_tilemap_offset;
            } else {
                source_x = (x + ppu->latched_scx) & 0xFF;
                source_y = (scanline + ppu->latched_scy) & 0xFF;
                tilemap_offset = bg_tilemap_offset;
            }

            tile_x = (uint8_t)(source_x >> 3);
            tile_y = (uint8_t)(source_y >> 3);
            pixel_x = source_x & 7;
            pixel_y = source_y & 7;
            run = 8 - pixel_x;
            if (!in_window && window_enable) {
                int window_x = (int)ppu->latched_wx - 7;
                if (x < window_x && x + run > window_x) {
                    run = window_x - x;
                }
            }
            if (x + run > GB_SCREEN_WIDTH) {
                run = GB_SCREEN_WIDTH - x;
            }

            tile_idx = vram[tilemap_offset + tile_y * 32u + tile_x];
            tile_offset = unsigned_tile_data
                ? (uint16_t)(tile_idx * 16u + pixel_y * 2)
                : (uint16_t)(0x1000 + ((int8_t)tile_idx * 16) + pixel_y * 2);
            lo = vram[tile_offset];
            hi = vram[tile_offset + 1u];

            for (int i = 0; i < run; i++) {
                int bit = 7 - (pixel_x + i);
                uint8_t raw_color = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
                uint8_t shade = shade_lut[raw_color];
                int screen_x = x + i;

                ppu->framebuffer[row_base + screen_x] = shade;
                ppu->color_framebuffer[row_base + screen_x] = color_lut[raw_color];
                bg_raw[screen_x] = raw_color;
            }

            x += run;
        }

        if (ppu->window_triggered && window_enable) {
            ppu->window_line++;
        }
        return;
    }

    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        uint8_t raw_color = 0;
        uint8_t palette_number = 0;
        uint8_t tile_bank = 0;
        bool priority = false;
        bool in_window = window_enable && (x >= (int)ppu->latched_wx - 7);
        uint16_t tilemap_addr;
        uint8_t tile_x;
        uint8_t tile_y;
        uint8_t tile_idx;
        uint8_t attr = 0;
        int pixel_x;
        int pixel_y;
        uint16_t tile_addr;
        uint8_t lo;
        uint8_t hi;
        int bit;

        if (!bg_visible && !in_window) {
            ppu->framebuffer[scanline * GB_SCREEN_WIDTH + x] = 0;
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] = dmg_palette_rgb555[0];
            bg_raw[x] = 0;
            bg_priority[x] = 0;
            continue;
        }

        if (in_window) {
            int win_x = x - ((int)ppu->latched_wx - 7);
            int win_y = ppu->window_line;
            tilemap_addr = get_window_tilemap_addr(lcdc);
            tile_x = (uint8_t)(win_x / 8);
            tile_y = (uint8_t)(win_y / 8);
            pixel_x = win_x % 8;
            pixel_y = win_y % 8;
        } else {
            int bg_x = (x + ppu->latched_scx) & 0xFF;
            int bg_y = (scanline + ppu->latched_scy) & 0xFF;
            tilemap_addr = get_bg_tilemap_addr(lcdc);
            tile_x = (uint8_t)(bg_x / 8);
            tile_y = (uint8_t)(bg_y / 8);
            pixel_x = bg_x % 8;
            pixel_y = bg_y % 8;
        }

        tile_idx = vram_read_bank(ctx, 0, (uint16_t)(tilemap_addr + tile_y * 32 + tile_x));
        if (cgb_mode) {
            attr = vram_read_bank(ctx, 1, (uint16_t)(tilemap_addr + tile_y * 32 + tile_x));
            palette_number = attr & 0x07;
            tile_bank = (attr & 0x08) ? 1 : 0;
            priority = (attr & 0x80) != 0;
            if (attr & 0x20) pixel_x = 7 - pixel_x;
            if (attr & 0x40) pixel_y = 7 - pixel_y;
        }

        tile_addr = get_tile_data_addr(lcdc, tile_idx, false);
        lo = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2));
        hi = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2 + 1));
        bit = 7 - pixel_x;
        raw_color = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));

        ppu->framebuffer[scanline * GB_SCREEN_WIDTH + x] = cgb_mode
            ? raw_color
            : apply_palette(raw_color, ppu->latched_bgp);
        ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] =
            resolve_bg_color(ppu, ctx, palette_number, raw_color, ppu->latched_bgp);
        bg_raw[x] = raw_color;
        bg_priority[x] = priority ? 1 : 0;
    }

    if (ppu->window_triggered && window_enable) {
        ppu->window_line++;
    }

    (void)cgb_compat_mode;
}

typedef struct {
    int oam_index;
    int screen_x;
    uint8_t x_pos;
    uint8_t flags;
    uint8_t palette;
    bool behind_bg;
    uint8_t lo;
    uint8_t hi;
} ScanlineSprite;

static void sort_scanline_sprites(ScanlineSprite* sprites, int sprite_count) {
    for (int i = 1; i < sprite_count; i++) {
        ScanlineSprite sprite = sprites[i];
        int j = i - 1;

        while (j >= 0) {
            bool higher_priority = (sprite.x_pos < sprites[j].x_pos) ||
                                   (sprite.x_pos == sprites[j].x_pos &&
                                    sprite.oam_index < sprites[j].oam_index);
            if (!higher_priority) {
                break;
            }
            sprites[j + 1] = sprites[j];
            j--;
        }

        sprites[j + 1] = sprite;
    }
}

static void render_dmg_sprites_scanline_fast(GBPPU* ppu,
                                             const uint8_t* bg_raw,
                                             const ScanlineSprite* sprites,
                                             int sprite_count) {
    uint8_t drawn[GB_SCREEN_WIDTH];
    uint8_t obp0_lut[4];
    uint8_t obp1_lut[4];
    uint16_t obp0_color_lut[4];
    uint16_t obp1_color_lut[4];
    const size_t row_base = (size_t)ppu->ly * GB_SCREEN_WIDTH;

    memset(drawn, 0, sizeof(drawn));
    for (int i = 0; i < 4; i++) {
        obp0_lut[i] = (uint8_t)((ppu->latched_obp0 >> (i * 2)) & 0x03);
        obp1_lut[i] = (uint8_t)((ppu->latched_obp1 >> (i * 2)) & 0x03);
        obp0_color_lut[i] = dmg_palette_rgb555[obp0_lut[i]];
        obp1_color_lut[i] = dmg_palette_rgb555[obp1_lut[i]];
    }

    for (int i = 0; i < sprite_count; i++) {
        const ScanlineSprite* sprite = &sprites[i];
        const uint8_t* shade_lut = sprite->palette ? obp1_lut : obp0_lut;
        const uint16_t* color_lut = sprite->palette ? obp1_color_lut : obp0_color_lut;

        for (int sprite_px = 0; sprite_px < 8; sprite_px++) {
            int screen_x = sprite->screen_x + sprite_px;
            int bit_pos;
            uint8_t color;
            uint8_t shade;

            if (screen_x < 0 || screen_x >= GB_SCREEN_WIDTH || drawn[screen_x]) {
                continue;
            }

            bit_pos = (sprite->flags & OAM_FLIP_X) ? sprite_px : (7 - sprite_px);
            color = (uint8_t)(((sprite->lo >> bit_pos) & 1) | (((sprite->hi >> bit_pos) & 1) << 1));
            if (color == 0) {
                continue;
            }

            drawn[screen_x] = 1;
            if (sprite->behind_bg && bg_raw && bg_raw[screen_x] != 0) {
                continue;
            }

            shade = shade_lut[color];
            ppu->framebuffer[row_base + screen_x] = shade;
            ppu->color_framebuffer[row_base + screen_x] = color_lut[color];
        }
    }
}

static void render_sprites_scanline(GBPPU* ppu,
                                    GBContext* ctx,
                                    const uint8_t* bg_raw,
                                    const uint8_t* bg_priority) {
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool cgb_compat_mode = ppu_is_cgb_compat_mode(ctx);
    bool dmg_priority_mode = !cgb_mode || ppu->opri != 0;
    uint8_t sprite_height;
    int sprite_count = 0;
    ScanlineSprite sprites[40];

    if (!(ppu->latched_lcdc & LCDC_OBJ_ENABLE)) {
        return;
    }

    sprite_height = (ppu->latched_lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

    for (int i = 0; i < 40 && sprite_count < 40; i++) {
        const OAMEntry* sprite = (const OAMEntry*)(ctx->oam + i * 4);
        int sprite_y = (int)sprite->y - 16;

        if (ppu->ly < sprite_y || ppu->ly >= sprite_y + sprite_height) {
            continue;
        }

        {
            int line = (int)ppu->ly - sprite_y;
            uint8_t tile_idx = sprite->tile;
            uint8_t tile_bank = 0;

            if (sprite_height == 16) {
                tile_idx &= 0xFE;
            }

            if (sprite->flags & OAM_FLIP_Y) {
                line = sprite_height - 1 - line;
            }

            if (cgb_mode && (sprite->flags & OAM_CGB_BANK)) {
                tile_bank = 1;
            }

            sprites[sprite_count].oam_index = i;
            sprites[sprite_count].screen_x = (int)sprite->x - 8;
            sprites[sprite_count].x_pos = sprite->x;
            sprites[sprite_count].flags = sprite->flags;
            sprites[sprite_count].palette = cgb_mode
                ? (sprite->flags & OAM_CGB_PALETTE)
                : ((sprite->flags & OAM_PALETTE) ? 1 : 0);
            sprites[sprite_count].behind_bg = (sprite->flags & OAM_PRIORITY) != 0;
            sprites[sprite_count].lo = vram_read_bank(ctx, tile_bank, (uint16_t)(0x8000 + tile_idx * 16 + line * 2));
            sprites[sprite_count].hi = vram_read_bank(ctx, tile_bank, (uint16_t)(0x8000 + tile_idx * 16 + line * 2 + 1));
            sprite_count++;
        }
    }

    if (dmg_priority_mode && sprite_count > 0 && sprite_count <= 40) /*(dmg_priority_mode)*/ {
        sort_scanline_sprites(sprites, sprite_count);
    }

    if (!cgb_mode && !cgb_compat_mode) {
        render_dmg_sprites_scanline_fast(ppu, bg_raw, sprites, sprite_count);
        return;
    }

    for (int screen_x = 0; screen_x < GB_SCREEN_WIDTH; screen_x++) {
        const ScanlineSprite* chosen_sprite = NULL;
        uint8_t chosen_color = 0;

        for (int i = 0; i < sprite_count; i++) {
            const ScanlineSprite* sprite = &sprites[i];
            int sprite_px = screen_x - sprite->screen_x;
            int bit_pos;
            uint8_t color;

            if (sprite_px < 0 || sprite_px >= 8) {
                continue;
            }

            bit_pos = (sprite->flags & OAM_FLIP_X) ? sprite_px : (7 - sprite_px);
            
            
            // Correção de ponteiro de bit: decodifica a transparência de cor vazia (0) nativa do Game Boy
            color = (uint8_t)(((sprite->lo >> bit_pos) & 1) | (((sprite->hi >> bit_pos) & 1) << 1));

            if (color == 0) {
                continue;
            }

            chosen_sprite = sprite;
            chosen_color = color;
            break;
        }

        if (!chosen_sprite) {
            continue;
        }

        if (cgb_mode) {
            uint8_t bg_color = bg_raw ? bg_raw[screen_x] : 0;
            uint8_t bg_attr_priority = bg_priority ? bg_priority[screen_x] : 0;

            if (bg_color != 0) {
                if (!(ppu->latched_lcdc & LCDC_BG_ENABLE)) {
                    /* LCDC bit 0 clear gives OBJ priority in CGB mode. */
                } else if (bg_attr_priority || chosen_sprite->behind_bg) {
                    continue;
                }
            }

            ppu->framebuffer[ppu->ly * GB_SCREEN_WIDTH + screen_x] = chosen_color;
            ppu->color_framebuffer[ppu->ly * GB_SCREEN_WIDTH + screen_x] =
                resolve_obj_color(ppu, ctx, chosen_sprite->palette, chosen_color,
                                  (chosen_sprite->flags & OAM_PALETTE) ? ppu->latched_obp1 : ppu->latched_obp0);
        } else {
            uint8_t bg_color = bg_raw ? bg_raw[screen_x] : 0;
            uint8_t dmg_palette_reg = chosen_sprite->palette ? ppu->latched_obp1 : ppu->latched_obp0;
            uint8_t shade = apply_palette(chosen_color, dmg_palette_reg);

            if (chosen_sprite->behind_bg && bg_color != 0) {
                continue;
            }

            ppu->framebuffer[ppu->ly * GB_SCREEN_WIDTH + screen_x] = shade;
            ppu->color_framebuffer[ppu->ly * GB_SCREEN_WIDTH + screen_x] =
                resolve_obj_color(ppu, ctx, chosen_sprite->palette, chosen_color, dmg_palette_reg);
        }
    }
}

/* ============================================================================
 * Dot-aware pixel-transfer reference path
 *
 * This is deliberately kept separate from the scanline renderer above. The
 * reference path samples live registers and VRAM as pixels are emitted, and
 * accounts for the FIFO startup, SCX discard, window restart, and OBJ fetch
 * stalls that make mode 3 variable. A future batched renderer may use the
 * scanline helpers only after proving that none of these observable events can
 * occur for the span being batched.
 * ========================================================================== */

typedef struct {
    uint8_t raw_color;
    uint8_t palette;
    bool priority;
} DotBackgroundPixel;

typedef struct {
    bool present;
    uint8_t raw_color;
    uint8_t palette;
    uint8_t dmg_pal;    /* bit 4 do OAM (0 = OBP0, 1 = OBP1), também no modo CGB */
    bool behind_bg;
} DotObjectPixel;

static DotBackgroundPixel ppu_fetch_background_dot(const GBPPU* ppu,
                                                    const GBContext* ctx) {
    DotBackgroundPixel pixel = {0, 0, false};
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool bg_enabled = cgb_mode || (ppu->lcdc & LCDC_BG_ENABLE) != 0;
    const bool in_window = ppu->window_active_line;
    uint16_t tilemap_addr;
    int source_x;
    int source_y;

    if (!bg_enabled) {
        return pixel;
    }

    if (in_window) {
        source_x = ppu->window_pixel_x;
        source_y = ppu->window_line;
        tilemap_addr = get_window_tilemap_addr(ppu->lcdc);
    } else {
        source_x = (ppu->draw_x + ppu->scx) & 0xFF;
        source_y = (ppu->ly + ppu->scy) & 0xFF;
        tilemap_addr = get_bg_tilemap_addr(ppu->lcdc);
    }

    uint8_t pixel_x = (uint8_t)(source_x & 7);
    uint8_t pixel_y = (uint8_t)(source_y & 7);
    const uint8_t tile_x = (uint8_t)((source_x >> 3) & 31);
    const uint8_t tile_y = (uint8_t)((source_y >> 3) & 31);
    const uint16_t map_entry = (uint16_t)(tilemap_addr + tile_y * 32u + tile_x);
    const uint8_t tile_idx = vram_read_bank(ctx, 0, map_entry);
    uint8_t tile_bank = 0;
    uint8_t attr = 0;

    if (cgb_mode) {
        attr = vram_read_bank(ctx, 1, map_entry);
        pixel.palette = attr & OAM_CGB_PALETTE;
        tile_bank = (attr & OAM_CGB_BANK) ? 1 : 0;
        pixel.priority = (attr & OAM_PRIORITY) != 0;
        if (attr & OAM_FLIP_X) pixel_x = (uint8_t)(7 - pixel_x);
        if (attr & OAM_FLIP_Y) pixel_y = (uint8_t)(7 - pixel_y);
    }

    const uint16_t tile_addr = get_tile_data_addr(ppu->lcdc, tile_idx, false);
    const uint8_t lo = vram_read_bank(ctx, tile_bank,
                                      (uint16_t)(tile_addr + pixel_y * 2u));
    const uint8_t hi = vram_read_bank(ctx, tile_bank,
                                      (uint16_t)(tile_addr + pixel_y * 2u + 1u));
    const int bit = 7 - pixel_x;
    pixel.raw_color = (uint8_t)(((lo >> bit) & 1u) |
                                (((hi >> bit) & 1u) << 1u));
    return pixel;
}

static DotObjectPixel ppu_fetch_object_dot(const GBPPU* ppu,
                                            const GBContext* ctx) {
    DotObjectPixel chosen = {false, 0, 0, 0, false};
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool dmg_priority = !cgb_mode || ppu->opri != 0;
    int chosen_x = 256;
    int chosen_index = 256;

    if (!(ppu->lcdc & LCDC_OBJ_ENABLE)) {
        return chosen;
    }

    for (uint8_t slot = 0; slot < ppu->visible_sprite_count; ++slot) {
        const int oam_index = ppu->visible_sprite_indices[slot];
        const int screen_x = (int)ppu->visible_sprite_x[slot] - 8;
        const int sprite_pixel = (int)ppu->draw_x - screen_x;
        if (sprite_pixel < 0 || sprite_pixel >= 8) {
            continue;
        }

        const size_t oam_offset = (size_t)oam_index * 4u;
        const uint8_t tile_byte = ctx->oam[oam_offset + 2u];
        const uint8_t flags = ctx->oam[oam_offset + 3u];
        int line = (int)ppu->ly - ((int)ppu->visible_sprite_y[slot] - 16);
        uint8_t tile = tile_byte;
        uint8_t bank = 0;
        if (ppu->line_sprite_height == 16) {
            tile &= 0xFE;
        }
        if (flags & OAM_FLIP_Y) {
            line = ppu->line_sprite_height - 1 - line;
        }
        if (line < 0 || line >= ppu->line_sprite_height) {
            continue;
        }
        if (cgb_mode && (flags & OAM_CGB_BANK)) {
            bank = 1;
        }

        const uint16_t tile_addr = (uint16_t)(0x8000u + tile * 16u + line * 2u);
        const uint8_t lo = vram_read_bank(ctx, bank, tile_addr);
        const uint8_t hi = vram_read_bank(ctx, bank, (uint16_t)(tile_addr + 1u));
        const int bit = (flags & OAM_FLIP_X) ? sprite_pixel : (7 - sprite_pixel);
        const uint8_t raw = (uint8_t)(((lo >> bit) & 1u) |
                                      (((hi >> bit) & 1u) << 1u));
        if (raw == 0) {
            continue;
        }

        bool wins = !chosen.present;
        if (!wins && dmg_priority) {
            wins = screen_x < chosen_x ||
                   (screen_x == chosen_x && oam_index < chosen_index);
        } else if (!wins) {
            wins = oam_index < chosen_index;
        }
        if (!wins) {
            continue;
        }

        chosen.present = true;
        chosen.raw_color = raw;
        chosen.palette = cgb_mode
            ? (flags & OAM_CGB_PALETTE)
            : ((flags & OAM_PALETTE) ? 1 : 0);
        chosen.dmg_pal = (flags & OAM_PALETTE) ? 1 : 0;
        chosen.behind_bg = (flags & OAM_PRIORITY) != 0;
        chosen_x = screen_x;
        chosen_index = oam_index;
    }
    return chosen;
}

/* Passo 2: decide se a PPU emulada gera pixels.
 * Comparar: gera (e a referencia). Nativo: nao gera.
 * Emulado: gera (caminho de referencia). */
static bool ppu_skip_emulated_pixels(const GBPPU* ppu) {
    switch (ppu_native_get_mode()) {
        case PPU_RENDERER_COMPARE: return false;
        case PPU_RENDERER_NATIVE:  return true;
        default:                   return false;
    }
}

static void ppu_render_dot(GBPPU* ppu, GBContext* ctx) {
    if (ppu_skip_emulated_pixels(ppu)) return;
    const size_t framebuffer_index =
        (size_t)ppu->ly * GB_SCREEN_WIDTH + ppu->draw_x;
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const DotBackgroundPixel bg = ppu_fetch_background_dot(ppu, ctx);
    const DotObjectPixel obj = ppu_fetch_object_dot(ppu, ctx);
    bool use_object = obj.present;

    if (use_object && bg.raw_color != 0) {
        if (cgb_mode) {
            if ((ppu->lcdc & LCDC_BG_ENABLE) &&
                (bg.priority || obj.behind_bg)) {
                use_object = false;
            }
        } else if (obj.behind_bg) {
            use_object = false;
        }
    }

    if (use_object) {
        const uint8_t palette_reg = obj.dmg_pal ? ppu->obp1 : ppu->obp0;
        ppu->framebuffer[framebuffer_index] = cgb_mode
            ? obj.raw_color
            : apply_palette(obj.raw_color, palette_reg);
        ppu->color_framebuffer[framebuffer_index] =
            resolve_obj_color(ppu, ctx, obj.palette, obj.raw_color, palette_reg);
    } else {
        ppu->framebuffer[framebuffer_index] = cgb_mode
            ? bg.raw_color
            : apply_palette(bg.raw_color, ppu->bgp);
        ppu->color_framebuffer[framebuffer_index] =
            resolve_bg_color(ppu, ctx, bg.palette, bg.raw_color, ppu->bgp);
    }
}

/* Render a sprite-free run that stays within one fetched background/window
 * tile row. The caller proves that no window, object-fetch, STAT, or mode
 * boundary can occur inside the run. Palette resolution remains per pixel so
 * this produces the same framebuffer values as ppu_render_dot(). */
static void ppu_render_background_span(GBPPU* ppu,
                                       GBContext* ctx,
                                       uint32_t span) {
    if (ppu_skip_emulated_pixels(ppu)) return;
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool bg_enabled = cgb_mode || (ppu->lcdc & LCDC_BG_ENABLE) != 0;
    const bool in_window = ppu->window_active_line;
    uint16_t tilemap_addr = 0;
    int source_x = 0;
    int source_y = 0;
    uint8_t tile_bank = 0;
    uint8_t palette = 0;
    uint8_t lo = 0;
    uint8_t hi = 0;
    bool flip_x = false;

    if (in_window) {
        source_x = ppu->window_pixel_x;
        source_y = ppu->window_line;
        tilemap_addr = get_window_tilemap_addr(ppu->lcdc);
    } else {
        source_x = (ppu->draw_x + ppu->scx) & 0xFF;
        source_y = (ppu->ly + ppu->scy) & 0xFF;
        tilemap_addr = get_bg_tilemap_addr(ppu->lcdc);
    }

    if (bg_enabled) {
        const uint8_t tile_x = (uint8_t)((source_x >> 3) & 31);
        const uint8_t tile_y = (uint8_t)((source_y >> 3) & 31);
        const uint16_t map_entry =
            (uint16_t)(tilemap_addr + tile_y * 32u + tile_x);
        const uint8_t tile_idx = vram_read_bank(ctx, 0, map_entry);
        uint8_t pixel_y = (uint8_t)(source_y & 7);
        uint8_t attr = 0;

        if (cgb_mode) {
            attr = vram_read_bank(ctx, 1, map_entry);
            palette = attr & OAM_CGB_PALETTE;
            tile_bank = (attr & OAM_CGB_BANK) ? 1u : 0u;
            flip_x = (attr & OAM_FLIP_X) != 0;
            if (attr & OAM_FLIP_Y) {
                pixel_y = (uint8_t)(7u - pixel_y);
            }
        }

        const uint16_t tile_addr =
            get_tile_data_addr(ppu->lcdc, tile_idx, false);
        lo = vram_read_bank(
            ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2u));
        hi = vram_read_bank(
            ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2u + 1u));
    }

    for (uint32_t offset = 0; offset < span; ++offset) {
        uint8_t raw_color = 0;
        if (bg_enabled) {
            uint8_t pixel_x = (uint8_t)((source_x + (int)offset) & 7);
            if (flip_x) {
                pixel_x = (uint8_t)(7u - pixel_x);
            }
            const uint8_t bit = (uint8_t)(7u - pixel_x);
            raw_color = (uint8_t)(((lo >> bit) & 1u) |
                                  (((hi >> bit) & 1u) << 1u));
        }

        const size_t framebuffer_index =
            (size_t)ppu->ly * GB_SCREEN_WIDTH + ppu->draw_x + offset;
        ppu->framebuffer[framebuffer_index] = cgb_mode
            ? raw_color
            : apply_palette(raw_color, ppu->bgp);
        ppu->color_framebuffer[framebuffer_index] =
            resolve_bg_color(ppu, ctx, palette, raw_color, ppu->bgp);
    }
}

static void ppu_select_line_sprites(GBPPU* ppu, const GBContext* ctx) {
    ppu->visible_sprite_count = 0;
    ppu->line_sprite_height = (ppu->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    for (uint8_t index = 0;
         index < 40;
         ++index) {
        const size_t offset = (size_t)index * 4u;
        const uint8_t y = ctx->oam[offset];
        const int screen_y = (int)y - 16;
        if ((int)ppu->ly < screen_y ||
            (int)ppu->ly >= screen_y + ppu->line_sprite_height) {
            continue;
        }
        if (ppu->visible_sprite_count < 40) {
            const uint8_t slot = ppu->visible_sprite_count++;
            ppu->visible_sprite_indices[slot] = index;
            ppu->visible_sprite_x[slot] = ctx->oam[offset + 1u];
            ppu->visible_sprite_y[slot] = y;
        }
    }
}

static void ppu_begin_dot_transfer(GBPPU* ppu, const GBContext* ctx) {
    latch_scanline_registers(ppu);
    // Salva o scroll lido da memória ERAM no momento exato do latch da scanline/frame
    if (ctx && ctx->eram && ctx->eram_size >= 0x0904) {
        ppu->frame_widescreen_scroll_x = (uint16_t)ctx->eram[0x0902u] << 8u | ctx->eram[0x0903u];
        ppu->frame_widescreen_scroll_y = (uint16_t)ctx->eram[0x0900u] << 8u | ctx->eram[0x0901u];
    }
    ppu_select_line_sprites(ppu, ctx);
    ppu->mode_cycles = 0;
    ppu->mode3_length = 0;
    ppu->draw_x = 0;
    ppu->draw_startup = (uint8_t)(12u + (ppu->scx & 7u));
    ppu->draw_stall = 0;
    ppu->fetched_sprite_mask = 0;
    ppu->considered_bg_tiles = 0;
    ppu->window_active_line = false;
    ppu->window_rendered_line = false;
    ppu->window_pixel_x = 0;
    if (ppu->ly == ppu->wy) {
        ppu->window_y_triggered = true;
    }
    ppu_native_begin_line(ppu, ctx);
}

static bool ppu_try_start_window(GBPPU* ppu, const GBContext* ctx) {
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool bg_visible = cgb_mode || (ppu->lcdc & LCDC_BG_ENABLE) != 0;
    if (ppu->window_active_line ||
        !ppu->window_y_triggered ||
        !(ppu->lcdc & LCDC_WINDOW_ENABLE) ||
        !bg_visible ||
        ppu->wx > 166) {
        return false;
    }

    int trigger_x = (int)ppu->wx - 7;
    if (trigger_x < 0) trigger_x = 0;
    if ((int)ppu->draw_x != trigger_x) {
        return false;
    }

    ppu->window_active_line = true;
    ppu->window_rendered_line = true;
    ppu->window_triggered = true;
    ppu->window_pixel_x = ppu->wx < 7 ? (uint8_t)(7 - ppu->wx) : 0;
    /* WX=0 with fractional SCX skips one of the normal six restart dots. */
    ppu->draw_stall = (uint8_t)((ppu->wx == 0 && (ppu->scx & 7)) ? 4 : 5);
    return true;
}

static unsigned ppu_begin_object_fetches(GBPPU* ppu, const GBContext* ctx) {
    unsigned penalty = 0;
    const bool cgb_mode = ppu_is_cgb_mode(ctx);

    for (uint8_t slot = 0; slot < ppu->visible_sprite_count; ++slot) {
        const uint64_t bit = (uint64_t)1u << slot;
        if (ppu->fetched_sprite_mask & bit) {
            continue;
        }
        const uint8_t oam_x = ppu->visible_sprite_x[slot];
        const int screen_x = (int)oam_x - 8;
        const int trigger_x = screen_x < 0 ? 0 : screen_x;
        if (trigger_x >= GB_SCREEN_WIDTH || trigger_x != (int)ppu->draw_x) {
            continue;
        }

        ppu->fetched_sprite_mask |= bit;
        if (!cgb_mode && !(ppu->lcdc & LCDC_OBJ_ENABLE)) {
            continue;
        }
        int source_x = ppu->window_active_line
            ? ppu->window_pixel_x
            : (screen_x + ppu->scx) & 0xFF;
        const unsigned tile = (unsigned)((source_x >> 3) & 31) |
                              (ppu->window_active_line ? 32u : 0u);
        const uint64_t tile_bit = UINT64_C(1) << tile;
        if (!(ppu->considered_bg_tiles & tile_bit)) {
            const int pixels_right = 7 - (source_x & 7);
            if (pixels_right > 2) {
                penalty += (unsigned)(pixels_right - 2);
            }
            ppu->considered_bg_tiles |= tile_bit;
        }
        penalty += 6;
    }
    return penalty;
}

static uint32_t ppu_draw_stable_span(GBPPU* ppu,
                                     GBContext* ctx,
                                     uint32_t available_dots) {
    if (available_dots < 2u ||
        ppu->draw_startup != 0u ||
        ppu->draw_stall != 0u ||
        ppu->visible_mode != PPU_MODE_DRAW ||
        ppu->visible_sprite_count != 0u ||
        ctx->ppu_trace_file != NULL ||
        ppu->draw_x + 1u >= GB_SCREEN_WIDTH) {
        return 0;
    }

    uint32_t span = available_dots;
    const uint32_t before_final_pixel =
        (uint32_t)GB_SCREEN_WIDTH - 1u - ppu->draw_x;
    if (span > before_final_pixel) {
        span = before_final_pixel;
    }

    if (!ppu->window_active_line) {
        const bool cgb_mode = ppu_is_cgb_mode(ctx);
        const bool bg_visible =
            cgb_mode || (ppu->lcdc & LCDC_BG_ENABLE) != 0;
        const bool window_can_start =
            ppu->window_y_triggered &&
            (ppu->lcdc & LCDC_WINDOW_ENABLE) != 0 &&
            bg_visible &&
            ppu->wx <= 166;
        if (window_can_start) {
            int trigger_x = (int)ppu->wx - 7;
            if (trigger_x < 0) {
                trigger_x = 0;
            }
            if ((int)ppu->draw_x == trigger_x) {
                return 0;
            }
            if ((int)ppu->draw_x < trigger_x) {
                const uint32_t until_window =
                    (uint32_t)(trigger_x - (int)ppu->draw_x);
                if (span > until_window) {
                    span = until_window;
                }
            }
        }
    }

    const int source_x = ppu->window_active_line
        ? ppu->window_pixel_x
        : (ppu->draw_x + ppu->scx) & 0xFF;
    const uint32_t until_tile_boundary =
        8u - (uint32_t)(source_x & 7);
    if (span > until_tile_boundary) {
        span = until_tile_boundary;
    }
    if (span < 2u) {
        return 0;
    }

    ppu_render_background_span(ppu, ctx, span);
    ppu->draw_x = (uint16_t)(ppu->draw_x + span);
    if (ppu->window_active_line) {
        ppu->window_pixel_x = (uint16_t)(ppu->window_pixel_x + span);
    }
    return span;
}

static bool ppu_draw_one_dot(GBPPU* ppu, GBContext* ctx) {
    if (ppu->draw_startup > 0) {
        ppu->draw_startup--;
        return false;
    }
    if (ppu->draw_stall > 0) {
        ppu->draw_stall--;
        return false;
    }
    if (ppu_try_start_window(ppu, ctx)) {
        return false;
    }

    const unsigned object_penalty = ppu_begin_object_fetches(ppu, ctx);
    if (object_penalty > 0) {
        ppu->draw_stall = (uint8_t)(object_penalty - 1u);
        return false;
    }

    ppu_render_dot(ppu, ctx);
    ppu->draw_x++;
    if (ppu->window_active_line) {
        ppu->window_pixel_x++;
    }
    return ppu->draw_x >= GB_SCREEN_WIDTH;
}
/* ============================================================================
 * Widescreen (16:9) background extension
 *
 * Everything above this point reproduces real GB hardware faithfully,
 * dot-for-dot, at 160x144. Real hardware never draws more than that. This
 * section adds a *separate*, display-only wide framebuffer: the real 160
 * columns are copied in unchanged, and extra background-only columns are
 * added on each side using the same scroll/tilemap wraparound math the
 * hardware itself uses (SCX/SCY against a 256x256 wrapping BG map).
 *
 * The window layer and sprites are intentionally NOT extended - they're
 * positioned relative to the real 160px screen (HUD, the player character,
 * etc.) and stretching their logic sideways would look wrong or break HUD
 * elements.
 *
 * Because this reveals background tiles that were never meant to be visible
 * on real hardware, the margins can occasionally show unused/garbage tile
 * data for a given game or level - this is inherent to the technique, not a
 * bug, which is why the feature is opt-in and off by default.
 * ========================================================================== */

typedef struct {
    uint8_t raw_color;
    uint8_t palette;
    bool priority;
    uint32_t rgba;
    /* Passo 5d: origem do pixel, para as laterais em HD */
    uint16_t tile_ref;  /* 1 + banco * 384 + tile (cache do ppu_native); 0 = sem tile */
    uint8_t uvf;        /* coluna | linha << 3 | 0x40 flip X | 0x80 flip Y */
    uint8_t block;      /* passo 3D-2: bloco da fase + 1 (0 = fora do layout) */
    uint8_t block_down; /* passo 3D-2b: bloco logo abaixo + 1 (ondas sobre a água) */
} WidescreenBackgroundPixel;

typedef struct {
    bool present;
    uint8_t raw_color;
    uint8_t palette;
    uint8_t dmg_pal;    /* bit 4 do OAM (0 = OBP0, 1 = OBP1), também no modo CGB */
    bool behind_bg;
    int screen_x;
    uint8_t oam_index;
    uint16_t tile_ref;  /* passo 5d */
    uint8_t uvf;
} WidescreenObjectPixel;

/* Passo 5d: coluna/linha no espaço da tela (antes do flip) + flips. */
static inline uint8_t ws_uvf(unsigned u, unsigned v, uint8_t flags) {
    return (uint8_t)((u & 7u) | ((v & 7u) << 3) |
                     ((flags & OAM_FLIP_X) ? 0x40u : 0u) |
                     ((flags & OAM_FLIP_Y) ? 0x80u : 0u));
}

static inline uint16_t ws_tile_ref(uint8_t bank, uint16_t tile_addr) {
    return (uint16_t)(1u + (unsigned)bank * 384u + (unsigned)((tile_addr - 0x8000u) >> 4));
}

#define WIDESCREEN_ATTRIBUTE_CACHE_CAPACITY (32u * 32u)

typedef struct {
    uint16_t world_x;
    uint16_t world_y;
    uint16_t next;
    uint8_t attributes;
} WidescreenBlockAttributeCandidate;

typedef struct {
    uint16_t heads[0x80][4];
    WidescreenBlockAttributeCandidate candidates[WIDESCREEN_ATTRIBUTE_CACHE_CAPACITY];
    uint16_t count;
} WidescreenBlockAttributeCache;

static int ppu_unwrap_widescreen_map_coordinate(int map_tile, int reference) {
    int coordinate = (reference & ~0xFF) + map_tile * 8;
    if (coordinate - reference > 128) {
        coordinate -= 256;
    } else if (reference - coordinate > 128) {
        coordinate += 256;
    }
    return coordinate;
}

static bool ppu_find_widescreen_block_attributes(
    const WidescreenBlockAttributeCache* cache,
    uint8_t block_id,
    size_t tile_in_block,
    int world_x,
    int world_y,
    uint8_t* attributes) {
    uint16_t candidate_index = cache->heads[block_id][tile_in_block];
    int best_distance = 0x7FFFFFFF;
    bool found = false;
    while (candidate_index != UINT16_MAX) {
        const WidescreenBlockAttributeCandidate* candidate =
            &cache->candidates[candidate_index];
        const int distance = abs((int)candidate->world_x - world_x) +
            abs((int)candidate->world_y - world_y);
        if (distance < best_distance) {
            best_distance = distance;
            *attributes = candidate->attributes;
            found = true;
        }
        candidate_index = candidate->next;
    }
    return found;
}

/* Passo 13a: o tile do mundo com canto (x0, y0) esta na parte atualizada do
 * tilemap (a tela e uma coluna/linha alem dela, passo 7f)? Decidido pelo tile
 * inteiro: antes a divisa (camera - 8) cortava o tile no meio e parte dos
 * pixels usava a cor da VRAM e parte a cor resolvida. Vale para o desenho e
 * para o aprendizado do cache. */
static inline bool ppu_ws_tile_fresh(const GBPPU* ppu, int x0, int y0) {
    const int left = (int)ppu->frame_widescreen_scroll_x - 0x50 - 8;
    const int top = (int)ppu->frame_widescreen_scroll_y - 0x48 - 8;
    return x0 + 8 > left && x0 < left + GB_SCREEN_WIDTH + 16 &&
           y0 + 8 > top && y0 < top + GB_SCREEN_HEIGHT + 16;
}

/* Passo 7g: pagina da tabela DX que bate com o que esta na tela.
 * A pagina escolhida pelo numero da fase ($A800) erra em salas com outro jogo
 * de tiles (caverna depois da porta). A cada frame, os tiles visiveis (com o
 * atributo real da VRAM) votam na pagina que melhor os explica. */
#define DX_TABLE_BASE 0x84000u
#define DX_PAGE_FIRST 0x40
#define DX_PAGE_LAST 0x7F
static int g_dx_page = -1;          /* -1 = sem calibracao confiavel */
/* Passo 7h: diagnostico, pinta cada pixel da lateral pela origem da cor. */
static bool g_ws_attr_debug = false;
void ppu_set_widescreen_attr_debug(bool enabled) { g_ws_attr_debug = enabled; }
bool ppu_get_widescreen_attr_debug(void) { return g_ws_attr_debug; }
int ppu_get_dx_page(void) { return g_dx_page; }
static uint32_t g_dx_page_misses = 0;
/* Passo 7k: o que cada tile mostra na tela neste frame. A pagina calibrada
 * acerta 90% dos tiles, mas pode errar alguns (ex.: areia e pedras do chao
 * da praia saiam azuis na lateral). Se a tela mostra o tile com uma paleta
 * so, e ela contradiz a tabela, vale a tela.
 * estado: 0 = nao aparece, 1 = um atributo so, 2 = mais de um. */
static uint8_t g_dx_screen_state[256];
static uint8_t g_dx_screen_attr[256];

static bool ppu_dx_rom_ok(const GBContext* ctx) {
    return ctx && ctx->rom && ctx->rom_size >= 0x100000u && ctx->rom[0x143] == 0xC0;
}

static bool ppu_dx_attribute_from_page(const GBContext* ctx, int page, uint8_t tile, uint8_t* attr) {
    const size_t offset = DX_TABLE_BASE + ((size_t)page << 8) + tile;
    if (!ppu_dx_rom_ok(ctx) || offset >= ctx->rom_size) return false;
    const uint8_t value = ctx->rom[offset];
    if (value >= 8) return false;
    *attr = value;
    return true;
}

/* tiles[i] e pals[i] (paleta 0-7) vindos da parte atualizada do tilemap. */
static void ppu_dx_calibrate(const GBContext* ctx, const uint8_t* tiles, const uint8_t* pals, int n) {
    if (!ppu_dx_rom_ok(ctx) || n < 6) return;
    int best_page = -1, best_score = -1, best_valid = 0;
    /* Passo 7m: empate. Antes ganhava a pagina de numero mais baixo, e a tela
     * (poucos tiles distintos) empata em varias paginas; a escolhida podia
     * ser de outra fase e errar os tiles que ainda nao apareceram (barris e
     * chao da praia saiam brancos/azuis). Agora, no empate, ganha a pagina
     * da fase ($A800), depois a pagina atual, depois a mais baixa. */
    const int level_page = (ctx->eram && ctx->eram_size > 0x800u && ctx->eram[0x800] <= 0x2D)
        ? 0x41 + ctx->eram[0x800] : -1;
    int best_rank = -1, ties = 0;
    int level_score = -1, level_valid = 0;   /* Passo 7n */
    for (int page = DX_PAGE_FIRST; page <= DX_PAGE_LAST; ++page) {
        const size_t base = DX_TABLE_BASE + ((size_t)page << 8);
        if (base + 0x100u > ctx->rom_size) break;
        int score = 0, valid = 0;
        for (int i = 0; i < n; ++i) {
            const uint8_t v = ctx->rom[base + tiles[i]];
            if (v >= 8) continue;
            valid++;
            if (v == pals[i]) score++;
        }
        const int rank = (page == level_page) ? 2 : (page == g_dx_page) ? 1 : 0;
        if (page == level_page) {   /* Passo 7n */
            level_score = score;
            level_valid = valid;
        }
        if (score > best_score) {
            ties = 1;
        } else if (score == best_score) {
            ties++;
        }
        if (score > best_score || (score == best_score && rank > best_rank)) {
            best_score = score;
            best_page = page;
            best_valid = valid;
            best_rank = rank;
        }
    }
    /* Passo 7n: na praia da fase 1 (fase 10), a pagina 52 explicava 20/22
     * tiles da tela e a da fase (51) um pouco menos, mas so a 51 acerta os
     * barris que ainda nao apareceram. A pagina da fase fica, mesmo perdendo,
     * se perde por ate 3 tiles e explica pelo menos 75% da tela. Numa sala
     * com outro jogo de tiles (caverna) ela explica bem menos e perde. */
    bool level_kept = false;
    if (level_page >= 0 && best_page != level_page && level_valid >= 6 &&
        level_score * 4 >= level_valid * 3 && best_score - level_score <= 3) {
        best_page = level_page;
        best_score = level_score;
        best_valid = level_valid;
        level_kept = true;
    }
    /* So troca com folga: 90% dos tiles com entrada na tabela batendo
     * (75% para a pagina da fase mantida acima). */
    if (best_page >= 0 && best_valid >= 6 &&
        (level_kept || best_score * 10 >= best_valid * 9)) {
        if (best_page != g_dx_page) {
            fprintf(stderr, "[DX] tabela de paletas das laterais: pagina %02X (fase %02X, %d/%d tiles, %d paginas empatadas%s)\n",
                    best_page, (ctx->eram && ctx->eram_size > 0x800u) ? ctx->eram[0x800] : 0xFF,
                    best_score, best_valid, ties,
                    best_page == level_page ? ", e a da fase" : "");
            if (level_page >= 0 && level_score >= 0) {   /* Passo 7n */
                fprintf(stderr, "[DX]   pagina da fase %02X: %d/%d tiles%s\n",
                        level_page, level_score, level_valid,
                        level_kept ? " (mantida: perdeu por pouco)" : "");
            }
        }
        g_dx_page = best_page;
        g_dx_page_misses = 0;
    } else if (g_dx_page >= 0 && ++g_dx_page_misses > 30) {
        g_dx_page = -1;   /* a tela deixou de bater por meio segundo: volta ao normal */
    }
}

/* Tabela de paleta CGB do patch DX (banco ROM 0x22). Retorna false se a ROM
 * não for a DX, se a fase for desconhecida ou se o tile usa rotina especial. */
static bool ppu_dx_attribute_from_rom(const GBContext* ctx,
                                      uint8_t tile,
                                      uint8_t* attr) {
    if (!ctx || !ctx->rom || ctx->rom_size < 0x100000u ||
        ctx->rom[0x143] != 0xC0 ||
        !ctx->eram || ctx->eram_size <= 0x800u) {
        return false;
    }
    const uint8_t level = ctx->eram[0x800];
    if (level > 0x2D) {
        return false;
    }
    const size_t offset = 0x84000u + ((size_t)(0x41u + level) << 8) + tile;
    if (offset >= ctx->rom_size) {
        return false;
    }
    const uint8_t value = ctx->rom[offset];
    if (value >= 8) {
        return false;
    }
    *attr = value;
    return true;
}


/* Passo 7j: para um bloco que nunca apareceu, a cor do mesmo tile na parte
 * atualizada do tilemap (perto da tela), na mesma linha ou na mais proxima,
 * pelo lado da lateral. Ex.: o ceu da lateral direita continua o ceu que
 * esta na ultima coluna da tela. Tabelas montadas uma vez por frame. */
static uint64_t s_ws_near_frame = UINT64_MAX;
static uint8_t s_ws_near_left[32][256];    /* atributo + 1 (0 = nao tem) */
static uint8_t s_ws_near_right[32][256];

static void ppu_ws_build_near(const GBPPU* ppu, const GBContext* ctx, uint16_t tilemap_addr) {
    memset(s_ws_near_left, 0, sizeof(s_ws_near_left));
    memset(s_ws_near_right, 0, sizeof(s_ws_near_right));
    const int left = (int)ppu->frame_widescreen_scroll_x - 0x50 - 8;
    const int top = (int)ppu->frame_widescreen_scroll_y - 0x48 - 8;
    for (int wy = top; wy < top + GB_SCREEN_HEIGHT + 16; wy += 8) {
        const int mr = (wy >> 3) & 31;
        for (int wx = left; wx < left + GB_SCREEN_WIDTH + 16; wx += 8) {
            const uint16_t e = (uint16_t)(tilemap_addr + mr * 32 + ((wx >> 3) & 31));
            const uint8_t t = vram_read_bank(ctx, 0, e);
            const uint8_t a = (uint8_t)(vram_read_bank(ctx, 1, e) + 1u);
            if (!s_ws_near_left[mr][t]) s_ws_near_left[mr][t] = a;   /* mais a esquerda */
            s_ws_near_right[mr][t] = a;                              /* mais a direita */
        }
    }
}

static bool ppu_ws_nearest_fresh_attr(const GBPPU* ppu, const GBContext* ctx, uint16_t tilemap_addr,
                                      uint8_t tile_idx, int world_x, int world_y, uint8_t* attr) {
    if (ctx->completed_frames != s_ws_near_frame) {
        s_ws_near_frame = ctx->completed_frames;
        ppu_ws_build_near(ppu, ctx, tilemap_addr);
    }
    const int center = (int)ppu->frame_widescreen_scroll_x - 0x50 + GB_SCREEN_WIDTH / 2;
    uint8_t (*tbl)[256] = (world_x < center) ? s_ws_near_left : s_ws_near_right;
    const int mr = (world_y >> 3) & 31;
    for (int k = 0; k < 20; ++k) {
        const uint8_t a = tbl[(mr + k) & 31][tile_idx];
        if (a) { *attr = (uint8_t)(a - 1u); return true; }
        if (k) {
            const uint8_t b = tbl[(mr - k) & 31][tile_idx];
            if (b) { *attr = (uint8_t)(b - 1u); return true; }
        }
    }
    return false;
}

/* ============================================================================
 * Passo 13b: mapa largo das laterais
 *
 * A cor de cada tile do mundo nas laterais (tile, atributo CGB, de onde veio
 * a cor) é resolvida uma vez e guardada aqui; o desenho de cada pixel só lê.
 * Depois do 13a a resolução já era por tile; o mapa só evita refazê-la a
 * cada pixel e a cada linha, e é o que um desenho na GPU vai ler.
 *
 * Uma entrada vale enquanto nada que a resolução usa mudou: a geração
 * (g_ws_map_gen) muda quando o cache de atributos é remontado (câmera, LCDC,
 * modo, escrita no tilemap, frame novo), quando os blocos aprendidos são
 * zerados (troca de sala) e no carregamento de savestate. O bloco, o tile e a
 * fase ($A800) são conferidos na leitura, porque o jogo pode mudar o layout
 * (WRAM) ou a tabela de blocos (ERAM) no meio do frame sem nenhum aviso.
 *
 * Origem (source), a mesma do diagnóstico 7h: 0 VRAM perto da tela, 1 cache
 * do frame, 2 tabela DX calibrada, 3 bloco aprendido, 4 tabela pela fase,
 * 5 palpite do tile, 6 nada, 7 mesmo tile na tela.
 * Camada (layer): reservada para a profundidade do modo 3D (0 = fundo).
 * ========================================================================== */
typedef struct {
    uint32_t gen;        /* 0 = vazio */
    int16_t tx, ty;      /* tile do mundo (world >> 3) */
    uint8_t block_id;
    uint8_t tile_idx;
    uint8_t level;       /* $A800 quando foi resolvido */
    uint8_t attr;        /* atributo CGB (paleta, banco, flips, prioridade) */
    uint8_t source;
    uint8_t layer;
    bool map_current;    /* atributo veio da VRAM (vale a prioridade do BG) */
} WsMapTile;

#define WS_MAP_SIDE 64u   /* janela das laterais < 64 tiles nas duas direções */
static WsMapTile g_ws_map[WS_MAP_SIDE * WS_MAP_SIDE];
static uint32_t g_ws_map_gen = 1;

static void ppu_ws_map_invalidate(void) {
    if (++g_ws_map_gen == 0) {
        g_ws_map_gen = 1;
        memset(g_ws_map, 0, sizeof(g_ws_map));
    }
}

/* Resolução de um tile (código do 7e-7n/13a, sem mudança de regra). */
static void ppu_ws_resolve_tile(const GBPPU* ppu,
                                const GBContext* ctx,
                                int tile_x0,
                                int tile_y0,
                                uint8_t block_id,
                                size_t tile_in_block,
                                uint8_t tile_idx,
                                const WidescreenBlockAttributeCache* attribute_cache,
                                WsMapTile* out) {
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const uint16_t tilemap_addr = get_bg_tilemap_addr(ppu->latched_lcdc);
    const uint16_t map_entry = (uint16_t)(tilemap_addr +
        (((uint32_t)tile_y0 >> 3) & 31u) * 32u +
        (((uint32_t)tile_x0 >> 3) & 31u));
    uint8_t attr = cgb_mode ? vram_read_bank(ctx, 1, map_entry) : 0;
    /* Passo 7f: o tilemap da VRAM so esta atualizado na tela e uma coluna/
     * linha alem dela; mais longe ele guarda restos de onde a camera passou.
     * Conferir so o numero do tile nao basta (o tile liso e usado por varios
     * blocos com paletas diferentes): fora dessa faixa, nao usa a VRAM.
     * Passo 13a: decidido pelo canto do tile. */
    const bool map_fresh = ppu_ws_tile_fresh(ppu, tile_x0, tile_y0);
    const bool has_current_map_attribute =
        !cgb_mode || (map_fresh && vram_read_bank(ctx, 0, map_entry) == tile_idx);

    /* Passo 7e: primeiro o que o jogo mostrou na tela para este bloco (cache
     * do frame e blocos aprendidos); a tabela da ROM DX e o palpite por tile
     * ficam para blocos que ainda nao apareceram. A tabela e indexada so pela
     * fase ($A800) e erra em salas com outro jogo de tiles (ex.: a caverna
     * depois de uma porta), pintando o bloco com a paleta de outro. */
    int attr_source = 0;   /* passo 7h: 0 = VRAM perto da tela */
    if (cgb_mode && !has_current_map_attribute) {
        bool found = attribute_cache &&
            ppu_find_widescreen_block_attributes(
                attribute_cache, block_id, tile_in_block,
                tile_x0, tile_y0, &attr);   /* passo 13a: distancia do tile */
        attr_source = found ? 1 : 6;
        uint8_t rom_attr;
        /* Passo 7g: a tabela DX na pagina que bate com a tela vem antes do
         * que foi aprendido (que pode ser de outra sala). */
        if (!found && g_dx_page >= 0 &&
            ppu_dx_attribute_from_page(ctx, g_dx_page, tile_idx, &rom_attr)) {
            attr = rom_attr;
            found = true;
            attr_source = 2;
            /* Passo 7k: a tela mostra este tile com outra paleta (e so com ela). */
            if (g_dx_screen_state[tile_idx] == 1 &&
                (g_dx_screen_attr[tile_idx] & OAM_CGB_PALETTE) != rom_attr) {
                attr = g_dx_screen_attr[tile_idx];
                attr_source = 7;
            }
        }
        if (!found && ppu->ws_block_known[block_id][tile_in_block]) {
            attr = ppu->ws_block_attr[block_id][tile_in_block];
            found = true;
            attr_source = 3;
        }
        if (!found && ppu_ws_nearest_fresh_attr(ppu, ctx, tilemap_addr, tile_idx,
                                                tile_x0, tile_y0, &attr)) {
            found = true;
            attr_source = 7;   /* passo 7j */
        }
        if (!found) {
            if (ppu_dx_attribute_from_rom(ctx, tile_idx, &rom_attr)) {
                attr = rom_attr;
                attr_source = 4;
            } else if (ppu->ws_tile_known[tile_idx]) {
                attr = ppu->ws_tile_attr[tile_idx];
                attr_source = 5;
            }
        }
    }
    out->attr = attr;
    out->source = (uint8_t)attr_source;
    out->map_current = has_current_map_attribute;
    out->layer = 0;
}

/* Entrada do mapa largo para o tile do mundo com canto (tile_x0, tile_y0),
 * resolvida agora se não estiver lá ou estiver velha. */
static const WsMapTile* ppu_ws_map_tile(const GBPPU* ppu,
                                        const GBContext* ctx,
                                        int tile_x0,
                                        int tile_y0,
                                        uint8_t block_id,
                                        size_t tile_in_block,
                                        uint8_t tile_idx,
                                        const WidescreenBlockAttributeCache* attribute_cache) {
    static WsMapTile scratch;
    const int tx = tile_x0 >> 3, ty = tile_y0 >> 3;
    const uint8_t level = ctx->eram_size > 0x800u ? ctx->eram[0x800] : 0u;
    if (!attribute_cache) {   /* sem cache não guarda (não acontece hoje) */
        ppu_ws_resolve_tile(ppu, ctx, tile_x0, tile_y0, block_id, tile_in_block,
                            tile_idx, attribute_cache, &scratch);
        return &scratch;
    }
    WsMapTile* e = &g_ws_map[((unsigned)ty & (WS_MAP_SIDE - 1u)) * WS_MAP_SIDE +
                             ((unsigned)tx & (WS_MAP_SIDE - 1u))];
    if (e->gen != g_ws_map_gen || e->tx != tx || e->ty != ty ||
        e->block_id != block_id || e->tile_idx != tile_idx || e->level != level) {
        ppu_ws_resolve_tile(ppu, ctx, tile_x0, tile_y0, block_id, tile_in_block,
                            tile_idx, attribute_cache, e);
        e->gen = g_ws_map_gen;
        e->tx = (int16_t)tx;
        e->ty = (int16_t)ty;
        e->block_id = block_id;
        e->tile_idx = tile_idx;
        e->level = level;
    }
    return e;
}

static bool ppu_fetch_warioland_world_pixel(
    const GBPPU* ppu,
    const GBContext* ctx,
    int world_x,
    int world_y,
    const WidescreenBlockAttributeCache* attribute_cache,
    WidescreenBackgroundPixel* pixel) {
    if (world_x < 0 || world_y < 0 || world_x >= 0x1000 || world_y >= 0x0200) {
        return false;
    }

    const int block_x = world_x >> 4;
    const int block_y = world_y >> 4;
    const size_t layout_offset = (size_t)block_y * 0x100u + (size_t)block_x;
    const uint8_t block_id = ctx->wram[layout_offset] & 0x7Fu;
    const size_t block_offset = 0x0D00u + (size_t)block_id * 4u;
    if (block_offset + 3u >= ctx->eram_size) {
        return false;
    }

    const int tile_x = (world_x >> 3) & 1;
    const int tile_y = (world_y >> 3) & 1;
    const uint8_t tile_idx = ctx->eram[block_offset + (size_t)tile_y * 2u + tile_x];
    const size_t tile_in_block = (size_t)tile_y * 2u + tile_x;

    /* Passo 13b: cor do tile pelo mapa largo (resolvida uma vez por tile). */
    const WsMapTile* mt = ppu_ws_map_tile(ppu, ctx, world_x & ~7, world_y & ~7,
                                          block_id, tile_in_block, tile_idx,
                                          attribute_cache);
    const uint8_t attr = mt->attr;
    const int attr_source = mt->source;

    int pixel_x = world_x & 7;
    int pixel_y = world_y & 7;
    const uint8_t tile_bank = (attr & OAM_CGB_BANK) ? 1u : 0u;
    pixel->palette = attr & OAM_CGB_PALETTE;
    pixel->priority = mt->map_current && (attr & OAM_PRIORITY) != 0;
    if (attr & OAM_FLIP_X) {
        pixel_x = 7 - pixel_x;
    }
    if (attr & OAM_FLIP_Y) {
        pixel_y = 7 - pixel_y;
    }
    const uint16_t tile_addr = get_tile_data_addr(
        ppu->latched_lcdc,
        tile_idx,
        false);
    const uint8_t lo = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2));
    const uint8_t hi = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2 + 1));
    const int bit = 7 - pixel_x;
    pixel->raw_color = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1u));
    pixel->rgba = rgb555_to_rgba(
        resolve_bg_color(ppu, ctx, pixel->palette, pixel->raw_color, ppu->latched_bgp));
    if (g_ws_attr_debug && attr_source > 0) {
        /* Passo 7h: 1 cache verde, 2 tabela calibrada azul, 3 bloco aprendido
         * amarelo, 4 tabela pela fase magenta, 5 palpite do tile vermelho,
         * 6 nada (cinza). Sem tinta = VRAM perto da tela. */
        static const uint32_t kTint[8] = {0, 0xFF00C000u, 0xFF2060FFu, 0xFFFFE000u,
                                          0xFFFF00FFu, 0xFFFF2020u, 0xFF808080u,
                                          0xFF00E0E0u /* 7: vizinho na tela */};
        const uint32_t c = pixel->rgba, t = kTint[attr_source];
        const uint32_t r = (((c >> 16) & 0xFFu) + ((t >> 16) & 0xFFu)) / 2u;
        const uint32_t g = (((c >> 8) & 0xFFu) + ((t >> 8) & 0xFFu)) / 2u;
        const uint32_t b = ((c & 0xFFu) + (t & 0xFFu)) / 2u;
        pixel->rgba = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    pixel->tile_ref = ws_tile_ref(tile_bank, tile_addr);
    pixel->uvf = ws_uvf((unsigned)world_x & 7u, (unsigned)world_y & 7u, attr);
    pixel->block = (uint8_t)(block_id + 1u);   /* passo 3D-2 */
    pixel->block_down = (block_y + 1) < 0x20
        ? (uint8_t)((ctx->wram[layout_offset + 0x100u] & 0x7Fu) + 1u) : 0u;   /* passo 3D-2b */
    return true;
}

/* O layout de blocos da fase (WRAM/ERAM) só existe nos modos 2 e 3.
 * No mapa (modo 1) e nas outras telas, as margens vêm do tilemap da VRAM.
 * Passo 3D-2: exportada (o 3D usa o layout para achar o chão). */
bool ppu_widescreen_uses_level_layout(const GBContext* ctx) {
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x08C3u) {
        return false;
    }
    const uint8_t mode = ctx->eram[0x08C3];
    return mode == 2 || mode == 3;
}

/* Passo 7q: cor do pixel da borda da tela original nesta linha (x = 0 com
 * right = false, x = 159 com right = true). So o fundo, sem sprites. */
static WidescreenBackgroundPixel ppu_fetch_widescreen_background_pixel(
    const GBPPU* ppu,
    const GBContext* ctx,
    int extended_x,
    const WidescreenBlockAttributeCache* attribute_cache);
static uint32_t ppu_ws_screen_edge_rgba(const GBPPU* ppu,
                                        const GBContext* ctx,
                                        const WidescreenBlockAttributeCache* attribute_cache,
                                        bool right) {
    return ppu_fetch_widescreen_background_pixel(
        ppu, ctx, right ? GB_SCREEN_WIDTH - 1 : 0, attribute_cache).rgba;
}

static WidescreenBackgroundPixel ppu_fetch_widescreen_background_pixel(
    const GBPPU* ppu,
    const GBContext* ctx,
    int extended_x,
    const WidescreenBlockAttributeCache* attribute_cache) {
    WidescreenBackgroundPixel pixel = {0, 0, false, 0, 0, 0, 0, 0};
    const uint8_t lcdc = ppu->latched_lcdc;
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool bg_visible = cgb_mode ? true : ((lcdc & LCDC_BG_ENABLE) != 0);

    if (!(lcdc & LCDC_LCD_ENABLE) || !bg_visible) {
        pixel.rgba = rgb555_to_rgba(dmg_palette_rgb555[0]);
        return pixel;
    }


    if ((extended_x < 0 || extended_x >= GB_SCREEN_WIDTH) &&
        ppu_widescreen_uses_level_layout(ctx)) {
        // 1. Posições de scroll lógicas da câmara lidas da ERAM
        const uint16_t base_scroll_x = ppu->frame_widescreen_scroll_x;
        const uint16_t base_scroll_y = ppu->frame_widescreen_scroll_y;

        // 2. Calcula o deslocamento de vibração/shake do hardware
        // O registador SCX (latched_scx) contem a posição X real enviada para a PPU na scanline.
        // A diferença entre o scroll esperado da câmara e o registador é o "tremor".
        int shake_offset_x = (int)(ppu->latched_scx & 0x07) - ((int)(base_scroll_x - 0x50) & 0x07);
        int shake_offset_y = (int)(ppu->latched_scy & 0x07) - ((int)(base_scroll_y - 0x48) & 0x07);

        // Se o shake for maior em píxeis inteiros (ex: SCY oscila entre valores maiores)
        int scy_delta = (int)ppu->latched_scy - ((int)(base_scroll_y - 0x48) & 0xFF);
        if (scy_delta > 128) scy_delta -= 256;
        if (scy_delta < -128) scy_delta += 256;

        int scx_delta = (int)ppu->latched_scx - ((int)(base_scroll_x - 0x50) & 0xFF);
        if (scx_delta > 128) scx_delta -= 256;
        if (scx_delta < -128) scx_delta += 256;

        // 3. Aplica o desvio do tremor às coordenadas globais do mundo
        const int world_x = (int)base_scroll_x - 0x50 + extended_x + scx_delta;
        const int world_y = (int)base_scroll_y - 0x48 + ppu->ly + scy_delta;
       
        if (ppu_fetch_warioland_world_pixel(
            ppu, ctx, world_x, world_y, attribute_cache, &pixel)) {
            return pixel;
        }
        

    }

    const int window_x = (int)ppu->latched_wx - 7;
    const bool window_enabled = (lcdc & LCDC_WINDOW_ENABLE) &&
        (ppu->latched_wx <= 166) &&
        (ppu->latched_wy <= ppu->ly) &&
        (cgb_mode || bg_visible);
    const bool in_window = window_enabled &&
        extended_x >= window_x && extended_x < GB_SCREEN_WIDTH;

    const int source_x = in_window
        ? extended_x - window_x
        : (extended_x + ppu->latched_scx);
    const int source_y = in_window
        ? (ppu->ly - ppu->latched_wy)
        : (ppu->ly + ppu->latched_scy);
    const uint16_t tilemap_addr = in_window
        ? get_window_tilemap_addr(lcdc)
        : get_bg_tilemap_addr(lcdc);
    const uint8_t tile_x = (uint8_t)(((uint32_t)source_x >> 3) & 31u);
    const uint8_t tile_y = (uint8_t)(((uint32_t)source_y >> 3) & 31u);
    int pixel_x = (int)((uint32_t)source_x & 7u);
    int pixel_y = (int)((uint32_t)source_y & 7u);

    const uint16_t map_entry = (uint16_t)(tilemap_addr + tile_y * 32u + tile_x);
    const uint8_t tile_idx = vram_read_bank(ctx, 0, map_entry);

    uint8_t palette_number = 0;
    uint8_t tile_bank = 0;
    bool flip_x = false;
    uint8_t ws_flags = 0;

    if (cgb_mode) {
        const uint8_t attr = vram_read_bank(ctx, 1, map_entry);
        ws_flags = attr;
        palette_number = attr & OAM_CGB_PALETTE;
        tile_bank = (attr & OAM_CGB_BANK) ? 1u : 0u;
        pixel.priority = (attr & OAM_PRIORITY) != 0;
        flip_x = (attr & OAM_FLIP_X) != 0;
        if (attr & OAM_FLIP_Y) {
            pixel_y = 7 - pixel_y;
        }
    }

    const uint16_t tile_addr = get_tile_data_addr(lcdc, tile_idx, false);
    const uint8_t lo = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2));
    const uint8_t hi = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2 + 1));

    if (flip_x) {
        pixel_x = 7 - pixel_x;
    }
    const int bit = 7 - pixel_x;
    pixel.raw_color = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
    pixel.palette = palette_number;
    pixel.rgba = rgb555_to_rgba(
        resolve_bg_color(ppu, ctx, palette_number, pixel.raw_color, ppu->latched_bgp));
    pixel.tile_ref = ws_tile_ref(tile_bank, tile_addr);
    pixel.uvf = ws_uvf((uint32_t)source_x & 7u, (uint32_t)source_y & 7u, ws_flags);
    return pixel;
}

/* Fades a pixel toward black as edge_t goes 0 -> 1. Used to soften the
 * outer edge of the widescreen margins, since side-scrollers stream their
 * background tilemap column-by-column just ahead of the visible screen -
 * the extended margins can catch tiles that haven't been (re)written yet,
 * which shows up as stale/flickering content as the camera scrolls. This
 * doesn't fix that (it can't, without knowing the game's own streaming
 * logic), it just makes the artifact read as an intentional vignette
 * instead of an obvious glitch. */
static uint32_t fade_widescreen_edge(uint32_t color, double edge_t, bool enabled, float amount) {
    if (!enabled || edge_t <= 0.0) {
        return color;
    }
    if (edge_t > 1.0) {
        edge_t = 1.0;
    }
    double clamped_amount = amount;
    if (clamped_amount < 0.0f) clamped_amount = 0.0f;
    if (clamped_amount > 1.0f) clamped_amount = 1.0f;

    const double keep = 1.0 - (edge_t * clamped_amount);
    const uint8_t a = (uint8_t)((color >> 24) & 0xFF);
    const uint8_t r = (uint8_t)(((color >> 16) & 0xFF) * keep);
    const uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * keep);
    const uint8_t b = (uint8_t)((color & 0xFF) * keep);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t ppu_composite_widescreen_pixel(
    const GBPPU* ppu,
    const GBContext* ctx,
    const WidescreenBackgroundPixel* bg,
    const WidescreenObjectPixel* obj) {
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    const bool bg_has_pixels = bg->raw_color != 0;
    bool use_object = obj->present;

    if (use_object && bg_has_pixels) {
        if (cgb_mode) {
            if ((ppu->latched_lcdc & LCDC_BG_ENABLE) &&
                (bg->priority || obj->behind_bg)) {
                use_object = false;
            }
        } else if (obj->behind_bg) {
            use_object = false;
        }
    }

    if (use_object) {
        const uint16_t color15 = resolve_obj_color(
            ppu, ctx, obj->palette, obj->raw_color,
            obj->dmg_pal ? ppu->latched_obp1 : ppu->latched_obp0);
        return rgb555_to_rgba(color15);
    }

    return bg->rgba;
}

//inicio codigo subfases - esconder lixo


/* piscando2
typedef struct {
    bool     valid_l, valid_r;
    int      left_edge, right_edge;
    uint16_t last_scroll;
    int      prev_cam;
    uint8_t  acc_flags;
    uint8_t  hold_l, hold_r;      // frames seguidos com flag + câmera parada
    uint8_t  last_mode, last_level, last_room;
} WsRoomBounds;
static WsRoomBounds g_ws_bounds;

#define WS_BOUND_HOLD_FRAMES 8

static void ppu_update_room_bounds(const GBContext* ctx, uint16_t scroll_x, bool frame_start) {
    WsRoomBounds* b = &g_ws_bounds;
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x0938) return;

    const uint8_t mode  = ctx->eram[0x08C3];
    const uint8_t level = ctx->eram[0x0800];
    const uint8_t room  = ctx->eram[0x0804];
    const int jump = abs((int)scroll_x - (int)b->last_scroll);

    if ((mode != 2 && mode != 3) || mode != b->last_mode ||
        level != b->last_level || room != b->last_room || jump > 64) {
        b->valid_l = b->valid_r = false;
        b->hold_l = b->hold_r = 0;
        b->acc_flags = 0;
    }
    b->last_mode = mode; b->last_level = level; b->last_room = room;
    b->last_scroll = scroll_x;
    if (mode != 2 && mode != 3) return;

    b->acc_flags |= ctx->eram[0x0938];
    if (!frame_start) return;

    const int cam_left = (int)scroll_x - 0x50;
    const bool stationary = (cam_left == b->prev_cam);

    b->hold_r = (stationary && (b->acc_flags & 0x01)) ? (uint8_t)(b->hold_r < 255 ? b->hold_r + 1 : 255) : 0;
    b->hold_l = (stationary && (b->acc_flags & 0x02)) ? (uint8_t)(b->hold_l < 255 ? b->hold_l + 1 : 255) : 0;

    if (b->hold_r >= WS_BOUND_HOLD_FRAMES) { b->valid_r = true; b->right_edge = cam_left + GB_SCREEN_WIDTH; }
    if (b->hold_l >= WS_BOUND_HOLD_FRAMES) { b->valid_l = true; b->left_edge  = cam_left; }

    if (b->valid_r && cam_left + GB_SCREEN_WIDTH > b->right_edge) b->valid_r = false;
    if (b->valid_l && cam_left < b->left_edge)                     b->valid_l = false;

    b->prev_cam = cam_left;
    b->acc_flags = 0;
}
*/
/*piscando 0: origem
typedef struct {
    bool     valid_l, valid_r;
    int      left_edge;     // world_x do primeiro pixel válido da sala
    int      right_edge;    // world_x logo após o último pixel válido
    uint16_t last_scroll;
    uint8_t  last_mode, last_level, last_room;
} WsRoomBounds;
static WsRoomBounds g_ws_bounds;

static void ppu_update_room_bounds(const GBContext* ctx, uint16_t scroll_x) {
    WsRoomBounds* b = &g_ws_bounds;
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x0938) return;

    const uint8_t mode  = ctx->eram[0x08C3];
    const uint8_t level = ctx->eram[0x0800];
    const uint8_t room  = ctx->eram[0x0804];
    const int jump = abs((int)scroll_x - (int)b->last_scroll);

    // nova sala/fase, fora da gameplay ou teleporte de porta: esquece os limites 
    if ((mode != 2 && mode != 3) || mode != b->last_mode ||
        level != b->last_level || room != b->last_room || jump > 64) {
        b->valid_l = b->valid_r = false;
    }
    b->last_mode = mode; b->last_level = level; b->last_room = room;
    b->last_scroll = scroll_x;
    if (mode != 2 && mode != 3) return;

    const int cam_left = (int)scroll_x - 0x50;
    const uint8_t flags = ctx->eram[0x0938];
    if (flags & 0x01) { b->valid_r = true; b->right_edge = cam_left + GB_SCREEN_WIDTH; }
    if (flags & 0x02) { b->valid_l = true; b->left_edge  = cam_left; }

    // autocorreção: se a câmera passou do limite guardado, ele estava errado
    if (b->valid_r && cam_left + GB_SCREEN_WIDTH > b->right_edge) b->valid_r = false;
    if (b->valid_l && cam_left < b->left_edge)                     b->valid_l = false;
}
fim piscando0*/


/* piscando1: a cada frame, o jogo atualiza o scroll da câmara na ERAM, mas só
 * atualiza os flags de limite no início da scanline 0. Se a câmera se mover
 * durante a renderização da tela, o limite pode não ser atualizado até o
 * próximo frame. Para evitar que a margem do widescreen fique piscando,
 * acumulamos os flags de limite durante o frame e só aplicamos no início
 * da próxima scanline 0. 
typedef struct {
    bool     valid_l, valid_r;
    int      left_edge, right_edge;
    uint16_t last_scroll;
    int      prev_cam;
    uint8_t  acc_flags;
    uint8_t  last_mode, last_level, last_room;
} WsRoomBounds;
static WsRoomBounds g_ws_bounds;

static void ppu_update_room_bounds(const GBContext* ctx, uint16_t scroll_x, bool frame_start) {
    WsRoomBounds* b = &g_ws_bounds;
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x0938) return;

    const uint8_t mode  = ctx->eram[0x08C3];
    const uint8_t level = ctx->eram[0x0800];
    const uint8_t room  = ctx->eram[0x0804];
    const int jump = abs((int)scroll_x - (int)b->last_scroll);

    if ((mode != 2 && mode != 3) || mode != b->last_mode ||
        level != b->last_level || room != b->last_room || jump > 64) {
        b->valid_l = b->valid_r = false;
        b->acc_flags = 0;
    }
    b->last_mode = mode; b->last_level = level; b->last_room = room;
    b->last_scroll = scroll_x;
    if (mode != 2 && mode != 3) return;

    b->acc_flags |= ctx->eram[0x0938];
    if (!frame_start) return;

    const int cam_left = (int)scroll_x - 0x50;
    const bool stationary = (cam_left == b->prev_cam);

    if (stationary) {
        if (b->acc_flags & 0x01) { b->valid_r = true; b->right_edge = cam_left + GB_SCREEN_WIDTH; }
        if (b->acc_flags & 0x02) { b->valid_l = true; b->left_edge  = cam_left; }
    }
    if (b->valid_r && cam_left + GB_SCREEN_WIDTH > b->right_edge) b->valid_r = false;
    if (b->valid_l && cam_left < b->left_edge)                     b->valid_l = false;

    b->prev_cam = cam_left;
    b->acc_flags = 0;
}

fim piscando1 */ 

#define WS_WALL_LEFT_OFFSET   0x10   /* borda esquerda visível = (col<<8)+0x10 */
#define WS_WALL_RIGHT_OFFSET  0x100  /* borda direita visível  = (col<<8)+0x100 */

static void ppu_room_walls(const GBContext* ctx, uint16_t scroll_x,
                           bool* has_left, int* left_edge,
                           bool* has_right, int* right_edge) {
    *has_left = *has_right = false;
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x0E00) return;

    const uint8_t row  = ctx->eram[0x0900];
    const uint8_t col  = (uint8_t)(scroll_x >> 8);   /* = eram[0x0902] */
    const uint8_t idx  = (uint8_t)((uint8_t)((row << 4) | (row >> 4)) + col);
    const uint8_t cell = ctx->eram[0x0CE0 + idx];

    if (cell & 0x02) { *has_left  = true; *left_edge  = ((int)col << 8) + WS_WALL_LEFT_OFFSET; } // ideal 0x02
    if (cell & 0x01) { *has_right = true; *right_edge = ((int)col << 8) + WS_WALL_RIGHT_OFFSET; } // ideal 0x01
}


// fim do codigo para subfases


/* ============================================================================
 * Margens do widescreen: versao rapida (passo 3a)
 *
 * Mesma logica da versao original (removida no passo 3c), com duas mudancas
 * que nao alteram o resultado:
 *   1. Quando o renderizador e Nativo/Comparar, o centro de 160 px e
 *      sobrescrito por ppu_native_end_frame() no VBlank. Entao so as margens
 *      sao calculadas aqui.
 *   2. Os sprites que cruzam a linha sao escolhidos uma vez por linha (com o
 *      mesmo filtro e a mesma ordem de OAM), em vez de varrer os 40 sprites
 *      em cada um dos 256 pixels.
 * ========================================================================== */

typedef struct {
    int screen_x;
    uint8_t oam_index;
    uint8_t flags;
    uint8_t palette;
    uint8_t lo;
    uint8_t hi;
    uint16_t tile_ref;  /* passo 5d: tile 8x8 desta linha do sprite */
    uint8_t screen_v;   /* linha dentro do tile, espaço da tela */
} WsLineSprite;

/* Passo 5d: sprite cuja caixa 8x8 cobre o pixel onde nenhum sprite tem cor. */
typedef struct {
    uint16_t tile_ref;  /* 0 = nenhum */
    uint8_t uvf;
    uint8_t palette;
    uint8_t dmg_pal;    /* bit 4 do OAM (0 = OBP0, 1 = OBP1), também no modo CGB */
} WsSpriteBox;

static int ppu_ws_collect_line_sprites(const GBPPU* ppu,
                                       const GBContext* ctx,
                                       WsLineSprite* out) {
    int count = 0;
    if (!(ppu->latched_lcdc & LCDC_OBJ_ENABLE)) {
        return 0;
    }
    const bool cgb_mode = ppu_is_cgb_mode(ctx);
    for (uint8_t oam_index = 0; oam_index < 40; ++oam_index) {
        const size_t oam_offset = (size_t)oam_index * 4u;
        const uint8_t current_y = ctx->oam[oam_offset];
        const uint8_t current_x = ctx->oam[oam_offset + 1u];
        const uint8_t current_tile = ctx->oam[oam_offset + 2u];
        const uint8_t current_flags = ctx->oam[oam_offset + 3u];

        if (current_y == 0 /*|| current_x == 0 */|| current_y >= 160) {
            continue;
        }
        if (gbrt_oam_y_wrapped(ctx, oam_index)) {   /* passo 17a: acima/abaixo da tela de verdade */
            continue;
        }

        const int screen_y = (int)current_y - 16;
        int screen_x = (int)current_x - 8;
        int true_x = 0;
        if (gbrt_oam_true_screen_x(ctx, oam_index, &true_x)) {
            /* passo 16a: sprite de ator com o X verdadeiro (9 bits) */
            screen_x = true_x;
        } else {
            const int center = (GB_SCREEN_WIDTH / 2);
            int best = screen_x;
            const int alt_low = screen_x - 256;
            const int alt_high = screen_x + 256;
            if (abs(alt_low - center) < abs(best - center)) {
                best = alt_low;
            }
            if (abs(alt_high - center) < abs(best - center)) {
                best = alt_high;
            }
            screen_x = best;
        }

        if (ppu->ly < screen_y || ppu->ly >= screen_y + ppu->line_sprite_height) {
            continue;
        }

        int line = (int)ppu->ly - screen_y;
        uint8_t tile = current_tile;
        uint8_t bank = 0;
        if (ppu->line_sprite_height == 16) {
            tile &= 0xFE;
        }
        if (current_flags & OAM_FLIP_Y) {
            line = ppu->line_sprite_height - 1 - line;
        }
        if (line < 0 || line >= ppu->line_sprite_height) {
            continue;
        }
        if (cgb_mode && (current_flags & OAM_CGB_BANK)) {
            bank = 1;
        }

        const uint16_t tile_addr = (uint16_t)(0x8000u + tile * 16u + line * 2u);
        WsLineSprite* s = &out[count++];
        s->screen_x = screen_x;
        s->oam_index = oam_index;
        s->flags = current_flags;
        s->palette = cgb_mode
            ? (current_flags & OAM_CGB_PALETTE)
            : ((current_flags & OAM_PALETTE) ? 1 : 0);
        s->lo = vram_read_bank(ctx, bank, tile_addr);
        s->hi = vram_read_bank(ctx, bank, (uint16_t)(tile_addr + 1u));
        s->tile_ref = (uint16_t)(1u + (unsigned)bank * 384u + (unsigned)tile + ((unsigned)line >> 3));
        s->screen_v = (uint8_t)(((int)ppu->ly - screen_y) & 7);
    }
    return count;
}

static WidescreenObjectPixel ppu_ws_object_pixel(const WsLineSprite* sprites,
                                                 int count,
                                                 bool dmg_priority,
                                                 int extended_x,
                                                 WsSpriteBox* box) {
    WidescreenObjectPixel chosen = {false, 0, 0, 0, false, 0, 0, 0, 0};
    int chosen_x = 256;
    int chosen_index = 256;
    if (box) {
        box->tile_ref = 0;
    }

    for (int i = 0; i < count; ++i) {
        const WsLineSprite* s = &sprites[i];
        const int sprite_pixel = extended_x - s->screen_x;
        if (sprite_pixel < 0 || sprite_pixel >= 8) {
            continue;
        }
        const int bit = (s->flags & OAM_FLIP_X) ? sprite_pixel : (7 - sprite_pixel);
        const uint8_t raw = (uint8_t)(((s->lo >> bit) & 1u) |
                                      (((s->hi >> bit) & 1u) << 1u));
        if (raw == 0) {
            if (box && !box->tile_ref) {
                box->tile_ref = s->tile_ref;
                box->uvf = ws_uvf((unsigned)sprite_pixel, s->screen_v, s->flags);
                box->palette = s->palette;
                box->dmg_pal = (s->flags & OAM_PALETTE) ? 1 : 0;
            }
            continue;
        }

        bool wins = !chosen.present;
        if (!wins) {
            if (dmg_priority) {
                wins = s->screen_x < chosen_x ||
                       (s->screen_x == chosen_x && s->oam_index < chosen_index);
            } else {
                wins = s->oam_index < chosen_index;
            }
        }
        if (!wins) {
            continue;
        }

        chosen.present = true;
        chosen.raw_color = raw;
        chosen.palette = s->palette;
        chosen.dmg_pal = (s->flags & OAM_PALETTE) ? 1 : 0;
        chosen.behind_bg = (s->flags & OAM_PRIORITY) != 0;
        chosen.screen_x = s->screen_x;
        chosen.oam_index = s->oam_index;
        chosen.tile_ref = s->tile_ref;
        chosen.uvf = ws_uvf((unsigned)sprite_pixel, s->screen_v, s->flags);
        chosen_x = s->screen_x;
        chosen_index = s->oam_index;
    }
    return chosen;
}

/* Escreve a linha em out_row (GB_WIDESCREEN_WIDTH pixels). Com skip_center,
 * as colunas do centro nao sao tocadas. */
/* Passo 3b (parte 1): mesma montagem do cache de atributos que
 * a montagem original (removida no passo 3c), mais barata:
 *  - so inicializa o que e lido depois (heads e count), sem zerar os
 *    ~9 KB de candidates (a busca so percorre candidatos ligados em heads);
 *  - le o mapa de tiles direto da VRAM (os enderecos 0x9800-0x9FFF sao
 *    sempre validos), sem passar por vram_read_bank();
 *  - calcula as 32 coordenadas world_x uma vez, e nao em cada linha do mapa.
 * Os atributos aprendidos (ws_block_*, ws_tile_*) sao gravados na mesma
 * ordem que na versao original. */
static void ppu_build_widescreen_block_attribute_cache_fast(
    GBPPU* ppu,
    const GBContext* ctx,
    WidescreenBlockAttributeCache* cache) {
    memset(cache->heads, 0xFF, sizeof(cache->heads)); /* UINT16_MAX */
    cache->count = 0;
    if (!ctx || !ctx->vram) {
        return;
    }
    if (!ppu_is_cgb_mode(ctx) || !ctx->wram || !ctx->eram) {
        return;
    }

    const int reference_x = (int)ppu->frame_widescreen_scroll_x - 0x50;
    const int reference_y = (int)ppu->frame_widescreen_scroll_y - 0x48;
    const uint16_t tilemap_offset =
        (uint16_t)(get_bg_tilemap_addr(ppu->latched_lcdc) - 0x8000u);
    const uint8_t* map0 = ctx->vram + tilemap_offset;
    const uint8_t* map1 = ctx->vram + VRAM_SIZE + tilemap_offset;

    int world_xs[32];
    for (int map_x = 0; map_x < 32; ++map_x) {
        world_xs[map_x] = ppu_unwrap_widescreen_map_coordinate(map_x, reference_x);
    }

    /* Passo 7g: tiles visiveis (um por numero de tile) para calibrar a
     * pagina da tabela DX. */
    {
        uint8_t cal_tiles[256], cal_pals[256];
        bool seen_tile[256] = {false};
        int cal_n = 0;
        memset(g_dx_screen_state, 0, sizeof(g_dx_screen_state));   /* Passo 7k */
        for (int map_y = 0; map_y < 32; ++map_y) {
            const int wy = ppu_unwrap_widescreen_map_coordinate(map_y, reference_y);
            if (wy < reference_y || wy >= reference_y + GB_SCREEN_HEIGHT) continue;
            for (int map_x = 0; map_x < 32; ++map_x) {
                const int wx = world_xs[map_x];
                if (wx < reference_x || wx >= reference_x + GB_SCREEN_WIDTH) continue;
                const uint8_t t = map0[map_y * 32 + map_x];
                {   /* Passo 7k */
                    const uint8_t a = map1[map_y * 32 + map_x];
                    if (g_dx_screen_state[t] == 0) {
                        g_dx_screen_state[t] = 1;
                        g_dx_screen_attr[t] = a;
                    } else if (g_dx_screen_attr[t] != a) {
                        g_dx_screen_state[t] = 2;
                    }
                }
                if (seen_tile[t]) continue;
                seen_tile[t] = true;
                cal_tiles[cal_n] = t;
                cal_pals[cal_n] = (uint8_t)(map1[map_y * 32 + map_x] & OAM_CGB_PALETTE);
                cal_n++;
            }
        }
        ppu_dx_calibrate(ctx, cal_tiles, cal_pals, cal_n);
    }

    for (int map_y = 0; map_y < 32; ++map_y) {
        const int world_y = ppu_unwrap_widescreen_map_coordinate(map_y, reference_y);
        if (world_y < 0 || world_y >= 0x0200) {
            continue;
        }
        const size_t layout_row = (size_t)(world_y >> 4) * 0x100u;
        const size_t tile_row = (size_t)((world_y >> 3) & 1) * 2u;
        const uint8_t* row0 = map0 + map_y * 32;
        const uint8_t* row1 = map1 + map_y * 32;

        if (!ppu_ws_tile_fresh(ppu, reference_x, world_y)) {
            continue;   /* passo 7f/13a: linha do tilemap fora da tela = resto antigo */
        }
        for (int map_x = 0; map_x < 32; ++map_x) {
            const int world_x = world_xs[map_x];
            if (world_x < 0 || world_x >= 0x1000) {
                continue;
            }
            if (!ppu_ws_tile_fresh(ppu, world_x, reference_y)) {
                continue;   /* passo 7f/13a: coluna do tilemap fora da tela = resto antigo */
            }

            const uint8_t block_id = ctx->wram[layout_row + (size_t)(world_x >> 4)] & 0x7Fu;
            const size_t block_offset = 0x0D00u + (size_t)block_id * 4u;
            if (block_offset + 3u >= ctx->eram_size) {
                continue;
            }

            const size_t tile_in_block = tile_row + (size_t)((world_x >> 3) & 1);
            const uint8_t learned_tile = ctx->eram[block_offset + tile_in_block];
            if (row0[map_x] != learned_tile) {
                continue;
            }

            const uint8_t learned_attr = row1[map_x];
            ppu->ws_block_known[block_id][tile_in_block] = true;
            ppu->ws_block_attr[block_id][tile_in_block]  = learned_attr;
            ppu->ws_tile_known[learned_tile] = true;
            ppu->ws_tile_attr[learned_tile]  = learned_attr;

            if (cache->count < WIDESCREEN_ATTRIBUTE_CACHE_CAPACITY) {
                const uint16_t candidate_index = cache->count++;
                WidescreenBlockAttributeCandidate* candidate =
                    &cache->candidates[candidate_index];
                candidate->world_x = (uint16_t)(world_x & ~7);
                candidate->world_y = (uint16_t)(world_y & ~7);
                candidate->attributes = learned_attr;
                candidate->next = cache->heads[block_id][tile_in_block];
                cache->heads[block_id][tile_in_block] = candidate_index;
            }
        }
    }
}

/* Passo 3b (parte 2): o cache de atributos das margens e montado uma vez por
 * frame e reaproveitado nas linhas seguintes. E remontado no meio do frame se
 * mudar algo que ele usa e que da para detectar barato: camera (ERAM $A900-
 * $A903), mapa de BG escolhido no LCDC, modo CGB, modo de jogo (o que tambem
 * cobre o reset dos atributos aprendidos ao entrar no modo 2) ou qualquer
 * escrita no mapa de tiles da VRAM (contador do ppu_native).
 * Mudancas no layout da fase (WRAM) ou na tabela de blocos (ERAM $AD00) no
 * meio do frame so entram no frame seguinte. */
static WidescreenBlockAttributeCache g_ws_attr_cache;
/* Passo 12a: estava dentro da função (static local); fora para poder zerar. */
static uint64_t g_ws_lcd_spans_seen = 0;
static struct {
    bool valid;
    uint8_t last_ly;
    uint16_t scroll_x;
    uint16_t scroll_y;
    uint8_t map_bit;
    bool cgb;
    uint8_t game_mode;
    uint32_t map_generation;
} g_ws_attr_key;

static const WidescreenBlockAttributeCache* ppu_ws_attribute_cache_for_line(
    GBPPU* ppu,
    const GBContext* ctx) {
    const uint8_t game_mode =
        (ctx && ctx->eram && ctx->eram_size > 0x08C3) ? ctx->eram[0x08C3] : 0xFFu;
    const bool cgb = ppu_is_cgb_mode(ctx);
    const uint8_t map_bit = (uint8_t)(ppu->latched_lcdc & LCDC_BG_TILEMAP);
    const uint32_t map_generation = ppu_native_vram_map_generation();

    if (!g_ws_attr_key.valid ||
        ppu->ly <= g_ws_attr_key.last_ly ||               /* frame novo */
        g_ws_attr_key.scroll_x != ppu->frame_widescreen_scroll_x ||
        g_ws_attr_key.scroll_y != ppu->frame_widescreen_scroll_y ||
        g_ws_attr_key.map_bit != map_bit ||
        g_ws_attr_key.cgb != cgb ||
        g_ws_attr_key.game_mode != game_mode ||
        g_ws_attr_key.map_generation != map_generation) {
        ppu_build_widescreen_block_attribute_cache_fast(ppu, ctx, &g_ws_attr_cache);
        ppu_ws_map_invalidate();   /* passo 13b */
        g_ws_attr_key.valid = true;
        g_ws_attr_key.scroll_x = ppu->frame_widescreen_scroll_x;
        g_ws_attr_key.scroll_y = ppu->frame_widescreen_scroll_y;
        g_ws_attr_key.map_bit = map_bit;
        g_ws_attr_key.cgb = cgb;
        g_ws_attr_key.game_mode = game_mode;
        g_ws_attr_key.map_generation = map_generation;
    }
    g_ws_attr_key.last_ly = ppu->ly;
    return &g_ws_attr_cache;
}

/* Passo 3D-4a: tiles do mundo fora da tela, para o 3D (ver ppu.h). */
void ppu_world_tiles(GBPPU* ppu, const GBContext* ctx, int tx0, int ty0, int tw, int th, uint16_t* out) {
    memset(out, 0, (size_t)tw * (size_t)th * sizeof(uint16_t));
    if (!ppu || !ctx || !ctx->wram || !ctx->eram || ctx->eram_size < 0x0D00u + 0x200u) return;
    const WidescreenBlockAttributeCache* cache = ppu_ws_attribute_cache_for_line(ppu, ctx);
    const uint16_t scroll_x = (uint16_t)((uint16_t)ctx->eram[0x0902u] << 8u | ctx->eram[0x0903u]);
    bool wl, wr;
    int wlx = 0, wrx = 0;
    ppu_room_walls(ctx, scroll_x, &wl, &wlx, &wr, &wrx);
    for (int j = 0; j < th; ++j) {
        for (int i = 0; i < tw; ++i) {
            const int wx = (tx0 + i) * 8, wy = (ty0 + j) * 8;
            if (wx < 0 || wy < 0 || wx >= 0x1000 || wy >= 0x0200) continue;
            if ((wl && wx < wlx) || (wr && wx >= wrx)) continue;
            const size_t layout_offset = (size_t)(wy >> 4) * 0x100u + (size_t)(wx >> 4);
            const uint8_t block_id = ctx->wram[layout_offset] & 0x7Fu;
            const size_t tile_in_block = (size_t)((wy >> 3) & 1) * 2u + (size_t)((wx >> 3) & 1);
            const uint8_t tile_idx = ctx->eram[0x0D00u + (size_t)block_id * 4u + tile_in_block];
            const WsMapTile* mt = ppu_ws_map_tile(ppu, ctx, wx, wy, block_id, tile_in_block, tile_idx, cache);
            const uint8_t attr = (uint8_t)((mt->attr & 0xEFu) | 0x10u);
            out[(size_t)j * (size_t)tw + (size_t)i] = (uint16_t)(tile_idx | ((uint16_t)attr << 8u));
        }
    }
}

/* Passo 7p / 3D-1b: no mapa regional (game_mode = 1, fora do mapa geral =
 * pagina DX 43), a ultima fileira de tiles da tela e a faixa com o nome da
 * regiao. As laterais a continuam (7p/7q) e o 3D a trata como HUD. */
bool ppu_ws_map_band_line(const GBContext* ctx, int ly) {
    return ctx && ctx->eram && ctx->eram_size > 0x08C3u &&
           ctx->eram[0x08C3] == 1 && (0x41 + ctx->eram[0x0800]) != 0x43 &&
           ly >= GB_SCREEN_HEIGHT - 8 && ly < GB_SCREEN_HEIGHT;
}

static void ppu_render_widescreen_scanline_fast(GBPPU* ppu,
                                                const GBContext* ctx,
                                                uint32_t* out_row,
                                                bool skip_center) {
    if (ctx && ctx->eram && ctx->eram_size >= 0x0904) {
        ppu->frame_widescreen_scroll_x = (uint16_t)ctx->eram[0x0902u] << 8u | ctx->eram[0x0903u];
        ppu->frame_widescreen_scroll_y = (uint16_t)ctx->eram[0x0900u] << 8u | ctx->eram[0x0901u];
    }

    if (ctx && ctx->eram && ctx->eram_size > 0x08C3) {
        const uint8_t mode_now = ctx->eram[0x08C3];
        /* Passo 7i: tambem a cada vez que o LCD desliga e religa (porta,
         * troca de sala): os mesmos blocos podem ter outra cor na sala nova,
         * e a cor aprendida da sala anterior pintava a lateral errado. */
        const bool room_changed = ctx->total_lcd_off_span_count != g_ws_lcd_spans_seen;
        g_ws_lcd_spans_seen = ctx->total_lcd_off_span_count;
        if ((mode_now == 2 && ppu->ws_prev_game_mode != 2) || room_changed) {
            memset(ppu->ws_block_known, 0, sizeof(ppu->ws_block_known));
            memset(ppu->ws_tile_known, 0, sizeof(ppu->ws_tile_known));
            g_dx_page = -1;   /* passo 7g */
            ppu_ws_map_invalidate();   /* passo 13b */
        }
        ppu->ws_prev_game_mode = mode_now;
    }

    const WidescreenBlockAttributeCache* attribute_cache =
        ppu_ws_attribute_cache_for_line(ppu, ctx);

    const uint16_t scroll_x = ppu->frame_widescreen_scroll_x;
    bool wall_l, wall_r;
    int wall_l_x = 0, wall_r_x = 0;
    ppu_room_walls(ctx, scroll_x, &wall_l, &wall_l_x, &wall_r, &wall_r_x);

    uint8_t game_mode = 0;
    if (ctx && ctx->eram && ctx->eram_size > 0x08C3) {
        game_mode = ctx->eram[0x08C3];
    }
    const bool is_sidescrolling_gameplay = (game_mode == 3 || game_mode == 2);

    /* Passo 7p: no mapa regional (game_mode = 1, fora do mapa geral = pagina DX 43),
     * a ultima fileira de tiles da tela (faixa com o nome da regiao) continua
     * branca pelas laterais. */
    const bool ws_map_band = ppu_ws_map_band_line(ctx, ppu->ly);   /* passo 3D-1b: função */
    /* Passo 7q: a faixa continua com a cor da borda da tela original
     * (a cor da faixa do centro varia de mapa para mapa). */
    uint32_t ws_map_band_left = 0xFFFFFFFFu, ws_map_band_right = 0xFFFFFFFFu;
    if (ws_map_band) {
        ws_map_band_left = ppu_ws_screen_edge_rgba(ppu, ctx, attribute_cache, false);
        ws_map_band_right = ppu_ws_screen_edge_rgba(ppu, ctx, attribute_cache, true);
    }

    WsLineSprite sprites[40];
    const int sprite_count = ppu_ws_collect_line_sprites(ppu, ctx, sprites);
    /* Passo 7l: linha coberta pela janela (HUD) na largura toda do centro.
     * Atras do HUD fica a ultima fileira de blocos da fase, que no jogo
     * original nunca aparece (e nao tem cor conhecida): laterais pretas. */
    const bool ws_hud_band = is_sidescrolling_gameplay &&
        (ppu->latched_lcdc & LCDC_WINDOW_ENABLE) &&
        ppu->latched_wy <= ppu->ly && ppu->latched_wx <= 7;
    const bool dmg_priority = !ppu_is_cgb_mode(ctx) || ppu->opri != 0;

    /* Passo 5d: origem de cada pixel das laterais, para o HD e a captura. */
    PPUNativeMarginPixel ws_rec[PPU_NATIVE_MARGIN_PIXELS];
    const bool want_rec = ppu_native_margins_wanted();
    if (want_rec) {
        memset(ws_rec, 0, sizeof(ws_rec));
        for (int m = 0; m < PPU_NATIVE_MARGIN_PIXELS; ++m) {
            ws_rec[m].rgba = 0xFF000000u;
            ws_rec[m].bg_rgba = 0xFF000000u;
        }
    }

    for (int extended_x = -GB_WIDESCREEN_MARGIN;
         extended_x < GB_SCREEN_WIDTH + GB_WIDESCREEN_MARGIN;
         extended_x++) {
        if (skip_center && extended_x == 0) {
            extended_x = GB_SCREEN_WIDTH - 1; /* o for leva para 160 */
            continue;
        }
        const int dest_x = extended_x + GB_WIDESCREEN_MARGIN;
        const bool margin = extended_x < 0 || extended_x >= GB_SCREEN_WIDTH;
        const int world_x = (int)scroll_x - 0x50 + extended_x;

        if (is_sidescrolling_gameplay) {
            if (world_x < 0) {
                out_row[dest_x] = 0xFF000000u;
                continue;
            }
            /*if (margin && ws_hud_band) {   // Passo 7l: hud no widescreen
                out_row[dest_x] = 0xFFFFFFFFu; // branco do HUD, para preto 0xFF000000
                continue;
            }*/
            if (margin) {
                if ((wall_l && world_x < wall_l_x) ||
                    (wall_r && world_x >= wall_r_x)) {
                    out_row[dest_x] = 0xFF000000u;
                    continue;
                }
            }
        }

        if (margin && ws_map_band) {   /* Passo 7p / 7q */
            const uint32_t band_rgba = extended_x < 0 ? ws_map_band_left : ws_map_band_right;
            out_row[dest_x] = band_rgba;
            if (want_rec) {
                PPUNativeMarginPixel* r = &ws_rec[extended_x < 0
                    ? extended_x + GB_WIDESCREEN_MARGIN
                    : extended_x - GB_SCREEN_WIDTH + GB_WIDESCREEN_MARGIN];
                r->rgba = band_rgba;
                r->bg_rgba = band_rgba;
            }
            continue;
        }

        WidescreenBackgroundPixel bg = ppu_fetch_widescreen_background_pixel(
            ppu, ctx, extended_x, attribute_cache);
        if (margin) {
            bg.priority = false;
        }
        WsSpriteBox box;
        WidescreenObjectPixel obj =
            ppu_ws_object_pixel(sprites, sprite_count, dmg_priority, extended_x, &box);
        if (margin) {
            obj.behind_bg = false;
        }
        const uint32_t bg_unfaded = bg.rgba;

        if (extended_x < 0) {
            const double left_edge_t = (double)(-extended_x) / (double)GB_WIDESCREEN_MARGIN;
            bg.rgba = fade_widescreen_edge(bg.rgba, left_edge_t, ppu->widescreen_fade_enabled, ppu->widescreen_fade_amount);
        } else if (extended_x >= GB_SCREEN_WIDTH) {
            const double right_edge_t = (double)(extended_x - GB_SCREEN_WIDTH + 1) / (double)GB_WIDESCREEN_MARGIN;
            bg.rgba = fade_widescreen_edge(bg.rgba, right_edge_t, ppu->widescreen_fade_enabled, ppu->widescreen_fade_amount);
        }

        out_row[dest_x] = ppu_composite_widescreen_pixel(ppu, ctx, &bg, &obj);

        if (want_rec && margin) {
            PPUNativeMarginPixel* r = &ws_rec[extended_x < 0
                ? extended_x + GB_WIDESCREEN_MARGIN
                : extended_x - GB_SCREEN_WIDTH + GB_WIDESCREEN_MARGIN];
            r->rgba = out_row[dest_x];
            r->bg_rgba = bg_unfaded;
            r->bg_tile = bg.tile_ref;
            r->bg_block = bg.block;   /* passo 3D-2 */
            r->bg_block_down = bg.block_down;
            r->bg_uvf = bg.uvf;
            r->bg_raw = bg.raw_color;
            r->bg_pal = bg.palette;
            /* Mesma conta de fade_widescreen_edge(). */
            const double edge_t = extended_x < 0
                ? (double)(-extended_x) / (double)GB_WIDESCREEN_MARGIN
                : (double)(extended_x - GB_SCREEN_WIDTH + 1) / (double)GB_WIDESCREEN_MARGIN;
            if (ppu->widescreen_fade_enabled && edge_t > 0.0) {
                double clamped_amount = ppu->widescreen_fade_amount;
                if (clamped_amount < 0.0f) clamped_amount = 0.0f;
                if (clamped_amount > 1.0f) clamped_amount = 1.0f;
                r->fade = true;
                r->keep = 1.0 - ((edge_t > 1.0 ? 1.0 : edge_t) * clamped_amount);
            }
            if (obj.present) {
                r->obj_tile = obj.tile_ref;
                r->obj_uvf = obj.uvf;
                r->obj_raw = obj.raw_color;
                r->obj_pal = obj.palette;
                r->obj_rgba = rgb555_to_rgba(resolve_obj_color(
                    ppu, ctx, obj.palette, obj.raw_color,
                    obj.dmg_pal ? ppu->latched_obp1 : ppu->latched_obp0));
            } else if (box.tile_ref) {
                r->obj_tile = box.tile_ref;
                r->obj_uvf = box.uvf;
                r->obj_raw = 0;
                r->obj_pal = box.palette;
                r->obj_rgba = rgb555_to_rgba(resolve_obj_color(
                    ppu, ctx, box.palette, 0,
                    box.dmg_pal ? ppu->latched_obp1 : ppu->latched_obp0));
            }
        }
    }
    if (want_rec) {
        ppu_native_margin_line(ppu, ctx, ws_rec);
    }
}

/* Margens do widescreen. Chamado no fim do modo 3 de cada linha. */
static void ppu_render_widescreen_scanline(GBPPU* ppu, const GBContext* ctx) {
    if (!ppu->widescreen_enabled || !ppu->widescreen_framebuffer) {
        return;
    }
    if (ppu->ly >= GB_SCREEN_HEIGHT) {
        return;
    }
    /* No Nativo/Comparar o centro vem do ppu_native no VBlank. */
    const bool skip_center = ppu_native_get_mode() != PPU_RENDERER_EMULATED;
    ppu_render_widescreen_scanline_fast(
        ppu, ctx,
        ppu->widescreen_framebuffer + (size_t)ppu->ly * GB_WIDESCREEN_WIDTH,
        skip_center);
}

void ppu_render_scanline(GBPPU* ppu, GBContext* ctx) {
    uint8_t bg_raw[GB_SCREEN_WIDTH];
    uint8_t bg_priority[GB_SCREEN_WIDTH];

    if (!gbrt_rgb_framebuffer_enabled && !ctx->ppu_trace_file) {
        uint8_t scanline = ppu->ly;
        uint8_t lcdc = ppu->latched_lcdc;
        bool cgb_mode = ppu_is_cgb_mode(ctx);
        bool bg_visible = cgb_mode ? true : ((lcdc & LCDC_BG_ENABLE) != 0);
        bool window_enable = (lcdc & LCDC_WINDOW_ENABLE) &&
                             (ppu->latched_wx <= 166) &&
                             (ppu->latched_wy <= scanline) &&
                             (cgb_mode || bg_visible);

        if (window_enable && !ppu->window_triggered) {
            ppu->window_triggered = true;
        }
        if (ppu->window_triggered && window_enable) {
            ppu->window_line++;
        }
        return;
    }

    if (ctx->ppu_trace_file && ppu->ly == 0) {
        gbrt_log_oam_snapshot(ctx, "scanline-0");
    }

    if (ctx->ppu_trace_file) {
        gbrt_log_ppu_scanline(ctx,
                              ppu->ly,
                              ppu->mode,
                              ppu->lcdc,
                              ppu->stat,
                              ppu->scx,
                              ppu->scy,
                              ppu->wx,
                              ppu->wy,
                              ppu->bgp,
                              ppu->obp0,
                              ppu->obp1,
                              ppu->window_line,
                              ppu->window_triggered);
    }

    memset(bg_raw, 0, sizeof(bg_raw));
    memset(bg_priority, 0, sizeof(bg_priority));

    render_bg_scanline(ppu, ctx, bg_raw, bg_priority);
    render_sprites_scanline(ppu, ctx, bg_raw, bg_priority);
}

static void convert_to_rgb(GBPPU* ppu) {
    static int convert_count = 0;

    if (!gbrt_rgb_framebuffer_enabled) {
        return;
    }

    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = rgb555_to_rgba(ppu->color_framebuffer[i]);
    }

    convert_count++;
    if (convert_count <= 5 || (convert_count % 60 == 0)) {
        bool has_content = dbg_has_nonzero_pixels(ppu->framebuffer, GB_FRAMEBUFFER_SIZE);
        (void)has_content;
        DBG_FRAME("Frame %d converted to RGB - has_content=%d", convert_count, has_content);
        dbg_dump_framebuffer(ppu->framebuffer, GB_SCREEN_WIDTH);
    }
}

/* ============================================================================
 * PPU Mode State Machine
 * ========================================================================== */

static void update_stat(GBPPU* ppu, GBContext* ctx) {
    ppu->stat = (uint8_t)((ppu->stat & ~STAT_MODE_MASK) |
                          (ppu->visible_mode & STAT_MODE_MASK));

    if (ppu->ly == ppu->lyc) {
        ppu->stat |= STAT_LYC_MATCH;
    } else {
        ppu->stat &= (uint8_t)~STAT_LYC_MATCH;
    }

    ctx->io[0x41] = ppu->stat;
    ctx->io[0x44] = ppu->ly;
}

static void update_stat_deferring_lyc_rise(GBPPU* ppu, GBContext* ctx) {
    const uint8_t previous_match = ppu->stat & STAT_LYC_MATCH;
    update_stat(ppu, ctx);
    /* The line clock clears a stale equality immediately, but a new equality
     * is not exposed until visible mode 2 begins four dots later. */
    if (!previous_match && (ppu->stat & STAT_LYC_MATCH)) {
        ppu->stat &= (uint8_t)~STAT_LYC_MATCH;
        ctx->io[0x41] = ppu->stat;
    }
}

static void check_stat_interrupt(GBPPU* ppu, GBContext* ctx, const char* reason) {
    uint8_t source_state_mask = 0;
    uint8_t source_enable_mask = 0;
    uint8_t active_source_mask = 0;
    bool previous_state = ppu->stat_irq_state;
    bool current_state = false;

    if (ppu->stat_irq_mode == PPU_MODE_HBLANK) source_state_mask |= 0x1;
    if (ppu->stat_irq_mode == PPU_MODE_VBLANK) source_state_mask |= 0x2;
    if (ppu->stat_irq_mode == PPU_MODE_OAM) source_state_mask |= 0x4;
    if (ppu->vblank_oam_irq_source) source_state_mask |= 0x4;
    if (ppu->stat & STAT_LYC_MATCH) source_state_mask |= 0x8;

    if (ppu->stat & STAT_HBLANK_INT) source_enable_mask |= 0x1;
    if (ppu->stat & STAT_VBLANK_INT) source_enable_mask |= 0x2;
    if (ppu->stat & STAT_OAM_INT) source_enable_mask |= 0x4;
    if (ppu->stat & STAT_LYC_INT) source_enable_mask |= 0x8;

    active_source_mask = source_state_mask & source_enable_mask;
    current_state = active_source_mask != 0;

    if (ctx->ppu_trace_file) {
        gbrt_log_stat_irq_check(ctx,
                                reason,
                                ppu->ly,
                                ppu->mode,
                                ppu->stat,
                                source_state_mask,
                                source_enable_mask,
                                active_source_mask,
                                previous_state,
                                current_state);
    }

    if (current_state && !previous_state) {
        uint8_t if_before = ctx->io[0x0F];
        ctx->io[0x0F] |= 0x02;
        if (ctx->ppu_trace_file) {
            gbrt_log_stat_irq_request(ctx,
                                      reason,
                                      ppu->ly,
                                      ppu->mode,
                                      ppu->stat,
                                      active_source_mask,
                                      if_before,
                                      ctx->io[0x0F]);
        }
    }

    ppu->stat_irq_state = current_state;
}

void ppu_tick(GBPPU* ppu, GBContext* ctx, uint32_t cycles) {
    gbrt_note_ppu_tick(ctx,
                       (ppu->lcdc & LCDC_LCD_ENABLE) != 0 ? cycles : 0u);
    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        return;
    }

    while (cycles > 0) {
        switch (ppu->mode) {
            case PPU_MODE_OAM: {
                /* LY/internal OAM advance four dots before STAT exposes
                 * visible mode 2. The mode-2 interrupt source rises on the
                 * third hidden OAM dot, one dot before the mode bits. */
                if (ppu->visible_mode == PPU_MODE_HBLANK &&
                    ppu->mode_cycles < 4u) {
                    const uint32_t next_event =
                        ppu->stat_irq_mode != PPU_MODE_OAM ? 3u : 4u;
                    const uint32_t remaining = next_event - ppu->mode_cycles;
                    const uint32_t step = cycles < remaining ? cycles : remaining;
                    ppu->mode_cycles += step;
                    cycles -= step;
                    if (ppu->mode_cycles < next_event) {
                        break;
                    }
                    if (ppu->mode_cycles == 3u &&
                        ppu->stat_irq_mode != PPU_MODE_OAM) {
                        ppu->stat_irq_mode = PPU_MODE_OAM;
                        check_stat_interrupt(ppu, ctx, "oam-irq-early");
                        break;
                    }
                    if (ppu->mode_cycles == 4u) {
                        ppu->visible_mode = PPU_MODE_OAM;
                        update_stat(ppu, ctx);
                        break;
                    }
                }

                const uint32_t remaining = CYCLES_OAM_SCAN - ppu->mode_cycles;
                const uint32_t step = cycles < remaining ? cycles : remaining;
                ppu->mode_cycles += step;
                cycles -= step;
                if (ppu->mode_cycles < CYCLES_OAM_SCAN) {
                    break;
                }
                ppu->mode = PPU_MODE_DRAW;
                ppu->stat_irq_mode = PPU_MODE_DRAW;
                ppu_begin_dot_transfer(ppu, ctx);
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "oam->draw");
                break;
            }

            case PPU_MODE_DRAW: {
#ifndef GBRT_DISABLE_PPU_STABLE_SPANS
                const uint32_t stable_span =
                    ppu_draw_stable_span(ppu, ctx, cycles);
                if (stable_span > 0u) {
                    ppu->mode_cycles =
                        (uint16_t)(ppu->mode_cycles + stable_span);
                    cycles -= stable_span;
                    gbrt_note_ppu_draw_span(ctx, stable_span);
                    gbrt_note_ppu_stable_span(ctx, stable_span);
                    break;
                }
#endif

                const uint16_t previous_draw_x = ppu->draw_x;
                ppu->mode_cycles++;
                cycles--;
                if (ppu->visible_mode != PPU_MODE_DRAW &&
                    ppu->mode_cycles >= 3) {
                    ppu->visible_mode = PPU_MODE_DRAW;
                    update_stat(ppu, ctx);
                }
                const bool transfer_complete = ppu_draw_one_dot(ppu, ctx);
                gbrt_note_ppu_draw_dot(ctx, ppu->draw_x != previous_draw_x);
                if (!transfer_complete) {
                    break;
                }
                ppu_render_widescreen_scanline(ppu, ctx);
                ppu_native_end_line(ppu, ctx);

                ppu->mode3_length = (uint16_t)ppu->mode_cycles;
                if (ppu->lcd_startup_phase == LCD_STARTUP_DRAW) {
                    const uint32_t used =
                        LCD_STARTUP_DRAW_DOT + ppu->mode3_length;
                    ppu->hblank_length = used < LCD_STARTUP_LINE1_DOT
                        ? (uint16_t)(LCD_STARTUP_LINE1_DOT - used)
                        : 0;
                    ppu->lcd_startup_phase = LCD_STARTUP_HBLANK;
                } else {
                    ppu->hblank_length = ppu->mode3_length <
                                                 (CYCLES_SCANLINE - CYCLES_OAM_SCAN)
                        ? (uint16_t)(CYCLES_SCANLINE - CYCLES_OAM_SCAN -
                                     ppu->mode3_length)
                        : 0;
                }
                if (ppu->window_rendered_line) {
                    ppu->window_line++;
                }
                if (ctx->ppu_trace_file && ppu->ly == 0) {
                    gbrt_log_oam_snapshot(ctx, "scanline-0");
                }
                if (ctx->ppu_trace_file) {
                    gbrt_log_ppu_scanline(ctx,
                                          ppu->ly,
                                          ppu->mode,
                                          ppu->lcdc,
                                          ppu->stat,
                                          ppu->scx,
                                          ppu->scy,
                                          ppu->wx,
                                          ppu->wy,
                                          ppu->bgp,
                                          ppu->obp0,
                                          ppu->obp1,
                                          ppu->window_line,
                                          ppu->window_triggered);
                }
                ppu->mode = PPU_MODE_HBLANK;
                /* The internal transfer is complete, but CPU-visible STAT
                 * and bus gating retain mode 3 through this boundary dot on
                 * sprite-free DMG lines. Sprite fetch completion exposes
                 * HBlank immediately, matching the mode-0 timing table. */
                const bool sprite_line = ppu->visible_sprite_count > 0u;
                if (sprite_line) {
                    ppu->visible_mode = PPU_MODE_HBLANK;
                }
                ppu->stat_irq_mode = PPU_MODE_HBLANK;
                ppu->mode_cycles = 0;
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "draw->hblank");
                gbrt_hdma_hblank(ctx);
                break;
            }

            case PPU_MODE_HBLANK: {
                if (ppu->visible_mode == PPU_MODE_DRAW &&
                    ppu->mode_cycles < 1u) {
                    const uint32_t step = cycles > 0u ? 1u : 0u;
                    ppu->mode_cycles += step;
                    cycles -= step;
                    if (ppu->mode_cycles < 1u) {
                        break;
                    }
                    ppu->visible_mode = PPU_MODE_HBLANK;
                    update_stat(ppu, ctx);
                    break;
                }

                if (ppu->lcd_startup_phase == LCD_STARTUP_MODE0) {
                    const uint32_t remaining =
                        LCD_STARTUP_DRAW_DOT - ppu->mode_cycles;
                    const uint32_t step = cycles < remaining ? cycles : remaining;
                    ppu->mode_cycles += step;
                    cycles -= step;
                    if (ppu->mode_cycles < LCD_STARTUP_DRAW_DOT) {
                        break;
                    }
                    ppu->mode = PPU_MODE_DRAW;
                    ppu->visible_mode = PPU_MODE_DRAW;
                    ppu->stat_irq_mode = PPU_MODE_DRAW;
                    ppu->mode_cycles = 0;
                    ppu->lcd_startup_phase = LCD_STARTUP_DRAW;
                    ppu_begin_dot_transfer(ppu, ctx);
                    update_stat(ppu, ctx);
                    check_stat_interrupt(ppu, ctx, "lcd-startup-draw");
                    break;
                }

                const bool entering_vblank =
                    ppu->scanline + 1u == VISIBLE_SCANLINES;
                const bool early_oam_edge =
                    ppu->lcd_startup_phase == LCD_STARTUP_NONE &&
                    !entering_vblank &&
                    ppu->hblank_length > 0u &&
                    ppu->mode_cycles < ppu->hblank_length - 1u;
                const bool cgb_vblank_oam_edge =
                    entering_vblank &&
                    ppu_is_cgb_hardware(ctx) &&
                    ppu->hblank_length >= 4u &&
                    ppu->mode_cycles < ppu->hblank_length - 4u;
                uint32_t next_event = ppu->hblank_length;
                if (early_oam_edge &&
                    ppu->hblank_length - 1u < next_event) {
                    next_event = ppu->hblank_length - 1u;
                }
                if (cgb_vblank_oam_edge &&
                    ppu->hblank_length - 4u < next_event) {
                    next_event = ppu->hblank_length - 4u;
                }
                if (ppu->mode_cycles < next_event) {
                    const uint32_t remaining = next_event - ppu->mode_cycles;
                    const uint32_t step = cycles < remaining ? cycles : remaining;
                    ppu->mode_cycles += step;
                    cycles -= step;
                    if (ppu->mode_cycles < next_event) {
                        break;
                    }
                }

                if (early_oam_edge &&
                    ppu->mode_cycles == ppu->hblank_length - 1u) {
                    ppu->ly = (uint8_t)(ppu->scanline + 1u);
                    update_stat_deferring_lyc_rise(ppu, ctx);
                    ppu->stat_irq_mode = PPU_MODE_OAM;
                    check_stat_interrupt(ppu, ctx, "oam-irq-early");
                    break;
                }

                /* CGB-family hardware exposes the line-144 mode-2 STAT
                 * source one M-cycle before the VBlank interrupt. */
                if (cgb_vblank_oam_edge &&
                    ppu->mode_cycles == ppu->hblank_length - 4u) {
                    ppu->stat_irq_mode = PPU_MODE_OAM;
                    check_stat_interrupt(ppu, ctx, "vblank-oam-cgb-early");
                    break;
                }

                ppu->mode_cycles = 0;
                ppu->scanline++;
                ppu->ly = ppu->scanline;

                if (ppu->lcd_startup_phase == LCD_STARTUP_HBLANK) {
                    ppu->mode = PPU_MODE_OAM;
                    ppu->visible_mode = PPU_MODE_HBLANK;
                    ppu->stat_irq_mode = PPU_MODE_HBLANK;
                    ppu->lcd_startup_phase = LCD_STARTUP_NONE;
                    update_stat_deferring_lyc_rise(ppu, ctx);
                    check_stat_interrupt(ppu, ctx, "lcd-startup-line1");
                    break;
                }

                if (ppu->ly >= VISIBLE_SCANLINES) {
                    ppu->mode = PPU_MODE_VBLANK;
                    ppu->visible_mode = PPU_MODE_VBLANK;
                    ppu->stat_irq_mode = PPU_MODE_VBLANK;
                    ppu->vblank_oam_irq_source =
                        !ppu_is_cgb_hardware(ctx);
                    if (!ppu->frame_ready) {
                        if (ppu_native_get_mode() != PPU_RENDERER_EMULATED) {
                            ppu_native_end_frame(ppu, ctx);
                        } else {
                            convert_to_rgb(ppu);
                        }
                        ppu->frame_ready = true;
                        ctx->frame_done = 1;
                    }
                    ctx->io[0x0F] |= 0x01;
                } else {
                    ppu->mode = PPU_MODE_OAM;
                    ppu->visible_mode = PPU_MODE_HBLANK;
                    if (ppu->stat_irq_mode != PPU_MODE_OAM) {
                        ppu->stat_irq_mode = PPU_MODE_HBLANK;
                    }
                }

                if (ppu->mode == PPU_MODE_OAM &&
                    ppu->visible_mode == PPU_MODE_HBLANK) {
                    update_stat_deferring_lyc_rise(ppu, ctx);
                } else {
                    update_stat(ppu, ctx);
                }
                check_stat_interrupt(ppu, ctx, "hblank->next");
                if (ppu->vblank_oam_irq_source) {
                    ppu->vblank_oam_irq_source = false;
                    check_stat_interrupt(ppu, ctx, "vblank-oam-dmg-end");
                }
                break;
            }

            case PPU_MODE_VBLANK: {
                uint32_t next_event = CYCLES_SCANLINE;
                if (ppu->scanline == 153 && ppu->mode_cycles < 4) {
                    next_event = 4;
                }
                const uint32_t remaining = next_event - ppu->mode_cycles;
                const uint32_t step = cycles < remaining ? cycles : remaining;
                ppu->mode_cycles += step;
                cycles -= step;
                if (ppu->mode_cycles < next_event) {
                    break;
                }

                /* LY reads as 0 from dot 4 of line 153, while mode 1 remains. */
                if (ppu->scanline == 153 && ppu->mode_cycles == 4) {
                    ppu->ly = 0;
                    update_stat(ppu, ctx);
                    check_stat_interrupt(ppu, ctx, "ly153-dot4");
                    break;
                }

                ppu->mode_cycles = 0;
                if (ppu->scanline == 153) {
                    ppu->scanline = 0;
                    ppu->ly = 0;
                    ppu->window_line = 0;
                    ppu->window_triggered = false;
                    ppu->window_y_triggered = false;
                    ppu->window_active_line = false;
                    ppu->window_rendered_line = false;
                    ppu->mode = PPU_MODE_OAM;
                    ppu->visible_mode = PPU_MODE_OAM;
                    ppu->stat_irq_mode = PPU_MODE_OAM;
                } else {
                    ppu->scanline++;
                    ppu->ly = ppu->scanline;
                    ppu->visible_mode = PPU_MODE_VBLANK;
                    ppu->stat_irq_mode = PPU_MODE_VBLANK;
                }

                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "vblank->next");
                break;
            }
        }
    }
}

uint32_t ppu_cycles_until_next_event(const GBPPU* ppu,
                                     const GBContext* ctx) {
    if (!ppu || !(ppu->lcdc & LCDC_LCD_ENABLE)) {
        return UINT32_MAX;
    }

    switch (ppu->mode) {
        case PPU_MODE_OAM: {
            if (ppu->visible_mode == PPU_MODE_HBLANK &&
                ppu->mode_cycles < 4u) {
                const uint32_t target =
                    ppu->stat_irq_mode != PPU_MODE_OAM ? 3u : 4u;
                return target - ppu->mode_cycles;
            }
            return ppu->mode_cycles < CYCLES_OAM_SCAN
                ? CYCLES_OAM_SCAN - ppu->mode_cycles
                : 0u;
        }

        case PPU_MODE_DRAW:
            /* Mode 3 completion and early visible-mode publication are driven
             * by the FIFO one dot at a time. */
            return 1u;

        case PPU_MODE_HBLANK: {
            if (ppu->visible_mode == PPU_MODE_DRAW &&
                ppu->mode_cycles < 1u) {
                return 1u - ppu->mode_cycles;
            }

            if (ppu->lcd_startup_phase == LCD_STARTUP_MODE0) {
                return ppu->mode_cycles < LCD_STARTUP_DRAW_DOT
                    ? LCD_STARTUP_DRAW_DOT - ppu->mode_cycles
                    : 0u;
            }

            const bool entering_vblank =
                ppu->scanline + 1u == VISIBLE_SCANLINES;
            const bool early_oam_edge =
                ppu->lcd_startup_phase == LCD_STARTUP_NONE &&
                !entering_vblank &&
                ppu->hblank_length > 0u &&
                ppu->mode_cycles < ppu->hblank_length - 1u;
            const bool cgb_vblank_oam_edge =
                entering_vblank &&
                ppu_is_cgb_hardware(ctx) &&
                ppu->hblank_length >= 4u &&
                ppu->mode_cycles < ppu->hblank_length - 4u;
            uint32_t target = ppu->hblank_length;
            if (early_oam_edge && ppu->hblank_length - 1u < target) {
                target = ppu->hblank_length - 1u;
            }
            if (cgb_vblank_oam_edge && ppu->hblank_length - 4u < target) {
                target = ppu->hblank_length - 4u;
            }
            return ppu->mode_cycles < target
                ? target - ppu->mode_cycles
                : 0u;
        }

        case PPU_MODE_VBLANK: {
            uint32_t target = CYCLES_SCANLINE;
            if (ppu->scanline == 153u && ppu->mode_cycles < 4u) {
                target = 4u;
            }
            return ppu->mode_cycles < target
                ? target - ppu->mode_cycles
                : 0u;
        }

        default:
            return 0u;
    }
}

/* ============================================================================
 * Register Access
 * ========================================================================== */

uint8_t ppu_read_register(GBPPU* ppu, uint16_t addr) {
    switch (addr) {
        case 0xFF40: return ppu->lcdc;
        case 0xFF41: return (uint8_t)(ppu->stat | 0x80);
        case 0xFF42: return ppu->scy;
        case 0xFF43: return ppu->scx;
        case 0xFF44: return ppu->ly;
        case 0xFF45: return ppu->lyc;
        case 0xFF46: return ppu->dma;
        case 0xFF47: return ppu->bgp;
        case 0xFF48: return ppu->obp0;
        case 0xFF49: return ppu->obp1;
        case 0xFF4A: return ppu->wy;
        case 0xFF4B: return ppu->wx;
        case 0xFF68: return (uint8_t)(ppu->bgpi | 0x40);
        case 0xFF69:
            if (ppu->visible_mode == PPU_MODE_DRAW) return 0xFF;
            return ppu->bg_palette_ram[ppu->bgpi & 0x3F];
        case 0xFF6A: return (uint8_t)(ppu->obpi | 0x40);
        case 0xFF6B:
            if (ppu->visible_mode == PPU_MODE_DRAW) return 0xFF;
            return ppu->obj_palette_ram[ppu->obpi & 0x3F];
        default: return 0xFF;
    }
}

void ppu_write_register(GBPPU* ppu, GBContext* ctx, uint16_t addr, uint8_t value) {
    static int ppu_write_count = 0;
    uint8_t old_value;

    ppu_write_count++;
    old_value = ppu_read_register(ppu, addr);

    if (ppu_write_count <= 100 || (addr == 0xFF40 && (value == 0x91 || value == 0x00))) {
        DBG_REGS("PPU write #%d: addr=0x%04X value=0x%02X (A=0x%02X)",
                 ppu_write_count, addr, value, ctx ? ctx->a : 0);
    }

    switch (addr) {
        case 0xFF40:
        {
            uint8_t old_lcdc = ppu->lcdc;
            ppu->lcdc = value;
            if ((old_lcdc & LCDC_LCD_ENABLE) && !(value & LCDC_LCD_ENABLE)) {
                memset(ppu->native_line, 0, sizeof(ppu->native_line));
                ppu->ly = 0;
                ppu->scanline = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->window_y_triggered = false;
                ppu->window_active_line = false;
                ppu->window_rendered_line = false;
                ppu->mode = PPU_MODE_HBLANK;
                ppu->visible_mode = PPU_MODE_HBLANK;
                ppu->stat_irq_mode = PPU_MODE_HBLANK;
                ppu->lcd_startup_phase = LCD_STARTUP_NONE;
                ppu->mode_cycles = 0;
                ppu->mode3_length = CYCLES_PIXEL_DRAW;
                ppu->hblank_length = CYCLES_HBLANK;
                ppu->frame_ready = false;
                /* LY resets immediately, but the comparison clock stops: the
                 * existing LYC-match bit and STAT line state are retained. */
                ppu->stat = (uint8_t)(ppu->stat & ~STAT_MODE_MASK);
                ctx->io[0x41] = ppu->stat;
                ctx->io[0x44] = 0;
                gbrt_note_lcd_transition(ctx, false, old_lcdc, value, ppu->ly, ppu->mode);
            } else if (!(old_lcdc & LCDC_LCD_ENABLE) && (value & LCDC_LCD_ENABLE)) {
                ppu->ly = 0;
                ppu->scanline = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->window_y_triggered = false;
                ppu->window_active_line = false;
                ppu->window_rendered_line = false;
                ppu->mode = PPU_MODE_HBLANK;
                ppu->visible_mode = PPU_MODE_HBLANK;
                ppu->stat_irq_mode = PPU_MODE_HBLANK;
                ppu->lcd_startup_phase = LCD_STARTUP_MODE0;
                ppu->mode_cycles = 0;
                ppu->mode3_length = CYCLES_PIXEL_DRAW;
                ppu->hblank_length = CYCLES_HBLANK;
                ppu->frame_ready = false;
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "lcd-enable-lyc");
                gbrt_note_lcd_transition(ctx, true, old_lcdc, value, ppu->ly, ppu->mode);
            }
            break;
        }

        case 0xFF41:
            ppu->stat = (uint8_t)((ppu->stat & 0x07) | (value & 0x78));
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "stat-write");
            }
            break;

        case 0xFF42: ppu->scy = value; break;
        case 0xFF43: ppu->scx = value; break;

        case 0xFF45:
            ppu->lyc = value;
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "lyc-write");
            }
            break;

        case 0xFF46: ppu->dma = value; break;
        case 0xFF47: ppu->bgp = value; break;
        case 0xFF48: ppu->obp0 = value; break;
        case 0xFF49: ppu->obp1 = value; break;
        case 0xFF4A: ppu->wy = value; break;
        case 0xFF4B: ppu->wx = value; break;

        case 0xFF68:
            ppu->bgpi = value & 0xBF;
            break;

        case 0xFF69:
        {
            uint8_t index = ppu->bgpi & 0x3F;
            if (ppu->mode != PPU_MODE_DRAW) {
                ppu->bg_palette_ram[index] = value;
            }
            if (ppu->bgpi & 0x80) {
                ppu->bgpi = (uint8_t)((ppu->bgpi & 0x80) | ((index + 1) & 0x3F));
            }
            break;
        }

        case 0xFF6A:
            ppu->obpi = value & 0xBF;
            break;

        case 0xFF6B:
        {
            uint8_t index = ppu->obpi & 0x3F;
            if (ppu->mode != PPU_MODE_DRAW) {
                ppu->obj_palette_ram[index] = value;
            }
            if (ppu->obpi & 0x80) {
                ppu->obpi = (uint8_t)((ppu->obpi & 0x80) | ((index + 1) & 0x3F));
            }
            break;
        }

        default:
            break;
    }

    if (ctx) {
        if (addr >= 0xFF40 && addr <= 0xFF4B) {
            ctx->io[addr - 0xFF00] = ppu_read_register(ppu, addr);
        } else if (addr == 0xFF68 || addr == 0xFF6A) {
            ctx->io[addr - 0xFF00] = ppu_read_register(ppu, addr);
        } else if (addr == 0xFF69) {
            ctx->io[0x68] = ppu_read_register(ppu, 0xFF68);
        } else if (addr == 0xFF6B) {
            ctx->io[0x6A] = ppu_read_register(ppu, 0xFF6A);
        }
    }

    gbrt_log_ppu_register_write(ctx,
                                addr,
                                old_value,
                                ppu_read_register(ppu, addr),
                                ppu->ly,
                                ppu->mode);
}

/* ============================================================================
 * Frame Handling
 * ========================================================================== */
// Sempre que houver uma atualização de VRAM pelo código recompilado:


bool ppu_frame_ready(GBPPU* ppu) {
    return ppu->frame_ready;
}

void ppu_clear_frame_ready(GBPPU* ppu) {
    ppu->frame_ready = false;
}

const uint32_t* ppu_get_framebuffer(GBPPU* ppu) {
    return ppu->rgb_framebuffer;
}

uint32_t ppu_get_dmg_shade_rgb(uint8_t shade) {
    return rgb555_to_rgba(dmg_palette_rgb555[shade & 3u]);
}

void ppu_set_widescreen_enabled(GBPPU* ppu, bool enabled) {
    if (!ppu) {
        return;
    }
    if (enabled && !ppu->widescreen_framebuffer) {
        ppu->widescreen_framebuffer =
            (uint32_t*)malloc((size_t)GB_WIDESCREEN_FRAMEBUFFER_SIZE * sizeof(uint32_t));
        if (!ppu->widescreen_framebuffer) {
            /* Allocation failed - stay in normal mode instead of crashing. */
            ppu->widescreen_enabled = false;
            return;
        }
        memset(ppu->widescreen_framebuffer, 0,
               (size_t)GB_WIDESCREEN_FRAMEBUFFER_SIZE * sizeof(uint32_t));
    }
    ppu->widescreen_enabled = enabled;
}

bool ppu_get_widescreen_enabled(const GBPPU* ppu) {
    return ppu && ppu->widescreen_enabled;
}

const uint32_t* ppu_get_widescreen_framebuffer(GBPPU* ppu) {
    return ppu ? ppu->widescreen_framebuffer : NULL;
}
void ppu_set_widescreen_fade_enabled(GBPPU* ppu, bool enabled) {
    if (ppu) {
        ppu->widescreen_fade_enabled = enabled;
    }
}

bool ppu_get_widescreen_fade_enabled(const GBPPU* ppu) {
    return ppu && ppu->widescreen_fade_enabled;
}

void ppu_set_widescreen_fade_amount(GBPPU* ppu, float amount) {
    if (!ppu) {
        return;
    }
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    ppu->widescreen_fade_amount = amount;
}

float ppu_get_widescreen_fade_amount(const GBPPU* ppu) {
    return ppu ? ppu->widescreen_fade_amount : 0.0f;
}

/* ============================================================================
 * Compatibilidade: a flag antiga agora escolhe o modo do ppu_native.
 * ========================================================================== */

void ppu_set_native_render_enabled(GBPPU* ppu, bool enabled) {
    if (ppu) ppu->native_render_enabled = enabled;
    ppu_native_set_mode(enabled ? PPU_RENDERER_NATIVE : PPU_RENDERER_EMULATED);
}

bool ppu_get_native_render_enabled(const GBPPU* ppu) {
    (void)ppu;
    return ppu_native_get_mode() != PPU_RENDERER_EMULATED;
}

/* Passo 12a: estado das laterais que fica fora do GBPPU (e por isso fora do
 * savestate): página DX calibrada, cache de atributos do frame, tabelas do
 * "mesmo tile perto" e a contagem de trocas de sala. Sem zerar, um savestate
 * carregado herdava a calibração de antes e as laterais dependiam do que
 * tinha passado na tela. Os blocos aprendidos (ws_block_known/ws_tile_known)
 * estão no GBPPU e vêm do próprio savestate. */
void ppu_reset_widescreen_heuristics(const GBContext* ctx) {
    g_dx_page = -1;
    g_dx_page_misses = 0;
    g_ws_attr_key.valid = false;
    s_ws_near_frame = UINT64_MAX;
    g_ws_lcd_spans_seen = ctx ? ctx->total_lcd_off_span_count : 0;
    ppu_ws_map_invalidate();   /* passo 13b */
}
