/**
 * @file android_bridge.h
 * @brief Ponte com o lado Java no Android (armazenamento, seletor de arquivos).
 *
 * Fora do Android todas as funções são stubs vazios, então este arquivo pode
 * ser compilado em qualquer plataforma.
 */
#pragma once

#include <string>

namespace android_bridge {

/* Tipo de arquivo pedido ao seletor. */
enum PickKind { PICK_ROM = 0, PICK_PATCH = 1 };

/*
 * Chamar no início do main() no Android:
 *  - pasta de trabalho = <interno>/runtime (onde fica o wario.o2r)
 *  - extrai tools/wl_sm3_dx_v12_patch.json dos assets do APK
 *  - redireciona stdout/stderr para <pasta do usuário>/last_run.log
 */
void prepare_storage();

/*
 * Pasta acessível pelo usuário (gerenciador de arquivos / USB):
 * Android/data/<pacote>/files/  — com barra no fim. É onde ficam os mods.
 * Fora do Android devolve string vazia.
 */
std::string user_dir();

/* Abre o seletor de documentos do sistema (assíncrono). */
bool request_file_pick(PickKind kind);

/*
 * Consulta o resultado do seletor. Devolve true quando chegou uma resposta;
 * path vem vazio se o usuário cancelou. O arquivo já foi copiado para o
 * armazenamento interno do app, então é um caminho comum de sistema de arquivos.
 */
bool poll_picked_file(int& kind, std::string& path);

} // namespace android_bridge
