#include "asset_extractor.hpp"
#include "platform_sdl.h"
#include "wl_sm3_patch.hpp"
#include "gbrt.h"
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <cstring>

extern "C" {
    const GBConfig* hack1_2_WL_SM3_default_config(void);
}

static std::filesystem::path find_patch_manifest(const char* executable_path) {
    const std::filesystem::path relative_path = "tools/wl_sm3_dx_v12_patch.json";
    const std::filesystem::path executable = std::filesystem::absolute(executable_path);
    const std::filesystem::path candidates[] = {
        std::filesystem::current_path() / relative_path,
        executable.parent_path() / relative_path,
        executable.parent_path().parent_path() / relative_path,
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("Manifesto tools/wl_sm3_dx_v12_patch.json nao encontrado");
}

int main(int argc, char* argv[]) {
    const std::filesystem::path rom_path = argc > 1 ? argv[1] : "rom.gb";
    std::vector<uint8_t> patched_rom;
    try {
        const std::filesystem::path manifest_path = find_patch_manifest(argv[0]);
        patched_rom = wl_sm3_patch::load_and_patch_rom(rom_path, manifest_path);
        std::cout << "IPS aplicado em memoria: " << rom_path << " -> "
                  << patched_rom.size() << " bytes\n";
    } catch (const std::exception& error) {
        std::cerr << "Falha ao preparar ROM: " << error.what() << '\n'
                  << "Uso: wario_land_port <caminho/para/rom-original.gb>\n";
        return 1;
    }

    AssetExtractor extractor;
    
    // 1. Roda o assistente do wario.o2r lícito aberto (Estilo ZIP do PaperBoat)
    if (!extractor.check_and_prepare_assets(rom_path)) {
        return 1;
    }

    std::cout << "[VFS] Inicializando alocacao estruturada de contexto...\n";

    // 2. Configurações de Hardware Oficiais do Compilador
    GBConfig runtime_config = *hack1_2_WL_SM3_default_config();
    runtime_config.model = runtime_config.cartridge_supports_cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    runtime_config.cgb_compatibility_mode = false;
    runtime_config.native_presentation_enabled = false;
    runtime_config.enable_audio = true; 

    // 3. Aloca o contexto oficial via motor GBRT
    GBContext* ctx = gb_context_create(&runtime_config);
    if (!ctx) {
        std::cerr << "❌ Erro Crítico: Falha ao criar o contexto oficial da engine.\n";
        return 1;
    }
    
    gb_context_set_save_id(ctx, "wario_land_sm3_dx_v12");
    if (!gb_context_load_rom(ctx, patched_rom.data(), patched_rom.size())) {
        std::cerr << "Falha ao carregar a ROM corrigida no runtime.\n";
        gb_context_destroy(ctx);
        return 1;
    }
    gb_context_reset(ctx, true);

#ifdef GB_HAS_SDL2
    // 4. Inicializa os drivers gráficos do SDL2 e monta a janela física do PC
    if (!gb_platform_init(5)) {
        std::cerr << "❌ Erro Crítico: Falha ao inicializar a plataforma SDL2.\n";
        gb_context_destroy(ctx);
        return 1;
    }
    // Vincula o contexto aos barramentos de textura da platform_sdl e do ImGui
    gb_platform_register_context(ctx);
#endif

    std::cout << "🎮 Wario Land Nativo pronto. Iniciando loop grafico estável em 60Hz...\n";

    // 5. LOOP GRÁFICO SÍNCRONO BASEADO NO _MAIN.C ORIGINAL
    // Agora que a PPU está corrigida, a flag ctx->frame_done vai funcionar perfeitamente!
    const uint32_t lcd_smooth_slice_cycles = 70224u; // 1 frame completo de Game Boy
    bool running = true;
    
    while (running) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        
        while (!ctx->frame_done && !ctx->stopped) {
            // Executa ciclos lícitos na CPU nativa de PC
            gb_run_cycles(ctx, lcd_smooth_slice_cycles);
            
            // Garante que os inputs e o ImGui respondam em tempo real durante as sub-fatias
            if (!gb_platform_poll_events(ctx)) {
                running = false;
                break;
            }
        }
        
        if (!running) break;

        // Com o frame fechado perfeitamente, descarrega a sua PPU widescreen e renderiza o ImGui
        if (ctx->frame_done) {
            const uint32_t* fb = gb_get_framebuffer(ctx);
            if (fb) {
                gb_platform_render_frame(fb);
            }
            
            // Sincroniza com o clock do PC (cravando em 60 FPS via hardware)
            gb_platform_vsync(ctx->frame_cycles);
        }
    }

    // 6. Encerramento limpo ao fechar a janela
    std::cout << "[GBRT] Salvando progresso e fechando barramentos...\n";
#ifdef GB_HAS_SDL2
    gb_platform_shutdown();
#endif
    gb_context_destroy(ctx);
    return 0;
}
