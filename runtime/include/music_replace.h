/**
 * @file music_replace.h
 * @brief Passo 6c: troca a música do Game Boy por trilhas .ogg dos mods.
 *
 * Nos mods (.o2r ou pasta mods/hd), qualquer arquivo dentro de uma pasta
 * "music" cujo nome começa com o número da música em hexa vira a trilha
 * dessa música: music/04.ogg, music/04_rice_beach.ogg, ...
 * Loop: tags LOOPSTART / LOOPLENGTH (ou LOOPEND) do próprio .ogg, ou
 * music/music.json:
 *   { "04": { "loop_start": 123456, "loop_end": 0, "loop": true, "volume": 1.0 },
 *     "2B": { "loop": false } }
 * (loop_start/loop_end em amostras do .ogg; loop_end 0 = fim do arquivo.)
 * Enquanto uma trilha toca, a música do GB é silenciada pelo portão do
 * audio.c (os efeitos continuam).
 */
#ifndef MUSIC_REPLACE_H
#define MUSIC_REPLACE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Relê as trilhas dos mods ligados (chamar depois de hd_pack_io_load_pack). */
void music_replace_rescan(void);
int music_replace_track_count(void);
/* Lista curta das músicas com trilha, ex.: "04 15 2B". */
const char* music_replace_track_list(void);

/* Thread principal, quando $A61D muda. delay_frames = quanto áudio do GB
 * ainda está no buffer (a trilha entra junto com o que se ouve). */
void music_replace_set_song(int song, uint32_t delay_frames);
/* A música atual tem trilha tocando (usado para ligar o portão). */
bool music_replace_active(void);
/* Libera trilhas que já pararam (thread principal, uma vez por frame). */
void music_replace_collect(void);

void music_replace_set_enabled(bool enabled);
bool music_replace_get_enabled(void);
void music_replace_set_volume(float volume);   /* 0..2 */
float music_replace_get_volume(void);
/* Passo 6d: mensagens "[Musica] ..." no terminal (erros sempre aparecem). */
void music_replace_set_log(bool enabled);

/* Thread de áudio: soma as trilhas em out (estéreo int16, frames quadros).
 * master = volume geral (0 quando mudo). */
void music_replace_mix(int16_t* out, uint32_t frames, uint32_t device_rate, float master);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_REPLACE_H */
