#!/usr/bin/env python3
"""
Aplica as mudanças do port Android em main.cpp e runtime/src/platform_sdl.cpp.

Todas as mudanças ficam atrás de `#ifdef __ANDROID__` ou não mudam nada no
desktop (o gamepad na tela começa desligado fora do Android).

Uso (na pasta projeto/):
    python3 tools/aplicar_port_android.py            # aplica
    python3 tools/aplicar_port_android.py --check    # só confere se dá para aplicar
    python3 tools/aplicar_port_android.py --reverter-audio [--check]
        # desfaz as mudanças de áudio/ritmo das versões v2 a v5

Cria um .orig de cada arquivo antes de alterar. Se já foi aplicado, não faz nada.
Se algum trecho de referência não for encontrado (você mexeu no arquivo), para
sem alterar nada e diz qual trecho faltou.
"""
import sys
from pathlib import Path

MARKER = "[android-port]"

# ---------------------------------------------------------------------------
# main.cpp
# ---------------------------------------------------------------------------
MAIN_EDITS = [
    # 1. include da ponte Android
    (
        '#include "gbrt.h"\n',
        '#include "gbrt.h"\n'
        '#include "android_bridge.h"   /* [android-port] */\n',
    ),
    # 2. textos do launcher que falam em "arrastar" (não existe no Android)
    (
        "namespace launcher {\n",
        "namespace launcher {\n"
        "\n"
        "/* [android-port] textos que mudam por plataforma */\n"
        "#ifdef __ANDROID__\n"
        "#define LAUNCHER_HINT_EMPTY \"Tap the button to choose the file.\"\n"
        "#define LAUNCHER_HINT_READY \"Tap LOAD & PLAY.\"\n"
        "#define LAUNCHER_HINT_TIP   \"The files are copied into the app's own storage.\"\n"
        "#else\n"
        "#define LAUNCHER_HINT_EMPTY \"Or drag the file into this window.\"\n"
        "#define LAUNCHER_HINT_READY \"Click LOAD & PLAY or press Enter.\"\n"
        "#define LAUNCHER_HINT_TIP   \"Tip: drag the ROM and the patch straight into this window.\"\n"
        "#endif\n",
    ),
    (
        '        "Browse ROM",\n        "Or drag the file into this window.",\n',
        '        "Browse ROM",\n        LAUNCHER_HINT_EMPTY,\n',
    ),
    (
        '        "Browse Patch",\n        "Or drag the file into this window.",\n',
        '        "Browse Patch",\n        LAUNCHER_HINT_EMPTY,\n',
    ),
    (
        '                    can_play ? "Click LOAD & PLAY or press Enter."\n'
        '                             : "Tip: drag the ROM and the patch straight into this window.");\n',
        '                    can_play ? LAUNCHER_HINT_READY\n'
        '                             : LAUNCHER_HINT_TIP);\n',
    ),
    # 3. botões "Browse": no Android abrem o seletor do sistema (assíncrono)
    (
        "            const fs::path p = open_file_dialog(window, rom_dialog);\n"
        "            if (!p.empty()) rom.set(p, EXPECTED_ROM_SHA256, now());\n",
        "#ifdef __ANDROID__   /* [android-port] */\n"
        "            android_bridge::request_file_pick(android_bridge::PICK_ROM);\n"
        "#else\n"
        "            const fs::path p = open_file_dialog(window, rom_dialog);\n"
        "            if (!p.empty()) rom.set(p, EXPECTED_ROM_SHA256, now());\n"
        "#endif\n",
    ),
    (
        "            const fs::path p = open_file_dialog(window, patch_dialog);\n"
        "            if (!p.empty()) patch.set(p, EXPECTED_PATCH_SHA256, now());\n",
        "#ifdef __ANDROID__   /* [android-port] */\n"
        "            android_bridge::request_file_pick(android_bridge::PICK_PATCH);\n"
        "#else\n"
        "            const fs::path p = open_file_dialog(window, patch_dialog);\n"
        "            if (!p.empty()) patch.set(p, EXPECTED_PATCH_SHA256, now());\n"
        "#endif\n",
    ),
    # 4. tamanho lógico do launcher (layout de 960x760 escalado para a tela)
    (
        "    FileSlot rom, patch;\n",
        "    FileSlot rom, patch;\n"
        "#ifdef __ANDROID__   /* [android-port] */\n"
        "    int logical_w = 0, logical_h = 0;\n"
        "    (void)rom_dialog; (void)patch_dialog;\n"
        "#endif\n",
    ),
    (
        "        if (done) break;\n"
        "\n"
        "        ImGui_ImplSDLRenderer2_NewFrame();\n"
        "        ImGui_ImplSDL2_NewFrame();\n",
        "        if (done) break;\n"
        "\n"
        "#ifdef __ANDROID__   /* [android-port] */\n"
        "        {\n"
        "            // Resposta do seletor de arquivos: decide ROM/patch pelo hash, como no drag-and-drop.\n"
        "            int picked_kind = 0;\n"
        "            std::string picked;\n"
        "            if (android_bridge::poll_picked_file(picked_kind, picked) && !picked.empty() && modal_msg.empty())\n"
        "                handle_drop(path_from_utf8(picked));\n"
        "\n"
        "            // O layout foi feito para 960x760. Renderiza nessa escala lógica (altura fixa,\n"
        "            // largura acompanha a proporção da tela) e o SDL amplia; toques são convertidos.\n"
        "            int out_w = 0, out_h = 0;\n"
        "            SDL_GetRendererOutputSize(renderer, &out_w, &out_h);\n"
        "            if (out_w > 0 && out_h > 0) {\n"
        "                int lw = (int)((float)WINDOW_H * (float)out_w / (float)out_h);\n"
        "                int lh = WINDOW_H;\n"
        "                if (lw < WINDOW_W) {   // tela em retrato: cresce na altura\n"
        "                    lw = WINDOW_W;\n"
        "                    lh = (int)((float)WINDOW_W * (float)out_h / (float)out_w);\n"
        "                }\n"
        "                if (lw != logical_w || lh != logical_h) {\n"
        "                    logical_w = lw;\n"
        "                    logical_h = lh;\n"
        "                    SDL_RenderSetLogicalSize(renderer, logical_w, logical_h);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "#endif\n"
        "\n"
        "        ImGui_ImplSDLRenderer2_NewFrame();\n"
        "        ImGui_ImplSDL2_NewFrame();\n"
        "#ifdef __ANDROID__   /* [android-port] */\n"
        "        if (logical_w > 0) {\n"
        "            io.DisplaySize = ImVec2((float)logical_w, (float)logical_h);\n"
        "            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);\n"
        "        }\n"
        "#endif\n",
    ),
    # 5. no Android o SDL continua iniciado entre o launcher e o jogo
    (
        "    SDL_DestroyRenderer(renderer);\n"
        "    SDL_DestroyWindow(window);\n"
        "    SDL_Quit();\n"
        "\n"
        "    return ready_to_launch;\n",
        "    SDL_DestroyRenderer(renderer);\n"
        "    SDL_DestroyWindow(window);\n"
        "#ifndef __ANDROID__   /* [android-port] no Android o SDL segue vivo até o fim do app */\n"
        "    SDL_Quit();\n"
        "#endif\n"
        "\n"
        "    return ready_to_launch;\n",
    ),
    # 6. main() vira game_main(); o novo main() prepara o Android e encerra o processo
    (
        "int main(int argc, char* argv[]) {\n"
        "    // ---------------------------------------------------------------------\n"
        "    // 1. Fast path",
        "static int game_main(int argc, char* argv[]) {   /* [android-port] corpo original do main() */\n"
        "    // ---------------------------------------------------------------------\n"
        "    // 1. Fast path",
    ),
]

