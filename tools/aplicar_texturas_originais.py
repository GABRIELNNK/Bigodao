#!/usr/bin/env python3
"""
Passo 31a: texturas P&B originais embutidas no wario.o2r.

- O launcher, antes de recriar o wario.o2r, gera as texturas originais (os
  mesmos PNGs do gerar_mod_original_bw.py) a partir da ROM original, seguindo a
  receita original_bw_receita.h, e grava em original_bw/ dentro do wario.o2r.
- Um wario.o2r antigo (sem as texturas desta versão da receita) abre o
  launcher com o aviso "Update needed"; cancelar segue com o pacote antigo.
- Com Cores = Original (preto e branco ou verde do DMG), as texturas entram
  como a primeira fonte de mods ("Original textures (built-in)"), abaixo dos
  mods da pasta mods: um mod colorido continua colorido por cima. A rampa
  P&B (cinza ou verde) vale só para as texturas embutidas.
- Resolução interna padrão: 2x.

Arquivos:
    original_bw.hpp                      (novo) gera os PNGs no launcher
    tools/gerar_receita_original_bw.py   (novo) gera original_bw_receita.h
    asset_extractor.hpp                  arquivos extras no wario.o2r + info_value()
    main.cpp                             checagem da versão, aviso no launcher, geração
    runtime/include/hd_pack_io.h, runtime/src/hd_pack_io.cpp   fonte embutida
    runtime/include/ppu_native.h, runtime/src/ppu_native.c     tile "embutido" e rampa só nele
    runtime/src/gpu_ppu.cpp              o mesmo no shader (bit 0x4000 do meta)
    runtime/src/ppu.c                    rampa nos 4 tons exatos
    runtime/src/platform_sdl.cpp         recarrega ao trocar Cores, avisos, 2x padrão
    tools/progresso_renderizador.md      anotação do passo 31a

Uso (na raiz do projeto):
    python3 tools/aplicar_texturas_originais.py --conferir   # mostra onde muda, sem gravar
    python3 tools/aplicar_texturas_originais.py              # aplica

Depois de aplicar, gere a receita (precisa do disassembly em tools/disassembly):
    python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r

Cria um .orig de cada arquivo antes de alterar. Se já foi aplicado, não faz
nada. Se algum trecho de referência não for encontrado, para sem alterar nada.
"""
import difflib
import sys
from pathlib import Path

