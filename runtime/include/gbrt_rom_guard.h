/**
 * @file gbrt_rom_guard.h
 * @brief Carga da ROM com validação: o runtime aborta se o jogo rodar sem ela.
 *
 * A ROM não vai no executável; chega em execução pelo launcher ou pelo .o2r.
 * gb_run_cycles() e gb_step() abortam se a imagem em ctx->rom não tiver passado
 * por esta função.
 */
#ifndef GBRT_ROM_GUARD_H
#define GBRT_ROM_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @return false, sem carregar nada, se o SHA-256 (hex) da imagem não bater. */
bool gb_context_load_validated_rom(GBContext* ctx,
                                   const uint8_t* data,
                                   size_t size,
                                   const char* expected_sha256);

#ifdef __cplusplus
}
#endif

#endif
