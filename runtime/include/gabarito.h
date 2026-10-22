/**
 * @file gabarito.h
 * @brief Gabarito do renderizador (passo 12a).
 *
 * Um gabarito é um savestate mais o quadro que o renderizador desenha a partir
 * dele. Para conferir o renderizador depois de uma mudança (ppu.c, GPU...),
 * cada savestate é carregado de novo, o jogo roda alguns quadros sem nenhum
 * botão apertado e o quadro é comparado ponto a ponto com o gravado.
 *
 * O quadro gravado não é a foto da tela no momento do F4: é o resultado dessa
 * mesma repetição (carregar, rodar, desenhar). Assim os dois lados da
 * comparação passam pelo mesmo caminho e o resultado não depende do que tinha
 * passado na tela antes.
 *
 * Arquivos em <base>/mods/gabarito:
 *   gab_NNN.state       savestate
 *   gab_NNN.json        escala, mods ligados, contexto, modo do renderizador
 *   gab_NNN_1x.png      saída da PPU em 1x (com as laterais, se widescreen)
 *   gab_NNN_4x.png      quadro HD em 4x (só quando havia tiles HD ligados)
 *   gab_NNN_dif_1x.png / _dif_4x.png  pontos diferentes em vermelho (conferência)
 *
 * Compara a saída do renderizador (PPU nativa + laterais + HD), antes do
 * fundo ambiente das laterais, filtros e pós-processamento.
 */
#ifndef GABARITO_H
#define GABARITO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/* Pedidos (do menu ou do F4). O trabalho acontece em gabarito_process(). */
void gabarito_request_capture(void);
void gabarito_request_check(void);
/* Regrava as imagens de todos os gabaritos com o resultado atual (depois de
 * uma mudança de propósito no desenho). */
void gabarito_request_rewrite(void);
bool gabarito_busy(void);

/* Chamado pela plataforma entre um quadro e outro (início do vsync). Devolve
 * true se rodou algo: o estado do jogo foi devolvido ao que era, mas o áudio
 * e o último quadro guardados pela plataforma precisam ser descartados, como
 * depois de carregar um savestate. */
bool gabarito_process(GBContext* ctx);

const char* gabarito_dir(void);
const char* gabarito_status(void);
int gabarito_result_count(void);
const char* gabarito_result_text(int index);
bool gabarito_result_ok(int index);

#ifdef __cplusplus
}
#endif

#endif /* GABARITO_H */