MAIN_APPEND = """
/* [android-port] */
int main(int argc, char* argv[]) {
#ifdef __ANDROID__
    // Pasta de trabalho, manifesto do patch (assets do APK) e log em arquivo.
    android_bridge::prepare_storage();
#endif
    const int rc = game_main(argc, argv);
#ifdef __ANDROID__
    // No Android a .so continua carregada depois que o SDL_main retorna; se o
    // usuário reabrir o app no mesmo processo, todos os "static" estariam sujos.
    // Encerrar o processo garante um início limpo.
    fflush(stdout);
    fflush(stderr);
    std::exit(rc);
#endif
    return rc;
}
"""

# ---------------------------------------------------------------------------
# runtime/src/platform_sdl.cpp
# ---------------------------------------------------------------------------
PLATFORM_EDITS = [
    (
        '#include "post_processing.h"\n',
        '#include "post_processing.h"\n'
        '#include "touch_controls.h"   /* [android-port] */\n'
        '#include "android_bridge.h"\n',
    ),
    # gamepad na tela soma com teclado e controle
    (
        "static bool input_action_is_pressed(GBInputAction action) {\n",
        "static bool input_action_is_pressed(GBInputAction action) {\n"
        "    /* [android-port] gamepad na tela */\n"
        "    if (!g_show_menu && (int)action < GB_JOYPAD_ACTION_COUNT && touch_controls::pressed((int)action)) {\n"
        "        return true;\n"
        "    }\n",
    ),
    # eventos de toque
    (
        "        case SDL_CONTROLLERAXISMOTION: {\n"
        "            uint8_t previous_dpad = g_joypad_dpad;\n",
        "        case SDL_FINGERDOWN:      /* [android-port] gamepad na tela */\n"
        "        case SDL_FINGERMOTION:\n"
        "        case SDL_FINGERUP: {\n"
        "            uint8_t previous_dpad = g_joypad_dpad;\n"
        "            uint8_t previous_buttons = g_joypad_buttons;\n"
        "            if (!touch_controls::handle_event(*event, g_window, g_show_menu)) {\n"
        "                break;\n"
        "            }\n"
        "            if (touch_controls::take_menu_request()) {\n"
        "                g_show_menu = !g_show_menu;\n"
        "                touch_controls::reset();\n"
        "            }\n"
        "            update_effective_joypad_state();\n"
        "            const bool port_captured = dispatch_captured_port_input(\n"
        "                ctx, previous_dpad, previous_buttons);\n"
        "            if (ctx && !port_captured &&\n"
        "                input_transition_creates_press(previous_dpad,\n"
        "                                              previous_buttons,\n"
        "                                              g_joypad_dpad,\n"
        "                                              g_joypad_buttons,\n"
        "                                              dpad_selected,\n"
        "                                              buttons_selected)) {\n"
        "                request_joypad_interrupt(ctx);\n"
        "            }\n"
        "            break;\n"
        "        }\n"
        "\n"
        "        case SDL_CONTROLLERAXISMOTION: {\n"
        "            uint8_t previous_dpad = g_joypad_dpad;\n",
    ),
    # ao ir para segundo plano, solta os dedos
    (
        "        case SDL_APP_WILLENTERBACKGROUND:\n"
        "            set_app_suspended(true);\n",
        "        case SDL_APP_WILLENTERBACKGROUND:\n"
        "            touch_controls::reset();   /* [android-port] */\n"
        "            update_effective_joypad_state();\n"
        "            set_app_suspended(true);\n",
    ),
    # desenho da sobreposição
    (
        "    if (g_port_frame_valid &&\n",
        "    if (!g_show_menu) {   /* [android-port] gamepad na tela */\n"
        "        touch_controls::draw(ImGui::GetForegroundDrawList(),\n"
        "                             imgui_io.DisplaySize.x,\n"
        "                             imgui_io.DisplaySize.y,\n"
        "                             g_controller != NULL);\n"
        "    }\n"
        "\n"
        "    if (g_port_frame_valid &&\n",
    ),
    # pasta de mods visível para o usuário no Android
    (
        "        char* hd_base = SDL_GetBasePath();\n"
        "        hd_pack_io_set_base_dir(hd_base ? hd_base : \"\");\n",
        "#if defined(__ANDROID__)   /* [android-port] Android/data/<pacote>/files/ */\n"
        "        char* hd_base = NULL;\n"
        "        hd_pack_io_set_base_dir(android_bridge::user_dir().c_str());\n"
        "#else\n"
        "        char* hd_base = SDL_GetBasePath();\n"
        "        hd_pack_io_set_base_dir(hd_base ? hd_base : \"\");\n"
        "#endif\n",
    ),
    # preferências
    (
        "    set_default_input_bindings();\n"
        "    hd_pack_io_clear_disabled_names();\n",
        "    set_default_input_bindings();\n"
        "    touch_controls::set_enabled(touch_controls::enabled_by_default());   /* [android-port] */\n"
        "    hd_pack_io_clear_disabled_names();\n",
    ),
    (
        "                if (strcmp(key, \"display.hd_tiles\") == 0) {\n",
        "                if (strcmp(key, \"input.touch_controls\") == 0) {   /* [android-port] */\n"
        "                    touch_controls::set_enabled(strcmp(value, \"0\") != 0);\n"
        "                    continue;\n"
        "                }\n"
        "                if (strcmp(key, \"display.hd_tiles\") == 0) {\n",
    ),
    (
        "    fprintf(file, \"display.hd_tiles=%d\\n\", ppu_native_hd_get_tiles_enabled() ? 1 : 0);\n",
        "    fprintf(file, \"display.hd_tiles=%d\\n\", ppu_native_hd_get_tiles_enabled() ? 1 : 0);\n"
        "    fprintf(file, \"input.touch_controls=%d\\n\", touch_controls::enabled() ? 1 : 0);   /* [android-port] */\n",
    ),
    # opção no menu
    (
        "        if (ImGui::Button(\"Reset Controls\")) {\n",
        "        {   /* [android-port] */\n"
        "            bool touch_on = touch_controls::enabled();\n"
        "            if (ImGui::Checkbox(\"Touch Controls (on-screen gamepad)\", &touch_on)) {\n"
        "                touch_controls::set_enabled(touch_on);\n"
        "                update_effective_joypad_state();\n"
        "                save_runtime_preferences();\n"
        "            }\n"
        "        }\n"
        "        if (ImGui::Button(\"Reset Controls\")) {\n",
    ),
]