EDITS = {
    'asset_extractor.hpp': [
        (
            (
                '\n'
                '    const std::filesystem::path& path() const { return path_; }\n'
                '\n'
                '    bool exists() const {\n'
                '        std::error_code ec;\n'
                '        return std::filesystem::is_regular_file(path_, ec);\n'
            ),
            (
                '\n'
                '    const std::filesystem::path& path() const { return path_; }\n'
                '\n'
                '    // Passo 31a: arquivos extras no pacote (ex.: original_bw/*.png). O leitor da\n'
                '    // ROM ignora o que não é fatia, então isso não muda a remontagem.\n'
                '    struct ExtraFile {\n'
                '        std::string name;\n'
                '        std::vector<uint8_t> data;\n'
                '    };\n'
                '\n'
                '    // Valor de uma linha "chave=valor" do bigodao/o2r_info.txt ("" se não houver).\n'
                '    std::string info_value(const std::string& key) const {\n'
                '        std::ifstream in(path_, std::ios::binary);\n'
                '        if (!in) return "";\n'
                '        std::vector<uint8_t> z((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());\n'
                '        std::map<std::string, std::vector<uint8_t>> files;\n'
                '        std::string err;\n'
                '        if (!read_zip(z, files, err)) return "";\n'
                '        auto it = files.find(INFO_NAME);\n'
                '        if (it == files.end()) return "";\n'
                '        std::istringstream ss(std::string(it->second.begin(), it->second.end()));\n'
                '        std::string line;\n'
                '        while (std::getline(ss, line)) {\n'
                "            if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=') {\n"
                '                return line.substr(key.size() + 1);\n'
                '            }\n'
                '        }\n'
                '        return "";\n'
                '    }\n'
                '\n'
                '    bool exists() const {\n'
                '        std::error_code ec;\n'
                '        return std::filesystem::is_regular_file(path_, ec);\n'
            ),
        ),
        (
            (
                '\n'
                '    // Gera o .o2r a partir da ROM patcheada. Escreve num arquivo temporário, confere\n'
                '    // que dá para remontar a ROM byte a byte e só então substitui o pacote antigo.\n'
                '    bool build(const std::vector<uint8_t>& rom, std::string& err) const {\n'
                '        namespace fs = std::filesystem;\n'
                '        if (rom.size() <= MIN_ROM_SIZE) {\n'
                '            err = "ROM too small (" + std::to_string(rom.size()) + " bytes)";\n'
            ),
            (
                '\n'
                '    // Gera o .o2r a partir da ROM patcheada. Escreve num arquivo temporário, confere\n'
                '    // que dá para remontar a ROM byte a byte e só então substitui o pacote antigo.\n'
                '    // extra: arquivos a mais no pacote; extra_info: linhas a mais no o2r_info.txt.\n'
                '    bool build(const std::vector<uint8_t>& rom, std::string& err,\n'
                '               const std::vector<ExtraFile>& extra = {}, const std::string& extra_info = "") const {\n'
                '        namespace fs = std::filesystem;\n'
                '        if (rom.size() <= MIN_ROM_SIZE) {\n'
                '            err = "ROM too small (" + std::to_string(rom.size()) + " bytes)";\n'
            ),
        ),
        (
            (
                '                add_file(zf, s.name, &rom[s.begin], end - s.begin, cd, names);\n'
                '            }\n'
                '\n'
                '            const std::string info = "format=" + std::to_string(FORMAT_VERSION) + "\\n"\n'
                '                                     "rom_size=" + std::to_string(rom.size()) + "\\n"\n'
                '                                     "content=Wario Land: Super Mario Land 3 + DX v1.2 (patched)\\n";\n'
                '            add_file(zf, INFO_NAME, reinterpret_cast<const uint8_t*>(info.data()), info.size(), cd, names);\n'
                '\n'
                '            const uint32_t cd_offset = static_cast<uint32_t>(zf.tellp());\n'
            ),
            (
                '                add_file(zf, s.name, &rom[s.begin], end - s.begin, cd, names);\n'
                '            }\n'
                '\n'
                '            // Passo 31a: arquivos extras (texturas originais etc.)\n'
                '            for (const ExtraFile& f : extra) {\n'
                '                add_file(zf, f.name, f.data.data(), f.data.size(), cd, names);\n'
                '            }\n'
                '\n'
                '            const std::string info = "format=" + std::to_string(FORMAT_VERSION) + "\\n"\n'
                '                                     "rom_size=" + std::to_string(rom.size()) + "\\n"\n'
                '                                     "content=Wario Land: Super Mario Land 3 + DX v1.2 (patched)\\n" + extra_info;\n'
                '            add_file(zf, INFO_NAME, reinterpret_cast<const uint8_t*>(info.data()), info.size(), cd, names);\n'
                '\n'
                '            const uint32_t cd_offset = static_cast<uint32_t>(zf.tellp());\n'
            ),
        ),
        (
            (
                '                zf.write(names[i].data(), static_cast<std::streamsize>(names[i].size()));\n'
                '            }\n'
                '            EndOfCentralDir eocd;\n'
                '            eocd.diskEntries = eocd.totalEntries = static_cast<uint16_t>(cd.size());\n'
                '            eocd.centralDirSize = static_cast<uint32_t>(zf.tellp()) - cd_offset;\n'
                '            eocd.centralDirOffset = cd_offset;\n'
            ),
            (
                '                zf.write(names[i].data(), static_cast<std::streamsize>(names[i].size()));\n'
                '            }\n'
                '            EndOfCentralDir eocd;\n'
                '            if (cd.size() > 0xFFFF) { err = "too many files for the package"; return false; }\n'
                '            eocd.diskEntries = eocd.totalEntries = static_cast<uint16_t>(cd.size());\n'
                '            eocd.centralDirSize = static_cast<uint32_t>(zf.tellp()) - cd_offset;\n'
                '            eocd.centralDirOffset = cd_offset;\n'
            ),
        ),
    ],
    'main.cpp': [
        (
            (
                '#include "platform_sdl.h"\n'
                '#include "ppu.h"\n'
                '#include "wl_sm3_patch.hpp"\n'
                '#include "gbrt.h"\n'
                '#include "android_bridge.h"   /* [android-port] */\n'
                '#include "app_icon.h"         /* [fix-v3] */\n'
            ),
            (
                '#include "platform_sdl.h"\n'
                '#include "ppu.h"\n'
                '#include "wl_sm3_patch.hpp"\n'
                '#include "original_bw.hpp"   /* passo 31a */\n'
                '#include "hd_pack_io.h"     /* passo 31a */\n'
                '#include "gbrt.h"\n'
                '#include "android_bridge.h"   /* [android-port] */\n'
                '#include "app_icon.h"         /* [fix-v3] */\n'
            ),
        ),
        (
            (
                '} // namespace launcher\n'
                '\n'
                '// Execute Launcher GUI via ImGui + SDL2\n'
                'static bool run_imgui_launcher(std::string& out_rom_path, std::string& out_patch_path, const char* exec_path) {\n'
                '    using namespace launcher;\n'
                '\n'
                '    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {\n'
            ),
            (
                '} // namespace launcher\n'
                '\n'
                '// Execute Launcher GUI via ImGui + SDL2\n'
                '/* Passo 31a: notice != "" abre o launcher já com um aviso (ex.: wario.o2r desatualizado). */\n'
                'static bool run_imgui_launcher(std::string& out_rom_path, std::string& out_patch_path, const char* exec_path,\n'
                '                               const std::string& notice = "") {\n'
                '    using namespace launcher;\n'
                '\n'
                '    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {\n'
            ),
        ),
        (
            (
                '        if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0.0f) android_dp = ddpi / 160.0f;\n'
                '    }\n'
                '#endif\n'
                '    std::string modal_msg;\n'
                '    bool ready_to_launch = false;\n'
                '    bool done = false;\n'
                '    const Uint64 t_start = SDL_GetPerformanceCounter();\n'
            ),
            (
                '        if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0.0f) android_dp = ddpi / 160.0f;\n'
                '    }\n'
                '#endif\n'
                '    std::string modal_msg = notice;\n'
                '    std::string modal_title = notice.empty() ? "Could not start the game" : "Update needed";   /* passo 31a */\n'
                '    bool modal_is_info = !notice.empty();\n'
                '    bool ready_to_launch = false;\n'
                '    bool done = false;\n'
                '    const Uint64 t_start = SDL_GetPerformanceCounter();\n'
            ),
        ),
        (
            (
                '            done = true;\n'
                '        } catch (const std::exception& e) {\n'
                '            modal_msg = std::string("Failed to prepare the ROM:\\n") + e.what();\n'
                '        }\n'
                '    };\n'
                '\n'
            ),
            (
                '            done = true;\n'
                '        } catch (const std::exception& e) {\n'
                '            modal_msg = std::string("Failed to prepare the ROM:\\n") + e.what();\n'
                '            modal_title = "Could not start the game";\n'
                '            modal_is_info = false;\n'
                '        }\n'
                '    };\n'
                '\n'
            ),
        ),
        (
            (
                '            const ImVec2 a((W - mw) * 0.5f, (H - mh) * 0.5f), b(a.x + mw, a.y + mh);\n'
                '            soft_shadow(dl, a, b, U(16.0f), IM_COL32(0, 0, 0, 255), 0.8f, U(4.0f), 6, U(8.0f));\n'
                '            dl->AddRectFilled(a, b, IM_COL32(24, 26, 38, 255), U(16.0f));\n'
                '            dl->AddRect(a, b, IM_COL32(239, 68, 68, 90), U(16.0f), 0, U(1.5f));\n'
                '            icon_cross(dl, ImVec2(a.x + pad, a.y + pad + U(5.0f)), U(12.0f), COL_RED, U(2.5f));\n'
                '            dl->AddText(fonts.title, U(FS_TITLE), ImVec2(a.x + pad + U(24.0f), a.y + pad), COL_TEXT, "Could not start the game");\n'
                '            dl->AddText(fonts.body, U(FS_BODY), ImVec2(a.x + pad, a.y + pad + U(36.0f)), COL_SUB,\n'
                '                        modal_msg.c_str(), nullptr, mw - pad * 2.0f);\n'
                '            const float ok_w = blue_button_width(fonts, "OK");\n'
            ),
            (
                '            const ImVec2 a((W - mw) * 0.5f, (H - mh) * 0.5f), b(a.x + mw, a.y + mh);\n'
                '            soft_shadow(dl, a, b, U(16.0f), IM_COL32(0, 0, 0, 255), 0.8f, U(4.0f), 6, U(8.0f));\n'
                '            dl->AddRectFilled(a, b, IM_COL32(24, 26, 38, 255), U(16.0f));\n'
                '            dl->AddRect(a, b, modal_is_info ? IM_COL32(96, 165, 250, 110) : IM_COL32(239, 68, 68, 90), U(16.0f), 0, U(1.5f));\n'
                '            if (modal_is_info) {\n'
                '                dl->AddCircleFilled(ImVec2(a.x + pad + U(6.0f), a.y + pad + U(11.0f)), U(6.0f), IM_COL32(96, 165, 250, 255), 16);\n'
                '            } else {\n'
                '                icon_cross(dl, ImVec2(a.x + pad, a.y + pad + U(5.0f)), U(12.0f), COL_RED, U(2.5f));\n'
                '            }\n'
                '            dl->AddText(fonts.title, U(FS_TITLE), ImVec2(a.x + pad + U(24.0f), a.y + pad), COL_TEXT, modal_title.c_str());\n'
                '            dl->AddText(fonts.body, U(FS_BODY), ImVec2(a.x + pad, a.y + pad + U(36.0f)), COL_SUB,\n'
                '                        modal_msg.c_str(), nullptr, mw - pad * 2.0f);\n'
                '            const float ok_w = blue_button_width(fonts, "OK");\n'
            ),
        ),
        (
            (
                '    std::string custom_ips_path = argc > 2 ? argv[2] : "";\n'
                '\n'
                '    std::vector<uint8_t> patched_rom;\n'
                '\n'
                '    if (rom_input_path.empty() && assets.exists()) {\n'
                '        std::string err;\n'
                '        if (assets.load(patched_rom, err)) {\n'
                '            std::cout << "[VFS] Loaded patched ROM from " << assets.path().filename().string()\n'
                '                      << " (" << patched_rom.size() << " bytes).\\n";\n'
                '        } else {\n'
                '            std::cerr << "[VFS] " << assets.path().filename().string() << " can\'t be used (" << err\n'
                '                      << "). It will be rebuilt from the ROM.\\n";\n'
            ),
            (
                '    std::string custom_ips_path = argc > 2 ? argv[2] : "";\n'
                '\n'
                '    std::vector<uint8_t> patched_rom;\n'
                '    std::vector<uint8_t> previous_rom;   /* passo 31a: o do wario.o2r antigo, se o launcher for cancelado */\n'
                '    std::string launcher_notice;\n'
                '    const std::string bw_version = std::to_string(original_bw::recipe_version());\n'
                '\n'
                '    if (rom_input_path.empty() && assets.exists()) {\n'
                '        std::string err;\n'
                '        if (assets.load(patched_rom, err)) {\n'
                '            std::cout << "[VFS] Loaded patched ROM from " << assets.path().filename().string()\n'
                '                      << " (" << patched_rom.size() << " bytes).\\n";\n'
                '            /* Passo 31a: wario.o2r sem as texturas originais desta versão: refazer pelo launcher */\n'
                '            if (original_bw::available() && assets.info_value("original_bw_recipe") != bw_version) {\n'
                '                std::cout << "[VFS] " << assets.path().filename().string()\n'
                '                          << " needs an update (original textures). Opening the launcher...\\n";\n'
                '                launcher_notice =\n'
                '                    "Your wario.o2r was created by an older version of Bigodao and needs to be updated "\n'
                '                    "(it is missing the original black and white textures).\\n\\n"\n'
                '                    "Select the original ROM and the DX patch again, then press LOAD & PLAY "\n'
                '                    "to rebuild it.";\n'
                '                previous_rom.swap(patched_rom);\n'
                '            }\n'
                '        } else {\n'
                '            std::cerr << "[VFS] " << assets.path().filename().string() << " can\'t be used (" << err\n'
                '                      << "). It will be rebuilt from the ROM.\\n";\n'
            ),
        ),
        (
            (
                '    if (patched_rom.empty()) {\n'
                '        if (rom_input_path.empty()) {\n'
                '            std::cout << "[Launcher] No usable " << assets.path().filename().string() << ". Opening the launcher...\\n";\n'
                '            if (!run_imgui_launcher(rom_input_path, custom_ips_path, argv[0])) {\n'
                '                std::cout << "[Launcher] Cancelled by user.\\n";\n'
                '                return 0;\n'
                '            }\n'
                '        }\n'
                '\n'
                '        try {\n'
                '            const fs::path manifest_path = find_patch_manifest(argv[0]);\n'
                '            if (!custom_ips_path.empty()) {\n'
                '                std::cout << "Using IPS patch: " << custom_ips_path << "\\n";\n'
                '                patched_rom = wl_sm3_patch::load_and_patch_rom_with_override(rom_input_path, manifest_path, custom_ips_path);\n'
            ),
            (
                '    if (patched_rom.empty()) {\n'
                '        if (rom_input_path.empty()) {\n'
                '            std::cout << "[Launcher] No usable " << assets.path().filename().string() << ". Opening the launcher...\\n";\n'
                '            if (!run_imgui_launcher(rom_input_path, custom_ips_path, argv[0], launcher_notice)) {\n'
                '                if (previous_rom.empty()) {\n'
                '                    std::cout << "[Launcher] Cancelled by user.\\n";\n'
                '                    return 0;\n'
                '                }\n'
                '                /* Passo 31a: sem atualizar, segue com o wario.o2r antigo (sem as texturas originais) */\n'
                '                std::cout << "[Launcher] Update cancelled; using the old " << assets.path().filename().string() << ".\\n";\n'
                '                patched_rom.swap(previous_rom);\n'
                '            }\n'
                '        }\n'
                '    }\n'
                '\n'
                '    if (patched_rom.empty()) {   /* passo 31a: pulado se o launcher de atualização foi cancelado */\n'
                '        std::vector<uint8_t> original_rom;\n'
                '        try {\n'
                '            const fs::path manifest_path = find_patch_manifest(argv[0]);\n'
                '            original_rom = wl_sm3_patch::read_binary_file(rom_input_path);   /* passo 31a */\n'
                '            if (!custom_ips_path.empty()) {\n'
                '                std::cout << "Using IPS patch: " << custom_ips_path << "\\n";\n'
                '                patched_rom = wl_sm3_patch::load_and_patch_rom_with_override(rom_input_path, manifest_path, custom_ips_path);\n'
            ),
        ),
        (
            (
                '\n'
                '        // ---------------------------------------------------------------------\n'
                '        // 3. Asset extraction (.o2r) — from now on the game starts from it alone\n'
                '        // ---------------------------------------------------------------------\n'
                '        std::string err;\n'
                '        if (!assets.build(patched_rom, err)) {\n'
                '            std::cerr << "[VFS] Warning: could not create " << assets.path().string() << " (" << err\n'
                '                      << "). The game will start, but the ROM will be needed again next time.\\n";\n'
                '        }\n'
                '    }\n'
                '\n'
                '    std::cout << "[VFS] Initializing runtime context allocation...\\n";\n'
                '\n'
            ),
            (
                '\n'
                '        // ---------------------------------------------------------------------\n'
                '        // 3. Asset extraction (.o2r) — from now on the game starts from it alone\n'
                '        //    Passo 31a: com as texturas P&B originais em original_bw/ (da ROM original)\n'
                '        // ---------------------------------------------------------------------\n'
                '        std::vector<AssetExtractor::ExtraFile> extra;\n'
                '        std::string extra_info;\n'
                '        if (original_bw::available()) {\n'
                '            std::vector<original_bw::File> bw;\n'
                '            std::string bw_err;\n'
                '            const bool bw_ok = original_bw::build(original_rom, patched_rom, bw, bw_err);\n'
                '            if (bw_ok) {\n'
                '                for (original_bw::File& f : bw) extra.push_back({std::move(f.name), std::move(f.data)});\n'
                '                std::cout << "[VFS] Original textures: " << extra.size() << " tiles.\\n";\n'
                '            } else {\n'
                '                std::cerr << "[VFS] Original textures not created (" << bw_err << ").\\n";\n'
                '            }\n'
                '            /* a versão vai mesmo quando falha, para não abrir o launcher toda vez */\n'
                '            extra_info = "original_bw_recipe=" + bw_version + "\\noriginal_bw=" + (bw_ok ? "1" : "0") + "\\n";\n'
                '        }\n'
                '        std::string err;\n'
                '        if (!assets.build(patched_rom, err, extra, extra_info)) {\n'
                '            std::cerr << "[VFS] Warning: could not create " << assets.path().string() << " (" << err\n'
                '                      << "). The game will start, but the ROM will be needed again next time.\\n";\n'
                '        }\n'
                '    }\n'
                '    hd_pack_io_set_builtin_source(assets.path().string().c_str(), original_bw::FOLDER);   /* passo 31a */\n'
                '\n'
                '    std::cout << "[VFS] Initializing runtime context allocation...\\n";\n'
                '\n'
            ),
        ),
    ],
    'runtime/include/hd_pack_io.h': [
        (
            (
                ' * Em cada fonte: b_<tile>.png / o_<tile>.png (qualquer subpasta) e pack.json.\n'
                ' * Devolve quantos tiles HD ficaram carregados. */\n'
                'int hd_pack_io_load_pack(void);\n'
                'const char* hd_pack_io_pack_dir(void);   /* <base>/mods/hd */\n'
                'const char* hd_pack_io_mods_dir(void);   /* <base>/mods */\n'
                'const char* hd_pack_io_pack_status(void);\n'
            ),
            (
                ' * Em cada fonte: b_<tile>.png / o_<tile>.png (qualquer subpasta) e pack.json.\n'
                ' * Devolve quantos tiles HD ficaram carregados. */\n'
                'int hd_pack_io_load_pack(void);\n'
                '\n'
                '/* Passo 31a: texturas P&B originais embutidas no wario.o2r (pasta prefix, ex.\n'
                ' * "original_bw/"). Entram como a primeira fonte da lista (abaixo dos mods da\n'
                ' * pasta mods) só quando a opção Cores está em Original. Chamar antes do\n'
                ' * primeiro hd_pack_io_load_pack(). */\n'
                'void hd_pack_io_set_builtin_source(const char* o2r_path, const char* prefix);\n'
                '/* O wario.o2r tem as texturas originais? */\n'
                'bool hd_pack_io_builtin_present(void);\n'
                'const char* hd_pack_io_pack_dir(void);   /* <base>/mods/hd */\n'
                'const char* hd_pack_io_mods_dir(void);   /* <base>/mods */\n'
                'const char* hd_pack_io_pack_status(void);\n'
            ),
        ),
    ],
    'runtime/src/hd_pack_io.cpp': [
        (
            (
                '    std::string name;      /* "meu_mod.o2r" ou "hd/" */\n'
                '    fs::path path;\n'
                '    bool is_folder = false;\n'
                '    bool enabled = true;\n'
                '    int tiles = 0;\n'
                '    std::string info;\n'
            ),
            (
                '    std::string name;      /* "meu_mod.o2r" ou "hd/" */\n'
                '    fs::path path;\n'
                '    bool is_folder = false;\n'
                '    bool builtin = false;  /* passo 31a: texturas originais dentro do wario.o2r */\n'
                '    std::string prefix;    /* passo 31a: só as entradas do ZIP nesta pasta */\n'
                '    bool enabled = true;\n'
                '    int tiles = 0;\n'
                '    std::string info;\n'
            ),
        ),
        (
            (
                '    }\n'
                '    for (const ZipEntry& e : entries) {\n'
                "        if (e.name.empty() || e.name.back() == '/') continue;    /* pasta */\n"
                '        const std::string leaf = lower_ascii(leaf_of(e.name));\n'
                '        const bool is_pack = leaf == "pack.json";\n'
                '        const bool is_png = leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".png") == 0;\n'
            ),
            (
                '    }\n'
                '    for (const ZipEntry& e : entries) {\n'
                "        if (e.name.empty() || e.name.back() == '/') continue;    /* pasta */\n"
                '        if (!src.prefix.empty() && e.name.compare(0, src.prefix.size(), src.prefix) != 0) continue;   /* passo 31a */\n'
                '        const std::string leaf = lower_ascii(leaf_of(e.name));\n'
                '        const bool is_pack = leaf == "pack.json";\n'
                '        const bool is_png = leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".png") == 0;\n'
            ),
        ),
        (
            (
                '        return 0;\n'
                '    }\n'
                '    int loaded = 0, no_ref = 0;\n'
                '    for (const PendingTile& t : tiles) {\n'
                '        char kind;\n'
                '        uint64_t hash;\n'
            ),
            (
                '        return 0;\n'
                '    }\n'
                '    int loaded = 0, no_ref = 0;\n'
                '    ppu_native_hd_set_builtin_loading(src.builtin);   /* passo 31a: recebem a rampa P&B */\n'
                '    for (const PendingTile& t : tiles) {\n'
                '        char kind;\n'
                '        uint64_t hash;\n'
            ),
        ),
        (
            (
                '        stbi_image_free(data);\n'
                '        const auto it = palettes.find(t.stem);\n'
                '        const uint32_t* ref = (it != palettes.end()) ? it->second.data() : nullptr;\n'
                '        if (!ref) no_ref++;\n'
                '        if (ppu_native_hd_add_tile_ex(kind, hash, context, pal, w, argb.data(), ref)) {\n'
                '            loaded++;\n'
                '        } else {\n'
                '            skipped++;\n'
                '        }\n'
                '    }\n'
                '    src.tiles = loaded;\n'
                '    char buf[160];\n'
                '    snprintf(buf, sizeof(buf), "%d tiles%s%s", loaded,\n'
            ),
            (
                '        stbi_image_free(data);\n'
                '        const auto it = palettes.find(t.stem);\n'
                '        const uint32_t* ref = (it != palettes.end()) ? it->second.data() : nullptr;\n'
                '        if (!ref && !src.builtin) no_ref++;\n'
                '        if (ppu_native_hd_add_tile_ex(kind, hash, context, pal, w, argb.data(), ref)) {\n'
                '            loaded++;\n'
                '        } else {\n'
                '            skipped++;\n'
                '        }\n'
                '    }\n'
                '    ppu_native_hd_set_builtin_loading(false);\n'
                '    src.tiles = loaded;\n'
                '    char buf[160];\n'
                '    snprintf(buf, sizeof(buf), "%d tiles%s%s", loaded,\n'
            ),
        ),
        (
            (
                '    return false;\n'
                '}\n'
                '\n'
                '/* Lista as fontes de mods (não carrega nada). */\n'
                'static void scan_mods(void) {\n'
                '    g_mods.clear();\n'
                '    const fs::path mods_dir = fs::path(hd_pack_io_pack_dir()).parent_path();\n'
                '    std::error_code ec;\n'
                '    if (fs::is_directory(mods_dir, ec)) {\n'
            ),
            (
                '    return false;\n'
                '}\n'
                '\n'
                '/* Passo 31a: fonte embutida (texturas P&B originais no wario.o2r) */\n'
                'static bool zip_list_file(const fs::path& path, std::vector<ZipEntry>& out, std::string& err);\n'
                'static fs::path g_builtin_path;\n'
                'static std::string g_builtin_prefix;\n'
                'static bool g_builtin_present = false;\n'
                '\n'
                'void hd_pack_io_set_builtin_source(const char* o2r_path, const char* prefix) {\n'
                '    g_builtin_path = o2r_path ? fs::path(o2r_path) : fs::path();\n'
                '    g_builtin_prefix = prefix ? prefix : "";\n'
                '    g_builtin_present = false;\n'
                '    std::vector<ZipEntry> entries;\n'
                '    std::string err;\n'
                '    if (!g_builtin_path.empty() && !g_builtin_prefix.empty() && zip_list_file(g_builtin_path, entries, err)) {\n'
                '        for (const ZipEntry& e : entries) {\n'
                '            if (e.name.compare(0, g_builtin_prefix.size(), g_builtin_prefix) == 0) {\n'
                '                g_builtin_present = true;\n'
                '                break;\n'
                '            }\n'
                '        }\n'
                '    }\n'
                '    fprintf(stderr, "[HD] texturas originais embutidas: %s\\n", g_builtin_present ? "sim" : "nao");\n'
                '}\n'
                '\n'
                'bool hd_pack_io_builtin_present(void) {\n'
                '    return g_builtin_present;\n'
                '}\n'
                '\n'
                '/* Lista as fontes de mods (não carrega nada). */\n'
                'static void scan_mods(void) {\n'
                '    g_mods.clear();\n'
                '    /* Passo 31a: primeiro da lista = abaixo dos mods da pasta (eles sobrepõem) */\n'
                '    if (g_builtin_present && ppu_get_color_mode() != PPU_COLOR_DX) {\n'
                '        HdModSource s;\n'
                '        s.name = "Original textures (built-in)";\n'
                '        s.path = g_builtin_path;\n'
                '        s.builtin = true;\n'
                '        s.prefix = g_builtin_prefix;\n'
                '        g_mods.push_back(s);\n'
                '    }\n'
                '    const fs::path mods_dir = fs::path(hd_pack_io_pack_dir()).parent_path();\n'
                '    std::error_code ec;\n'
                '    if (fs::is_directory(mods_dir, ec)) {\n'
            ),
        ),
        (
            (
                '    if (!visit) return;\n'
                '    for (size_t i = 0; i < g_mods.size(); ++i) {\n'
                '        const HdModSource& s = g_mods[i];\n'
                '        if (!s.enabled) continue;\n'
                '        std::error_code ec;\n'
                '        if (s.is_folder) {\n'
                '            for (auto it = fs::recursive_directory_iterator(s.path, ec);\n'
            ),
            (
                '    if (!visit) return;\n'
                '    for (size_t i = 0; i < g_mods.size(); ++i) {\n'
                '        const HdModSource& s = g_mods[i];\n'
                '        if (!s.enabled || s.builtin) continue;   /* passo 31a: o embutido só tem tiles */\n'
                '        std::error_code ec;\n'
                '        if (s.is_folder) {\n'
                '            for (auto it = fs::recursive_directory_iterator(s.path, ec);\n'
            ),
        ),
    ],
    'runtime/include/ppu_native.h': [
        (
            (
                'uint32_t ppu_native_hd_table_capacity(void);\n'
                '/* Entrada i da tabela: false se vazia. ref = NULL sem paleta de referência. */\n'
                'bool ppu_native_hd_table_entry(uint32_t i, int* size, const uint32_t** px, const uint32_t** ref);\n'
                '\n'
                'void ppu_native_gpu_capture(bool enabled);\n'
                'bool ppu_native_gpu_capture_enabled(void);\n'
            ),
            (
                'uint32_t ppu_native_hd_table_capacity(void);\n'
                '/* Entrada i da tabela: false se vazia. ref = NULL sem paleta de referência. */\n'
                'bool ppu_native_hd_table_entry(uint32_t i, int* size, const uint32_t** px, const uint32_t** ref);\n'
                '/* Passo 31a: o tile da posição i veio das texturas originais embutidas? */\n'
                'bool ppu_native_hd_table_entry_builtin(uint32_t i);\n'
                '/* Passo 31a: os tiles adicionados enquanto ligado são os embutidos (no modo\n'
                ' * Original passam pela rampa P&B; os dos mods da pasta ficam com as cores deles). */\n'
                'void ppu_native_hd_set_builtin_loading(bool on);\n'
                '\n'
                'void ppu_native_gpu_capture(bool enabled);\n'
                'bool ppu_native_gpu_capture_enabled(void);\n'
            ),
        ),
    ],
    'runtime/src/ppu_native.c': [
        (
            (
                '    uint16_t size;\n'
                '    bool has_ref;\n'
                '    uint32_t ref[4];     /* paleta em que o tile foi pintado (pack.json) */\n'
                '} HdTile;\n'
                '#define HD_TABLE_CAP 32768u   /* potência de 2 (passo 11a: era 16384) */\n'
                'static HdTile g_hd_tiles[HD_TABLE_CAP];\n'
            ),
            (
                '    uint16_t size;\n'
                '    bool has_ref;\n'
                '    uint32_t ref[4];     /* paleta em que o tile foi pintado (pack.json) */\n'
                '    bool builtin;        /* passo 31a: texturas originais embutidas */\n'
                '} HdTile;\n'
                '#define HD_TABLE_CAP 32768u   /* potência de 2 (passo 11a: era 16384) */\n'
                'static HdTile g_hd_tiles[HD_TABLE_CAP];\n'
            ),
        ),
        (
            (
                '    bool active;\n'
                '    uint32_t mul[3];   /* 16.16, arredondado para cima: ref * mul >> 16 == atual */\n'
                '    uint32_t add[3];   /* usado quando o canal de referência é 0 */\n'
                '} HdRecolor;\n'
                '\n'
                'static inline void hd_recolor_setup(HdRecolor* r, const HdTile* t, uint8_t idx, uint32_t cur) {\n'
                '    r->active = false;\n'
                '    if (!t->has_ref) {\n'
                '        return;\n'
                '    }\n'
                '    const uint32_t ref = t->ref[idx & 3u];\n'
            ),
            (
                '    bool active;\n'
                '    uint32_t mul[3];   /* 16.16, arredondado para cima: ref * mul >> 16 == atual */\n'
                '    uint32_t add[3];   /* usado quando o canal de referência é 0 */\n'
                '    bool mono;         /* passo 31a: tile embutido no modo Original: vai para a rampa P&B */\n'
                '} HdRecolor;\n'
                '\n'
                'static inline void hd_recolor_setup(HdRecolor* r, const HdTile* t, uint8_t idx, uint32_t cur) {\n'
                '    r->active = false;\n'
                '    r->mono = g_mono && t->builtin;\n'
                '    /* Passo 31a: no modo Original a paleta da linha é P&B; os mods coloridos\n'
                '     * ficam com as cores do PNG (sem recolorir pela paleta). */\n'
                '    if (g_mono || !t->has_ref) {\n'
                '        return;\n'
                '    }\n'
                '    const uint32_t ref = t->ref[idx & 3u];\n'
            ),
        ),
        (
            (
                '}\n'
                '\n'
                'static inline uint32_t hd_recolor_apply(const HdRecolor* r, uint32_t c) {\n'
                '    if (g_mono) {\n'
                '        /* PNG HD é colorido: no modo original vira a rampa P&B pela luminância */\n'
                '        c = hd_recolor_apply_rgb(r, c);\n'
                '        return ppu_mono_tint_rgba(c);\n'
                '    }\n'
            ),
            (
                '}\n'
                '\n'
                'static inline uint32_t hd_recolor_apply(const HdRecolor* r, uint32_t c) {\n'
                '    if (r->mono) {\n'
                '        /* Passo 31a: só os tiles embutidos vão para a rampa P&B (cinza ou verde) */\n'
                '        c = hd_recolor_apply_rgb(r, c);\n'
                '        return ppu_mono_tint_rgba(c);\n'
                '    }\n'
            ),
        ),
        (
            (
                '    return true;\n'
                '}\n'
                '\n'
                '/* Passo 14b */\n'
                'void ppu_native_gpu_capture(bool enabled) {\n'
                '    g_gpu_on = enabled;\n'
            ),
            (
                '    return true;\n'
                '}\n'
                '\n'
                'bool ppu_native_hd_table_entry_builtin(uint32_t i) {   /* passo 31a */\n'
                '    return i < HD_TABLE_CAP && g_hd_tiles[i].key != 0 && g_hd_tiles[i].builtin;\n'
                '}\n'
                '\n'
                'static bool g_hd_loading_builtin = false;   /* passo 31a */\n'
                '\n'
                'void ppu_native_hd_set_builtin_loading(bool on) {\n'
                '    g_hd_loading_builtin = on;\n'
                '}\n'
                '\n'
                '/* Passo 14b */\n'
                'void ppu_native_gpu_capture(bool enabled) {\n'
                '    g_gpu_on = enabled;\n'
            ),
        ),
        (
            (
                '    t->px = px;\n'
                '    t->size = (uint16_t)size;\n'
                '    t->has_ref = ref_palette != NULL;\n'
                '    for (unsigned c = 0; c < 4u; ++c) {\n'
                '        t->ref[c] = ref_palette ? ref_palette[c] : 0u;\n'
                '    }\n'
            ),
            (
                '    t->px = px;\n'
                '    t->size = (uint16_t)size;\n'
                '    t->has_ref = ref_palette != NULL;\n'
                '    t->builtin = g_hd_loading_builtin;   /* passo 31a */\n'
                '    for (unsigned c = 0; c < 4u; ++c) {\n'
                '        t->ref[c] = ref_palette ? ref_palette[c] : 0u;\n'
                '    }\n'
            ),
        ),
    ],
    'runtime/src/ppu.c': [
        (
            (
                '}\n'
                '\n'
                'uint32_t ppu_mono_tint_rgba(uint32_t argb) {\n'
                '    const uint32_t r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;\n'
                '    const uint32_t lum = (r * 77u + g * 150u + b * 29u) >> 8;   /* 0..255 */\n'
                '    uint32_t dark = 0, light = 0;\n'
                '    ppu_mono_ramp_ends(&dark, &light);\n'
                '    uint32_t out = 0xFF000000u;\n'
                '    for (unsigned sh = 0; sh < 24u; sh += 8u) {\n'
                '        const uint32_t d = (dark >> sh) & 0xFFu, l = (light >> sh) & 0xFFu;\n'
                '        out |= ((d * (255u - lum) + l * lum + 127u) / 255u) << sh;\n'
                '    }\n'
                '    return out;\n'
                '}\n'
                '\n'
                '\n'
            ),
            (
                '}\n'
                '\n'
                'uint32_t ppu_mono_tint_rgba(uint32_t argb) {\n'
                '    /* Passo 31a: cai num dos 4 tons da rampa (as texturas originais usam\n'
                '     * exatamente 255/170/85/0; no verde os tons do meio ficam os do DMG). */\n'
                '    const uint32_t r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;\n'
                '    const uint32_t lum = (r * 77u + g * 150u + b * 29u) >> 8;   /* 0..255 */\n'
                '    const uint8_t shade = lum >= 213u ? 0u : lum >= 128u ? 1u : lum >= 43u ? 2u : 3u;\n'
                '    return rgb555_to_rgba(ppu_mono_shade_rgb555(shade));\n'
                '}\n'
                '\n'
                '\n'
            ),
        ),
    ],
    'runtime/src/gpu_ppu.cpp': [
        (
            (
                '/* Passo 14b-2: escritas no meio do quadro, organizadas por endereço */\n'
                'GLuint g_tex_dirty = 0, g_tex_log = 0;\n'
                'GLint g_loc_haslog = -1, g_loc_scale = -1, g_loc_hd = -1;\n'
                'GLint g_loc_mono = -1, g_loc_mono_dark = -1, g_loc_mono_light = -1;   /* modo de cor original (P&B) */\n'
                'constexpr int kLogW = 256, kLogH = (PPU_NATIVE_GPU_LOG_MAX + kLogW - 1) / kLogW;\n'
                'std::vector<uint16_t> g_dirty;        /* 0x4000 x (início + 1, quantidade) */\n'
                'std::vector<uint8_t> g_logtex;        /* kLogW*kLogH x (line_plus1, valor) */\n'
            ),
            (
                '/* Passo 14b-2: escritas no meio do quadro, organizadas por endereço */\n'
                'GLuint g_tex_dirty = 0, g_tex_log = 0;\n'
                'GLint g_loc_haslog = -1, g_loc_scale = -1, g_loc_hd = -1;\n'
                'GLint g_loc_mono = -1, g_loc_mono_ramp = -1;   /* modo de cor original (P&B) */\n'
                'constexpr int kLogW = 256, kLogH = (PPU_NATIVE_GPU_LOG_MAX + kLogW - 1) / kLogW;\n'
                'std::vector<uint16_t> g_dirty;        /* 0x4000 x (início + 1, quantidade) */\n'
                'std::vector<uint8_t> g_logtex;        /* kLogW*kLogH x (line_plus1, valor) */\n'
            ),
        ),
        (
            (
                '    "uniform int u_scale;\\n"\n'
                '    "uniform int u_hd;\\n"\n'
                '    "uniform int u_mono;\\n"         /* modo de cor original (P&B): paleta do sprite pelo bit 4 do OAM */\n'
                '    "uniform ivec3 u_mono_dark;\\n"  /* rampa P&B (R, G, B) para os tiles HD */\n'
                '    "uniform ivec3 u_mono_light;\\n"\n'
                '    "uniform int u_out;\\n"   /* passo 3D-1: 0 = cor, 1 = só o fundo (sem sprites), 2 = classe */\n'
                '    "uniform int u_winfloor;\\n"   /* passo 3D-2g: janela = chão (g00) */\n'
                '    "uniform int u_marks;\\n"   /* passo 3D-2b: fora das fases, tipo marcado à mão por tile (linha 16 de u_slot) */\n'
            ),
            (
                '    "uniform int u_scale;\\n"\n'
                '    "uniform int u_hd;\\n"\n'
                '    "uniform int u_mono;\\n"         /* modo de cor original (P&B): paleta do sprite pelo bit 4 do OAM */\n'
                '    "uniform ivec4 u_mono_ramp[4];\\n"  /* passo 31a: os 4 tons P&B (R, G, B) para os tiles embutidos */\n'
                '    "uniform int u_out;\\n"   /* passo 3D-1: 0 = cor, 1 = só o fundo (sem sprites), 2 = classe */\n'
                '    "uniform int u_winfloor;\\n"   /* passo 3D-2g: janela = chão (g00) */\n'
                '    "uniform int u_marks;\\n"   /* passo 3D-2b: fora das fases, tipo marcado à mão por tile (linha 16 de u_slot) */\n'
            ),
        ),
        (
            (
                '    "    int size = int(m.a);\\n"\n'
                '    "    int hx = cs, hy = rs;\\n"\n'
                '    "    if (size != span) { hx = cs * size / span; hy = rs * size / span; }\\n"\n'
                '    "    return texelFetch(u_atlas, ivec3(int(m.r) + hx, int(m.g) + hy, int(m.b) & 0x7FFF), 0);\\n"\n'
                '    "}\\n"\n'
                '    /* Recoloração proporcional (hd_recolor_setup/apply) */\n'
                '    "struct Rc { bool on; uvec3 mul; uvec3 add; };\\n"\n'
                '    "Rc rc_setup(uint s1, int idx, uvec3 cur) {\\n"\n'
                '    "    Rc r; r.on = false; r.mul = uvec3(65536u); r.add = uvec3(0u);\\n"\n'
                '    "    int s = int(s1) - 1;\\n"\n'
                '    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\\n"\n'
                '    "    if ((m.b & 0x8000u) == 0u) return r;\\n"\n'
            ),
            (
                '    "    int size = int(m.a);\\n"\n'
                '    "    int hx = cs, hy = rs;\\n"\n'
                '    "    if (size != span) { hx = cs * size / span; hy = rs * size / span; }\\n"\n'
                '    "    return texelFetch(u_atlas, ivec3(int(m.r) + hx, int(m.g) + hy, int(m.b) & 0x3FFF), 0);\\n"\n'
                '    "}\\n"\n'
                '    /* Recoloração proporcional (hd_recolor_setup/apply) */\n'
                '    "struct Rc { bool on; uvec3 mul; uvec3 add; };\\n"\n'
                '    "Rc rc_setup(uint s1, int idx, uvec3 cur) {\\n"\n'
                '    "    Rc r; r.on = false; r.mul = uvec3(65536u); r.add = uvec3(0u);\\n"\n'
                '    "    if (u_mono != 0) return r;\\n"   /* passo 31a: no P&B os mods não são recoloridos pela paleta */\n'
                '    "    int s = int(s1) - 1;\\n"\n'
                '    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\\n"\n'
                '    "    if ((m.b & 0x8000u) == 0u) return r;\\n"\n'
            ),
        ),
        (
            (
                '    "    if (!r.on) return c;\\n"\n'
                '    "    return min(((c * r.mul) >> 16u) + r.add, uvec3(255u));\\n"\n'
                '    "}\\n"\n'
                '    /* Modo original (P&B), igual a ppu_mono_tint_rgba: a cor do PNG HD vai para a\n'
                '     * rampa pela luminância. ocol: paleta do sprite na tabela da linha (no P&B,\n'
                '     * 0 = OBP0 e 1 = OBP1 pelo bit 4 do OAM; senão, a paleta CGB). */\n'
                '    "uvec3 hdm(uvec3 c) {\\n"\n'
                '    "    if (u_mono == 0) return c;\\n"\n'
                '    "    uint l = (c.r * 77u + c.g * 150u + c.b * 29u) >> 8u;\\n"\n'
                '    "    return (uvec3(u_mono_dark) * (255u - l) + uvec3(u_mono_light) * l + 127u) / 255u;\\n"\n'
                '    "}\\n"\n'
                '    "int ocol(int fl) { return u_mono != 0 ? ((fl & 0x10) != 0 ? 1 : 0) : (fl & 7); }\\n"\n'
                '    /* Passo 14b-4: laterais, como margin_hd do ppu_native (registros de ppu.c) */\n'
            ),
            (
                '    "    if (!r.on) return c;\\n"\n'
                '    "    return min(((c * r.mul) >> 16u) + r.add, uvec3(255u));\\n"\n'
                '    "}\\n"\n'
                '    /* Modo original (P&B), igual a ppu_mono_tint_rgba: no tile HD embutido (texturas\n'
                '     * originais, bit 0x4000 do meta) a cor do PNG cai num dos 4 tons da rampa; os\n'
                '     * mods da pasta ficam com as cores deles (passo 31a). ocol: paleta do sprite na\n'
                '     * tabela da linha (no P&B, 0 = OBP0 e 1 = OBP1 pelo bit 4 do OAM; senão, a CGB). */\n'
                '    "uvec3 hdm(uint s1, uvec3 c) {\\n"\n'
                '    "    if (u_mono == 0) return c;\\n"\n'
                '    "    int s = int(s1) - 1;\\n"\n'
                '    "    uvec4 m = texelFetch(u_meta, ivec2(s & 255, s >> 8), 0);\\n"\n'
                '    "    if ((m.b & 0x4000u) == 0u) return c;\\n"\n'
                '    "    uint l = (c.r * 77u + c.g * 150u + c.b * 29u) >> 8u;\\n"\n'
                '    "    int k = l >= 213u ? 0 : (l >= 128u ? 1 : (l >= 43u ? 2 : 3));\\n"\n'
                '    "    return uvec3(u_mono_ramp[k].rgb);\\n"\n'
                '    "}\\n"\n'
                '    "int ocol(int fl) { return u_mono != 0 ? ((fl & 0x10) != 0 ? 1 : 0) : (fl & 7); }\\n"\n'
                '    /* Passo 14b-4: laterais, como margin_hd do ppu_native (registros de ppu.c) */\n'
            ),
        ),
        (
            (
                '    "    uvec3 c = fade3(fade, m, bg_rgba);\\n"\n'
                '    "    if (bs != 0u) {\\n"\n'
                '    "        uvec4 h = hd_texel(bs, int(bg_uvf & 7u), int((bg_uvf >> 3) & 7u), uvf_flags(bg_uvf), sx, sy, n);\\n"\n'
                '    "        if (h.a != 0u) c = fade3(fade, m, hdm(rc_apply(rc_setup(bs, bg_raw, bg_rgba), h)));\\n"\n'
                '    "    }\\n"\n'
                '    "    uvec3 cbg = c;\\n"\n'
                '    "    int cls = clsb;\\n"\n'
                '    "    if (ot) {\\n"\n'
                '    "        if (os != 0u) {\\n"\n'
                '    "            uvec4 h = hd_texel(os, int(obj_uvf & 7u), int((obj_uvf >> 3) & 7u), uvf_flags(obj_uvf), sx, sy, n);\\n"\n'
                '    "            if (h.a >= 128u) { c = hdm(rc_apply(rc_setup(os, obj_raw, obj_rgba), h)); cls = 3; }\\n"\n'
                '    "        } else {\\n"\n'
                '    "            c = obj_rgba; cls = 3;\\n"\n'
                '    "        }\\n"\n'
            ),
            (
                '    "    uvec3 c = fade3(fade, m, bg_rgba);\\n"\n'
                '    "    if (bs != 0u) {\\n"\n'
                '    "        uvec4 h = hd_texel(bs, int(bg_uvf & 7u), int((bg_uvf >> 3) & 7u), uvf_flags(bg_uvf), sx, sy, n);\\n"\n'
                '    "        if (h.a != 0u) c = fade3(fade, m, hdm(bs, rc_apply(rc_setup(bs, bg_raw, bg_rgba), h)));\\n"\n'
                '    "    }\\n"\n'
                '    "    uvec3 cbg = c;\\n"\n'
                '    "    int cls = clsb;\\n"\n'
                '    "    if (ot) {\\n"\n'
                '    "        if (os != 0u) {\\n"\n'
                '    "            uvec4 h = hd_texel(os, int(obj_uvf & 7u), int((obj_uvf >> 3) & 7u), uvf_flags(obj_uvf), sx, sy, n);\\n"\n'
                '    "            if (h.a >= 128u) { c = hdm(os, rc_apply(rc_setup(os, obj_raw, obj_rgba), h)); cls = 3; }\\n"\n'
                '    "        } else {\\n"\n'
                '    "            c = obj_rgba; cls = 3;\\n"\n'
                '    "        }\\n"\n'
            ),
        ),
        (
            (
                '    "        uint s1 = slot_of(0, attr & 7, ((attr & 0x08) != 0 ? 384 : 0) + tidx);\\n"\n'
                '    "        if (s1 != 0u) {\\n"\n'
                '    "            uvec4 h = hd_texel(s1, wx & 7, wy & 7, attr & 0x60, X - cx * n, Y - cy * n, n);\\n"\n'
                '    "            if (h.a != 0u) c = hdm(rc_apply(rc_setup(s1, raw, c), h));\\n"\n'
                '    "        }\\n"\n'
                '    "    }\\n"\n'
                '    "    g_kind = 0;\\n"\n'
            ),
            (
                '    "        uint s1 = slot_of(0, attr & 7, ((attr & 0x08) != 0 ? 384 : 0) + tidx);\\n"\n'
                '    "        if (s1 != 0u) {\\n"\n'
                '    "            uvec4 h = hd_texel(s1, wx & 7, wy & 7, attr & 0x60, X - cx * n, Y - cy * n, n);\\n"\n'
                '    "            if (h.a != 0u) c = hdm(s1, rc_apply(rc_setup(s1, raw, c), h));\\n"\n'
                '    "        }\\n"\n'
                '    "    }\\n"\n'
                '    "    g_kind = 0;\\n"\n'
            ),
        ),
        (
            (
                '    "                uint os = slot_of(1, opal, ogi);\\n"\n'
                '    "                if (os != 0u) {\\n"\n'
                '    "                    uvec4 ho = hd_texel(os, ou, ov, ofl, X - cx * n, Y - cy * n, n);\\n"\n'
                '    "                    if (ho.a >= 128u) oc = hdm(rc_apply(rc_setup(os, oraw, oc), ho)); else show = false;\\n"\n'
                '    "                }\\n"\n'
                '    "            }\\n"\n'
                '    "            if (show) { emit(oc, c, 3); return; }\\n"\n'
            ),
            (
                '    "                uint os = slot_of(1, opal, ogi);\\n"\n'
                '    "                if (os != 0u) {\\n"\n'
                '    "                    uvec4 ho = hd_texel(os, ou, ov, ofl, X - cx * n, Y - cy * n, n);\\n"\n'
                '    "                    if (ho.a >= 128u) oc = hdm(os, rc_apply(rc_setup(os, oraw, oc), ho)); else show = false;\\n"\n'
                '    "                }\\n"\n'
                '    "            }\\n"\n'
                '    "            if (show) { emit(oc, c, 3); return; }\\n"\n'
            ),
        ),
        (
            (
                '    "    uvec3 c = bg_orig;\\n"\n'
                '    "    if (bs != 0u) {\\n"\n'
                '    "        uvec4 hb = hd_texel(bs, bu, bv, attr & 0x60, sx, sy, n);\\n"\n'
                '    "        if (hb.a != 0u) c = hdm(rc_apply(rc_setup(bs, bgraw, bg_orig), hb));\\n"\n'
                '    "    }\\n"\n'
                '    "    uvec3 cbg = c;\\n"\n'
                '    "    int cls = hudpix ? 2 : 1;\\n"\n'
                '    "    if (can_win) {\\n"\n'
                '    "        if (os != 0u) {\\n"\n'
                '    "            uvec4 ho = hd_texel(os, cu, cv, cfl, sx, sy, n);\\n"\n'
                '    "            if (ho.a >= 128u) { c = hdm(rc_apply(rc_setup(os, oraw, pal(32 + cdm * 4 + oraw, y)), ho)); cls = 3; }\\n"\n'
                '    "        } else if (oraw != 0) {\\n"\n'
                '    "            c = obj_orig; cls = 3;\\n"\n'
                '    "        }\\n"\n'
            ),
            (
                '    "    uvec3 c = bg_orig;\\n"\n'
                '    "    if (bs != 0u) {\\n"\n'
                '    "        uvec4 hb = hd_texel(bs, bu, bv, attr & 0x60, sx, sy, n);\\n"\n'
                '    "        if (hb.a != 0u) c = hdm(bs, rc_apply(rc_setup(bs, bgraw, bg_orig), hb));\\n"\n'
                '    "    }\\n"\n'
                '    "    uvec3 cbg = c;\\n"\n'
                '    "    int cls = hudpix ? 2 : 1;\\n"\n'
                '    "    if (can_win) {\\n"\n'
                '    "        if (os != 0u) {\\n"\n'
                '    "            uvec4 ho = hd_texel(os, cu, cv, cfl, sx, sy, n);\\n"\n'
                '    "            if (ho.a >= 128u) { c = hdm(os, rc_apply(rc_setup(os, oraw, pal(32 + cdm * 4 + oraw, y)), ho)); cls = 3; }\\n"\n'
                '    "        } else if (oraw != 0) {\\n"\n'
                '    "            c = obj_orig; cls = 3;\\n"\n'
                '    "        }\\n"\n'
            ),
        ),
        (
            (
                '    drain_errors("atlas");\n'
                '    g_atlas_tiles = 0;\n'
                '    const uint32_t cap = ppu_native_hd_table_capacity();\n'
                '    struct Item { uint32_t slot; int size; const uint32_t* px; const uint32_t* ref; };\n'
                '    std::vector<Item> items;\n'
                '    for (uint32_t i = 0; i < cap; ++i) {\n'
                '        Item it{i, 0, nullptr, nullptr};\n'
                '        if (ppu_native_hd_table_entry(i, &it.size, &it.px, &it.ref)) items.push_back(it);\n'
                '    }\n'
                '    std::vector<uint16_t> meta((size_t)256 * 128 * 4, 0);\n'
                '    std::vector<uint32_t> ref((size_t)1024 * 128, 0);\n'
            ),
            (
                '    drain_errors("atlas");\n'
                '    g_atlas_tiles = 0;\n'
                '    const uint32_t cap = ppu_native_hd_table_capacity();\n'
                '    struct Item { uint32_t slot; int size; const uint32_t* px; const uint32_t* ref; bool builtin; };\n'
                '    std::vector<Item> items;\n'
                '    for (uint32_t i = 0; i < cap; ++i) {\n'
                '        Item it{i, 0, nullptr, nullptr, false};\n'
                '        if (ppu_native_hd_table_entry(i, &it.size, &it.px, &it.ref)) {\n'
                '            it.builtin = ppu_native_hd_table_entry_builtin(i);   /* passo 31a */\n'
                '            items.push_back(it);\n'
                '        }\n'
                '    }\n'
                '    std::vector<uint16_t> meta((size_t)256 * 128 * 4, 0);\n'
                '    std::vector<uint32_t> ref((size_t)1024 * 128, 0);\n'
            ),
        ),
        (
            (
                '        uint16_t* m = &meta[(size_t)it.slot * 4];\n'
                '        m[0] = (uint16_t)pos[k].x;\n'
                '        m[1] = (uint16_t)pos[k].y;\n'
                '        m[2] = (uint16_t)(pos[k].layer | (it.ref ? 0x8000 : 0));\n'
                '        m[3] = (uint16_t)it.size;\n'
                '        if (it.ref) {\n'
                '            for (int c = 0; c < 4; ++c) ref[(size_t)it.slot * 4 + c] = it.ref[c];\n'
            ),
            (
                '        uint16_t* m = &meta[(size_t)it.slot * 4];\n'
                '        m[0] = (uint16_t)pos[k].x;\n'
                '        m[1] = (uint16_t)pos[k].y;\n'
                '        m[2] = (uint16_t)(pos[k].layer | (it.ref ? 0x8000 : 0) | (it.builtin ? 0x4000 : 0));   /* passo 31a */\n'
                '        m[3] = (uint16_t)it.size;\n'
                '        if (it.ref) {\n'
                '            for (int c = 0; c < 4; ++c) ref[(size_t)it.slot * 4 + c] = it.ref[c];\n'
            ),
        ),
        (
            (
                '     * ppu_native; aqui só a escolha OBP0/OBP1 dos sprites e o HD (a GPU só\n'
                '     * monta quadros do modo CGB). */\n'
                '    const bool mono = ppu_get_color_mode() != PPU_COLOR_DX;\n'
                '    uint32_t dark = 0, light = 0;\n'
                '    ppu_mono_ramp_ends(&dark, &light);\n'
                '    gl.Uniform1i(g_loc_mono, mono ? 1 : 0);\n'
                '    gl.Uniform3i(g_loc_mono_dark, (GLint)((dark >> 16) & 255u), (GLint)((dark >> 8) & 255u), (GLint)(dark & 255u));\n'
                '    gl.Uniform3i(g_loc_mono_light, (GLint)((light >> 16) & 255u), (GLint)((light >> 8) & 255u), (GLint)(light & 255u));\n'
                '}\n'
                '\n'
                'bool g_nofade = false;   /* 3D-4a: laterais sem escurecimento (u_ext = 2 no margin_main) */\n'
            ),
            (
                '     * ppu_native; aqui só a escolha OBP0/OBP1 dos sprites e o HD (a GPU só\n'
                '     * monta quadros do modo CGB). */\n'
                '    const bool mono = ppu_get_color_mode() != PPU_COLOR_DX;\n'
                '    gl.Uniform1i(g_loc_mono, mono ? 1 : 0);\n'
                '    GLint ramp[16];\n'
                '    for (int k = 0; k < 4; ++k) {   /* passo 31a: os 4 tons, como ppu_mono_tint_rgba */\n'
                '        const uint32_t v = 255u - (uint32_t)k * 85u;\n'
                '        const uint32_t c = ppu_mono_tint_rgba(0xFF000000u | (v << 16) | (v << 8) | v);\n'
                '        ramp[k * 4 + 0] = (GLint)((c >> 16) & 255u);\n'
                '        ramp[k * 4 + 1] = (GLint)((c >> 8) & 255u);\n'
                '        ramp[k * 4 + 2] = (GLint)(c & 255u);\n'
                '        ramp[k * 4 + 3] = 255;\n'
                '    }\n'
                '    gl.Uniform4iv(g_loc_mono_ramp, 4, ramp);\n'
                '}\n'
                '\n'
                'bool g_nofade = false;   /* 3D-4a: laterais sem escurecimento (u_ext = 2 no margin_main) */\n'
            ),
        ),
        (
            (
                '    g_loc_scale = gl.GetUniformLocation(g_prog, "u_scale");\n'
                '    g_loc_hd = gl.GetUniformLocation(g_prog, "u_hd");\n'
                '    g_loc_mono = gl.GetUniformLocation(g_prog, "u_mono");\n'
                '    g_loc_mono_dark = gl.GetUniformLocation(g_prog, "u_mono_dark");\n'
                '    g_loc_mono_light = gl.GetUniformLocation(g_prog, "u_mono_light");\n'
                '    g_loc_hastl = gl.GetUniformLocation(g_prog, "u_hastl");\n'
                '    g_loc_margin = gl.GetUniformLocation(g_prog, "u_margin");\n'
                '    g_loc_out = gl.GetUniformLocation(g_prog, "u_out");\n'
            ),
            (
                '    g_loc_scale = gl.GetUniformLocation(g_prog, "u_scale");\n'
                '    g_loc_hd = gl.GetUniformLocation(g_prog, "u_hd");\n'
                '    g_loc_mono = gl.GetUniformLocation(g_prog, "u_mono");\n'
                '    g_loc_mono_ramp = gl.GetUniformLocation(g_prog, "u_mono_ramp");   /* passo 31a */\n'
                '    g_loc_hastl = gl.GetUniformLocation(g_prog, "u_hastl");\n'
                '    g_loc_margin = gl.GetUniformLocation(g_prog, "u_margin");\n'
                '    g_loc_out = gl.GetUniformLocation(g_prog, "u_out");\n'
            ),
        ),
    ],
    'runtime/src/platform_sdl.cpp': [
        (
            (
                'static int g_texture_scale = 1;          /* escala com que a textura foi criada */\n'
                '\n'
                '// Supersampling\n'
                'static int g_internal_res_scale = 1; // 1x (Nativo), 2x, 3x, 4x — 1x padrão (passo 20a)\n'
                'static const char* g_internal_res_names[] = {\n'
                '    "1x (Nativo GB - 160x144)",\n'
                '    "2x Internal (320x288)",\n'
            ),
            (
                'static int g_texture_scale = 1;          /* escala com que a textura foi criada */\n'
                '\n'
                '// Supersampling\n'
                'static int g_internal_res_scale = 2; // 1x (Nativo), 2x, 3x, 4x — 2x padrão (passo 31a; era 1x no 20a)\n'
                'static const char* g_internal_res_names[] = {\n'
                '    "1x (Nativo GB - 160x144)",\n'
                '    "2x Internal (320x288)",\n'
            ),
        ),
        (
            (
                '    g_max_speed_mode = false;\n'
                '    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;\n'
                '    g_render_filter_mode = GB_RENDER_FILTER_LINEAR;   /* passo 14p */\n'
                '    g_internal_res_scale = 1;                         /* passo 20a: 1x padrao */\n'
                '    g_video_backend_pref = 1;                         /* passo 14p: OpenGL (vale ao abrir de novo) */\n'
                '    g_gpu_screen_pref = 1;                            /* passo 14c-3: a GPU monta a tela */\n'
                '    g_3d_mode = 0;                                    /* passo 3D-1 */\n'
            ),
            (
                '    g_max_speed_mode = false;\n'
                '    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;\n'
                '    g_render_filter_mode = GB_RENDER_FILTER_LINEAR;   /* passo 14p */\n'
                '    g_internal_res_scale = 2;                         /* passo 31a: 2x padrao (texturas originais) */\n'
                '    g_video_backend_pref = 1;                         /* passo 14p: OpenGL (vale ao abrir de novo) */\n'
                '    g_gpu_screen_pref = 1;                            /* passo 14c-3: a GPU monta a tela */\n'
                '    g_3d_mode = 0;                                    /* passo 3D-1 */\n'
            ),
        ),
        (
            (
                '                                         TR("Original (black and white)", "Original (preto e branco)"),\n'
                '                                         TR("Original (DMG green)", "Original (verde do DMG)")};\n'
                '            if (ImGui::Combo(TR("Colors", "Cores"), &color_mode, color_names, IM_ARRAYSIZE(color_names))) {\n'
                '                ppu_set_color_mode(color_mode);\n'
                '                save_runtime_preferences();\n'
                '            }\n'
                '            if (color_mode != PPU_COLOR_DX) {\n'
                '                if (const GBPPU* ppu = active_ppu()) {\n'
                '                    /* Diagnóstico: se ficarem em FC/FF/FF, o patch não escreve\n'
                '                     * mais esses registradores e o P&B sai errado. */\n'
            ),
            (
                '                                         TR("Original (black and white)", "Original (preto e branco)"),\n'
                '                                         TR("Original (DMG green)", "Original (verde do DMG)")};\n'
                '            if (ImGui::Combo(TR("Colors", "Cores"), &color_mode, color_names, IM_ARRAYSIZE(color_names))) {\n'
                '                const bool was_dx = ppu_get_color_mode() == PPU_COLOR_DX;\n'
                '                ppu_set_color_mode(color_mode);\n'
                '                /* Passo 31a: entrar/sair do Original liga/desliga as texturas originais\n'
                '                 * embutidas (fonte de mods abaixo dos mods da pasta) */\n'
                '                if (was_dx != (color_mode == PPU_COLOR_DX)) {\n'
                '                    hd_pack_io_load_pack();\n'
                '                    g_renderer_reset_pending = true;\n'
                '                }\n'
                '                save_runtime_preferences();\n'
                '            }\n'
                '            if (color_mode != PPU_COLOR_DX) {\n'
                '                if (!hd_pack_io_builtin_present()) {   /* passo 31a */\n'
                '                    ImGui::TextDisabled("%s", TR("Original textures not found in wario.o2r (rebuild it with the launcher).",\n'
                '                                                 "Texturas originais nao estao no wario.o2r (refaca pelo launcher)."));\n'
                '                } else if (effective_internal_scale() < 2) {\n'
                '                    ImGui::TextDisabled("%s", TR("Original textures need Internal Resolution 2x or more.",\n'
                '                                                 "As texturas originais precisam de Resolucao interna 2x ou mais."));\n'
                '                }\n'
                '                if (const GBPPU* ppu = active_ppu()) {\n'
                '                    /* Diagnóstico: se ficarem em FC/FF/FF, o patch não escreve\n'
                '                     * mais esses registradores e o P&B sai errado. */\n'
            ),
        ),
    ],
    'tools/progresso_renderizador.md': [
        (
            (
                '## Passo 20b — versão 1.6\n'
                '- RELEASE_NOTES.md: seção v.1.6 no topo (inimigos além da borda no widescreen/3D, morcegos, giroscópio, V-Sync no Android, 1x padrão, README de controles, Steam Deck como problema conhecido). A v.1.5 continua logo abaixo.\n'
                "- android/app/build.gradle: versionName '1.6', versionCode 3 (antes '1.1' / 2).\n"
            ),
            (
                '## Passo 20b — versão 1.6\n'
                '- RELEASE_NOTES.md: seção v.1.6 no topo (inimigos além da borda no widescreen/3D, morcegos, giroscópio, V-Sync no Android, 1x padrão, README de controles, Steam Deck como problema conhecido). A v.1.5 continua logo abaixo.\n'
                "- android/app/build.gradle: versionName '1.6', versionCode 3 (antes '1.1' / 2).\n"
                '\n'
                '## Passo 31a — texturas P&B originais embutidas no wario.o2r\n'
                '- `tools/gerar_receita_original_bw.py` (roda na máquina do dev, com o disassembly): gera `original_bw_receita.h` (raiz) só com números: hash do tile do DX + contexto, origem do tile na ROM original (.bin, .rlc + índice, água 1bpp) e BGP/OBP0. Confere que reproduz byte a byte o `.o2r` do `gerar_mod_original_bw.py`.\n'
                '- `original_bw.hpp`: no launcher, depois do IPS, segue a receita (confere o SHA-256 das duas ROMs), pinta PNGs 8x8 indexados e grava em `original_bw/` dentro do `wario.o2r` (`AssetExtractor::build` com arquivos extras). `o2r_info.txt` ganha `original_bw_recipe=<versão>` e `original_bw=1|0`. Sem o header no projeto, nada muda.\n'
                '- `main.cpp`: `wario.o2r` com outra versão da receita abre o launcher com o aviso "Update needed"; cancelar segue com o pacote antigo.\n'
                '- `hd_pack_io`: fonte embutida "Original textures (built-in)", primeira da lista (abaixo dos mods da pasta), só com Cores = Original. Trocar entre DX e Original recarrega os mods.\n'
                '- Rampa P&B (cinza/verde) só nos tiles embutidos (`HdTile.builtin`; GPU: bit 0x4000 do meta, `hdm(slot, cor)`), caindo exatamente nos 4 tons. Mods da pasta ficam com as cores deles e, no modo Original, sem recolorir pela paleta.\n'
                '- Resolução interna padrão: 2x (também no Reset Display).\n'
            ),
        ),
    ],
}
NEW_FILES = {
    'original_bw.hpp': (
        '#pragma once\n'
        '\n'
        '// =============================================================================\n'
        '// Texturas P&B originais (passo 31a)\n'
        '//\n'
        '// No launcher, antes de recriar o wario.o2r, segue a receita gerada por\n'
        '// tools/gerar_receita_original_bw.py (original_bw_receita.h): tira cada tile da\n'
        '// ROM ORIGINAL, pinta com os tons do Game Boy (registrador BGP/OBP0 da tela) e\n'
        '// grava um PNG 8x8 com o nome do tile do DX (b_<hash>[_fXX|_gXX].png). Os PNGs\n'
        '// vão para a pasta original_bw/ dentro do wario.o2r; o jogo os carrega como\n'
        '// uma fonte de mods embutida quando a opção "Cores" está em Original.\n'
        '//\n'
        '// Sem o original_bw_receita.h no projeto, available() devolve false e nada muda.\n'
        '// =============================================================================\n'
        '\n'
        '#include <cstdint>\n'
        '#include <cstdio>\n'
        '#include <cstring>\n'
        '#include <string>\n'
        '#include <vector>\n'
        '\n'
        '#include "gbrt_hash.h"\n'
        '\n'
        '#if defined(__has_include)\n'
        '#  if __has_include("original_bw_receita.h")\n'
        '#    include "original_bw_receita.h"\n'
        '#    define ORIGINAL_BW_HAS_RECIPE 1\n'
        '#  endif\n'
        '#endif\n'
        '\n'
        'namespace original_bw {\n'
        '\n'
        'static constexpr const char* FOLDER = "original_bw/";\n'
        '\n'
        'struct File {\n'
        '    std::string name;\n'
        '    std::vector<uint8_t> data;\n'
        '};\n'
        '\n'
        '#ifdef ORIGINAL_BW_HAS_RECIPE\n'
        'inline bool available() { return sizeof(kOriginalBwTextures) > 0; }\n'
        'inline uint32_t recipe_version() { return ORIGINAL_BW_RECIPE_VERSION; }\n'
        '#else\n'
        'inline bool available() { return false; }\n'
        'inline uint32_t recipe_version() { return 0; }\n'
        '#endif\n'
        '\n'
        'namespace detail {\n'
        '\n'
        '// Descompactador RLC do Wario Land (igual a gfx_decomp_rlc do gerar_mod_original_bw.py):\n'
        '// grava nas posições pares e depois nas ímpares de um buffer de 0x1800 bytes.\n'
        'inline std::vector<uint8_t> decomp_rlc(const uint8_t* data, size_t max_len, size_t buf_size = 0x1800) {\n'
        '    std::vector<uint8_t> out(buf_size, 0);\n'
        '    size_t i = 0, p = 0;\n'
        '    while (i < max_len) {\n'
        '        const uint8_t b = data[i++];\n'
        '        if (b == 0) return out;\n'
        '        const unsigned count = b & 0x7Fu;\n'
        '        const bool literal = (b & 0x80u) != 0;\n'
        '        uint8_t source = 0;\n'
        '        if (!literal) {\n'
        '            if (i >= max_len) return out;\n'
        '            source = data[i++];\n'
        '        }\n'
        '        for (unsigned k = 0; k < (literal ? count : b); ++k) {\n'
        '            if (literal && i >= max_len) break;\n'
        '            if (p >= buf_size) {\n'
        '                if (p == buf_size + 1) return out;\n'
        '                p = 1;\n'
        '            }\n'
        '            out[p] = literal ? data[i++] : source;\n'
        '            p += 2;\n'
        '        }\n'
        '    }\n'
        '    return out;\n'
        '}\n'
        '\n'
        'inline uint32_t crc32(const uint8_t* d, size_t n) {\n'
        '    uint32_t c = 0xFFFFFFFFu;\n'
        '    for (size_t i = 0; i < n; ++i) {\n'
        '        c ^= d[i];\n'
        '        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));\n'
        '    }\n'
        '    return ~c;\n'
        '}\n'
        '\n'
        'inline void put_be32(std::vector<uint8_t>& v, uint32_t x) {\n'
        '    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));\n'
        '    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);\n'
        '}\n'
        '\n'
        'inline void chunk(std::vector<uint8_t>& png, const char* tag, const std::vector<uint8_t>& data) {\n'
        '    put_be32(png, (uint32_t)data.size());\n'
        '    std::vector<uint8_t> body(tag, tag + 4);\n'
        '    body.insert(body.end(), data.begin(), data.end());\n'
        '    png.insert(png.end(), body.begin(), body.end());\n'
        '    put_be32(png, crc32(body.data(), body.size()));\n'
        '}\n'
        '\n'
        '// PNG 8x8 indexado (8 bits por ponto), zlib sem compressão (bloco "stored").\n'
        '// rgb: 4 cores; transparent0: cor 0 transparente (sprites).\n'
        'inline std::vector<uint8_t> png_8x8(const uint8_t idx[64], const uint8_t rgb[4][3], bool transparent0) {\n'
        "    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};\n"
        '    std::vector<uint8_t> ihdr;\n'
        '    put_be32(ihdr, 8); put_be32(ihdr, 8);\n'
        '    ihdr.insert(ihdr.end(), {8, 3, 0, 0, 0});           // 8 bits, paleta\n'
        '    chunk(png, "IHDR", ihdr);\n'
        '    std::vector<uint8_t> plte;\n'
        '    for (int c = 0; c < 4; ++c) plte.insert(plte.end(), {rgb[c][0], rgb[c][1], rgb[c][2]});\n'
        '    chunk(png, "PLTE", plte);\n'
        '    if (transparent0) chunk(png, "tRNS", {0, 255, 255, 255});\n'
        '    std::vector<uint8_t> raw;\n'
        '    for (int y = 0; y < 8; ++y) {\n'
        '        raw.push_back(0);                                 // filtro 0\n'
        '        raw.insert(raw.end(), idx + y * 8, idx + y * 8 + 8);\n'
        '    }\n'
        '    std::vector<uint8_t> z = {0x78, 0x01, 0x01,\n'
        '                              (uint8_t)(raw.size() & 0xFF), (uint8_t)(raw.size() >> 8),\n'
        '                              (uint8_t)(~raw.size() & 0xFF), (uint8_t)((~raw.size() >> 8) & 0xFF)};\n'
        '    z.insert(z.end(), raw.begin(), raw.end());\n'
        '    uint32_t a = 1, b = 0;                               // adler32\n'
        '    for (uint8_t x : raw) { a = (a + x) % 65521u; b = (b + a) % 65521u; }\n'
        '    put_be32(z, (b << 16) | a);\n'
        '    chunk(png, "IDAT", z);\n'
        '    chunk(png, "IEND", {});\n'
        '    return png;\n'
        '}\n'
        '\n'
        '}  // namespace detail\n'
        '\n'
        '// Cria os PNGs (nome = "original_bw/<tile>.png"). orig: ROM original;\n'
        '// dx: ROM do DX já com o patch. As duas precisam ser as da receita.\n'
        'inline bool build(const std::vector<uint8_t>& orig, const std::vector<uint8_t>& dx,\n'
        '                  std::vector<File>& out, std::string& err) {\n'
        '#ifndef ORIGINAL_BW_HAS_RECIPE\n'
        '    (void)orig; (void)dx; (void)out;\n'
        '    err = "recipe not compiled in";\n'
        '    return false;\n'
        '#else\n'
        '    if (!gbrt_sha256_matches_hex(orig.data(), orig.size(), kOriginalBwOrigSha256)) {\n'
        '        err = "original ROM is not the one the recipe was made for";\n'
        '        return false;\n'
        '    }\n'
        '    if (!gbrt_sha256_matches_hex(dx.data(), dx.size(), kOriginalBwDxSha256)) {\n'
        '        err = "patched ROM is not the one the recipe was made for (different IPS?)";\n'
        '        return false;\n'
        '    }\n'
        '    std::vector<std::vector<uint8_t>> bufs;\n'
        '    for (const OriginalBwAsset& a : kOriginalBwAssets) {\n'
        '        if ((size_t)a.offset + a.length > orig.size()) { err = "recipe asset out of range"; return false; }\n'
        '        bufs.push_back(detail::decomp_rlc(orig.data() + a.offset, a.length));\n'
        '    }\n'
        '    static const uint8_t kShades[4][3] = {{255, 255, 255}, {170, 170, 170}, {85, 85, 85}, {0, 0, 0}};\n'
        '    out.clear();\n'
        '    out.reserve(sizeof(kOriginalBwTextures) / sizeof(kOriginalBwTextures[0]));\n'
        '    for (const OriginalBwTexture& t : kOriginalBwTextures) {\n'
        '        const OriginalBwSource& s = kOriginalBwSources[t.source];\n'
        '        uint8_t tile[16] = {0};\n'
        '        if (s.type == 0) {\n'
        '            if ((size_t)s.offset + 16 > orig.size()) { err = "recipe offset out of range"; return false; }\n'
        '            std::memcpy(tile, orig.data() + s.offset, 16);\n'
        '        } else if (s.type == 1) {\n'
        '            if (s.asset < 0 || (size_t)s.asset >= bufs.size() || (size_t)s.tile * 16 + 16 > bufs[s.asset].size()) {\n'
        '                err = "recipe tile out of range"; return false;\n'
        '            }\n'
        '            std::memcpy(tile, bufs[s.asset].data() + s.tile * 16, 16);\n'
        '        } else {\n'
        '            for (int y = 0; y < 8; ++y) {\n'
        '                const size_t lo = (size_t)s.offset + (size_t)y * s.frames + s.frame;\n'
        '                if (lo >= orig.size()) { err = "recipe offset out of range"; return false; }\n'
        '                tile[y * 2] = orig[lo];\n'
        '                if (s.asset >= 0) {\n'
        '                    if ((size_t)s.asset >= bufs.size() || (size_t)s.tile * 16 + 16 > bufs[s.asset].size()) {\n'
        '                        err = "recipe tile out of range"; return false;\n'
        '                    }\n'
        '                    tile[y * 2 + 1] = bufs[s.asset][s.tile * 16 + y * 2 + 1];\n'
        '                }\n'
        '            }\n'
        '        }\n'
        '        uint8_t idx[64];\n'
        '        for (int y = 0; y < 8; ++y) {\n'
        '            for (int x = 0; x < 8; ++x) {\n'
        '                const int bit = 7 - x;\n'
        '                idx[y * 8 + x] = (uint8_t)(((tile[y * 2] >> bit) & 1) | (((tile[y * 2 + 1] >> bit) & 1) << 1));\n'
        '            }\n'
        '        }\n'
        '        uint8_t rgb[4][3];\n'
        '        for (int i = 0; i < 4; ++i) std::memcpy(rgb[i], kShades[(t.reg >> (i * 2)) & 3], 3);\n'
        '        char name[64];\n'
        '        std::snprintf(name, sizeof(name), "%s%c_%016llx%s%s.png", FOLDER, t.kind,\n'
        '                      (unsigned long long)t.hash, t.ctx[0] ? "_" : "", t.ctx);\n'
        "        out.push_back({name, detail::png_8x8(idx, rgb, t.kind == 'o')});\n"
        '    }\n'
        '    return true;\n'
        '#endif\n'
        '}\n'
        '\n'
        '}  // namespace original_bw\n'
    ),
    'tools/gerar_receita_original_bw.py': (
        '#!/usr/bin/env python3\n'
        '"""\n'
        'gerar_receita_original_bw.py\n'
        '\n'
        'Gera original_bw_receita.h (na raiz do projeto): a "receita" que o launcher usa\n'
        'para criar, dentro do wario.o2r, as mesmas texturas P&B do\n'
        'gerar_mod_original_bw.py, sem precisar do disassembly.\n'
        '\n'
        'A receita só tem números: para cada textura, o nome (hash do tile do DX e o\n'
        'contexto _fXX/_gXX) e de onde tirar o tile na ROM ORIGINAL (offset de um .bin,\n'
        'ou um .rlc + índice do tile, ou a água 1bpp do mapa), mais o registrador\n'
        'BGP/OBP0 que dá os tons. Nenhum byte do jogo vai para o header.\n'
        '\n'
        'A escolha de cada textura (votos, variantes por fase/tela) é a do\n'
        'gerar_mod_original_bw.py: este script importa as funções dele e, no fim,\n'
        'roda o próprio gerar_mod_original_bw.py e confere que a receita reproduz\n'
        'exatamente o mesmo .o2r (mesmos nomes e mesmos PNGs).\n'
        '\n'
        'Uso (na raiz do projeto, com o disassembly em tools/disassembly):\n'
        '    python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r\n'
        '    python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r --conferir   # não grava o .h\n'
        '\n'
        'O segundo arquivo é a ROM do DX (.gb) ou o wario.o2r do jogo. Rode de novo\n'
        'sempre que mudar o gerar_mod_original_bw.py: a versão da receita muda e o jogo\n'
        'pede para recriar o wario.o2r pelo launcher.\n'
        '"""\n'
        '\n'
        'import argparse\n'
        'import hashlib\n'
        'import io\n'
        'import os\n'
        'import subprocess\n'
        'import sys\n'
        'import tempfile\n'
        'import zipfile\n'
        'import zlib\n'
        'from collections import Counter, defaultdict\n'
        'from pathlib import Path\n'
        '\n'
        'sys.path.insert(0, str(Path(__file__).resolve().parent))\n'
        'import gerar_mod_original_bw as g  # noqa: E402\n'
        '\n'
        'ROOT = Path(__file__).resolve().parent.parent\n'
        'HEADER = ROOT / "original_bw_receita.h"\n'
        '\n'
        '# ----------------------------------------------------------------------------\n'
        '# Pares (tile original, tile DX) com a origem do tile original na ROM original\n'
        '# (mesma lógica de g.asset_pairs / g.agua_1bpp_pairs)\n'
        '# ----------------------------------------------------------------------------\n'
        '\n'
        'class Recipe:\n'
        '    def __init__(self):\n'
        '        self.assets = []          # (offset, tamanho) dos .rlc na ROM original\n'
        '        self.asset_index = {}\n'
        '\n'
        '    def asset(self, off, length):\n'
        '        key = (off, length)\n'
        '        if key not in self.asset_index:\n'
        '            self.asset_index[key] = len(self.assets)\n'
        '            self.assets.append(key)\n'
        '        return self.asset_index[key]\n'
        '\n'
        '\n'
        'def asset_pairs_src(rel, orig_rom, dx_rom, rcp):\n'
        '    """Igual a g.asset_pairs, mais a origem de cada tile original."""\n'
        '    raw = (g.GFX_DIR / rel).read_bytes()\n'
        '    pos = orig_rom.find(raw)\n'
        '    if rel.endswith(".rlc"):\n'
        '        orig = g.gfx_decomp_rlc(raw)\n'
        '        dx = g.gfx_decomp_rlc(dx_rom[pos:pos + 0x3000]) if pos >= 0 else orig\n'
        '        pairs = list(zip(g.tile_list(orig), g.tile_list(dx)))\n'
        '        if pos >= 0:\n'
        '            aid = rcp.asset(pos, len(raw))\n'
        '            return [(ot, dt, ("rlc", aid, i)) for i, (ot, dt) in enumerate(pairs)]\n'
        '        return [(ot, dt, None) for ot, dt in pairs]\n'
        '\n'
        '    if len(raw) % 16 != 0:\n'
        '        return []\n'
        '    if pos >= 0 and pos + len(raw) <= len(dx_rom):\n'
        '        pairs = list(zip(g.tile_list(raw), g.tile_list(dx_rom[pos:pos + len(raw)])))\n'
        '        return [(ot, dt, ("rom", pos + i * 16)) for i, (ot, dt) in enumerate(pairs)]\n'
        '\n'
        '    out = []\n'
        '    for t in g.tile_list(raw):\n'
        '        p = orig_rom.find(t)\n'
        '        out.append((t, dx_rom[p:p + 16] if p >= 0 else t, ("rom", p) if p >= 0 else None))\n'
        '    return out\n'
        '\n'
        '\n'
        'def agua_pairs_src(orig_rom, dx_rom, rcp):\n'
        '    """Igual a g.agua_1bpp_pairs, mais a origem."""\n'
        '    out = []\n'
        '    rlc_cache = {}\n'
        '    for rel, frames, rlc_rel, dests in g.AGUA_1BPP:\n'
        '        data = (g.GFX_DIR / rel).read_bytes()\n'
        '        stored = bytes(data[j * 8 + i] for i in range(8) for j in range(frames))\n'
        '        pos = orig_rom.find(stored)\n'
        '        dx_data = data\n'
        '        if pos >= 0:\n'
        '            dx_stored = dx_rom[pos:pos + len(stored)]\n'
        '            dx_data = bytes(dx_stored[i * frames + j] for j in range(frames) for i in range(8))\n'
        '\n'
        '        if rlc_rel not in rlc_cache:\n'
        '            rlc_cache[rlc_rel] = asset_pairs_src(rlc_rel, orig_rom, dx_rom, rcp)\n'
        '        base = rlc_cache[rlc_rel]\n'
        '        for addr in dests:\n'
        '            t = (addr - 0x8000) // 16\n'
        '            if t < len(base):\n'
        '                base_orig, base_dx, base_src = base[t]\n'
        '            else:\n'
        '                base_orig, base_dx, base_src = bytes(16), bytes(16), ("zero",)\n'
        '            for j in range(frames):\n'
        '                lo_o = data[j * 8:(j + 1) * 8]\n'
        '                lo_d = dx_data[j * 8:(j + 1) * 8]\n'
        '                to = bytes(b for y in range(8) for b in (lo_o[y], base_orig[y * 2 + 1]))\n'
        '                td = bytes(b for y in range(8) for b in (lo_d[y], base_dx[y * 2 + 1]))\n'
        '                src = None\n'
        '                if pos >= 0 and base_src is not None:\n'
        '                    if base_src[0] == "rlc":\n'
        '                        src = ("water", pos, frames, j, base_src[1], base_src[2])\n'
        '                    elif base_src[0] == "zero":\n'
        '                        src = ("water", pos, frames, j, -1, 0)\n'
        '                out.append((to, td, src))\n'
        '    return out\n'
        '\n'
        '# ----------------------------------------------------------------------------\n'
        '# Execução da receita (o mesmo que o C++ faz no launcher)\n'
        '# ----------------------------------------------------------------------------\n'
        '\n'
        'def tile_from_source(src, orig_rom, rlc_bufs):\n'
        '    kind = src[0]\n'
        '    if kind == "rom":\n'
        '        return orig_rom[src[1]:src[1] + 16]\n'
        '    if kind == "rlc":\n'
        '        buf = rlc_bufs[src[1]]\n'
        '        return buf[src[2] * 16:src[2] * 16 + 16]\n'
        '    if kind == "water":\n'
        '        _, pos, frames, j, aid, t = src\n'
        '        out = bytearray(16)\n'
        '        for y in range(8):\n'
        '            out[y * 2] = orig_rom[pos + y * frames + j]\n'
        '            out[y * 2 + 1] = rlc_bufs[aid][t * 16 + y * 2 + 1] if aid >= 0 else 0\n'
        '        return bytes(out)\n'
        '    raise ValueError(src)\n'
        '\n'
        '\n'
        'def decompress_assets(rcp, orig_rom):\n'
        '    return [g.gfx_decomp_rlc(orig_rom[off:off + n]) for off, n in rcp.assets]\n'
        '\n'
        '# ----------------------------------------------------------------------------\n'
        '\n'
        'def build(orig_rom, dx_rom):\n'
        '    """Mesmas escolhas do main() de gerar_mod_original_bw.py.\n'
        '    Devolve (rcp, textures) com textures = {nome: (tipo, tile_original, reg)}."""\n'
        '    rcp = Recipe()\n'
        '    labels = g.read_gfx_labels()\n'
        '    levels = g.read_level_rooms(labels)\n'
        '\n'
        '    level_bgp = defaultdict(Counter)\n'
        '    for _, rooms in levels:\n'
        '        for rel, bgp in rooms:\n'
        '            level_bgp[rel][bgp] += 1\n'
        '\n'
        '    def asset_bgp(rel):\n'
        '        name = rel.rsplit("/", 1)[-1]\n'
        '        if name in g.PALETA_TELA:\n'
        '            return g.PALETA_TELA[name][0]\n'
        '        if rel in level_bgp:\n'
        '            return level_bgp[rel].most_common(1)[0][0]\n'
        '        return g.BGP_PADRAO\n'
        '\n'
        '    def asset_obp(rel):\n'
        '        return g.PALETA_TELA.get(rel.rsplit("/", 1)[-1], (None, g.OBP0_PADRAO))[1]\n'
        '\n'
        '    src_of = {}       # bytes do tile original -> primeira origem conhecida\n'
        '    pairs_of = {}\n'
        '    for f in sorted(g.GFX_DIR.rglob("*")):\n'
        '        rel = f.relative_to(g.GFX_DIR).as_posix()\n'
        '        if f.suffix not in (".bin", ".rlc") or rel.startswith("_custom/"):\n'
        '            continue\n'
        '        if "_anim" in rel and rel.startswith("maps/") and rel not in (\n'
        '                "maps/ssteacup_anim.bin", "maps/stovecanyon_anim.bin"):\n'
        '            continue\n'
        '        pairs_of[rel] = asset_pairs_src(rel, orig_rom, dx_rom, rcp)\n'
        '    pairs_of["maps/agua_1bpp"] = agua_pairs_src(orig_rom, dx_rom, rcp)\n'
        '    for pairs in pairs_of.values():\n'
        '        for ot, _, src in pairs:\n'
        '            if src is not None and ot not in src_of:\n'
        '                src_of[ot] = src\n'
        '\n'
        '    votes = defaultdict(Counter)\n'
        '    ctx_votes = defaultdict(lambda: defaultdict(Counter))\n'
        '\n'
        '    def vote(table, rel, i, orig_t, dx_t, bgp):\n'
        '        for kind in g.asset_kinds(rel, i):\n'
        '            reg = bgp if kind == "b" else asset_obp(rel)\n'
        '            table[(kind, g.fnv1a_64(dx_t))][(orig_t, reg)] += 1\n'
        '\n'
        '    for rel, pairs in pairs_of.items():\n'
        '        bgp = asset_bgp(rel)\n'
        '        for i, (ot, dt, _) in enumerate(pairs):\n'
        '            vote(votes, rel, i, ot, dt, bgp)\n'
        '        ctx = g.CONTEXTO_TELA.get(rel.rsplit("/", 1)[-1])\n'
        '        if ctx:\n'
        '            for i, (ot, dt, _) in enumerate(pairs):\n'
        '                vote(ctx_votes[ctx], rel, i, ot, dt, bgp)\n'
        '\n'
        '    for level_id, rooms in levels:\n'
        '        table = ctx_votes[f"f{level_id:02x}"]\n'
        '        for rel, bgp in rooms:\n'
        '            for ot, dt, _ in pairs_of.get(rel, []):\n'
        '                table[("b", g.fnv1a_64(dt))][(ot, bgp)] += 1\n'
        '\n'
        '    textures = {}\n'
        '    default = {}\n'
        '    for (kind, h), counter in votes.items():\n'
        '        render = counter.most_common(1)[0][0]\n'
        '        default[(kind, h)] = render\n'
        '        textures[f"{kind}_{h:016x}"] = (kind, h, "", render)\n'
        '    for ctx, table in ctx_votes.items():\n'
        '        for (kind, h), counter in table.items():\n'
        '            render = counter.most_common(1)[0][0]\n'
        '            if default.get((kind, h)) != render:\n'
        '                textures[f"{kind}_{h:016x}_{ctx}"] = (kind, h, ctx, render)\n'
        '\n'
        '    # Origem de cada tile usado; sem origem conhecida, procura os 16 bytes na ROM\n'
        '    missing = 0\n'
        '    for _, _, _, (tile, _) in textures.values():\n'
        '        if tile not in src_of:\n'
        '            p = orig_rom.find(tile)\n'
        '            if p >= 0:\n'
        '                src_of[tile] = ("rom", p)\n'
        '            else:\n'
        '                missing += 1\n'
        '    return rcp, textures, src_of, missing\n'
        '\n'
        '\n'
        'def emit_header(rcp, textures, src_of, orig_sha, dx_sha):\n'
        '    sources, source_index = [], {}\n'
        '    rows = []\n'
        '    for name in sorted(textures):\n'
        '        kind, h, ctx, (tile, reg) = textures[name]\n'
        '        src = src_of[tile]\n'
        '        if src not in source_index:\n'
        '            source_index[src] = len(sources)\n'
        '            sources.append(src)\n'
        '        rows.append((h, source_index[src], reg, kind, ctx))\n'
        '\n'
        '    def src_row(s):\n'
        '        if s[0] == "rom":\n'
        '            return (0, s[1], 0, 0, -1, 0)\n'
        '        if s[0] == "rlc":\n'
        '            return (1, 0, 0, 0, s[1], s[2])\n'
        '        _, pos, frames, j, aid, t = s\n'
        '        return (2, pos, frames, j, aid, t)\n'
        '\n'
        '    body = io.StringIO()\n'
        '    body.write("static const OriginalBwAsset kOriginalBwAssets[] = {\\n")\n'
        '    for off, n in rcp.assets:\n'
        '        body.write(f"    {{0x{off:05X}u, {n}u}},\\n")\n'
        '    body.write("};\\n\\n")\n'
        '    body.write("/* tipo (0 = ROM, 1 = .rlc, 2 = água 1bpp), offset, quadros, quadro, .rlc, tile */\\n")\n'
        '    body.write("static const OriginalBwSource kOriginalBwSources[] = {\\n")\n'
        '    for s in sources:\n'
        '        t, off, fr, j, aid, tile = src_row(s)\n'
        '        body.write(f"    {{{t}, {fr}, {j}, {aid}, {tile}, 0x{off:05X}u}},\\n")\n'
        '    body.write("};\\n\\n")\n'
        '    body.write("/* hash do tile do DX, origem, registrador BGP/OBP0, tipo, contexto */\\n")\n'
        '    body.write("static const OriginalBwTexture kOriginalBwTextures[] = {\\n")\n'
        '    for h, si, reg, kind, ctx in rows:\n'
        '        body.write(f"    {{0x{h:016X}ull, {si}, 0x{reg:02X}, \'{kind}\', \\"{ctx}\\"}},\\n")\n'
        '    body.write("};\\n")\n'
        '    text = body.getvalue()\n'
        '    version = (zlib.crc32((orig_sha + dx_sha + text).encode()) & 0x7FFFFFFF) or 1\n'
        '\n'
        '    head = f"""// Gerado por tools/gerar_receita_original_bw.py — não editar.\n'
        '// Receita das texturas P&B originais: só offsets na ROM original, hashes dos\n'
        '// tiles do DX e registradores de paleta (nenhum byte do jogo).\n'
        '#pragma once\n'
        '#include <cstdint>\n'
        '\n'
        '#define ORIGINAL_BW_RECIPE_VERSION {version}u\n'
        'static const char kOriginalBwOrigSha256[] = "{orig_sha}";\n'
        'static const char kOriginalBwDxSha256[] = "{dx_sha}";\n'
        '\n'
        'struct OriginalBwAsset {{ uint32_t offset; uint32_t length; }};\n'
        'struct OriginalBwSource {{ uint8_t type; uint8_t frames; uint8_t frame; int16_t asset; uint16_t tile; uint32_t offset; }};\n'
        'struct OriginalBwTexture {{ uint64_t hash; uint32_t source; uint8_t reg; char kind; char ctx[4]; }};\n'
        '\n'
        '"""\n'
        '    return head + text, version, len(sources)\n'
        '\n'
        '\n'
        'def png_bytes_from_recipe(textures, src_of, orig_rom, rcp):\n'
        '    """PNGs como o gerar_mod_original_bw.py faz, mas com o tile tirado da receita."""\n'
        '    bufs = decompress_assets(rcp, orig_rom)\n'
        '    out = {}\n'
        '    for name, (kind, _, _, (tile, reg)) in textures.items():\n'
        '        t = tile_from_source(src_of[tile], orig_rom, bufs)\n'
        '        if t != tile:\n'
        '            raise SystemExit(f"[ERRO] A receita não reproduz o tile de {name}")\n'
        '        out[f"{name}.png"] = g.make_png_8x8(g.decode_tile_pixels(t), g.dmg_palette(reg, kind == "o"))\n'
        '    return out\n'
        '\n'
        '\n'
        'def main():\n'
        '    p = argparse.ArgumentParser(description="Gera original_bw_receita.h a partir do disassembly.")\n'
        '    p.add_argument("orig_rom", nargs="?", default="base_WL-SM3.gb")\n'
        '    p.add_argument("dx_rom", nargs="?", default="wario.o2r", help="ROM do DX (.gb) ou wario.o2r")\n'
        '    p.add_argument("--conferir", action="store_true", help="confere tudo sem gravar o .h")\n'
        '    p.add_argument("--sem-comparar", action="store_true",\n'
        '                   help="não roda o gerar_mod_original_bw.py para comparar")\n'
        '    args = p.parse_args()\n'
        '\n'
        '    orig_rom = g.load_rom(args.orig_rom)\n'
        '    dx_rom = g.load_rom(args.dx_rom)\n'
        '    orig_sha = hashlib.sha256(orig_rom).hexdigest()\n'
        '    dx_sha = hashlib.sha256(dx_rom).hexdigest()\n'
        '    print(f"ROM original: {len(orig_rom)} bytes  sha256 {orig_sha}")\n'
        '    print(f"ROM do DX:    {len(dx_rom)} bytes  sha256 {dx_sha}")\n'
        '\n'
        '    rcp, textures, src_of, missing = build(orig_rom, dx_rom)\n'
        '    if missing:\n'
        '        print(f"[ERRO] {missing} tile(s) sem origem na ROM original. Nada foi gravado.")\n'
        '        return 1\n'
        '    pngs = png_bytes_from_recipe(textures, src_of, orig_rom, rcp)\n'
        '    header, version, n_sources = emit_header(rcp, textures, src_of, orig_sha, dx_sha)\n'
        '    print(f"[ok]   {len(textures)} texturas, {n_sources} origens, {len(rcp.assets)} .rlc; versão {version}")\n'
        '\n'
        '    if not args.sem_comparar:\n'
        '        with tempfile.TemporaryDirectory() as tmp:\n'
        '            ref = Path(tmp) / "ref.o2r"\n'
        '            cmd = [sys.executable, str(Path(g.__file__)), args.orig_rom, args.dx_rom, "-o", str(ref)]\n'
        '            r = subprocess.run(cmd, capture_output=True, text=True)\n'
        '            if r.returncode != 0:\n'
        '                print(r.stdout + r.stderr)\n'
        '                print("[ERRO] gerar_mod_original_bw.py falhou. Nada foi gravado.")\n'
        '                return 1\n'
        '            with zipfile.ZipFile(ref) as z:\n'
        '                ref_files = {n: z.read(n) for n in z.namelist()}\n'
        '        if ref_files != pngs:\n'
        '            only_ref = sorted(set(ref_files) - set(pngs))[:5]\n'
        '            only_rcp = sorted(set(pngs) - set(ref_files))[:5]\n'
        '            diff = [n for n in pngs if n in ref_files and ref_files[n] != pngs[n]][:5]\n'
        '            print(f"[ERRO] A receita não bate com o gerar_mod_original_bw.py "\n'
        '                  f"(só no mod: {only_ref}, só na receita: {only_rcp}, diferentes: {diff}).")\n'
        '            return 1\n'
        '        print(f"[ok]   Igual ao gerar_mod_original_bw.py: {len(pngs)} PNGs, mesmos nomes e mesmos bytes.")\n'
        '\n'
        '    if args.conferir:\n'
        '        print(f"Conferência: nada foi gravado. Rode sem --conferir para gravar {HEADER.name}.")\n'
        '        return 0\n'
        '    old = HEADER.read_text(encoding="utf-8") if HEADER.exists() else None\n'
        '    if old == header:\n'
        '        print(f"[ok]   {HEADER.name} já estava igual.")\n'
        '    else:\n'
        '        HEADER.write_text(header, encoding="utf-8", newline="\\n")\n'
        '        print(f"[ok]   Gravado {HEADER} ({len(header) // 1024} KB).")\n'
        '    return 0\n'
        '\n'
        '\n'
        'if __name__ == "__main__":\n'
        '    sys.exit(main())\n'
    ),
}


