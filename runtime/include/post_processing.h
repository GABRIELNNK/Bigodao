/**
 * @file post_processing.h
 * @brief Filtros visuais por software avançados para o renderizador de Game Boy
 */

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GBPostFilterMode {
    GB_POST_FILTER_NONE = 0,
    GB_POST_FILTER_LCD_GRID,
    GB_POST_FILTER_SCANLINES,
    GB_POST_FILTER_SCALE2X,
    GB_POST_FILTER_HQ2X,
    GB_POST_FILTER_XBRZ_2X,
    GB_POST_FILTER_XBRZ_4X,
    GB_POST_FILTER_PIXEL_PERFECT_SHARP,
    GB_POST_FILTER_COUNT
} GBPostFilterMode;

/**
 * @brief Retorna a lista com os nomes legíveis de todos os filtros.
 */
const char** post_processing_get_names(void);

/**
 * @brief Processa e aplica o filtro selecionado no buffer de pixels da textura.
 */
void post_processing_apply(GBPostFilterMode mode,
                           uint32_t* pixels,
                           int width,
                           int height,
                           int pitch);

#ifdef __cplusplus
}
#endif