# ---------------------------------------------------------------------------
# v2: ritmo do jogo/áudio e widescreen ligado por padrão (todas as plataformas)
# ---------------------------------------------------------------------------
MARKER_V2 = "[fix-v2]"

PLATFORM_EDITS_V2 = [
    # Widescreen ligado ao abrir. A chave da preferência muda para que um
    # "display.widescreen=0" antigo, salvo automaticamente, não desligue.
    (
        "static bool g_widescreen_mode = false;\n",
        "static bool g_widescreen_mode = true;   /* [fix-v2] widescreen ligado por padrão */\n",
    ),
    (
        "                if (strcmp(key, \"display.widescreen\") == 0) {\n",
        "                if (strcmp(key, \"display.widescreen_v2\") == 0) {   /* [fix-v2] */\n",
    ),
    (
        "    fprintf(file, \"display.widescreen=%d\\n\", g_widescreen_mode ? 1 : 0);\n",
        "    fprintf(file, \"display.widescreen_v2=%d\\n\", g_widescreen_mode ? 1 : 0);   /* [fix-v2] */\n",
    ),
    # Buffer de áudio maior: no Android o dispositivo puxa blocos grandes
    # (AudioTrack), que não cabiam com folga em 16384 amostras.
    (
        "#define AUDIO_RING_SIZE 16384  /* ~370ms buffer - plenty of headroom */\n",
        "#define AUDIO_RING_SIZE 65536  /* [fix-v2] ~1,5 s; cabe o bloco grande do Android */\n",
    ),
    # Com áudio "faminto", antes o jogo rodava sem limite nenhum. Agora acelera
    # no máximo 25% até reabastecer.
    (
        "    uint64_t target_frame_time = next_frame_time;\n"
        "\n"
        "    if (!audio_starved && now < target_frame_time) {\n",
        "    /* [fix-v2] O piso (\"low watermark\") podia ficar maior que o ponto de partida e\n"
        "     * até maior que o buffer inteiro: o áudio parecia sempre \"faminto\" e o ritmo\n"
        "     * era desligado (jogo a 400 fps, som picotado). Aqui o piso é limitado. */\n"
        "    if (audio_starved && g_audio_low_watermark >= g_audio_start_threshold) {\n"
        "        audio_starved = audio_ring_fill_samples() < g_audio_start_threshold / 2;\n"
        "    }\n"
        "    if (audio_starved) {\n"
        "        /* [fix-v2] reabastece o áudio rodando até ~25% mais rápido, nunca sem limite */\n"
        "        next_frame_time -= frame_ticks / 5;\n"
        "    }\n"
        "    uint64_t target_frame_time = next_frame_time;\n"
        "\n"
        "    if (now < target_frame_time) {\n",
    ),
]


