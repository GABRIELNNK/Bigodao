/**
 * @file hd_pack_io.h
 * @brief Arquivos dos tiles HD (passo 5): despejo de tiles e captura de cena.
 *        O carregamento da pasta de mods entra no passo 5b.
 */
#ifndef HD_PACK_IO_H
#define HD_PACK_IO_H

#include <stdint.h>
#include "ppu_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pasta base (a do executável). O despejo vai para <base>/mods/dump. */
void hd_pack_io_set_base_dir(const char* base_dir);
const char* hd_pack_io_dump_dir(void);

/* Retira até max tiles da fila do ppu_native e grava cada um como PNG 8x8.
 * Arquivos que já existem não são reescritos. Devolve quantos gravou. */
int hd_pack_io_flush_dumps(int max);
uint32_t hd_pack_io_dumped_total(void);

/* Passo 5a.2: grava a cena capturada em <base>/mods/scenes:
 *   scene_NNN_bg.png / _obj.png        (1x, para referência)
 *   scene_NNN_bg_4x.png / _obj_4x.png  (base para pintar em 4x)
 *   scene_NNN.json                     (origem de cada pixel, para fatiar)
 * Devolve NNN ou -1. */
int hd_pack_io_write_scene(const PPUNativeScene* scene);
const char* hd_pack_io_scene_dir(void);
const char* hd_pack_io_last_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* HD_PACK_IO_H */
