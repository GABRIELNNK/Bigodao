#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#include "asset_extractor.hpp"
#include "platform_sdl.h"
#include "ppu.h"
#include "wl_sm3_patch.hpp"
#include "gbrt.h"

#include <SDL2/SDL.h>
#include "runtime/vendor/imgui/imgui.h"
#include "runtime/vendor/imgui/backends/imgui_impl_sdl2.h"
#include "runtime/vendor/imgui/backends/imgui_impl_sdlrenderer2.h"


#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <sstream>

// Expected SHA256 hashes
static const std::string EXPECTED_ROM_SHA256   = "ac1682f17abcf590311a233289ee325214c2d71ab3a5aa175004002d85075e56";
static const std::string EXPECTED_PATCH_SHA256 = "599d9f59090df3e1e77c1678e069290a2cd9ce59c71cf58d66c4dc3db8b7facd";

extern "C" {
    const GBConfig* hack1_2_WL_SM3_default_config(void);
}

// -----------------------------------------------------------------------------
// Pure C++ SHA-256 Implementation (No external OpenSSL dependency)
// -----------------------------------------------------------------------------
namespace SHA256_Util {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    #define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    #define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
    #define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
    #define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
    #define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
    #define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
    #define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

    struct Context {
        uint8_t data[64];
        uint32_t datalen;
        unsigned long long bitlen;
        uint32_t state[8];
    };

    static void init(Context *ctx) {
        ctx->datalen = 0;
        ctx->bitlen = 0;
        ctx->state[0] = 0x6a09e667;
        ctx->state[1] = 0xbb67ae85;
        ctx->state[2] = 0x3c6ef372;
        ctx->state[3] = 0xa54ff53a;
        ctx->state[4] = 0x510e527f;
        ctx->state[5] = 0x9b05688c;
        ctx->state[6] = 0x1f83d9ab;
        ctx->state[7] = 0x5be0cd19;
    }

    static void transform(Context *ctx, const uint8_t data[]) {
        uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
        for (i = 0, j = 0; i < 16; ++i, j += 4)
            m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
        for (; i < 64; ++i)
            m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

        a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
        e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

        for (i = 0; i < 64; ++i) {
            t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
            t2 = EP0(a) + MAJ(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
        ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
    }

    static void update(Context *ctx, const uint8_t data[], size_t len) {
        for (size_t i = 0; i < len; ++i) {
            ctx->data[ctx->datalen] = data[i];
            ctx->datalen++;
            if (ctx->datalen == 64) {
                transform(ctx, ctx->data);
                ctx->bitlen += 512;
                ctx->datalen = 0;
            }
        }
    }

    static void final(Context *ctx, uint8_t hash[]) {
        uint32_t i = ctx->datalen;
        if (ctx->datalen < 56) {
            ctx->data[i++] = 0x80;
            while (i < 56) ctx->data[i++] = 0x00;
        } else {
            ctx->data[i++] = 0x80;
            while (i < 64) ctx->data[i++] = 0x00;
            transform(ctx, ctx->data);
            memset(ctx->data, 0, 56);
        }

        ctx->bitlen += ctx->datalen * 8;
        ctx->data[63] = ctx->bitlen;
        ctx->data[62] = ctx->bitlen >> 8;
        ctx->data[61] = ctx->bitlen >> 16;
        ctx->data[60] = ctx->bitlen >> 24;
        ctx->data[59] = ctx->bitlen >> 32;
        ctx->data[58] = ctx->bitlen >> 40;
        ctx->data[57] = ctx->bitlen >> 48;
        ctx->data[56] = ctx->bitlen >> 56;
        transform(ctx, ctx->data);

        for (i = 0; i < 4; ++i) {
            hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
            hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
        }
    }
}

// Function to compute SHA256 hex string for a given file
static std::string calculate_file_sha256(const std::string& path) {
    if (!std::filesystem::is_regular_file(path)) return "";

    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    SHA256_Util::Context ctx;
    SHA256_Util::init(&ctx);

    uint8_t buffer[8192];
    while (file.read(reinterpret_cast<char*>(buffer), sizeof(buffer)) || file.gcount() > 0) {
        SHA256_Util::update(&ctx, buffer, file.gcount());
    }

    uint8_t hash[32];
    SHA256_Util::final(&ctx, hash);

    std::stringstream ss;
    for (int i = 0; i < 32; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// Locate patch JSON manifest
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
    throw std::runtime_error("Manifest tools/wl_sm3_dx_v12_patch.json not found.");
}

static std::string open_file_dialog(const char* title, const char* filter) {
#ifdef _WIN32
    // Versão Nativa para Windows
    OPENFILENAMEA ofn;
    char szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    
    // Converte o filtro simples para o formato do Windows (separado por \0)
    ofn.lpstrFilter = "Todos os Arquivos\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }
    return "";
#else
    // Mantém a versão original caso compile no Linux Nativo
    std::string command = "";
    if (system("which zenity > /dev/null 2>&1") == 0) {
        command = std::string("zenity --file-selection --title=\"") + title + "\" " + filter + " 2>/dev/null";
    } else if (system("which kdialog > /dev/null 2>&1") == 0) {
        command = std::string("kdialog --getopenfilename . \"") + filter + "\" --title \"" + title + "\" 2>/dev/null";
    }

    if (!command.empty()) {
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            std::string result = "";
            if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                result = buffer;
                result.erase(result.find_last_not_of("\r\n") + 1);
            }
            pclose(pipe);
            return result;
        }
    }
    return "";
#endif
}

// Execute Launcher GUI via ImGui + SDL2
static bool run_imgui_launcher(std::string& out_rom_path, std::string& out_patch_path, const char* exec_path) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Failed to initialize SDL2 for Launcher: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Bigodão Launcher - Wario Land DX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        660, 520, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        SDL_Quit();
        return false;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    static char rom_buf[512] = "";
    static char patch_buf[512] = "";
    
