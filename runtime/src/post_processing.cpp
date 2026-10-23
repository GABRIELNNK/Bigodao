/**
 * @file post_processing.cpp
 * @brief Implementação dos algoritmos de pós-processamento (HQ2x, xBRZ, Scale2x, etc.)
 */

#include "post_processing.h"
#include <cstring>
#include <vector>
#include <cmath>
#include <algorithm>

static const char* g_post_filter_names[GB_POST_FILTER_COUNT] = {
    "Off (Original)",
    "LCD Grid (Matriz de Pontos)",
    "Scanlines (CRT)",
    "Scale2x (AdvMAME2x)",
    "HQ2x (High Quality 2x)",
    "xBRZ 2x (Curvas Suaves)",
    "xBRZ 4x (Curvas Ultra)",
    "Pixel-Perfect Sharp (Subpixel Smoother)"
};

const char** post_processing_get_names(void) {
    return g_post_filter_names;
}

/* Helper para diferença de cor entre dois pixels no espaço RGB */
static inline bool color_diff(uint32_t c1, uint32_t c2, int threshold = 30) {
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    return (std::abs(r1 - r2) + std::abs(g1 - g2) + std::abs(b1 - b2)) > threshold;
}

/* Helper de interpolação linear simples de cor.
 * Passo 14e-1: peso em 1/256 e só inteiros (antes era float), para a GPU
 * (gpu_ppu, filtros) dar exatamente o mesmo resultado. Pesos usados:
 * 0,5 = 128; 0,25 = 64; 0,30 = 77; 0,45 = 115; 0,12 = 31. */
static inline uint32_t mix_colors(uint32_t c1, uint32_t c2, uint32_t w256 = 128u) {
    const uint32_t a = c1 & 0xFF000000;
    const uint32_t k = 256u - w256;
    const uint32_t r = (((c1 >> 16) & 0xFF) * k + ((c2 >> 16) & 0xFF) * w256) >> 8;
    const uint32_t g = (((c1 >> 8) & 0xFF) * k + ((c2 >> 8) & 0xFF) * w256) >> 8;
    const uint32_t b = ((c1 & 0xFF) * k + (c2 & 0xFF) * w256) >> 8;
    return a | (r << 16) | (g << 8) | b;
}

/* --- 1. LCD Grid --- */
static void apply_lcd_grid(uint32_t* pixels, int width, int height, int pitch) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    for (int y = 0; y < height; y++) {
        uint32_t* row = pixels + (y * pitch_pixels);
        const bool is_grid_row = (y % 2 == 1);
        for (int x = 0; x < width; x++) {
            if (is_grid_row || (x % 2 == 1)) {
                uint32_t p = row[x];
                uint32_t a = p & 0xFF000000;
                uint32_t r = (((p >> 16) & 0xFF) * 3u) / 4u;   /* = canal * 0,75 (passo 14e-1: inteiros) */
                uint32_t g = (((p >> 8)  & 0xFF) * 3u) / 4u;
                uint32_t b = (( p        & 0xFF) * 3u) / 4u;
                row[x] = a | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/* --- 2. Scanlines CRT --- */
static void apply_scanlines(uint32_t* pixels, int width, int height, int pitch) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    for (int y = 1; y < height; y += 2) {
        uint32_t* row = pixels + (y * pitch_pixels);
        for (int x = 0; x < width; x++) {
            uint32_t p = row[x];
            uint32_t a = p & 0xFF000000;
            uint32_t r = (((p >> 16) & 0xFF) * 6u) / 10u;   /* = canal * 0,60 (passo 14e-1: inteiros) */
            uint32_t g = (((p >> 8)  & 0xFF) * 6u) / 10u;
            uint32_t b = (( p        & 0xFF) * 6u) / 10u;
            row[x] = a | (r << 16) | (g << 8) | b;
        }
    }
}

/* --- 3. Scale2x --- */
static void apply_scale2x(uint32_t* pixels, int width, int height, int pitch) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    std::vector<uint32_t> src_copy((size_t)(pitch_pixels * height));
    std::memcpy(src_copy.data(), pixels, (size_t)(pitch * height));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int ym1 = (y > 0) ? y - 1 : y;
            int yp1 = (y < height - 1) ? y + 1 : y;
            int xm1 = (x > 0) ? x - 1 : x;
            int xp1 = (x < width - 1) ? x + 1 : x;

            uint32_t B = src_copy[(size_t)(y * pitch_pixels + x)];
            uint32_t A = src_copy[(size_t)(ym1 * pitch_pixels + x)];
            uint32_t E = src_copy[(size_t)(yp1 * pitch_pixels + x)];
            uint32_t C = src_copy[(size_t)(y * pitch_pixels + xm1)];
            uint32_t D = src_copy[(size_t)(y * pitch_pixels + xp1)];

            uint32_t E0 = B, E1 = B, E2 = B, E3 = B;

            if (C == A && C != D && A != E) E0 = A;
            if (A == D && A != C && D != E) E1 = D;
            if (C == E && C != D && E != A) E2 = C;
            if (E == D && E != C && D != A) E3 = E;

            uint32_t final_r = (((E0 >> 16) & 0xFF) + ((E1 >> 16) & 0xFF) + ((E2 >> 16) & 0xFF) + ((E3 >> 16) & 0xFF)) / 4;
            uint32_t final_g = (((E0 >> 8)  & 0xFF) + ((E1 >> 8)  & 0xFF) + ((E2 >> 8)  & 0xFF) + ((E3 >> 8)  & 0xFF)) / 4;
            uint32_t final_b = (( E0        & 0xFF) + ( E1        & 0xFF) + ( E2        & 0xFF) + ( E3        & 0xFF)) / 4;

            pixels[y * pitch_pixels + x] = (B & 0xFF000000) | (final_r << 16) | (final_g << 8) | final_b;
        }
    }
}

