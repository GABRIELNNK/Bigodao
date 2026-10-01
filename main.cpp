#include "asset_extractor.hpp"
#include "platform_sdl.h"
#include "gbrt.h"

int main(int argc, char* argv[]) {
    AssetExtractor extractor;
    
    // 1. Roda a verificação de segurança e extração de assets (.o2r) estilo PaperBoat
    if (!extractor.check_and_prepare_assets()) {
        return 1; // Fecha a execução se a ROM for inválida ou estiver ausente
    }

    // 2. Inicializa a janela gráfica do PC via SDL2 com escala padrão 5x
    if (!gb_platform_init(5)) {
        return 1;
    }

    // 3. Inicializa o contexto global do Wario Land Recompilado
    GBContext* ctx = gb_context_alloc();
    gb_platform_register_context(ctx);

    // 4. Loop Principal de Execução Nativa (Pacing cravado na parede)
    bool running = true;
    while (running) {
        // Captura entradas de controle/teclado e eventos de janela
        running = gb_platform_poll_events(ctx);
        
        // Executa um frame completo de instruções do Wario de forma nativa no PC
        gb_run_frame(ctx);
        
        // Sincroniza o frame com o clock real
        gb_platform_vsync(ctx->frame_cycles);
    }

    // 5. Desliga o sistema de forma limpa ao fechar a janela
    gb_platform_shutdown();
    gb_context_free(ctx);

    return 0;
}