# ---------------------------------------------------------------------------
# v3: ícone da janela (Linux/Windows) e diagnóstico de áudio no log
# ---------------------------------------------------------------------------
MARKER_V3 = "[fix-v3]"

MAIN_EDITS_V3 = [
    (
        '#include "android_bridge.h"   /* [android-port] */\n',
        '#include "android_bridge.h"   /* [android-port] */\n'
        '#include "app_icon.h"         /* [fix-v3] */\n',
    ),
    (
        "    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);\n",
        "    app_icon_apply(window);   /* [fix-v3] ícone da janela do launcher */\n"
        "    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);\n",
    ),
]

PLATFORM_EDITS_V3 = [
    (
        '#include "android_bridge.h"\n',
        '#include "android_bridge.h"\n'
        '#include "app_icon.h"   /* [fix-v3] */\n',
    ),
    (
        '    fprintf(stderr, "[SDL] Window created.\\n");\n',
        '    fprintf(stderr, "[SDL] Window created.\\n");\n'
        '    app_icon_apply(g_window);   /* [fix-v3] ícone da janela do jogo */\n',
    ),
    (
        "    if (audio_starved) {\n"
        "        /* [fix-v2] reabastece",
        "    {   /* [fix-v3] diagnóstico de áudio no log, a cada 5 segundos */\n"
        "        static uint64_t diag_t0 = 0, diag_written0 = 0, diag_under0 = 0;\n"
        "        static uint32_t diag_fill0 = 0, diag_frames = 0, diag_starved = 0;\n"
        "        const uint64_t diag_now = SDL_GetPerformanceCounter();\n"
        "        diag_frames++;\n"
        "        if (audio_starved) diag_starved++;\n"
        "        const double diag_secs = diag_t0 ? (double)(diag_now - diag_t0) / (double)freq : 0.0;\n"
        "        if (diag_t0 == 0 || diag_secs >= 5.0) {\n"
        "            const uint32_t fill = audio_ring_fill_samples();\n"
        "            const uint64_t under = g_audio_underruns.load(std::memory_order_relaxed);\n"
        "            if (diag_t0 != 0) {\n"
        "                const double produced = (double)(g_audio_samples_written - diag_written0);\n"
        "                const double consumed = produced - ((double)fill - (double)diag_fill0);\n"
        "                fprintf(stderr,\n"
        "                        \"[AUDIO] driver=%s dev=%uHz bloco=%u | jogo %.1f fps (vel %d%%) | \"\n"
        "                        \"gerado %.0f/s lido %.0f/s | buffer %u piso %u partida %u | \"\n"
        "                        \"faminto %u/%u frames | underruns +%llu\\n\",\n"
        "                        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : \"?\",\n"
        "                        g_audio_device_sample_rate, g_audio_device_buffer_samples,\n"
        "                        diag_frames / diag_secs, effective_speed_percent(),\n"
        "                        produced / diag_secs, consumed / diag_secs,\n"
        "                        fill, g_audio_low_watermark, g_audio_start_threshold,\n"
        "                        diag_starved, diag_frames,\n"
        "                        (unsigned long long)(under - diag_under0));\n"
        "            }\n"
        "            diag_t0 = diag_now;\n"
        "            diag_written0 = g_audio_samples_written;\n"
        "            diag_under0 = under;\n"
        "            diag_fill0 = fill;\n"
        "            diag_frames = 0;\n"
        "            diag_starved = 0;\n"
        "        }\n"
        "    }\n"
        "    if (audio_starved) {\n"
        "        /* [fix-v2] reabastece",
    ),
]


