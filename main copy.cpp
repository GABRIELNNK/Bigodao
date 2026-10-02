#include "asset_extractor.hpp"
#include "platform_sdl.h"
#include "gbrt.h"
#include <iostream>
#include <cstdlib>
#include <cstring>

extern "C" {
    // Declarações cruciais das funções geradas pelo recompilador para o Wario Land
    GBConfig* base_WL_SM3_default_config(void);
    void base_WL_SM3_init(GBContext* ctx);
}

int main(int argc, char* argv[]) {
    AssetExtractor extractor;
    
    // 1. Roda o assistente do wario.o2r lícito aberto (Estilo ZIP do PaperBoat)
    if (!extractor.check_and_prepare_assets()) {
        return 1;
    }

    std::cout << "[VFS] Inicializando alocacao estruturada de contexto...\n";

    // 2. Coleta as configurações de hardware originais
    GBConfig runtime_config = *base_WL_SM3_default_config();
    runtime_config.model = runtime_config.cartridge_supports_cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    runtime_config.cgb_compatibility_mode = false;
    runtime_config.native_presentation_enabled = false;

    // 3. Aloca o contexto oficial via engine para garantir estabilidade absoluta
    GBContext* ctx = gb_context_create(&runtime_config);
    if (!ctx) {
        std::cerr << "❌ Erro Crítico: Falha ao criar o contexto oficial da engine.\n";
        return 1;
    }
    gb_context_set_save_id(ctx, "base_WL_SM3");

    // Inicializa as tabelas estáticas de funções recompiladas
    base_WL_SM3_init(ctx);

    // 4. Inicializa os drivers de vídeo e janela do SDL2 com escala 5x
    if (!gb_platform_init(5)) {
        std::cerr << "❌ Erro Crítico: Falha ao inicializar a plataforma SDL2.\n";
        gb_context_destroy(ctx);
        return 1;
    }
    gb_platform_register_context(ctx);

    std::cout << "🎮 Wario Land Nativo iniciado com sucesso e com suporte a Janela Gráfica!\n";

    // 5. Loop Principal de Alta Performance Sincronizado
    bool running = true;
    while (running) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        
        while (!ctx->frame_done) {
            // Executa fatias de ciclos de clock nativos de PC
            gb_run_cycles(ctx, 70224u);
            
            // Trata eventos e inputs de teclado/controle mapeados na sua platform_sdl.cpp
            if (!gb_platform_poll_events(ctx)) {
                running = false;
                break;
            }
        }
        
        if (!running) break;

        // Renderiza o frame final composto pela PPU modificada em Widescreen
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (fb) {
            gb_platform_render_frame(fb);
        }
        
        // Aplica o VSync por relógio de parede cravado para manter o pacing lícito
        gb_platform_vsync(ctx->frame_cycles);
    }

    // 6. Encerramento limpo de memória e barramentos
    gb_platform_shutdown();
    gb_context_destroy(ctx);
    return 0;
}