/* --- 4. HQ2x (High Quality 2x) --- */
static void apply_hq2x(uint32_t* pixels, int width, int height, int pitch) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    std::vector<uint32_t> src((size_t)(pitch_pixels * height));
    std::memcpy(src.data(), pixels, (size_t)(pitch * height));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int ym1 = std::max(0, y - 1), yp1 = std::min(height - 1, y + 1);
            int xm1 = std::max(0, x - 1), xp1 = std::min(width - 1, x + 1);

            uint32_t P5 = src[(size_t)(y * pitch_pixels + x)];
            uint32_t P2 = src[(size_t)(ym1 * pitch_pixels + x)];
            uint32_t P8 = src[(size_t)(yp1 * pitch_pixels + x)];
            uint32_t P4 = src[(size_t)(y * pitch_pixels + xm1)];
            uint32_t P6 = src[(size_t)(y * pitch_pixels + xp1)];

            bool diff_top = color_diff(P5, P2);
            bool diff_bottom = color_diff(P5, P8);
            bool diff_left = color_diff(P5, P4);
            bool diff_right = color_diff(P5, P6);

            uint32_t blend = P5;
            if (diff_top && diff_left) blend = mix_colors(P5, mix_colors(P2, P4, 128u), 64u);
            else if (diff_bottom && diff_right) blend = mix_colors(P5, mix_colors(P8, P6, 128u), 64u);

            pixels[y * pitch_pixels + x] = blend;
        }
    }
}

/* --- 5 e 6. xBRZ (2x e 4x Simplificado por Software) --- */
static void apply_xbrz(uint32_t* pixels, int width, int height, int pitch, int scale) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    std::vector<uint32_t> src((size_t)(pitch_pixels * height));
    std::memcpy(src.data(), pixels, (size_t)(pitch * height));

    const uint32_t blend_weight = (scale == 4) ? 115u : 77u;   /* 0,45 / 0,30 em 1/256 */

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int ym1 = std::max(0, y - 1), yp1 = std::min(height - 1, y + 1);
            int xm1 = std::max(0, x - 1), xp1 = std::min(width - 1, x + 1);

            uint32_t E = src[(size_t)(y * pitch_pixels + x)];
            uint32_t B = src[(size_t)(ym1 * pitch_pixels + x)];
            uint32_t H = src[(size_t)(yp1 * pitch_pixels + x)];
            uint32_t D = src[(size_t)(y * pitch_pixels + xm1)];
            uint32_t F = src[(size_t)(y * pitch_pixels + xp1)];

            // Avalia gradientes horizontais e verticais em busca de bordas diagonais
            bool edge_diagonal_1 = color_diff(D, B) && color_diff(E, F);
            bool edge_diagonal_2 = color_diff(B, F) && color_diff(E, D);

            uint32_t out = E;
            if (edge_diagonal_1) {
                out = mix_colors(E, mix_colors(D, B, 128u), blend_weight);
            } else if (edge_diagonal_2) {
                out = mix_colors(E, mix_colors(B, F, 128u), blend_weight);
            }

            pixels[y * pitch_pixels + x] = out;
        }
    }
}

/* --- 7. Pixel-Perfect Sharp (Suavizador de Subpixel / Sub-stepping) --- */
static void apply_pixel_perfect_sharp(uint32_t* pixels, int width, int height, int pitch) {
    const int pitch_pixels = pitch / (int)sizeof(uint32_t);
    std::vector<uint32_t> src((size_t)(pitch_pixels * height));
    std::memcpy(src.data(), pixels, (size_t)(pitch * height));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int xp1 = std::min(width - 1, x + 1);
            int yp1 = std::min(height - 1, y + 1);

            uint32_t current = src[(size_t)(y * pitch_pixels + x)];
            uint32_t right = src[(size_t)(y * pitch_pixels + xp1)];
            uint32_t down = src[(size_t)(yp1 * pitch_pixels + x)];

            // Aplica uma suavização extremamente sutil apenas na transição direta de bordas (1 pixel de raio)
            if (color_diff(current, right, 40)) {
                current = mix_colors(current, right, 31u);   /* 0,12 */
            }
            if (color_diff(current, down, 40)) {
                current = mix_colors(current, down, 31u);
            }

            pixels[y * pitch_pixels + x] = current;
        }
    }
}

/* --- Disparador Principal de Filtros --- */
void post_processing_apply(GBPostFilterMode mode,
                           uint32_t* pixels,
                           int width,
                           int height,
                           int pitch) {
    if (mode == GB_POST_FILTER_NONE || !pixels || width <= 0 || height <= 0 || pitch <= 0) {
        return;
    }

    switch (mode) {
        case GB_POST_FILTER_LCD_GRID:
            apply_lcd_grid(pixels, width, height, pitch);
            break;

        case GB_POST_FILTER_SCANLINES:
            apply_scanlines(pixels, width, height, pitch);
            break;

        case GB_POST_FILTER_SCALE2X:
            apply_scale2x(pixels, width, height, pitch);
            break;

        case GB_POST_FILTER_HQ2X:
            apply_hq2x(pixels, width, height, pitch);
            break;

        case GB_POST_FILTER_XBRZ_2X:
            apply_xbrz(pixels, width, height, pitch, 2);
            break;

        case GB_POST_FILTER_XBRZ_4X:
            apply_xbrz(pixels, width, height, pitch, 4);
            break;

        case GB_POST_FILTER_PIXEL_PERFECT_SHARP:
            apply_pixel_perfect_sharp(pixels, width, height, pitch);
            break;

        default:
            break;
    }
}