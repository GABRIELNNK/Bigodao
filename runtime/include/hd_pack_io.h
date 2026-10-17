/**
 * @file hd_pack_io.h
 * @brief Arquivos dos tiles HD (passo 5): despejo de tiles e captura de cena.
 *        e carregamento dos mods HD (mods/<nome>.o2r e mods/hd).
 */
#ifndef HD_PACK_IO_H
#define HD_PACK_IO_H

#include <stdint.h>
#include <stddef.h> /* passo 6c */
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

/* Passo 5b/5c: mods HD.
 * Relê a lista de mods e carrega os ligados no ppu_native, substituindo o que
 * estava carregado. Fontes, nesta ordem (a de baixo sobrepõe a de cima):
 *   <base>/mods/<nome>.o2r  (ZIP com outra extensão), em ordem alfabética;
 *   <base>/mods/hd     (pasta solta).
 * Em cada fonte: b_<tile>.png / o_<tile>.png (qualquer subpasta) e pack.json.
 * Devolve quantos tiles HD ficaram carregados. */
int hd_pack_io_load_pack(void);
const char* hd_pack_io_pack_dir(void);   /* <base>/mods/hd */
const char* hd_pack_io_mods_dir(void);   /* <base>/mods */
const char* hd_pack_io_pack_status(void);

/* Lista de mods encontrada no último hd_pack_io_load_pack(). */
int hd_pack_io_mod_count(void);
const char* hd_pack_io_mod_name(int index);   /* "meu_mod.o2r" ou "hd/" */
const char* hd_pack_io_mod_info(int index);   /* "38 tiles", "desligado", erro */
bool hd_pack_io_mod_is_folder(int index);
bool hd_pack_io_mod_enabled(int index);
/* Liga/desliga e recarrega os tiles na hora. */
void hd_pack_io_set_mod_enabled(int index, bool enabled);

/* Nomes desligados (salvos nas preferências; mod novo começa ligado). */
void hd_pack_io_clear_disabled_names(void);
void hd_pack_io_add_disabled_name(const char* name);
int hd_pack_io_disabled_count(void);
const char* hd_pack_io_disabled_name(int index);

/* Passo 6c: arquivos avulsos dos mods ligados (usado pelas trilhas .ogg).
 * Visita cada arquivo das fontes ligadas, na ordem da lista (a de baixo
 * sobrepoe a de cima); path e relativo a raiz do .o2r ou da pasta. */
typedef void (*hd_pack_io_file_visitor)(int source_index, const char* source_name,
                                        const char* path, void* user);
void hd_pack_io_list_files(hd_pack_io_file_visitor visit, void* user);
/* Le um arquivo de uma fonte (so a parte dele no .o2r). Buffer de malloc:
 * liberar com free(). NULL se nao achar. */
uint8_t* hd_pack_io_read_file(int source_index, const char* path, size_t* out_size);
/* Muda a cada recarga ou liga/desliga de mod. */
uint32_t hd_pack_io_generation(void);

/* Passo 7a: gravador de poses de sprites. Enquanto ativo, a plataforma
 * entrega uma cena por frame (feed). Ao parar, grava a folha de poses em
 * <base>/mods/scenes/sprites_NNN.json + _obj.png + _obj_4x.png (formato de
 * cena: pinta-se o _obj_4x.png e fatia-se com tools/hd_slice.py --obj).
 * Retangulo em pixels da tela do jogo (160x144, sem as laterais): so entram
 * as poses com o centro dentro dele. */
void hd_pack_io_sprite_rec_start(void);
int hd_pack_io_sprite_rec_stop(void);          /* NNN gravado, ou -1 se nao havia poses */
bool hd_pack_io_sprite_rec_active(void);
void hd_pack_io_sprite_rec_feed(const PPUNativeScene* scene);
void hd_pack_io_sprite_rec_set_rect(bool enabled, int x, int y, int w, int h);
uint32_t hd_pack_io_sprite_rec_pose_count(void);
uint32_t hd_pack_io_sprite_rec_frames(void);
const char* hd_pack_io_sprite_rec_last(void);   /* caminho sem extensao */

#ifdef __cplusplus
}
#endif

#endif /* HD_PACK_IO_H */