def read_text(path):
    raw = path.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    return raw.replace("\r\n", "\n"), crlf


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def plan(text, edits):
    changes, errors, done = [], [], 0
    for i, (old, new) in enumerate(edits, 1):
        n_old = text.count(old)
        if n_old == 1:
            pos = text.index(old)
            same = 0
            for a, b in zip(old.splitlines(), new.splitlines()):
                if a != b:
                    break
                same += 1
            changes.append((line_of(text, pos) + same, old, new))
            text = text.replace(old, new, 1)
        elif text.count(new) == 1:
            done += 1
        else:
            first = next((l for l in old.splitlines() if l.strip()), old[:60])
            what = "não encontrado" if n_old == 0 else f"encontrado {n_old} vezes"
            errors.append(f"  - trecho {i} {what}: {first.strip()!r}")
    return text, changes, errors, done


def show(name, changes):
    for line, old, new in changes:
        print(f"\n--- {name}, por volta da linha {line}")
        diff = difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm="", n=1)
        for d in list(diff)[2:]:
            if d.startswith("@@"):
                continue
            print("    " + d)


def main():
    check_only = "--conferir" in sys.argv or "--check" in sys.argv
    root = Path(__file__).resolve().parent.parent
    results, created, failed = [], [], False

    for rel, content in NEW_FILES.items():
        path = root / rel
        if path.exists():
            text, _ = read_text(path)
            if text == content:
                print(f"[ok]   {rel}: já existe (igual)")
            else:
                print(f"[ERRO] {rel} já existe e é diferente; renomeie ou apague antes de aplicar")
                failed = True
            continue
        print(f"[ok]   {rel}: arquivo novo ({content.count(chr(10))} linhas)")
        created.append((path, content))

    for rel, edits in EDITS.items():
        path = root / rel
        if not path.exists():
            print(f"[ERRO] {rel} não existe")
            failed = True
            continue
        text, crlf = read_text(path)
        new_text, changes, errors, done = plan(text, edits)
        if errors:
            print(f"[ERRO] {rel}:\n" + "\n".join(errors))
            failed = True
            continue
        if not changes:
            print(f"[ok]   {rel}: já estava aplicado")
            continue
        extra = f" ({done} já aplicados)" if done else ""
        print(f"[ok]   {rel}: {len(changes)} trecho(s) para mudar{extra}")
        if check_only:
            show(rel, changes)
            print()
        results.append((path, new_text, crlf))

    if failed:
        print("\nNada foi alterado.")
        return 1
    if check_only:
        print("Conferência: nada foi gravado. Rode sem --conferir para aplicar.")
        return 0
    for path, content in created:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode("utf-8"))
        print(f"[ok]   {path.name}: criado")
    for path, new_text, crlf in results:
        backup = path.with_suffix(path.suffix + ".orig")
        if not backup.exists():
            backup.write_bytes(path.read_bytes())
        if crlf:
            new_text = new_text.replace("\n", "\r\n")
        path.write_bytes(new_text.encode("utf-8"))
        print(f"[ok]   {path.name}: gravado (cópia do original em {backup.name})")
    if not results and not created:
        print("Nada a fazer.")
    else:
        print("\nPróximo passo: python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r")
    return 0


if __name__ == "__main__":
    sys.exit(main())