# ---------------------------------------------------------------------------
# v4: buffer de áudio maior só no Android (no PC o tamanho original funcionava)
# ---------------------------------------------------------------------------
MARKER_V4 = "[fix-v4]"

PLATFORM_EDITS_V4 = [
    (
        "#define AUDIO_RING_SIZE 65536  /* [fix-v2] ~1,5 s; cabe o bloco grande do Android */\n",
        "#if defined(__ANDROID__)   /* [fix-v4] buffer maior só no Android */\n"
        "#define AUDIO_RING_SIZE 65536  /* ~1,5 s */\n"
        "#else\n"
        "#define AUDIO_RING_SIZE 16384  /* ~370ms buffer - plenty of headroom */\n"
        "#endif\n",
    ),
]


# ---------------------------------------------------------------------------
# v5: com áudio faltando, acelera no máximo 2% (antes 25%): a velocidade do
# jogo fica presa ao relógio; falta de som vira estalo, não jogo acelerado.
# ---------------------------------------------------------------------------
MARKER_V5 = "[fix-v5]"

PLATFORM_EDITS_V5 = [
    (
        "        /* [fix-v2] reabastece o áudio rodando até ~25% mais rápido, nunca sem limite */\n"
        "        next_frame_time -= frame_ticks / 5;\n",
        "        /* [fix-v5] reabastece o áudio rodando no máximo ~2% mais rápido */\n"
        "        next_frame_time -= frame_ticks / 50;\n",
    ),
]


