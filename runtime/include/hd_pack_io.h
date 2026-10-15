/**
 * @file hd_pack_io.h
 * @brief Arquivos dos tiles HD (passo 5): gravação do despejo em PNG.
 *        O carregamento da pasta de mods entra no passo 5b.
 */
#ifndef HD_PACK_IO_H
#define HD_PACK_IO_H

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* HD_PACK_IO_H */
