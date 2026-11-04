/**
 * @file post_processing.cpp
 * @brief Filtros de pós-processamento: LCD, Scanlines, Scale2x/3x e xBR.
 *
 * [filtros-v2] Reescrito. A textura chega já ampliada pela resolução interna
 * (cada pixel do Game Boy é um bloco scale x scale). Os escaladores leem um
 * ponto por bloco (a imagem em 1x) e decidem a cor de cada ponto do bloco
 * pela posição dele dentro do bloco. LCD e Scanlines seguem a grade dos
 * pixels do Game Boy em qualquer resolução interna.
 *
 * Tudo em inteiros: o shader da GPU (gpu_ppu.cpp, kFilterFragment) faz as
 * mesmas contas e o comparador CPU x GPU (passo 14e-1) exige o mesmo
 * resultado ponto a ponto.
 *
 * xBR: algoritmo de Hyllian (xBR nível 2, 2x), reescrito aqui.
 * Scale2x/Scale3x: regras do EPX/AdvMAME.
 */

#include "post_processing.h"

#include <cstdlib>
#include <cstring>
#include <vector>

static const char* g_post_filter_names[GB_POST_FILTER_COUNT] = {
    "Off",
    "LCD grid",
    "Scanlines (CRT)",
    "Scale2x / Scale3x (sharp)",
    "xBR (smooth)",
};

const char** post_processing_get_names(void) {
    return g_post_filter_names;
}

bool post_processing_is_scaler(GBPostFilterMode mode) {
    return mode == GB_POST_FILTER_SCALE2X || mode == GB_POST_FILTER_XBR;
}

/* ------------------------------------------------------------------------ */
/* Cores                                                                     */
/* ------------------------------------------------------------------------ */

static inline int ch_r(uint32_t c) { return (int)((c >> 16) & 0xFFu); }
static inline int ch_g(uint32_t c) { return (int)((c >> 8) & 0xFFu); }
static inline int ch_b(uint32_t c) { return (int)(c & 0xFFu); }