    std::string error_popup_msg = "";
    bool show_error_popup = false;
    bool ready_to_launch = false;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("MainLauncher", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        // Header
        ImGui::SetWindowFontScale(1.35f);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "BIGODÃO LAUNCHER");
        ImGui::SetWindowFontScale(1.0f);
        
        ImGui::TextDisabled("Wario Land: Super Mario Land 3 DX");
        ImGui::Separator();
        ImGui::Spacing();

        // -----------------------------------------------------------------
        // 1. SELECT & VALIDATE BASE ROM
        // -----------------------------------------------------------------
        ImGui::Text("1. Base ROM File (*.gb, *.gbc):");
        ImGui::PushItemWidth(io.DisplaySize.x - 140.0f);
        ImGui::InputText("##rom_path", rom_buf, IM_ARRAYSIZE(rom_buf));
        ImGui::PopItemWidth();
        
        ImGui::SameLine();
        if (ImGui::Button("Choose##rom", ImVec2(110, 0))) {
            std::string selected = open_file_dialog("Select Base ROM", "--file-filter='ROMs (*.gb *.gbc) | *.gb *.gbc'");
            if (!selected.empty()) {
                strncpy(rom_buf, selected.c_str(), sizeof(rom_buf) - 1);
            }
        }

        std::string current_rom_path = rom_buf;
        bool rom_valid = false;
        if (!current_rom_path.empty() && std::filesystem::is_regular_file(current_rom_path)) {
            std::string calc_hash = calculate_file_sha256(current_rom_path);
            rom_valid = (calc_hash == EXPECTED_ROM_SHA256);
            if (rom_valid) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "[OK] Valid Base ROM (SHA256 Match)!");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[X] Invalid ROM! SHA256 hash mismatch.");
            }
        } else if (!current_rom_path.empty()) {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[X] File not found.");
        } else {
            ImGui::TextDisabled("Waiting for ROM selection...");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // -----------------------------------------------------------------
        // 2. SELECT & VALIDATE IPS PATCH
        // -----------------------------------------------------------------
        ImGui::Text("2. IPS Patch File (*.ips):");
        ImGui::PushItemWidth(io.DisplaySize.x - 140.0f);
        ImGui::InputText("##patch_path", patch_buf, IM_ARRAYSIZE(patch_buf));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button("Choose##patch", ImVec2(110, 0))) {
            std::string selected = open_file_dialog("Select IPS Patch", "--file-filter='Patches (*.ips) | *.ips'");
            if (!selected.empty()) {
                strncpy(patch_buf, selected.c_str(), sizeof(patch_buf) - 1);
            }
        }

        std::string current_patch_path = patch_buf;
        bool patch_valid = false;
        if (!current_patch_path.empty() && std::filesystem::is_regular_file(current_patch_path)) {
            std::string calc_hash = calculate_file_sha256(current_patch_path);
            patch_valid = (calc_hash == EXPECTED_PATCH_SHA256);
            if (patch_valid) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "[OK] Valid IPS Patch (SHA256 Match)!");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[X] Invalid Patch! SHA256 hash mismatch.");
            }
        } else if (!current_patch_path.empty()) {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[X] File not found.");
        } else {
            ImGui::TextDisabled("Waiting for Patch selection...");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // -----------------------------------------------------------------
        // PLAY BUTTON & ERROR POPUP
        // -----------------------------------------------------------------
        bool can_play = rom_valid && patch_valid;

        if (!can_play) ImGui::BeginDisabled();
        
        if (ImGui::Button("LOAD & PLAY", ImVec2(160, 45))) {
            try {
                find_patch_manifest(exec_path);
                out_rom_path = current_rom_path;
                out_patch_path = current_patch_path;
                ready_to_launch = true;
                done = true;
            } catch (const std::exception& e) {
                error_popup_msg = std::string("Failed to prepare ROM:\n") + e.what();
                show_error_popup = true;
            }
        }

        if (!can_play) ImGui::EndDisabled();

        // Error Modal Popup
        if (show_error_popup) {
            ImGui::OpenPopup("Initialization Error");
        }

        if (ImGui::BeginPopupModal("Initialization Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_popup_msg.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                show_error_popup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        // Render Frame
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 15, 15, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return ready_to_launch;
}

int main(int argc, char* argv[]) {
    // ---------------------------------------------------------------------
    // 1. Check for extracted asset file (wario.o2r)
    // ---------------------------------------------------------------------
    const std::filesystem::path o2r_file = "wario.o2r";
    std::string rom_input_path = argc > 1 ? argv[1] : "";
    std::string custom_ips_path = argc > 2 ? argv[2] : "";

    if (!std::filesystem::exists(o2r_file) && rom_input_path.empty()) {
        std::cout << "[Launcher] File " << o2r_file << " not found. Launching ImGui Interface...\n";
        
        if (!run_imgui_launcher(rom_input_path, custom_ips_path, argv[0])) {
            std::cout << "[Launcher] Cancelled by user.\n";
            return 0;
        }
    }

    if (rom_input_path.empty()) {
        rom_input_path = "rom.gb";
    }

    // ---------------------------------------------------------------------
    // 2. IPS Patch Application
    // ---------------------------------------------------------------------
    std::vector<uint8_t> patched_rom;
    try {
        std::filesystem::path manifest_path = find_patch_manifest(argv[0]);

        if (!custom_ips_path.empty()) {
            std::cout << "Using custom IPS patch: " << custom_ips_path << "\n";
            patched_rom = wl_sm3_patch::load_and_patch_rom_with_override(rom_input_path, manifest_path, custom_ips_path);
        } else {
            patched_rom = wl_sm3_patch::load_and_patch_rom(rom_input_path, manifest_path);
        }

        std::cout << "IPS patch applied successfully in memory: " << rom_input_path << " -> "
                  << patched_rom.size() << " bytes\n";

    } catch (const std::exception& error) {
        std::cerr << "Failed to prepare ROM: " << error.what() << '\n'
                  << "Usage: " << argv[0] << " <rom.gb> [patch_path.ips]\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 3. Asset Extraction (.o2r)
    // ---------------------------------------------------------------------
    AssetExtractor extractor;
    
    if (!extractor.check_and_prepare_assets(rom_input_path)) {
        return 1;
    }

    std::cout << "[VFS] Initializing runtime context allocation...\n";

    // ---------------------------------------------------------------------
    // 4. Runtime Hardware Configuration
    // ---------------------------------------------------------------------
    GBConfig runtime_config = *hack1_2_WL_SM3_default_config();
    runtime_config.model = runtime_config.cartridge_supports_cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    runtime_config.cgb_compatibility_mode = false;
    runtime_config.native_presentation_enabled = false;
    runtime_config.enable_audio = true; 

    GBContext* ctx = gb_context_create(&runtime_config);
    if (!ctx) {
        std::cerr << "❌ Critical Error: Failed to create engine context.\n";
        return 1;
    }
    
    gb_context_set_save_id(ctx, "wario_land_sm3_dx_v12");
    if (!gb_context_load_rom(ctx, patched_rom.data(), patched_rom.size())) {
        std::cerr << "Failed to load patched ROM into runtime.\n";
        gb_context_destroy(ctx);
        return 1;
    }
    gb_context_reset(ctx, true);

#ifdef GB_HAS_SDL2
    // ---------------------------------------------------------------------
    // 5. Initialize SDL2 Rendering Platform
    // ---------------------------------------------------------------------
    if (!gb_platform_init(5)) {
        std::cerr << "❌ Critical Error: Failed to initialize SDL2 platform.\n";
        gb_context_destroy(ctx);
        return 1;
    }
    gb_platform_register_context(ctx);
#endif

    std::cout << "🎮 Native Wario Land ready. Starting 60Hz main loop...\n";

    // ---------------------------------------------------------------------
    // 6. Main Game Loop
    // ---------------------------------------------------------------------
    const uint32_t lcd_smooth_slice_cycles = 70224u;
    bool running = true;
    
    while (running) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        
        while (!ctx->frame_done && !ctx->stopped) {
            gb_run_cycles(ctx, lcd_smooth_slice_cycles);
            
            if (!gb_platform_poll_events(ctx)) {
                running = false;
                break;
            }
        }


        if (!running) break;

        // Quando o frame termina (Gatilho de VBlank / Fim dos ciclos):
        if (ctx->frame_done) {
            if (ctx->ppu) {
                GBPPU* ppu = (GBPPU*)ctx->ppu;
            
                // 1. Atualizar o cache de tiles apenas para os tiles marcados como dirty
                // (Evita redecodificar a VRAM inteira se nada mudou)
                for (uint16_t i = 0; i < 384 * 2; i++) {
                    if (ppu->tile_cache[i].dirty) {
                        ppu_decode_tile(ppu, i % 384, i / 384);
                    }
                }

                // 2. Renderiza a cena inteira nativamente
                ppu_render_frame_native(ppu, ctx);
            }

            // 3. Obter e enviar o framebuffer correto (Widescreen ou Standard)
            GBPPU* ppu = (GBPPU*)ctx->ppu;
            const uint32_t* fb = ppu_get_widescreen_enabled(ppu) 
                           ? ppu_get_widescreen_framebuffer(ppu) 
                           : ppu_get_framebuffer(ppu);

            if (fb) {
                gb_platform_render_frame(fb);
            }
            gb_platform_vsync(ctx->frame_cycles);
    }
        /*

        if (!running) break;

        if (ctx->frame_done) {
            const uint32_t* fb = gb_get_framebuffer(ctx);
            if (fb) {
                gb_platform_render_frame(fb);
            }
            gb_platform_vsync(ctx->frame_cycles);
        }*/
    }

    // ---------------------------------------------------------------------
    // 7. Cleanup & Shutdown
    // ---------------------------------------------------------------------
    std::cout << "[GBRT] Saving progress and shutting down...\n";
#ifdef GB_HAS_SDL2
    gb_platform_shutdown();
#endif
    gb_context_destroy(ctx);
    return 0;
}