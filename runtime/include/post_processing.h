/**
 * @file post_processing.h
 * @brief Filtros de pós-processamento ([filtros-v2]: LCD, Scanlines, Scale2x/3x, xBR)
 */

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* [filtros-v2] Os números 0-4 ficam iguais aos de antes nas preferências
 * (o 4 era HQ2x e vira xBR); 5-7 antigos são convertidos ao carregar. */
typedef enum GBPostFilterMode {
    GB_POST_FILTER_NONE = 0,
    GB_POST_FILTER_LCD_GRID,
    GB_POST_FILTER_SCANLINES,
    GB_POST_FILTER_SCALE2X,     /* Scale2x (2x e 4x) / Scale3x (3x) */
    GB_POST_FILTER_XBR,         /* xBR nível 2 */
    GB_POST_FILTER_COUNT
} GBPostFilterMode;

/** Nomes de todos os filtros (para o menu). */
const char** post_processing_get_names(void);

/** true para os filtros que reconstroem a imagem a partir do 1x (Scale2x, xBR). */
bool post_processing_is_scaler(GBPostFilterMode mode);

/**
 * Aplica o filtro na textura já ampliada: cada pixel do Game Boy é um bloco
 * scale x scale (resolução interna). Os escaladores precisam de scale >= 2.
 */
void post_processing_apply(GBPostFilterMode mode,
                           uint32_t* pixels,
                           int width,
                           int height,
                           int pitch,
                           int scale);

#ifdef __cplusplus
}
#endif