static inline uint32_t pack(uint32_t alpha_from, int r, int g, int b) {
    return (alpha_from & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* dst * (256 - w) + src * w, em 1/256 */
static inline uint32_t blend(uint32_t dst, uint32_t src, int w) {
    const int k = 256 - w;
    return pack(dst,
                (ch_r(dst) * k + ch_r(src) * w) >> 8,
                (ch_g(dst) * k + ch_g(src) * w) >> 8,
                (ch_b(dst) * k + ch_b(src) * w) >> 8);
}

static inline uint32_t avg2(uint32_t a, uint32_t b) {
    return pack(a, (ch_r(a) + ch_r(b)) >> 1, (ch_g(a) + ch_g(b)) >> 1, (ch_b(a) + ch_b(b)) >> 1);
}

static inline uint32_t avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return pack(a,
                (ch_r(a) + ch_r(b) + ch_r(c) + ch_r(d)) >> 2,
                (ch_g(a) + ch_g(b) + ch_g(c) + ch_g(d)) >> 2,
                (ch_b(a) + ch_b(b) + ch_b(c) + ch_b(d)) >> 2);
}

static inline uint32_t darken(uint32_t c, int num, int den) {
    return pack(c, ch_r(c) * num / den, ch_g(c) * num / den, ch_b(c) * num / den);
}

/* Diferença de cor do xBR (YUV pesado 48/7/6, coeficientes em 1/256) */
static inline int xbr_df(uint32_t a, uint32_t b) {
    const int dr = ch_r(a) - ch_r(b), dg = ch_g(a) - ch_g(b), db = ch_b(a) - ch_b(b);
    const int y = std::abs(77 * dr + 150 * dg + 29 * db);
    const int u = std::abs(-43 * dr - 85 * dg + 128 * db);
    const int v = std::abs(128 * dr - 107 * dg - 21 * db);
    return (48 * y + 7 * u + 6 * v) >> 8;
}

static inline bool xbr_eq(uint32_t a, uint32_t b) { return xbr_df(a, b) < 155; }

/* ------------------------------------------------------------------------ */
/* Imagem em 1x (um ponto por bloco)                                         */
/* ------------------------------------------------------------------------ */

struct Src1x {
    std::vector<uint32_t> p;
    int w = 0, h = 0;
    uint32_t at(int x, int y) const {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return p[(size_t)y * (size_t)w + (size_t)x];
    }
};

static void take_1x(const uint32_t* pixels, int width, int height, int pitch_px, int s, Src1x& out) {
    out.w = width / s;
    out.h = height / s;
    out.p.resize((size_t)out.w * (size_t)out.h);
    for (int y = 0; y < out.h; y++) {
        const uint32_t* row = pixels + (size_t)(y * s) * (size_t)pitch_px;
        for (int x = 0; x < out.w; x++) out.p[(size_t)y * (size_t)out.w + (size_t)x] = row[x * s];
    }
}

/* Ponto (sx, sy) de um bloco s x s a partir do resultado 2x (q[0] q[1] / q[2] q[3]).
 * 2x: direto; 4x: cada quarto vira 2x2; 3x: a fileira/coluna do meio é a média. */
static inline uint32_t pick_2x(const uint32_t q[4], int s, int sx, int sy) {
    if (s == 3) {
        const int cx = sx == 0 ? 0 : (sx == 2 ? 1 : -1);
        const int cy = sy == 0 ? 0 : (sy == 2 ? 1 : -1);
        if (cx >= 0 && cy >= 0) return q[cy * 2 + cx];
        if (cx >= 0) return avg2(q[cx], q[2 + cx]);
        if (cy >= 0) return avg2(q[cy * 2], q[cy * 2 + 1]);
        return avg4(q[0], q[1], q[2], q[3]);
    }
    const int half = s / 2;
    return q[(sy / half) * 2 + (sx / half)];
}

/* ------------------------------------------------------------------------ */
/* Scale2x / Scale3x (EPX)                                                   */
/* ------------------------------------------------------------------------ */

static void scale2x_px(const Src1x& in, int x, int y, uint32_t q[4]) {
    const uint32_t B = in.at(x, y - 1), D = in.at(x - 1, y), E = in.at(x, y);
    const uint32_t F = in.at(x + 1, y), H = in.at(x, y + 1);
    q[0] = (D == B && B != F && D != H) ? D : E;
    q[1] = (B == F && B != D && F != H) ? F : E;
    q[2] = (D == H && D != B && H != F) ? D : E;
    q[3] = (H == F && D != H && B != F) ? F : E;
}

static void scale3x_px(const Src1x& in, int x, int y, uint32_t q[9]) {
    const uint32_t A = in.at(x - 1, y - 1), B = in.at(x, y - 1), C = in.at(x + 1, y - 1);
    const uint32_t D = in.at(x - 1, y), E = in.at(x, y), F = in.at(x + 1, y);
    const uint32_t G = in.at(x - 1, y + 1), H = in.at(x, y + 1), I = in.at(x + 1, y + 1);
    const bool db = D == B && B != F && D != H;   /* quina de cima à esquerda */
    const bool bf = B == F && B != D && F != H;   /* cima à direita */
    const bool dh = D == H && D != B && H != F;   /* baixo à esquerda */
    const bool hf = H == F && D != H && B != F;   /* baixo à direita */
    q[0] = db ? D : E;
    q[1] = ((db && E != C) || (bf && E != A)) ? B : E;
    q[2] = bf ? F : E;
    q[3] = ((db && E != G) || (dh && E != A)) ? D : E;
    q[4] = E;
    q[5] = ((bf && E != I) || (hf && E != C)) ? F : E;
    q[6] = dh ? D : E;
    q[7] = ((dh && E != I) || (hf && E != G)) ? H : E;
    q[8] = hf ? F : E;
}

/* ------------------------------------------------------------------------ */
/* xBR 2x (nível 2)                                                          */
/* ------------------------------------------------------------------------ */

/* Uma quina (a de baixo à direita, girada pelos parâmetros): n3 = o ponto
 * da quina, n1 e n2 = os dois pontos vizinhos dele no bloco 2x. */
static void xbr_corner(uint32_t q[4],
                       uint32_t PE, uint32_t PI, uint32_t PH, uint32_t PF, uint32_t PG,
                       uint32_t PC, uint32_t PD, uint32_t PB,
                       uint32_t F4, uint32_t I4, uint32_t H5, uint32_t I5,
                       int n1, int n2, int n3) {
    if (PE == PH || PE == PF) return;
    const int e = xbr_df(PE, PC) + xbr_df(PE, PG) + xbr_df(PI, H5) + xbr_df(PI, F4) + (xbr_df(PH, PF) << 2);
    const int i = xbr_df(PH, PD) + xbr_df(PH, I5) + xbr_df(PF, I4) + xbr_df(PF, PB) + (xbr_df(PE, PI) << 2);
    const uint32_t px = (xbr_df(PE, PF) <= xbr_df(PE, PH)) ? PF : PH;
    const bool rule = (!xbr_eq(PF, PB) && !xbr_eq(PH, PD)) ||
                      (xbr_eq(PE, PI) && !xbr_eq(PF, I4) && !xbr_eq(PH, I5)) ||
                      xbr_eq(PE, PG) || xbr_eq(PE, PC);
    if (e < i && rule) {
        const int ke = xbr_df(PF, PG), ki = xbr_df(PH, PC);
        const bool ex2 = PE != PC && PB != PC;
        const bool ex3 = PE != PG && PD != PG;
        const bool shallow = (ke << 1) <= ki && ex3;
        const bool steep = ke >= (ki << 1) && ex2;
        if (shallow && steep) {
            q[n3] = blend(q[n3], px, 224);
            q[n2] = blend(q[n2], px, 64);
            q[n1] = q[n2];
        } else if (shallow) {
            q[n3] = blend(q[n3], px, 192);
            q[n2] = blend(q[n2], px, 64);
        } else if (steep) {
            q[n3] = blend(q[n3], px, 192);
            q[n1] = blend(q[n1], px, 64);
        } else {
            q[n3] = blend(q[n3], px, 128);
        }
    } else if (e <= i) {
        q[n3] = blend(q[n3], px, 64);
    }
}

static void xbr_px(const Src1x& in, int x, int y, uint32_t q[4]) {
    /*        A1 B1 C1
     *     A0 A  B  C  C4
     *     D0 D  E  F  F4
     *     G0 G  H  I  I4
     *        G5 H5 I5      */
    const uint32_t A1 = in.at(x - 1, y - 2), B1 = in.at(x, y - 2), C1 = in.at(x + 1, y - 2);
    const uint32_t A0 = in.at(x - 2, y - 1), A = in.at(x - 1, y - 1), B = in.at(x, y - 1);
    const uint32_t C = in.at(x + 1, y - 1), C4 = in.at(x + 2, y - 1);
    const uint32_t D0 = in.at(x - 2, y), D = in.at(x - 1, y), E = in.at(x, y);
    const uint32_t F = in.at(x + 1, y), F4 = in.at(x + 2, y);
    const uint32_t G0 = in.at(x - 2, y + 1), G = in.at(x - 1, y + 1), H = in.at(x, y + 1);
    const uint32_t I = in.at(x + 1, y + 1), I4 = in.at(x + 2, y + 1);
    const uint32_t G5 = in.at(x - 1, y + 2), H5 = in.at(x, y + 2), I5 = in.at(x + 1, y + 2);
    q[0] = q[1] = q[2] = q[3] = E;
    /* baixo à direita, cima à direita, cima à esquerda, baixo à esquerda */
    xbr_corner(q, E, I, H, F, G, C, D, B, F4, I4, H5, I5, 1, 2, 3);
    xbr_corner(q, E, C, F, B, I, A, H, D, B1, C1, F4, C4, 0, 3, 1);
    xbr_corner(q, E, A, B, D, C, G, F, H, D0, A0, B1, A1, 2, 1, 0);
    xbr_corner(q, E, G, D, H, A, I, B, F, H5, G5, D0, G0, 3, 0, 2);
}

/* ------------------------------------------------------------------------ */

static void apply_grid(uint32_t* pixels, int width, int height, int pitch_px, int s, bool lcd) {
    if (s < 2) return;
    const int first_dark_row = lcd ? s - 1 : (s + 1) / 2;   /* scanline: metade de baixo */
    for (int y = 0; y < height; y++) {
        uint32_t* row = pixels + (size_t)y * (size_t)pitch_px;
        const bool dark_row = (y % s) >= first_dark_row;
        for (int x = 0; x < width; x++) {
            if (lcd) {
                if (dark_row || (x % s) == s - 1) row[x] = darken(row[x], 3, 4);
            } else if (dark_row) {
                row[x] = darken(row[x], 6, 10);
            }
        }
    }
}

static void apply_scaler(uint32_t* pixels, int width, int height, int pitch_px, int s, bool xbr) {
    if (s < 2 || s > 4) return;
    Src1x in;
    take_1x(pixels, width, height, pitch_px, s, in);
    uint32_t q[9];
    for (int y = 0; y < in.h; y++) {
        for (int x = 0; x < in.w; x++) {
            const bool three = !xbr && s == 3;
            if (xbr) xbr_px(in, x, y, q);
            else if (three) scale3x_px(in, x, y, q);
            else scale2x_px(in, x, y, q);
            for (int sy = 0; sy < s; sy++) {
                uint32_t* row = pixels + (size_t)(y * s + sy) * (size_t)pitch_px + (size_t)(x * s);
                for (int sx = 0; sx < s; sx++) {
                    row[sx] = three ? q[sy * 3 + sx] : pick_2x(q, s, sx, sy);
                }
            }
        }
    }
}

void post_processing_apply(GBPostFilterMode mode,
                           uint32_t* pixels,
                           int width,
                           int height,
                           int pitch,
                           int scale) {
    if (mode == GB_POST_FILTER_NONE || !pixels || width <= 0 || height <= 0 || pitch <= 0 || scale < 1) {
        return;
    }
    const int pitch_px = pitch / (int)sizeof(uint32_t);
    switch (mode) {
        case GB_POST_FILTER_LCD_GRID:  apply_grid(pixels, width, height, pitch_px, scale, true); break;
        case GB_POST_FILTER_SCANLINES: apply_grid(pixels, width, height, pitch_px, scale, false); break;
        case GB_POST_FILTER_SCALE2X:   apply_scaler(pixels, width, height, pitch_px, scale, false); break;
        case GB_POST_FILTER_XBR:       apply_scaler(pixels, width, height, pitch_px, scale, true); break;
        default: break;
    }
}