# ---------------------------------------------------------------------------
# v6: o áudio nunca mais acelera o jogo. A velocidade segue só o relógio;
# se o dispositivo de som pedir mais do que o jogo gera, vira estalo.
# ---------------------------------------------------------------------------
MARKER_V6 = "[fix-v6]"

PLATFORM_EDITS_V6 = [
    (
        "        /* [fix-v5] reabastece o áudio rodando no máximo ~2% mais rápido */\n"
        "        next_frame_time -= frame_ticks / 50;\n",
        "        /* [fix-v6] não acelera mais o jogo por falta de áudio: o ritmo segue só o relógio */\n",
    ),
]


# ---------------------------------------------------------------------------
# v7: pasta de saves também no PC (<pasta do executável>/saves).
# O main() passa a chamar android_bridge::prepare_storage() em todas as
# plataformas; no PC ela só cria a pasta saves ao lado do executável.
# ---------------------------------------------------------------------------
MARKER_V7 = "[fix-v7]"

MAIN_EDITS_V7 = [
    (
        "int main(int argc, char* argv[]) {\n"
        "#ifdef __ANDROID__\n"
        "    // Pasta de trabalho, manifesto do patch (assets do APK) e log em arquivo.\n"
        "    android_bridge::prepare_storage();\n"
        "#endif\n",
        "int main(int argc, char* argv[]) {\n"
        "    /* [fix-v7] Android: pasta de trabalho, manifesto, log e saves.\n"
        "     *          PC: pasta saves ao lado do executável. */\n"
        "    android_bridge::prepare_storage();\n",
    ),
]


# ---------------------------------------------------------------------------
# Desfazer as tentativas de correção de áudio (v2 áudio, v3 diagnóstico, v4, v5).
# O problema de som do Android era o driver AAudio do SDL 2.30.0, resolvido
# no android_bridge.cpp; o código de áudio/ritmo do platform_sdl.cpp volta a
# ser exatamente o que era antes do port.
# ---------------------------------------------------------------------------
AUDIO_REVERT_STEPS = [
    (MARKER_V6, PLATFORM_EDITS_V6[0]),
    # (marcador que indica que a etapa foi aplicada, (old, new) da etapa)
    (MARKER_V5, PLATFORM_EDITS_V5[0]),
    (MARKER_V4, PLATFORM_EDITS_V4[0]),
    ("[fix-v3] diagnóstico de áudio", PLATFORM_EDITS_V3[2]),
    ("[fix-v2] O piso", PLATFORM_EDITS_V2[4]),
    ("[fix-v2] ~1,5 s", PLATFORM_EDITS_V2[3]),
]


def revert_audio(text):
    """Devolve (texto, desfeitas, erro)."""
    done = 0
    for marker, (old, new) in AUDIO_REVERT_STEPS:
        if marker not in text:
            continue
        if text.count(new) != 1:
            first = new.strip().splitlines()[0]
            return None, done, f"  - trecho aplicado não encontrado como esperado: {first!r}"
        text = text.replace(new, old, 1)
        done += 1
    return text, done, None


def main_revert(check_only):
    root = Path(__file__).resolve().parent.parent
    path = root / "runtime" / "src" / "platform_sdl.cpp"
    if not path.exists():
        print(f"[ERRO] {path} não existe")
        return 1
    original = path.read_text(encoding="utf-8")
    text, done, err = revert_audio(original)
    if err:
        print(f"[ERRO] platform_sdl.cpp:\n{err}\n\nNada foi alterado.")
        return 1
    if done == 0:
        print("[ok]   platform_sdl.cpp: nenhuma mudança de áudio para desfazer")
        return 0
    if check_only:
        print(f"[ok]   platform_sdl.cpp: {done} mudança(s) de áudio podem ser desfeitas")
        return 0
    backup = path.with_suffix(path.suffix + ".antes-reverter")
    backup.write_bytes(path.read_bytes())
    path.write_text(text, encoding="utf-8")
    print(f"[ok]   platform_sdl.cpp: {done} mudança(s) de áudio desfeitas (cópia em {backup.name})")
    return 0


def plan_text(text, edits, append, marker):
    if marker in text:
        return text, "já aplicado"
    missing = []
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            missing.append((old, n))
            continue
        text = text.replace(old, new, 1)
    if missing:
        msg = []
        for old, n in missing:
            first = old.strip().splitlines()[0]
            msg.append(f"  - trecho {'não encontrado' if n == 0 else f'encontrado {n} vezes'}: {first!r}")
        return None, "\n".join(msg)
    return text + append, None


def main():
    check_only = "--check" in sys.argv
    if "--reverter-audio" in sys.argv:
        return main_revert(check_only)
    root = Path(__file__).resolve().parent.parent
    # (arquivo, [(nome do grupo, marcador, edições, texto no fim)])
    targets = [
        (root / "main.cpp", [
            ("android", MARKER, MAIN_EDITS, MAIN_APPEND),
            ("ícone da janela", MARKER_V3, MAIN_EDITS_V3, ""),
            ("pasta de saves", MARKER_V7, MAIN_EDITS_V7, ""),
        ]),
        (root / "runtime" / "src" / "platform_sdl.cpp", [
            ("android", MARKER, PLATFORM_EDITS, ""),
            ("widescreen padrão", MARKER_V2, PLATFORM_EDITS_V2[:3], ""),
            ("ícone da janela", MARKER_V3, PLATFORM_EDITS_V3[:2], ""),
        ]),
    ]
    results = []
    failed = False
    for path, groups in targets:
        if not path.exists():
            print(f"[ERRO] {path} não existe")
            failed = True
            continue
        original = path.read_text(encoding="utf-8")
        text = original
        for name, marker, edits, append in groups:
            new_text, err = plan_text(text, edits, append, marker)
            if err == "já aplicado":
                print(f"[ok]   {path.name} ({name}): já estava aplicado")
            elif err:
                print(f"[ERRO] {path.name} ({name}):\n{err}")
                failed = True
                break
            else:
                print(f"[ok]   {path.name} ({name}): pode ser aplicado")
                text = new_text
        if text != original:
            results.append((path, text))
    if failed:
        print("\nNada foi alterado.")
        return 1
    if check_only:
        return 0
    for path, new_text in results:
        backup = path.with_suffix(path.suffix + ".orig")
        if not backup.exists():
            backup.write_bytes(path.read_bytes())
        path.write_text(new_text, encoding="utf-8")
        print(f"[ok]   {path.name}: gravado (cópia do original em {backup.name})")
    if not results:
        print("Nada a fazer.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
