/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "gbrt_port.h"
#include "ppu.h"
#include "ppu_native.h"
#include "hd_pack_io.h"
#include "gabarito.h"   /* passo 12a */
#include "audio_stats.h"
#include "audio.h" /* passo 6c.1 */
#include "music_replace.h" /* passo 6c */
#include "gbrt_debug.h"
#include "post_processing.h"
#include "touch_controls.h"   /* [android-port] */
#include "android_bridge.h"
#include "app_icon.h"   /* [fix-v3] */

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include <atomic>
#include <cmath>   /* passo 14p: fabsf */
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <map>   /* passo 3D-2b/2f */
#include "marcas3d_padrao.h"   /* passo 3D-2q */
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef GBRT_ENABLE_TEST_HOOKS
#include <thread>
#endif
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"   /* passo 14p: rolar a lista arrastando */
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "backends/imgui_impl_opengl3.h"   /* passo 14a */
#include "video_gl.h"                       /* passo 14a */
#include "gpu_ppu.h"                        /* passo 14b */


namespace fs = std::filesystem;

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;

/* ============================================================================
 * Passo 14a: vídeo por SDL_Renderer (como sempre foi) ou por OpenGL.
 * O quadro continua montado na CPU; as funções vid_* levam as texturas à tela
 * por um dos dois caminhos. A escolha vale ao abrir o jogo (preferência
 * display.video_backend); se o OpenGL não abrir, volta sozinho para o SDL.
 * ========================================================================== */
struct VidTex {
    SDL_Texture* sdl = nullptr;
    VglTex* gl = nullptr;
};
static bool g_use_gl = false;            /* caminho em uso nesta execução */
static int g_video_backend_pref = 1;     /* 0 = SDL_Renderer, 1 = OpenGL (próxima abertura); OpenGL padrão (passo 14p) */
static std::string g_video_status;       /* o que abriu (menu) */

static bool g_game_reset_pending = false;   /* passo 14p: botão Reset do menu */

static bool vid_ready(void) {
    return g_use_gl ? vgl_ready() : g_renderer != NULL;
}

static VidTex* vid_tex_create(int w, int h) {
    VidTex* t = new VidTex();
    if (g_use_gl) {
        t->gl = vgl_tex_create(w, h);
    } else if (g_renderer) {
        t->sdl = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    if (!t->gl && !t->sdl) {
        delete t;
        return NULL;
    }
    return t;
}

static void vid_tex_destroy(VidTex* t) {
    if (!t) return;
    if (t->sdl) SDL_DestroyTexture(t->sdl);
    if (t->gl) vgl_tex_destroy(t->gl);
    delete t;
}

static void vid_tex_set_linear(VidTex* t, bool linear) {
    if (!t) return;
    if (t->gl) {
        vgl_tex_set_linear(t->gl, linear);
        return;
    }
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(t->sdl, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
}

static void vid_tex_set_blend(VidTex* t, bool blend) {
    if (!t) return;
    if (t->gl) vgl_tex_set_blend(t->gl, blend);
    else SDL_SetTextureBlendMode(t->sdl, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
}

static void vid_tex_lock(VidTex* t, void** pixels, int* pitch) {
    if (t->gl) *pixels = vgl_tex_lock(t->gl, pitch);
    else SDL_LockTexture(t->sdl, NULL, pixels, pitch);
}

static void vid_tex_unlock(VidTex* t) {
    if (t->gl) vgl_tex_unlock(t->gl);
    else SDL_UnlockTexture(t->sdl);
}

static void vid_tex_update(VidTex* t, const SDL_Rect* rect, const void* pixels, int pitch) {
    if (!t) return;
    if (t->gl) vgl_tex_update(t->gl, rect, pixels, pitch);
    else SDL_UpdateTexture(t->sdl, rect, pixels, pitch);
}

static void vid_copy(VidTex* t, const SDL_Rect* src, const SDL_Rect* dst) {
    if (!t) return;
    if (t->gl) vgl_copy(t->gl, src, dst);
    else SDL_RenderCopy(g_renderer, t->sdl, src, dst);
}

static void vid_copy_f(VidTex* t, const SDL_Rect* src, const SDL_FRect* dst) {
    if (!t) return;
    if (t->gl) vgl_copy_f(t->gl, src, dst);
    else SDL_RenderCopyF(g_renderer, t->sdl, src, dst);
}

static void vid_set_clip(const SDL_Rect* clip) {
    if (g_use_gl) vgl_set_clip(clip);
    else SDL_RenderSetClipRect(g_renderer, clip);
}

static void vid_begin_frame(void) {
    if (g_use_gl) {
        vgl_begin_frame(g_window);
        return;
    }
    SDL_RenderSetViewport(g_renderer, NULL);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
}

static void vid_set_vsync(bool enabled) {
    if (g_use_gl) vgl_set_vsync(enabled);
    else if (g_renderer) SDL_RenderSetVSync(g_renderer, enabled ? 1 : 0);
}

static void vid_imgui_new_frame(void) {
    if (g_use_gl) ImGui_ImplOpenGL3_NewFrame();
    else ImGui_ImplSDLRenderer2_NewFrame();
}

static void vid_imgui_render(void) {
    if (g_use_gl) {
        vgl_set_clip(NULL);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        return;
    }
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
}

static void vid_present(void) {
    if (g_use_gl) vgl_present(g_window);
    else SDL_RenderPresent(g_renderer);
}

static VidTex* g_texture = NULL;

/* Passo 14b-1: o centro montado pela GPU, conferido com o da CPU. */
struct GpuCompareStats {
    uint64_t compared = 0, equal = 0, skipped_mid = 0, skipped_mode = 0;
    uint64_t skipped_overflow = 0;   /* passo 14b-2: registro de escritas cheio */
    /* Passo 14b-2a: quadros com VRAM escrita no meio, conferidos mesmo assim */
    uint64_t mid_compared = 0, mid_equal = 0;
    uint64_t mid_writes = 0, mid_tile = 0, mid_map = 0, mid_lines = 0;
    uint64_t mid_bad_tile_frames = 0, mid_bad_map_frames = 0;   /* diferentes: só tiles / só mapa */
    int last_mid_first = -1, last_mid_last = -1;
    uint32_t last_mid_diff = 0;
    int last_mid_x = -1, last_mid_y = -1;
    uint32_t last_diff = 0;
    int first_x = -1, first_y = -1;
    uint32_t first_cpu = 0, first_gpu = 0;
    double ms_total = 0.0;
    /* Passo 14b-3: centro em HD (quando o Internal Resolution passa de 1x) */
    uint64_t hd_compared = 0, hd_equal = 0, hd_with_tiles = 0;
    uint64_t hd_mid_compared = 0, hd_mid_equal = 0;   /* tiles escritos no meio do quadro */
    uint32_t hd_last_diff = 0;
    int hd_scale = 0, hd_x = -1, hd_y = -1;
    uint32_t hd_cpu = 0, hd_gpu = 0;
    double hd_ms_total = 0.0;
    /* Passo 14b-4: laterais do widescreen em HD */
    uint64_t m_compared = 0, m_equal = 0, m_fade_conflict = 0;
    uint32_t m_last_diff = 0;
    int m_x = -1, m_y = -1;
    uint32_t m_cpu = 0, m_gpu = 0;
    double m_ms_total = 0.0;
    /* Passo 14c-1: tela inteira (textura do jogo) */
    uint64_t s_compared = 0, s_equal = 0, s_skipped = 0;
    uint32_t s_last_diff = 0;
    int s_x = -1, s_y = -1, s_w = 0, s_h = 0;
    uint32_t s_cpu = 0, s_gpu = 0;
    double s_ms_total = 0.0;
    /* Passo 14d-1: camadas do 120 fps */
    uint64_t ip_compared = 0, ip_equal = 0;
    /* Passo 14e-1: tela com filtro de pós-processamento */
    uint64_t f_compared = 0, f_equal = 0;
    uint32_t f_last_diff = 0;
    int f_x = -1, f_y = -1, f_mode = 0;
    uint32_t f_cpu = 0, f_gpu = 0;
    uint32_t ip_last_bg = 0, ip_last_obj = 0;
    int ip_x = -1, ip_y = -1;
};
/* Passo 14c-2: a GPU monta a tela (Graphics > "Montagem da tela"). */
static int g_gpu_screen_pref = 1;        /* 0 = CPU, 1 = GPU (pref display.gpu_screen); GPU padrão (passo 14c-3) */
static VidTex* g_gpu_vtex = NULL;        /* a textura da GPU embrulhada para vid_copy */
static unsigned g_gpu_vtex_id = 0;
static bool g_gpu_screen_valid = false;  /* a última tela da GPU pode ser mostrada de novo */
static int g_gpu_screen_w = 0, g_gpu_screen_h = 0;
static bool g_gpu_screen_shown = false;  /* o último quadro mostrado veio da GPU */
static bool g_gpu_frame_on_gpu = false;  /* passo 14d-1: o quadro do jogo atual está na tela da GPU */
static std::string g_gpu_screen_error;
/* Passo 3D-1: modo 3D em camadas (Graphics > "Modo 3D"). */
static int g_3d_mode = 0;          /* 0 = desligado, 1 = camadas (pref display.mode3d) */
static int g_3d_tilt = 10;         /* graus (pref display.tilt3d) */
static int g_3d_depth = 60;        /* % (pref display.depth3d) */
static int g_3d_hide = 0;          /* 3D-4f: Desenvolvedor > "3D: esconder" (diagnóstico, não grava) */
static bool g_3d_debug = false;    /* 3D-3f: Desenvolvedor > "3D: mostrar classes" */
/* Passo 3D-2b: fora das fases, o tipo de cada tile (pelo desenho dele) é marcado à
 * mão: Desenvolvedor > "3D: marcar tiles". Clique esquerdo troca fundo -> chão ->
 * água -> porta/escada -> fundo; clique direito volta a fundo. Gravado em
 * mods/3d_marcas.txt (hash do tile em hex e o tipo). */
static bool g_3d_mark = false;
/* 3D-2f: por tela (contexto do pacote HD: "g00", "m44"...; 0 = todas as telas,
 * das marcas antigas): a marca da tela vale antes da geral; tipo 0 na tela = fundo
 * ali mesmo que a geral diga outra coisa */
static std::map<std::pair<uint16_t, uint64_t>, uint8_t> g_3d_marks;
static std::map<std::pair<uint16_t, uint64_t>, uint8_t> g_3d_marks_def;   /* 3D-2q: as padrão (no programa) */
static bool g_3d_marks_loaded = false;
static std::string g_3d_mark_info;
static bool g_3d_wide = true;   /* a imagem 3D tem as laterais (256 de largura) */
/* Passo 3D-2c: congelar o jogo (Desenvolvedor, ao lado de "3D: marcar tiles") para
 * marcar trechos que não têm pausa: o jogo para no fim do quadro e a tela continua
 * sendo redesenhada com o último quadro (menu e cliques funcionando). */
static bool g_freeze = false;
static bool g_freeze_redraw = false;
static bool g_freeze_quit = false;
static const uint32_t* g_freeze_fb = NULL;
static const PPUNativeGpuFrame* g_frame_last = NULL;
static std::string marks3d_path(void);
static void marks3d_load(void);
static void marks3d_table(const GBContext* ctx);
static uint16_t marks3d_context(const GBContext* ctx);
static const char* marks3d_context_name(uint16_t c);
static int g_3d_stick = 1;         /* passo 3D-5a: câmera no analógico direito (pref display.stick3d) */
static float g_3d_cam_tilt = 0.0f, g_3d_cam_yaw = 0.0f;   /* deslocamento atual pelo analógico (suavizado) */
static int g_3d_follow = 1;        /* câmera 3D segue o Wario (pref display.follow3d, 3D-4q) */
static int g_3d_front = 90;        /* % de um bloco à frente do chão; 0 = sem (pref display.front3d, 3D-6a) */
static int g_3d_spr = 90;          /* % espessura dos sprites (pref display.spr3d, 3D-5e) */
static int g_3d_yaw = -5;          /* graus, câmera girada para o lado (pref display.yaw3d, 3D-3e) */
static const PPUNativeGpuFrame* g_frame3d = NULL;   /* quadro capturado neste vsync */
static bool g_3d_valid = false;    /* as camadas da GPU têm o quadro atual */
static int g_3d_w = 0, g_3d_h = 0;
static bool g_3d_shown = false;    /* o último quadro foi desenhado em 3D */
static VidTex* g_3d_vtex = NULL;   /* passo 3D-3: a imagem 3D (fora da tela) embrulhada */
static unsigned g_3d_vtex_id = 0;
static bool g3d_active(void);
static bool g_gpu_center_ok = false;    /* passo 14c-1: a GPU montou o centro HD deste quadro */
static bool g_gpu_margins_ok = false;   /* e as laterais HD */
static bool g_gpu_compare = false;
static GpuCompareStats g_gpu_stats;
static std::string g_gpu_error;
static std::vector<uint32_t> g_gpu_read;
static std::vector<uint32_t> g_gpu_read_hd;   /* passo 14b-3 */
static std::vector<uint32_t> g_gpu_read_m;    /* passo 14b-4 */

/* Passo 14b-4: laterais HD da GPU contra as da CPU (só quando a CPU as
 * desenhou: widescreen ligado, escala acima de 1x, tiles HD). x na
 * diferença: 0..47 esquerda, 48..95 direita (vezes a escala). */
static void gpu_compare_margins_hd(const PPUNativeGpuFrame* fr) {
    if (!fr->margin_on || !fr->cpu_hd_margin || fr->hd_scale < 2) return;
    const int n = fr->hd_scale;
    const int w = PPU_NATIVE_GPU_MARGIN_W * n, h = GB_SCREEN_HEIGHT * n;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    g_gpu_read_m.resize((size_t)w * h);
    int got = 0;
    if (!gpu_ppu_compose_margins_hd(fr) || !gpu_ppu_read_margins_hd(g_gpu_read_m.data(), &got) ||
        got != n) {
        g_gpu_error = "a GPU nao conseguiu montar as laterais em HD";
        return;
    }
    g_gpu_margins_ok = true;   /* passo 14c-1 */
    GpuCompareStats& st = g_gpu_stats;
    st.m_ms_total += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                     (double)SDL_GetPerformanceFrequency();
    if (fr->fade_conflict) st.m_fade_conflict++;
    uint32_t diff = 0;
    int fx = -1, fy = -1;
    for (int i = 0; i < w * h; ++i) {
        if (((g_gpu_read_m[(size_t)i] ^ fr->cpu_hd_margin[i]) & 0x00FFFFFFu) != 0) {
            if (!diff) { fx = i % w; fy = i / w; }
            diff++;
        }
    }
    st.m_compared++;
    if (!diff) {
        st.m_equal++;
        return;
    }
    st.m_last_diff = diff;
    st.m_x = fx;
    st.m_y = fy;
    st.m_cpu = fr->cpu_hd_margin[(size_t)fy * w + fx];
    st.m_gpu = g_gpu_read_m[(size_t)fy * w + fx];
    if (st.m_compared - st.m_equal <= 20u) {
        fprintf(stderr, "[GPU-LAT] %ux: %u pontos diferentes, primeiro (%d,%d) %s cpu %08X gpu %08X%s\n",
                n, diff, fx, fy, fx < w / 2 ? "esquerda" : "direita", st.m_cpu, st.m_gpu,
                fr->fade_conflict ? " (escurecimento com dois valores na coluna)" : "");
    }
}

/* Passo 14b-3: o centro em HD da GPU contra o da CPU. Quadros com tile
 * redesenhado no meio (animações) contam à parte: a GPU troca o tile HD pela
 * linha do tempo que o ppu_native monta (hd_tl), e é bom ver isso separado. */
static void gpu_compare_frame_hd(const PPUNativeGpuFrame* fr) {
    if (fr->hd_scale < 2 || !fr->cpu_hd) return;
    const int n = fr->hd_scale;
    const int w = GB_SCREEN_WIDTH * n, h = GB_SCREEN_HEIGHT * n;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    g_gpu_read_hd.resize((size_t)w * h);
    int got = 0;
    if (!gpu_ppu_compose_hd(fr) || !gpu_ppu_read_hd(g_gpu_read_hd.data(), &got) || got != n) {
        g_gpu_error = "a GPU nao conseguiu montar o quadro em HD";
        return;
    }
    g_gpu_center_ok = true;   /* passo 14c-1 */
    GpuCompareStats& st = g_gpu_stats;
    st.hd_ms_total += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                      (double)SDL_GetPerformanceFrequency();
    uint32_t diff = 0;
    int fx = -1, fy = -1;
    for (int i = 0; i < w * h; ++i) {
        if (((g_gpu_read_hd[(size_t)i] ^ fr->cpu_hd[i]) & 0x00FFFFFFu) != 0) {
            if (!diff) { fx = i % w; fy = i / w; }
            diff++;
        }
    }
    st.hd_scale = n;
    const bool mid_tiles = fr->vram_changed_mid_frame && fr->mid_tile_writes > 0;
    if (mid_tiles) {
        st.hd_mid_compared++;
        if (!diff) st.hd_mid_equal++;
    } else {
        st.hd_compared++;
        if (fr->hd_on) st.hd_with_tiles++;
        if (!diff) st.hd_equal++;
    }
    if (!diff) return;
    st.hd_last_diff = diff;
    st.hd_x = fx;
    st.hd_y = fy;
    st.hd_cpu = fr->cpu_hd[(size_t)fy * w + fx];
    st.hd_gpu = g_gpu_read_hd[(size_t)fy * w + fx];
    const uint64_t bad = (st.hd_compared - st.hd_equal) + (st.hd_mid_compared - st.hd_mid_equal);
    if (bad <= 20u) {
        fprintf(stderr, "[GPU-HD] %ux: %u pontos diferentes, primeiro (%d,%d) cpu %08X gpu %08X%s\n",
                n, diff, fx, fy, st.hd_cpu, st.hd_gpu,
                mid_tiles ? " (tile escrito no meio do quadro)" : "");
    }
}

static void gpu_compare_frame_1x(const PPUNativeGpuFrame* fr);

static void gpu_compare_frame(void) {
    g_gpu_center_ok = false;   /* passo 14c-1 */
    g_gpu_margins_ok = false;
    g_frame3d = NULL;
    const PPUNativeGpuFrame* fr = g_freeze_redraw ? g_frame_last : ppu_native_gpu_take_frame();   /* 3D-2c */
    if (!fr) return;
    g_frame_last = fr;
    g_frame3d = fr;   /* passo 3D-1: as camadas são montadas na apresentação */
    if (!g_gpu_compare && g3d_active()) return;
    if (!g_gpu_compare) {   /* passo 14c-2: só monta (sem ler de volta) para a tela */
        if (!fr->cgb || fr->log_overflow || fr->hd_scale < 2 || !fr->hd_on) return;
        g_gpu_center_ok = gpu_ppu_compose_hd(fr);
        if (g_gpu_center_ok && fr->margin_on) g_gpu_margins_ok = gpu_ppu_compose_margins_hd(fr);
        return;
    }
    if (!fr->cgb) { g_gpu_stats.skipped_mode++; return; }
    if (fr->log_overflow) { g_gpu_stats.skipped_overflow++; return; }   /* passo 14b-2 */
    gpu_compare_frame_1x(fr);
    gpu_compare_frame_hd(fr);   /* passo 14b-3 */
    gpu_compare_margins_hd(fr); /* passo 14b-4 */
}

static void gpu_compare_frame_1x(const PPUNativeGpuFrame* fr) {
    const bool mid = fr->vram_changed_mid_frame;
    if (mid) {   /* passo 14b-2a: confere à parte, para ver se a escrita muda algo na tela */
        g_gpu_stats.skipped_mid++;
        g_gpu_stats.mid_writes += fr->mid_writes;
        g_gpu_stats.mid_tile += fr->mid_tile_writes;
        g_gpu_stats.mid_map += fr->mid_map_writes;
        g_gpu_stats.mid_lines += fr->mid_lines;
        g_gpu_stats.last_mid_first = fr->mid_first_line;
        g_gpu_stats.last_mid_last = fr->mid_last_line;
    }
    const uint64_t t0 = SDL_GetPerformanceCounter();
    g_gpu_read.resize((size_t)GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT);
    if (!gpu_ppu_compose(fr) || !gpu_ppu_read(g_gpu_read.data())) {
        g_gpu_error = "a GPU nao conseguiu montar o quadro";
        return;
    }
    g_gpu_stats.ms_total += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                            (double)SDL_GetPerformanceFrequency();
    uint32_t diff = 0;
    int fx = -1, fy = -1;
    for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i) {
        if (((g_gpu_read[(size_t)i] ^ fr->cpu[i]) & 0x00FFFFFFu) != 0) {
            if (!diff) { fx = i % GB_SCREEN_WIDTH; fy = i / GB_SCREEN_WIDTH; }
            diff++;
        }
    }
    if (mid) {   /* passo 14b-2a */
        g_gpu_stats.mid_compared++;
        if (!diff) {
            g_gpu_stats.mid_equal++;
        } else {
            if (fr->mid_map_writes == 0) g_gpu_stats.mid_bad_tile_frames++;
            if (fr->mid_tile_writes == 0) g_gpu_stats.mid_bad_map_frames++;
            g_gpu_stats.last_mid_diff = diff;
            g_gpu_stats.last_mid_x = fx;
            g_gpu_stats.last_mid_y = fy;
            const uint64_t badm = g_gpu_stats.mid_compared - g_gpu_stats.mid_equal;
            if (badm <= 20u) {
                fprintf(stderr, "[GPU-MID] %u pontos diferentes, primeiro (%d,%d) | escritas %u "
                        "(tiles %u, mapa %u) em %u linhas, da %u a %u\n",
                        diff, fx, fy, fr->mid_writes, fr->mid_tile_writes, fr->mid_map_writes,
                        fr->mid_lines, fr->mid_first_line, fr->mid_last_line);
            }
        }
        return;
    }
    g_gpu_stats.compared++;
    g_gpu_stats.last_diff = diff;
    if (!diff) {
        g_gpu_stats.equal++;
        return;
    }
    g_gpu_stats.first_x = fx;
    g_gpu_stats.first_y = fy;
    g_gpu_stats.first_cpu = fr->cpu[fy * GB_SCREEN_WIDTH + fx];
    g_gpu_stats.first_gpu = g_gpu_read[(size_t)(fy * GB_SCREEN_WIDTH + fx)];
    const uint64_t bad = g_gpu_stats.compared - g_gpu_stats.equal;
    if (bad <= 20u) {
        const uint8_t* l = fr->line[fy];
        fprintf(stderr, "[GPU-CMP] %u pontos diferentes, primeiro (%d,%d) cpu %08X gpu %08X | "
                "lcdc %02X scx %u scy %u wx %u win_line %u janela %u\n",
                diff, fx, fy, g_gpu_stats.first_cpu, g_gpu_stats.first_gpu,
                l[0], l[1], l[2], l[3], l[4], l[5]);
    }
}

/* Passo 14p: arrastar o dedo (ou o mouse) para cima/baixo sobre a lista de
 * configurações rola a lista, como no celular, sem precisar pegar a barra.
 * Chamado dentro da janela que rola (antes do EndChild). O arraste conta
 * quando passa de 10 px e é mais vertical que horizontal: aí o clique que
 * começou nele é cancelado (botão/caixa não dispara ao soltar) e a lista
 * acompanha o dedo. Arraste na horizontal continua mexendo nos sliders. */
static void imgui_drag_to_scroll(void) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiIO& io = ImGui::GetIO();
    static bool active = false;
    static ImGuiID owner = 0;
    if (!io.MouseDown[0]) {
        active = false;
        return;
    }
    if (active) {
        if (owner == win->ID) {
            ImGui::SetScrollY(win, win->Scroll.y - io.MouseDelta.y);
        }
        return;
    }
    if (win->ScrollMax.y <= 0.0f || !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 10.0f)) {
        return;
    }
    if (!win->InnerRect.Contains(io.MouseClickedPos[0]) || g.HoveredWindow == NULL ||
        g.HoveredWindow->RootWindow != win->RootWindow) {
        return;   /* começou fora da lista, ou há um popup por cima */
    }
    const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
    if (fabsf(d.y) < fabsf(d.x) * 1.2f) {
        return;   /* mais horizontal: slider */
    }
    active = true;
    owner = win->ID;
    ImGui::ClearActiveID();
    ImGui::SetScrollY(win, win->Scroll.y - d.y);   /* inclui o caminho até passar dos 10 px */
}
static int g_scale = 5;
static int g_windowed_width = GB_SCREEN_WIDTH * 5;
static int g_windowed_height = GB_SCREEN_HEIGHT * 5;
static SDL_Rect g_game_viewport = {0, 0, GB_SCREEN_WIDTH * 5, GB_SCREEN_HEIGHT * 5};
static uint32_t g_last_frame_time = 0;
static SDL_AudioDeviceID g_audio_device = 0;
static SDL_GameController* g_controller = NULL;
static bool g_vsync = false;  /* VSync OFF - we pace with wall clock for 59.7 FPS */
static bool g_audio_started = false;
static uint32_t g_audio_start_threshold = 0;

/* Performance timing diagnostics */
static double g_timing_render_total = 0.0;
static double g_timing_vsync_total = 0.0;
static uint32_t g_timing_frame_count = 0;
static GBPlatformTimingInfo g_last_timing = {};

/* Janela de 60 frames para média e pico de tempo. */
struct GBTimingWindow {
    double acc = 0.0;
    double max = 0.0;
    uint32_t count = 0;
    double shown_avg = 0.0;
    double shown_max = 0.0;
};

static void timing_window_add(GBTimingWindow& w, double ms) {
    w.acc += ms;
    if (ms > w.max) w.max = ms;
    if (++w.count >= 60) {
        w.shown_avg = w.acc / (double)w.count;
        w.shown_max = w.max;
        w.acc = 0.0;
        w.max = 0.0;
        w.count = 0;
    }
}

static GBTimingWindow g_emu_window;      /* fim do vsync -> início do render */
static GBTimingWindow g_render_window;   /* upload + compose + present */
static double g_last_vsync_end_ms = 0.0;
static int g_texture_scale = 1;          /* escala com que a textura foi criada */

// Supersampling
static int g_internal_res_scale = 4; // 1x (Nativo), 2x, 3x, 4x — 4x padrão (passo 14p)
static const char* g_internal_res_names[] = {
    "1x (Nativo GB - 160x144)",
    "2x Internal (320x288)",
    "3x Internal (480x432)",
    "4x Internal (640x576 - Retina/4K)"
};

/* Menu State */
static bool g_show_menu = false;
/* Passo 15a: idioma do menu (0 inglês, padrão; 1 português; pref ui.language) e a
 * categoria aberta na lista da esquerda (pref ui.menu_page) */
static int g_ui_lang = 0;
static int g_menu_page = 0;
static inline const char* TR(const char* en, const char* pt) { return g_ui_lang == 1 ? pt : en; }
/* Passo 3D-5a: o menu não abre sozinho na abertura do jogo. Tecla/botão que
 * chega na primeira fração de segundo (de quem abriu o jogo: launcher, Steam,
 * atalho, controle conectando) não abre o menu. */
static Uint32 g_menu_guard_until = 0;
static bool menu_toggle_allowed(void) {
    return g_menu_guard_until == 0 || SDL_TICKS_PASSED(SDL_GetTicks(), g_menu_guard_until);
}
static bool g_show_overlay = false;
static int g_speed_percent = 100;
static int g_palette_idx = 0;
static bool g_smooth_lcd_transitions = true;
static bool g_launcher_return_enabled = false;
static bool g_benchmark_mode = false;
static std::string g_persistence_dir;
static GBPersistenceTestFault g_persistence_test_fault =
    GB_PERSISTENCE_TEST_FAULT_NONE;
static GBPersistenceTestTarget g_persistence_test_target =
    GB_PERSISTENCE_TEST_TARGET_BATTERY;
static bool g_fullscreen = false;
static bool g_app_suspended = false;
static bool g_renderer_reset_pending = false;
static GBPlatformExitAction g_exit_action = GB_PLATFORM_EXIT_QUIT;
//static const char* g_palette_names[] = { "Original (Green)", "Black & White (Pocket)", "Amber (Plasma)" };
static const char* g_scale_names[] = { "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)", "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)",
    "12x (1920x1728)", "16x (2560x2304)", "20x (3200x2880)"  };
typedef enum GBRenderScalingMode {
    GB_RENDER_SCALING_PIXEL_PERFECT = 0,
    GB_RENDER_SCALING_ASPECT_FIT = 1,
    GB_RENDER_SCALING_ASPECT_FILL = 2,
    GB_RENDER_SCALING_STRETCH = 3,
} GBRenderScalingMode;
typedef enum GBRenderFilterMode {
    GB_RENDER_FILTER_NEAREST = 0,
    GB_RENDER_FILTER_LINEAR = 1,
} GBRenderFilterMode;
typedef enum GBControllerLabelProfile {
    GB_CONTROLLER_LABEL_GENERIC = 0,
    GB_CONTROLLER_LABEL_XBOX = 1,
    GB_CONTROLLER_LABEL_PLAYSTATION = 2,
    GB_CONTROLLER_LABEL_NINTENDO = 3,
} GBControllerLabelProfile;
typedef enum GBInputAction {
    GB_INPUT_ACTION_RIGHT = 0,
    GB_INPUT_ACTION_LEFT = 1,
    GB_INPUT_ACTION_UP = 2,
    GB_INPUT_ACTION_DOWN = 3,
    GB_INPUT_ACTION_A = 4,
    GB_INPUT_ACTION_B = 5,
    GB_INPUT_ACTION_SELECT = 6,
    GB_INPUT_ACTION_START = 7,
    GB_INPUT_ACTION_FAST_FORWARD = 8,
    GB_INPUT_ACTION_TOGGLE_MAX_SPEED = 9,
    GB_INPUT_ACTION_SAVE_STATE = 10,
    GB_INPUT_ACTION_LOAD_STATE = 11,
    GB_INPUT_ACTION_PREVIOUS_STATE_SLOT = 12,
    GB_INPUT_ACTION_NEXT_STATE_SLOT = 13,
    GB_INPUT_ACTION_TOGGLE_OVERLAY = 14,
    GB_INPUT_ACTION_TOGGLE_MUTE = 15,
    GB_INPUT_ACTION_TOGGLE_MENU = 16,
    GB_INPUT_ACTION_TOGGLE_PORT_UI = 17,
    GB_INPUT_ACTION_COUNT = 18,
} GBInputAction;
typedef enum GBInputBindingKind {
    GB_INPUT_BINDING_NONE = 0,
    GB_INPUT_BINDING_KEY = 1,
    GB_INPUT_BINDING_CONTROLLER_BUTTON = 2,
    GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE = 3,
    GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE = 4,
} GBInputBindingKind;
typedef enum GBBindingCaptureDevice {
    GB_CAPTURE_DEVICE_NONE = 0,
    GB_CAPTURE_DEVICE_KEYBOARD = 1,
    GB_CAPTURE_DEVICE_CONTROLLER = 2,
} GBBindingCaptureDevice;
typedef struct GBInputBinding {
    GBInputBindingKind kind;
    int16_t code;
} GBInputBinding;
static GBRenderScalingMode g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
static GBRenderFilterMode g_render_filter_mode = GB_RENDER_FILTER_LINEAR;   /* passo 14p: linear padrão */
static SDL_GameControllerType g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
static GBControllerLabelProfile g_controller_label_profile = GB_CONTROLLER_LABEL_GENERIC;
static std::string g_controller_name;
static bool g_audio_output_enabled = true;
static std::atomic<bool> g_audio_muted{false};
static uint32_t g_audio_latency_ms = 80;
static std::atomic<uint32_t> g_audio_volume_percent{100};
/* Passo 6e: volume so do som do Game Boy (efeitos e musica do GB), 0..200.
 * O volume geral vale por cima dele e das trilhas .ogg. */
static std::atomic<uint32_t> g_audio_gb_volume_percent{100};

static inline uint32_t gb_effective_volume_percent(void) {
    return (g_audio_volume_percent.load(std::memory_order_relaxed) *
            g_audio_gb_volume_percent.load(std::memory_order_relaxed)) / 100u;
}


/* Passo 6a: mixer com controle de taxa (veja sdl_audio_callback). */
static std::atomic<bool> g_audio_rate_control{true};
static std::atomic<int32_t> g_audio_rate_ppm{0};      /* ajuste atual, em partes por milhão */
static std::atomic<uint32_t> g_audio_target_fill{0};  /* nível alvo do buffer, em amostras */
static std::atomic<int32_t> g_audio_pace_ppm{0};      /* ajuste do ritmo dos frames (ppm) */
static uint64_t g_resample_frac = 0;                  /* thread de áudio: posição 32.32 */
static double g_rate_adjust = 0.0;                    /* thread de áudio: ajuste suavizado */
static double g_rate_integral = 0.0;                  /* thread de áudio: parte integral (ritmo) */
static uint32_t g_audio_low_watermark = 0;
static uint32_t g_audio_device_sample_rate = 44100;
static uint32_t g_audio_device_buffer_samples = 0;
static std::vector<std::string> g_audio_output_devices;
static std::string g_audio_target_device_name;
static std::string g_audio_active_device_name;
static constexpr int GB_SAVESTATE_SLOT_COUNT = 10;
static constexpr int GB_JOYPAD_ACTION_COUNT = 8;
static constexpr int GB_FAST_FORWARD_SPEED_PERCENT = 250;
static constexpr int GB_MAX_SHORTCUT_SPEED_PERCENT = 500;
static int g_savestate_slot = 0;
static std::string g_savestate_status;
/* Passo 14p: nomes do Scaling Mode removidos com a opção do menu. */
static const char* g_render_filter_names[] = {
    "Nearest",
    "Linear",
};
static const uint32_t g_palettes[][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // Original
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }  // Amber
};
static uint32_t g_lcd_off_framebuffer[GB_FRAMEBUFFER_SIZE];
static uint32_t g_last_guest_framebuffer[GB_FRAMEBUFFER_SIZE];
static uint32_t g_last_guest_widescreen_framebuffer[GB_WIDESCREEN_FRAMEBUFFER_SIZE];
static bool g_lcd_off_framebuffer_initialized = false;
static bool g_last_guest_framebuffer_valid = false;
static bool g_last_guest_widescreen_framebuffer_valid = false;
static uint64_t g_present_count = 0;
static GBContext* g_registered_ctx = NULL;
static bool g_widescreen_mode = true;   /* [fix-v2] widescreen ligado por padrão */
static bool g_widescreen_fade_enabled = true;
static int g_widescreen_fade_percent = 65; /* mirrors GBPPU's default of 0.65f */
static bool g_widescreen_auto_active = false;
/* Passo 10a: 0 = 60 fps (original), 1 = 120 fps (quadros intermediarios) */
static int g_fps_mode = 0;
static bool g_ip_mid_draw = false;   /* desenhando o quadro do meio */
static void ip_release_textures(void); /* current effective state, gated by sGameMode */

// Variável para guardar o estado do filtro selecionado
static GBPostFilterMode g_post_filter_mode = GB_POST_FILTER_NONE;

/* Com filtro Nearest e sem pós-processamento, ampliar na CPU não muda nada na
 * imagem: o SDL_RenderCopy já amplia na GPU. Nesse caso a textura fica em 1x.
 * A resolução interna maior só é usada quando um filtro se beneficia dela. */
/* Passo 4b: usa o centro em alta resolucao do ppu_native na textura. */
static bool g_use_hd_frame = true;

static int effective_internal_scale(void) {
    if (ppu_native_hd_get_tiles_enabled() && ppu_native_hd_tile_count() > 0) {
        return g_internal_res_scale < 1 ? 1 : g_internal_res_scale; /* passo 5b */
    }
    if (g_post_filter_mode == GB_POST_FILTER_NONE &&
        g_render_filter_mode == GB_RENDER_FILTER_NEAREST) {
        return 1;
    }
    return g_internal_res_scale < 1 ? 1 : g_internal_res_scale;
}

static GBPPU* active_ppu(void) {
    return g_registered_ctx ? (GBPPU*)g_registered_ctx->ppu : NULL;
}

/* Passo 6b: qual musica o jogo pediu ao driver de som.
 * RAM map do Wario Land (Data Crystal): $A61C = pedido de musica (01..30),
 * $A61D = musica tocando. Ficam na RAM do cartucho, como o sGameMode ($A8C3).
 * Lido uma vez por frame, sem mexer no jogo. */
#define WL_MUSIC_REQUEST_ADDR 0xA61C
#define WL_MUSIC_CURRENT_ADDR 0xA61D
#define WL_GAME_MODE_ADDR 0xA8C3
#define MUSIC_HISTORY_MAX 16

struct MusicHistoryEntry {
    uint32_t frame;
    uint8_t song;
    uint8_t game_mode;
};

static MusicHistoryEntry g_music_history[MUSIC_HISTORY_MAX];
static uint32_t g_music_history_total = 0;   /* trocas vistas (o vetor guarda as ultimas) */
static int g_music_current = -1;             /* -1 = ainda nao lido */
static int g_music_request_seen = -1;        /* ultimo pedido diferente de zero visto */
static uint32_t g_music_monitor_frames = 0;
static bool g_audio_lab_gate = false;        /* passo 6c: checkbox do laboratorio */
static bool g_music_log = false;             /* passo 6d: [Musica] no terminal */
static bool g_scene_for_recorder = false;    /* passo 7a: a captura pedida e do gravador */
static bool g_rec_rect_enabled = false;       /* passo 7a: so dentro do retangulo */
static int g_rec_rect[4] = {40, 24, 80, 96}; /* x, y, largura, altura (tela 160x144) */
static uint32_t g_music_mods_generation = 0;  /* passo 6c: recarga de mods vista */
static uint32_t audio_ring_fill_samples(void);

/* Passo 6c.1: vigia de 64 bytes da RAM do cartucho (padrao $A600, onde
 * ficam os gatilhos de efeito e a musica). Guarda quando cada byte mudou
 * para destacar o que esta mexendo. */
#define AUDIO_LAB_BYTES 64
static uint16_t g_audio_lab_base = 0xA600;
static uint16_t g_audio_lab_base_seen = 0;
static int g_audio_lab_value[AUDIO_LAB_BYTES];
static uint32_t g_audio_lab_changed_at[AUDIO_LAB_BYTES];
static uint32_t g_audio_lab_change_count[AUDIO_LAB_BYTES];
static uint32_t g_audio_lab_frames = 0;

static int read_cart_ram_byte(uint16_t addr);

static void audio_lab_update(void) {
    g_audio_lab_frames++;
    const bool rebase = (g_audio_lab_base != g_audio_lab_base_seen);
    for (int i = 0; i < AUDIO_LAB_BYTES; i++) {
        const int v = read_cart_ram_byte((uint16_t)(g_audio_lab_base + i));
        if (rebase) {
            g_audio_lab_value[i] = v;
            g_audio_lab_changed_at[i] = 0;
            g_audio_lab_change_count[i] = 0;
        } else if (v != g_audio_lab_value[i]) {
            g_audio_lab_value[i] = v;
            g_audio_lab_changed_at[i] = g_audio_lab_frames;
            g_audio_lab_change_count[i]++;
        }
    }
    g_audio_lab_base_seen = g_audio_lab_base;
}

static int read_cart_ram_byte(uint16_t addr) {
    const GBContext* ctx = g_registered_ctx;
    if (!ctx || !ctx->eram || addr < 0xA000) {
        return -1;
    }
    const size_t offset = (size_t)(addr - 0xA000);
    if (offset >= ctx->eram_size) {
        return -1;
    }
    return ctx->eram[offset];
}

/* Passo 7t: [Estado] no terminal a cada troca de modo, sub-modo, sMapId ou
 * pagina DX, para achar o trecho exato de uma transicao. */
static bool g_state_log = false;
static uint8_t gb_wl_game_mode_2(const GBContext* ctx);
static void state_log_update(void) {
    static int last[4] = {-1, -1, -1, -1};
    const GBContext* ctx = g_registered_ctx;
    if (!g_state_log || !ctx || !ctx->eram || ctx->eram_size <= 0x099D) {
        return;
    }
    const int now[4] = {ctx->eram[0x08C3], ctx->eram[0x08C4], ctx->eram[0x079C],
                        0x41 + ctx->eram[0x0800]};
    if (memcmp(now, last, sizeof(now)) != 0) {
        memcpy(last, now, sizeof(now));
        printf("[Estado] frame %u: modo %02X sub %02X sMapId %02X pagina %02X game_mode_2 %u\n",
               g_music_monitor_frames, now[0], now[1], now[2], now[3],
               (unsigned)gb_wl_game_mode_2(ctx));
        fflush(stdout);
    }
}

static void music_monitor_update(void) {
    g_music_monitor_frames++;
    state_log_update();   /* passo 7t */
    audio_lab_update(); /* passo 6c.1 */
    const int request = read_cart_ram_byte(WL_MUSIC_REQUEST_ADDR);
    if (request > 0) {
        g_music_request_seen = request;
    }
    const int current = read_cart_ram_byte(WL_MUSIC_CURRENT_ADDR);
    /* Passo 6c: trilhas .ogg. Reindexa quando os mods mudam; segue $A61D
     * (os primeiros frames tem a sobra do save e sao ignorados); o portao
     * do GB fica ligado enquanto uma trilha toca. */
    if (hd_pack_io_generation() != g_music_mods_generation) {
        g_music_mods_generation = hd_pack_io_generation();
        music_replace_rescan();
    }
    if (current >= 0 && g_music_monitor_frames > 8) {
        music_replace_set_song(current, audio_ring_fill_samples());
    }
    music_replace_collect();
    gb_audio_set_gate_enabled(g_audio_lab_gate || music_replace_active());
    if (current < 0 || current == g_music_current) {
        return;
    }
    g_music_current = current;
    const int mode = read_cart_ram_byte(WL_GAME_MODE_ADDR);
    MusicHistoryEntry& e = g_music_history[g_music_history_total % MUSIC_HISTORY_MAX];
    e.frame = g_music_monitor_frames;
    e.song = (uint8_t)current;
    e.game_mode = (uint8_t)(mode < 0 ? 0 : mode);
    g_music_history_total++;
    if (g_music_log) {   /* passo 6d */
        printf("[Musica] frame %u: musica %02X (modo de jogo %02X)\n",
               e.frame, (unsigned)e.song, (unsigned)e.game_mode);
        fflush(stdout);
    }
}

/* sGameMode ($A8C3, wl disassembly bank01.asm CheckMode) - only the actual
 * side-scrolling level states should get the widescreen margins; the title,
 * world map, bonus games, and other static-screen modes stream backgrounds
 * for a fixed 160px view and show garbage tiles in the extra margin. */
static bool gb_mode_is_side_scrolling(uint8_t game_mode) {
    switch (game_mode) {    /* game_mode:
                              mode 0 = intro + choose slot
                              mode 1 = mapa
                              mode 2 = level initit
                              mode 3 = gameplay
                              mode 4 = Level win - choose bonus and final coind and medals
                              resto = preta nas laterais*/
        case 1:
        //case 2:  /* Mode_LevelInit */    
        case 3:  /* Mode_Level */
            return true;
        default:
            return false;
    }
}

/* Passo 7r: game_mode_2, separa a fase normal da sala do chefe (modos 2 e 3).
 * Do disassembly (github.com/Kak2X/wl):
 *   sLvlScrollMode ($A99D) >= $F0 = sala sem rolagem (LVLSCROLL_NONE, "boss mode");
 *   sBossRoom      ($A995) != 0   = sala de chefe (Mode_LevelDoor_LoadRoom, bank01).
 * O jogo zera sLvlScrollMode quando o Wario morre; o sBossRoom segura o 22.
 *   21 = fase normal (rolagem lateral)
 *   22 = sala do chefe (tela fixa de 160 px)
 *    0 = fora da fase (outros modos) */
#define WL_GAME_MODE_2_LEVEL 21
#define WL_GAME_MODE_2_BOSS  22
/* Passo 7t (troca o 7s, que olhava a pagina DX 4C/4D e nao resolveu):
 * modo 1 (mapa) com sMapId ($A79C) num modo de preparo do mapa. Na volta da
 * tela do tesouro, o jogo entra no modo 1 com sMapId = MAP_MODE_INIT (0) e
 * passa por um *Init da regiao antes do fade; nesses quadros a tela ainda e a
 * antiga e as laterais mostravam lixo. Lista dos Init no constants.asm do
 * disassembly (MAP_MODE_*). */
#define WL_GAME_MODE_2_MAP_CLEAR 13
/* Passo 7v (disassembly, bank01 Map_Do / Mode_LevelInit):
 *   14 = modo 1 sub 01 (GM_MAP_COURSESCR): tela "COURSE No.XX". A VRAM tem a
 *        fonte e as laterais mostravam "000000".
 *   23 = modo 2 sub 00 (GM_LEVELINIT_LOAD): carregando a fase, LCD desligado.
 *   Modo 2 sub 01..03 (FADEBG, FADEOBJ, STARTLEVEL): a fase ja esta na tela,
 *   com o layout da fase pronto -> 21/22 como no modo 3. */
#define WL_GAME_MODE_2_COURSE_SCREEN 14
/* Passo 11e: 15 = modo 1 com a página DX $4D ($A800 = 0C): contador de moedas
 * no fim da fase, indo para o mapa regional. Laterais sem cenário (preto ou o
 * fundo ambiente do 8a). */
#define WL_GAME_MODE_2_COIN_TALLY 15
#define WL_GAME_MODE_2_LEVEL_LOAD 23
static bool gb_wl_map_mode_is_init(uint8_t map_id) {
    switch (map_id) {
        case 0x00: /* MAP_MODE_INIT */
        case 0x01: /* INITOVERWORLD */
        case 0x03: /* INITRICEBEACH */
        case 0x05: /* INITMTTEAPOT */
        case 0x09: /* INITSTOVECANYON */
        case 0x0B: /* INITSYRUPCASTLE */
        case 0x0D: /* INITPARSLEYWOODS */
        case 0x0F: /* INITSSTEACUP */
        case 0x11: /* INITSHERBETLAND */
        case 0x13: /* INITMTTEAPOTCUTSCENE */
        case 0x16: /* INITSSTEACUPCUTSCENE */
        case 0x18: /* INITPARSLEYWOODSCUTSCENE */
        case 0x1A: /* INITSYRUPCASTLEC38CUTSCENE */
        case 0x1C: /* INITSYRUPCASTLEC39CUTSCENE */
        case 0x1E: /* INITENDING2 */
        case 0x21: /* INITENDING1 */
            return true;
        default:
            return false;
    }
}
static uint8_t gb_wl_game_mode_2(const GBContext* ctx) {
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x099D) {
        return 0;
    }
    const uint8_t mode = ctx->eram[0x08C3];
    /* Passo 7u: o registro [Estado] do usuario mostrou a volta do tesouro:
     *   modo 04 sMapId 03 pagina 4D -> modo 01 sMapId 03 (2 quadros, 13)
     *   -> modo 01 sMapId 07 (MAP_MODE_FADEIN) pagina 44 (67 quadros, lixo)
     *   -> modo 01 sMapId 04 (mapa de Rice Beach).
     * O lixo fica no fade de entrada. Para nao apagar as laterais em todo fade
     * de mapa, so vale quando o mapa veio do modo 4 (fim de fase): marca na
     * troca 4 -> 1 e desmarca quando o mapa chega num modo que nao e de
     * preparo nem o fade de entrada. */
    static uint8_t s_prev_mode = 0xFF;
    static bool s_after_clear = false;
    if (mode != s_prev_mode) {
        s_after_clear = (s_prev_mode == 4 && mode == 1);
        s_prev_mode = mode;
    }
    if (mode == 1 && ctx->eram[0x08C4] == 0x01) {   /* passo 7v: COURSE No.XX */
        return WL_GAME_MODE_2_COURSE_SCREEN;
    }
    if (mode == 1 && ctx->eram[0x0800] == 0x0C) {   /* passo 11e: m4d */
        return WL_GAME_MODE_2_COIN_TALLY;
    }
    if (mode == 1) {   /* passo 7t / 7u */
        const uint8_t map_id = ctx->eram[0x079C];
        if (gb_wl_map_mode_is_init(map_id)) {
            return WL_GAME_MODE_2_MAP_CLEAR;
        }
        if (s_after_clear) {
            if (map_id == 0x07) {   /* MAP_MODE_FADEIN */
                return WL_GAME_MODE_2_MAP_CLEAR;
            }
            s_after_clear = false;
        }
        return 0;
    }
    if (mode != 2 && mode != 3) {
        return 0;
    }
    if (mode == 2 && ctx->eram[0x08C4] == 0x00) {   /* passo 7v */
        return WL_GAME_MODE_2_LEVEL_LOAD;
    }
    const bool boss = ctx->eram[0x099D] >= 0xF0 || ctx->eram[0x0995] != 0;
    return boss ? WL_GAME_MODE_2_BOSS : WL_GAME_MODE_2_LEVEL;
}

/* Passo 7r: laterais por game_mode_2. Comente um case para mudar. */
static bool gb_mode_2_wants_widescreen(uint8_t game_mode_2) {
    switch (game_mode_2) {
        case WL_GAME_MODE_2_BOSS:   /* 22: chefe, laterais pretas */
            return false;
        case WL_GAME_MODE_2_MAP_CLEAR:   /* 13: mapa se preparando, tela antiga (passo 7t) */
            return false;
        case WL_GAME_MODE_2_COURSE_SCREEN:   /* 14: tela COURSE No.XX (passo 7v) */
            return false;
        case WL_GAME_MODE_2_COIN_TALLY:      /* 15: contador de moedas, m4d (passo 11e) */
            return false;
        case WL_GAME_MODE_2_LEVEL_LOAD:      /* 23: carregando a fase (passo 7v) */
            return false;
        case WL_GAME_MODE_2_LEVEL:  /* 21: fase normal */
        default:                    /*  0: fora da fase, decide o game_mode */
            return true;
    }
}

static bool gb_platform_want_widescreen(const GBContext* ctx) {
    if (!g_widescreen_mode || !ctx || !ctx->eram || ctx->eram_size <= 0x08C3) {
        return false;
    }
    /* Passo 7v: o modo 2 volta a ter laterais (o case 2 foi tirado do switch
     * de gb_mode_is_side_scrolling por causa do COURSE e da carga, que agora
     * sao o 14 e o 23 do game_mode_2). */
    const uint8_t mode = ctx->eram[0x08C3];
    return (gb_mode_is_side_scrolling(mode) || mode == 2) &&
           gb_mode_2_wants_widescreen(gb_wl_game_mode_2(ctx));   /* passo 7r */
}

static bool gb_platform_should_black_fill_non_side_scrolling(const GBContext* ctx) {
    if (!g_widescreen_mode || !ctx || !ctx->eram || ctx->eram_size <= 0x08C3) {
        return false;
    }
    const uint8_t game_mode = ctx->eram[0x08C3];
    return (!gb_mode_is_side_scrolling(game_mode) && game_mode != 2) /*&& game_mode != 1*/ ||
           !gb_mode_2_wants_widescreen(gb_wl_game_mode_2(ctx));   /* passo 7r / 7v */
}

/* ============================================================================
 * Passo 8a: laterais sem cenario com fundo ambiente.
 * Nas telas em que o jogo so usa a tela original (titulo, COURSE, carga,
 * chefe, mapa se preparando...), as laterais eram pretas. Agora podem mostrar:
 *   1 = borda desfocada: a cor das colunas da borda da tela, esticada e
 *       desfocada;
 *   2 = centro desfocado: a propria tela ampliada, desfocada e escurecida
 *       atras (como video em pe).
 * Calculado em 1x (256x144) uma vez por quadro do jogo, com mistura com o
 * quadro anterior (sem piscar) e ampliado com interpolacao bilinear no HD.
 * As paredes das salas (fim da fase) continuam pretas.
 * ========================================================================== */
enum { WS_FILL_BLACK = 0, WS_FILL_EDGE = 1, WS_FILL_CENTER = 2 };
static int g_ws_fill_mode = WS_FILL_EDGE;
static int g_ws_fill_dim_percent = 45;   /* escurecimento das laterais */
static float g_ws_amb[GB_SCREEN_HEIGHT][GB_WIDESCREEN_WIDTH][3];
/* Passo 14c-1: o mesmo fundo em ponto fixo (valor * 256, arredondado), para a
 * ampliação HD ser feita só com inteiros (igual na CPU e na GPU). */
static uint16_t g_ws_amb_q[GB_SCREEN_HEIGHT][GB_WIDESCREEN_WIDTH][4];
static bool g_ws_amb_valid = false;
static int g_ws_amb_last_frame = -100;

static inline uint32_t ws_center_px(const uint32_t* src, bool wide, int x, int y) {
    if (x < 0) x = 0;
    if (x > GB_SCREEN_WIDTH - 1) x = GB_SCREEN_WIDTH - 1;
    if (y < 0) y = 0;
    if (y > GB_SCREEN_HEIGHT - 1) y = GB_SCREEN_HEIGHT - 1;
    return wide ? src[(size_t)y * GB_WIDESCREEN_WIDTH + GB_WIDESCREEN_MARGIN + x]
                : src[(size_t)y * GB_SCREEN_WIDTH + x];
}

/* Desfoque de caixa separavel (somas corridas), borda repetida. */
static void ws_box_blur_h(float (*img)[GB_WIDESCREEN_WIDTH][3], int r) {
    static float line[GB_WIDESCREEN_WIDTH][3];
    const int w = GB_WIDESCREEN_WIDTH;
    const float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        for (int c = 0; c < 3; c++) {
            float acc = 0.0f;
            for (int i = -r; i <= r; i++) acc += img[y][i < 0 ? 0 : (i >= w ? w - 1 : i)][c];
            for (int x = 0; x < w; x++) {
                line[x][c] = acc * inv;
                const int add = x + r + 1, sub = x - r;
                acc += img[y][add >= w ? w - 1 : add][c] - img[y][sub < 0 ? 0 : sub][c];
            }
        }
        memcpy(img[y], line, sizeof(line));
    }
}

static void ws_box_blur_v(float (*img)[GB_WIDESCREEN_WIDTH][3], int r) {
    static float col[GB_SCREEN_HEIGHT][3];
    const int h = GB_SCREEN_HEIGHT;
    const float inv = 1.0f / (float)(2 * r + 1);
    for (int x = 0; x < GB_WIDESCREEN_WIDTH; x++) {
        for (int c = 0; c < 3; c++) {
            float acc = 0.0f;
            for (int i = -r; i <= r; i++) acc += img[i < 0 ? 0 : (i >= h ? h - 1 : i)][x][c];
            for (int y = 0; y < h; y++) {
                col[y][c] = acc * inv;
                const int add = y + r + 1, sub = y - r;
                acc += img[add >= h ? h - 1 : add][x][c] - img[sub < 0 ? 0 : sub][x][c];
            }
        }
        for (int y = 0; y < h; y++) {
            img[y][x][0] = col[y][0];
            img[y][x][1] = col[y][1];
            img[y][x][2] = col[y][2];
        }
    }
}

static void ws_ambient_update(const uint32_t* src, bool wide, bool new_frame, int frame) {
    if (g_ws_amb_valid && !new_frame) {
        return;   /* quadro repetido (pausa, LCD desligado): mantem */
    }
    if (frame - g_ws_amb_last_frame > 2) {
        g_ws_amb_valid = false;   /* voltou depois de um tempo: nao mistura com o antigo */
    }
    g_ws_amb_last_frame = frame;

    static float pre[GB_SCREEN_HEIGHT][GB_WIDESCREEN_WIDTH][3];
    const int right0 = GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH;
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GB_WIDESCREEN_WIDTH; x++) {
            uint32_t c;
            if (g_ws_fill_mode == WS_FILL_EDGE) {
                const int cx = x < GB_WIDESCREEN_MARGIN ? 0
                             : (x >= right0 ? GB_SCREEN_WIDTH - 1 : x - GB_WIDESCREEN_MARGIN);
                c = ws_center_px(src, wide, cx, y);
            } else {
                /* tela ampliada 1,6x (160 -> 256), centrada */
                const float sx = 80.0f + ((float)x + 0.5f - 128.0f) * 0.625f;
                const float sy = 72.0f + ((float)y + 0.5f - 72.0f) * 0.625f;
                c = ws_center_px(src, wide, (int)sx, (int)sy);
            }
            pre[y][x][0] = (float)((c >> 16) & 0xFF);
            pre[y][x][1] = (float)((c >> 8) & 0xFF);
            pre[y][x][2] = (float)(c & 0xFF);
        }
    }
    const int rh = 6, rv = (g_ws_fill_mode == WS_FILL_EDGE) ? 10 : 6;
    for (int pass = 0; pass < 2; pass++) {
        ws_box_blur_h(pre, rh);
        ws_box_blur_v(pre, rv);
    }

    const float dim = 1.0f - (float)g_ws_fill_dim_percent / 100.0f;
    const float keep = g_ws_amb_valid ? 0.75f : 0.0f;
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < GB_WIDESCREEN_WIDTH; x++) {
            float t = 0.0f;   /* 0 junto da tela, 1 na borda da janela */
            if (x < GB_WIDESCREEN_MARGIN) t = (float)(GB_WIDESCREEN_MARGIN - x) / GB_WIDESCREEN_MARGIN;
            else if (x >= right0) t = (float)(x - right0 + 1) / GB_WIDESCREEN_MARGIN;
            const float f = dim * (1.0f - 0.35f * t);
            for (int c = 0; c < 3; c++) {
                const float v = pre[y][x][c] * f;
                g_ws_amb[y][x][c] = g_ws_amb[y][x][c] * keep + v * (1.0f - keep);
                const float q = g_ws_amb[y][x][c] * 256.0f + 0.5f;
                g_ws_amb_q[y][x][c] = (uint16_t)(q < 0.0f ? 0.0f : (q > 65535.0f ? 65535.0f : q));
            }
            g_ws_amb_q[y][x][3] = 0;
        }
    }
    g_ws_amb_valid = true;
}

static inline uint32_t ws_amb_pack(const float* p) {
    const int r = (int)(p[0] + 0.5f), g = (int)(p[1] + 0.5f), b = (int)(p[2] + 0.5f);
    return 0xFF000000u | ((uint32_t)(r > 255 ? 255 : r) << 16) |
           ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
}

/* Linha na resolucao base (1x). */
static void ws_ambient_row_1x(int y, uint32_t* row) {
    for (int x = 0; x < GB_WIDESCREEN_MARGIN; x++) {
        row[x] = ws_amb_pack(g_ws_amb[y][x]);
        const int xr = GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH + x;
        row[xr] = ws_amb_pack(g_ws_amb[y][xr]);
    }
}

/* Linhas HD (escala > 1) das laterais, com interpolacao bilinear.
 * Passo 14c-1: só com inteiros, a partir de g_ws_amb_q. Posição em unidades
 * de 1/(2*escala): fx = (X + 0.5)/escala - 0.5 vira 2X + 1 - escala. A GPU
 * faz a mesma conta (gpu_ppu, tela inteira). */
static inline uint32_t ws_amb_bilinear(int X, int Y, int scale, int lo, int hi) {
    const int d = 2 * scale;
    int fy = 2 * Y + 1 - scale;
    if (fy < 0) fy = 0;
    if (fy > d * (GB_SCREEN_HEIGHT - 1)) fy = d * (GB_SCREEN_HEIGHT - 1);
    const int y0 = fy / d, wy = fy - y0 * d;
    const int y1 = y0 + 1 < GB_SCREEN_HEIGHT ? y0 + 1 : y0;
    int fx = 2 * X + 1 - scale;
    if (fx < d * lo) fx = d * lo;
    if (fx > d * hi) fx = d * hi;
    const int x0 = fx / d, wx = fx - x0 * d;
    const int x1 = x0 + 1 <= hi ? x0 + 1 : x0;
    const uint32_t w00 = (uint32_t)((d - wx) * (d - wy)), w01 = (uint32_t)(wx * (d - wy));
    const uint32_t w10 = (uint32_t)((d - wx) * wy), w11 = (uint32_t)(wx * wy);
    const uint32_t den = (uint32_t)(d * d) * 256u;
    uint32_t out = 0xFF000000u;
    for (int c = 0; c < 3; c++) {
        const uint32_t v = (g_ws_amb_q[y0][x0][c] * w00 + g_ws_amb_q[y0][x1][c] * w01 +
                            g_ws_amb_q[y1][x0][c] * w10 + g_ws_amb_q[y1][x1][c] * w11 + den / 2u) / den;
        out |= (v > 255u ? 255u : v) << (16 - 8 * c);
    }
    return out;
}

static void ws_ambient_rows_hd(void* pixels, int pitch, int y, int scale) {
    const int right0 = GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH;
    for (int k = 0; k < scale; k++) {
        const int Y = y * scale + k;
        uint32_t* dst = (uint32_t*)((uint8_t*)pixels + (size_t)Y * pitch);
        for (int side = 0; side < 2; side++) {
            const int lo = side ? right0 : 0;
            const int hi = lo + GB_WIDESCREEN_MARGIN - 1;
            for (int X = lo * scale; X < (hi + 1) * scale; X++) {
                dst[X] = ws_amb_bilinear(X, Y, scale, lo, hi);
            }
        }
    }
}

static GBPortFrame g_port_frame = {};
static bool g_port_frame_valid = false;
static GBInputBinding g_keyboard_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static GBInputBinding g_controller_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_keyboard_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_controller_button_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_controller_axis_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_runtime_action_pressed[GB_INPUT_ACTION_COUNT] = {};
static Sint16 g_controller_axis_values[SDL_CONTROLLER_AXIS_MAX] = {};
static bool g_binding_capture_active = false;
static GBBindingCaptureDevice g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
static GBInputAction g_binding_capture_action = GB_INPUT_ACTION_RIGHT;
static int g_binding_capture_slot = 0;
static bool g_fast_forward_active = false;
static bool g_max_speed_mode = false;
/* passo 15a: nomes em português (o menu escolhe pelo idioma) */
static const char* g_input_action_names_pt[GB_INPUT_ACTION_COUNT] = {
    "Direita", "Esquerda", "Cima", "Baixo", "Game Boy A", "Game Boy B", "Select", "Start",
    "Avanco rapido (segurar)", "Velocidade maxima", "Salvar estado", "Carregar estado",
    "Posicao anterior", "Proxima posicao", "Mostrar overlay", "Mudo", "Abrir menu", "Painel do jogo",
};
static const char* action_name_ui(int a);
static const char* g_input_action_names[GB_INPUT_ACTION_COUNT] = {
    "Right",
    "Left",
    "Up",
    "Down",
    "Game Boy A",
    "Game Boy B",
    "Select",
    "Start",
    "Fast Forward (Hold)",
    "Toggle Max Speed",
    "Save State",
    "Load State",
    "Previous State Slot",
    "Next State Slot",
    "Toggle Overlay",
    "Toggle Mute",
    "Toggle Menu",
    "Toggle Game Panel",
};

static const char* action_name_ui(int a) { return g_ui_lang == 1 ? g_input_action_names_pt[a] : g_input_action_names[a]; }

static bool has_interpreter_activity(const GBContext* ctx) {
    return ctx != NULL &&
           (ctx->total_dispatch_fallbacks > 0 ||
            ctx->total_interpreter_entries > 0 ||
            ctx->has_unimplemented_interpreter_opcode);
}

static void update_audio_stats_from_ring(void);
static uint32_t current_audio_underruns(void);
static uint32_t current_audio_ring_fill_samples(void);
static uint32_t current_audio_ring_capacity(void);
static uint32_t audio_ring_fill_samples(void);
static void sdl_audio_callback(void* userdata, Uint8* stream, int len);
static void refresh_audio_output_devices(void);
static const char* current_audio_output_device_label(void);
static bool current_audio_output_device_available(void);
static void close_audio_output_device(void);
static bool reopen_audio_output_device(bool preserve_stats);
static void update_controller_axis_binding_state(void);
static void recompute_audio_targets(void);
static void refresh_audio_device_pause_state(void);
static void reset_audio_output_buffer(bool preserve_stats);
static char* trim_ascii(char* text);
static void update_effective_joypad_state(void);
static void save_runtime_preferences(void);
static bool save_savestate_slot(GBContext* ctx, int slot);
static bool load_savestate_slot(GBContext* ctx, int slot);
static bool delete_savestate_slot(GBContext* ctx, int slot);
static bool savestate_slot_exists(const GBContext* ctx, int slot, std::string* out_path);
static float settings_ui_scale_for_size(const ImVec2& display_size);
static const char* controller_menu_hint_text(void);
static bool input_action_is_runtime(GBInputAction action);
static int effective_speed_percent(void);
static void update_runtime_action_state(GBContext* ctx);

/* Joypad state - exported for gbrt.c to access */
uint8_t g_joypad_buttons = 0xFF;  /* Active low: Start, Select, B, A */
uint8_t g_joypad_dpad = 0xFF;     /* Active low: Down, Up, Left, Right */
static uint8_t g_manual_joypad_buttons = 0xFF;
static uint8_t g_manual_joypad_dpad = 0xFF;
static uint8_t g_script_joypad_buttons = 0xFF;
static uint8_t g_script_joypad_dpad = 0xFF;

/* ============================================================================
 * Automation State
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_SCRIPT_ENTRIES 2048
typedef enum {
    SCRIPT_ANCHOR_FRAME = 0,
    SCRIPT_ANCHOR_CYCLE = 1,
} ScriptAnchor;

typedef struct {
    ScriptAnchor anchor;
    uint64_t start;
    uint64_t end;
    uint64_t period;
    uint64_t duration;
    uint8_t dpad;    /* Active LOW mask to apply (0 = Pressed) */
    uint8_t buttons; /* Active LOW mask to apply (0 = Pressed) */
} ScriptEntry;

static ScriptEntry g_input_script[MAX_SCRIPT_ENTRIES];
static int g_script_count = 0;
static FILE* g_input_record_file = NULL;
static bool g_input_record_exit_handler_registered = false;
static bool g_input_record_wrote_entry = false;
static bool g_input_record_has_segment = false;
static uint64_t g_input_record_start_cycle = 0;
static uint64_t g_input_record_end_cycle = 0;
static uint8_t g_input_record_dpad = 0xFF;
static uint8_t g_input_record_buttons = 0xFF;

#define MAX_DUMP_FRAMES 100
static uint32_t g_dump_frames[MAX_DUMP_FRAMES];
static int g_dump_count = 0;
static uint32_t g_dump_present_frames[MAX_DUMP_FRAMES];
static int g_dump_present_count = 0;
static std::string g_screenshot_prefix = "screenshot";

static bool env_flag_enabled(const char* name) {
    const char* value = SDL_getenv(name);
    if (!value || !value[0]) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0 ||
           strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

static bool platform_default_fullscreen(void) {
    return true;   /* passo 14p: tela cheia padrão em todos (antes só no Android) */
}

static bool platform_uses_app_storage_for_relative_paths(void) {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

static const char* overlay_menu_hint_text(void) {
#if defined(__ANDROID__)
    return "Press Back, L3, or R3 for Menu";
#else
    return "Press ESC for Menu";
#endif
}

static float settings_ui_scale_for_size(const ImVec2& display_size) {
    float min_dimension = display_size.x < display_size.y ? display_size.x : display_size.y;
    if (min_dimension <= 0.0f) {
        return 1.0f;
    }

    

#if defined(__ANDROID__)
    float scale = min_dimension / 520.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 1.65f) scale = 1.65f;
#else
    float scale = min_dimension / 720.0f;
    if (scale < 0.95f) scale = 0.95f;
    if (scale > 1.25f) scale = 1.25f;
#endif

    return scale;
}

static const char* controller_menu_hint_text(void) {
#if defined(__ANDROID__)
    return "Guide, L3, R3";
#else
    return "Guide / Home";
#endif
}

static bool input_action_is_runtime(GBInputAction action) {
    return action >= GB_JOYPAD_ACTION_COUNT && action < GB_INPUT_ACTION_COUNT;
}

static int effective_speed_percent(void) {
    int speed_percent = (g_speed_percent > 0) ? g_speed_percent : 100;
    if (g_max_speed_mode) {
        return GB_MAX_SHORTCUT_SPEED_PERCENT;
    }
    if (g_fast_forward_active && speed_percent < GB_FAST_FORWARD_SPEED_PERCENT) {
        return GB_FAST_FORWARD_SPEED_PERCENT;
    }
    return speed_percent;
}

static const char* overlay_visibility_hint_text(void) {
#if defined(__ANDROID__)
    return "Use the settings menu or a shortcut binding to toggle the overlay";
#else
    return "Use F1 or a shortcut binding for Overlay";
#endif
}

static bool string_contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !haystack[0] || !needle[0]) {
        return false;
    }

    std::string text(haystack);
    std::string pattern(needle);
    for (char& ch : text) ch = (char)std::tolower((unsigned char)ch);
    for (char& ch : pattern) ch = (char)std::tolower((unsigned char)ch);
    return text.find(pattern) != std::string::npos;
}

static bool should_ignore_controller_name(const char* name) {
    return string_contains_case_insensitive(name, "qwerty") ||
           string_contains_case_insensitive(name, "keyboard") ||
           string_contains_case_insensitive(name, "keypad");
}

static const char* controller_type_name(SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3: return "PlayStation 3";
        case SDL_CONTROLLER_TYPE_PS4: return "PlayStation 4";
        case SDL_CONTROLLER_TYPE_PS5: return "PlayStation 5";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Nintendo Switch Pro";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con Left";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con Right";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con Pair";
        case SDL_CONTROLLER_TYPE_GOOGLE_STADIA: return "Stadia";
        case SDL_CONTROLLER_TYPE_AMAZON_LUNA: return "Luna";
        case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD: return "NVIDIA Shield";
        case SDL_CONTROLLER_TYPE_VIRTUAL: return "Virtual";
        case SDL_CONTROLLER_TYPE_UNKNOWN:
        default:
            return "Unknown";
    }
}

static GBControllerLabelProfile detect_controller_label_profile(SDL_GameController* controller) {
    if (controller) {
        const SDL_GameControllerType type = SDL_GameControllerGetType(controller);
        switch (type) {
            case SDL_CONTROLLER_TYPE_XBOX360:
            case SDL_CONTROLLER_TYPE_XBOXONE:
            case SDL_CONTROLLER_TYPE_GOOGLE_STADIA:
            case SDL_CONTROLLER_TYPE_AMAZON_LUNA:
            case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD:
                return GB_CONTROLLER_LABEL_XBOX;

            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
            case SDL_CONTROLLER_TYPE_PS5:
                return GB_CONTROLLER_LABEL_PLAYSTATION;

            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                return GB_CONTROLLER_LABEL_NINTENDO;

            default:
                break;
        }

        const char* name = SDL_GameControllerName(controller);
        if (string_contains_case_insensitive(name, "xbox") ||
            string_contains_case_insensitive(name, "xinput") ||
            string_contains_case_insensitive(name, "stadia") ||
            string_contains_case_insensitive(name, "luna") ||
            string_contains_case_insensitive(name, "shield") ||
            string_contains_case_insensitive(name, "kishi") ||
            string_contains_case_insensitive(name, "odin") ||
            string_contains_case_insensitive(name, "retroid")) {
            return GB_CONTROLLER_LABEL_XBOX;
        }
        if (string_contains_case_insensitive(name, "playstation") ||
            string_contains_case_insensitive(name, "dualshock") ||
            string_contains_case_insensitive(name, "dualsense") ||
            string_contains_case_insensitive(name, "ps3") ||
            string_contains_case_insensitive(name, "ps4") ||
            string_contains_case_insensitive(name, "ps5")) {
            return GB_CONTROLLER_LABEL_PLAYSTATION;
        }
        if (string_contains_case_insensitive(name, "switch") ||
            string_contains_case_insensitive(name, "joy-con") ||
            string_contains_case_insensitive(name, "joycon") ||
            string_contains_case_insensitive(name, "nintendo")) {
            return GB_CONTROLLER_LABEL_NINTENDO;
        }
    }

#if defined(__ANDROID__)
    return GB_CONTROLLER_LABEL_XBOX;
#else
    return GB_CONTROLLER_LABEL_GENERIC;
#endif
}

static const char* controller_face_south_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "A (Bottom)";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Cross (Bottom)";
        case GB_CONTROLLER_LABEL_NINTENDO: return "B (Bottom)";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "South / Bottom";
    }
}

static const char* controller_face_east_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "B (Right)";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Circle (Right)";
        case GB_CONTROLLER_LABEL_NINTENDO: return "A (Right)";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "East / Right";
    }
}

static const char* controller_left_shoulder_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "LB";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "L1";
        case GB_CONTROLLER_LABEL_NINTENDO: return "L";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Left Shoulder";
    }
}

static const char* controller_right_shoulder_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "RB";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "R1";
        case GB_CONTROLLER_LABEL_NINTENDO: return "R";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Right Shoulder";
    }
}

static const char* controller_back_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "View / Back";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Create / Share";
        case GB_CONTROLLER_LABEL_NINTENDO: return "-";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Back / Select";
    }
}

static const char* controller_start_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "Menu / Start";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Options";
        case GB_CONTROLLER_LABEL_NINTENDO: return "+";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Start";
    }
}

static const char* controller_guide_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "Xbox / Guide";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "PS";
        case GB_CONTROLLER_LABEL_NINTENDO: return "Home";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Guide / Home";
    }
}

static const char* controller_button_label(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return controller_face_south_label();
        case SDL_CONTROLLER_BUTTON_B: return controller_face_east_label();
        case SDL_CONTROLLER_BUTTON_X: return "West Face";
        case SDL_CONTROLLER_BUTTON_Y: return "North Face";
        case SDL_CONTROLLER_BUTTON_BACK: return controller_back_label();
        case SDL_CONTROLLER_BUTTON_GUIDE: return controller_guide_label();
        case SDL_CONTROLLER_BUTTON_START: return controller_start_label();
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return controller_left_shoulder_label();
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return controller_right_shoulder_label();
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left Stick Click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right Stick Click";
        default:
            return "Controller Button";
    }
}

static const char* controller_axis_label(SDL_GameControllerAxis axis, bool positive_direction) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: return positive_direction ? "Left Stick Right" : "Left Stick Left";
        case SDL_CONTROLLER_AXIS_LEFTY: return positive_direction ? "Left Stick Down" : "Left Stick Up";
        case SDL_CONTROLLER_AXIS_RIGHTX: return positive_direction ? "Right Stick Right" : "Right Stick Left";
        case SDL_CONTROLLER_AXIS_RIGHTY: return positive_direction ? "Right Stick Down" : "Right Stick Up";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Left Trigger";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Right Trigger";
        default:
            return positive_direction ? "Axis +" : "Axis -";
    }
}

static std::string binding_display_label(const GBInputBinding& binding) {
    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY: {
            const char* name = SDL_GetScancodeName((SDL_Scancode)binding.code);
            return (name && name[0]) ? std::string(name) : ("Scancode " + std::to_string((int)binding.code));
        }

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            return controller_button_label((SDL_GameControllerButton)binding.code);

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
            return controller_axis_label((SDL_GameControllerAxis)binding.code, true);

        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            return controller_axis_label((SDL_GameControllerAxis)binding.code, false);

        case GB_INPUT_BINDING_NONE:
        default:
            return "Unbound";
    }
}

static void start_binding_capture(GBBindingCaptureDevice device, GBInputAction action, int slot) {
    g_binding_capture_active = true;
    g_binding_capture_device = device;
    g_binding_capture_action = action;
    g_binding_capture_slot = slot;
}

static void cancel_binding_capture(void) {
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    g_binding_capture_slot = 0;
}

static void assign_binding_slot(GBBindingCaptureDevice device,
                                GBInputAction action,
                                int slot,
                                const GBInputBinding& binding) {
    if (slot < 0 || slot >= 2) {
        return;
    }

    if (device == GB_CAPTURE_DEVICE_KEYBOARD) {
        g_keyboard_bindings[action][slot] = binding;
        g_keyboard_binding_pressed[action][slot] = false;
    } else if (device == GB_CAPTURE_DEVICE_CONTROLLER) {
        g_controller_bindings[action][slot] = binding;
        g_controller_button_binding_pressed[action][slot] = false;
        g_controller_axis_binding_pressed[action][slot] = false;
        update_controller_axis_binding_state();
    }

    update_effective_joypad_state();
    save_runtime_preferences();
}

static void commit_binding_capture(const GBInputBinding& binding) {
    if (!g_binding_capture_active || g_binding_capture_slot < 0 || g_binding_capture_slot >= 2) {
        cancel_binding_capture();
        return;
    }

    assign_binding_slot(g_binding_capture_device, g_binding_capture_action, g_binding_capture_slot, binding);
    cancel_binding_capture();
}

static void clear_controller_state(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    memset(g_controller_button_binding_pressed, 0, sizeof(g_controller_button_binding_pressed));
    memset(g_controller_axis_binding_pressed, 0, sizeof(g_controller_axis_binding_pressed));
    memset(g_controller_axis_values, 0, sizeof(g_controller_axis_values));
    g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
    g_controller_label_profile = detect_controller_label_profile(NULL);
    g_controller_name.clear();
    update_effective_joypad_state();
}

static void refresh_controller_profile(void) {
    if (!g_controller) {
        g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
        g_controller_label_profile = detect_controller_label_profile(NULL);
        g_controller_name.clear();
        return;
    }

    g_controller_type = SDL_GameControllerGetType(g_controller);
    g_controller_label_profile = detect_controller_label_profile(g_controller);
    const char* name = SDL_GameControllerName(g_controller);
    g_controller_name = (name && name[0]) ? name : "Controller";
    fprintf(stderr,
            "[SDL] Controller: %s [%s]\n",
            g_controller_name.c_str(),
            controller_type_name(g_controller_type));
}

static bool open_controller_index(int joystick_index) {
    if (joystick_index < 0 || joystick_index >= SDL_NumJoysticks() || !SDL_IsGameController(joystick_index)) {
        return false;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(joystick_index);
    if (!controller) {
        return false;
    }

    if (should_ignore_controller_name(SDL_GameControllerName(controller))) {
        SDL_GameControllerClose(controller);
        return false;
    }

    clear_controller_state();
    g_controller = controller;
    refresh_controller_profile();
    return true;
}

static bool open_first_available_controller(void) {
    if (g_controller) {
        return true;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (open_controller_index(i)) {
            return true;
        }
    }
    return false;
}

static void install_supplemental_controller_mappings(void) {
#if defined(__APPLE__)
    // SDL2's built-in database does not yet cover the wired 8BitDo Ultimate
    // 2C on macOS. Do not replace a user or future SDL mapping when one is
    // already available.
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i) ||
            SDL_JoystickGetDeviceVendor(i) != 0x2dc8 ||
            SDL_JoystickGetDeviceProduct(i) != 0x301d) {
            continue;
        }
        SDL_GameControllerAddMapping(
            "03000000c82d00001d30000001000000,"
            "8BitDo Ultimate 2C Wired Controller,"
            "a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
            "dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,"
            "lefttrigger:a5,leftx:a0,lefty:a1,paddle1:b5,paddle2:b2,"
            "rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,"
            "righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,"
        );
    }
#endif
}

static SDL_JoystickID active_controller_instance_id(void) {
    if (!g_controller) {
        return -1;
    }
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(g_controller);
    return joystick ? SDL_JoystickInstanceID(joystick) : -1;
}

static bool ensure_parent_directory(const fs::path& path) {
    if (path.parent_path().empty()) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return !ec;
}

static std::string extract_path_leaf(const char* path) {
    if (!path || !path[0]) {
        return "game";
    }
    try {
        fs::path input(path);
        std::string leaf = input.filename().string();
        return leaf.empty() ? std::string(path) : leaf;
    } catch (...) {
        return std::string(path);
    }
}

static std::string make_pref_storage_dir(const char* app_component) {
    const char* safe_component = (app_component && app_component[0]) ? app_component : "runtime";
    char* pref_path = SDL_GetPrefPath("gbrecompiled", safe_component);
    if (!pref_path) {
        return std::string();
    }
    std::string result(pref_path);
    SDL_free(pref_path);
    return result;
}

static std::string resolve_writable_path(const char* requested_path, const char* app_component) {
    if (!requested_path || !requested_path[0]) {
        return std::string();
    }

    fs::path requested(requested_path);
    if (!platform_uses_app_storage_for_relative_paths() || requested.is_absolute()) {
        return requested.lexically_normal().string();
    }

    const std::string pref_dir = make_pref_storage_dir(app_component);
    if (pref_dir.empty()) {
        return requested.lexically_normal().string();
    }

    fs::path resolved = fs::path(pref_dir) / requested;
    ensure_parent_directory(resolved);
    return resolved.lexically_normal().string();
}

static GBInputBinding make_binding(GBInputBindingKind kind, int code) {
    GBInputBinding binding = {};
    binding.kind = kind;
    binding.code = (int16_t)code;
    return binding;
}

static const char* input_action_config_name(GBInputAction action) {
    switch (action) {
        case GB_INPUT_ACTION_RIGHT: return "right";
        case GB_INPUT_ACTION_LEFT: return "left";
        case GB_INPUT_ACTION_UP: return "up";
        case GB_INPUT_ACTION_DOWN: return "down";
        case GB_INPUT_ACTION_A: return "a";
        case GB_INPUT_ACTION_B: return "b";
        case GB_INPUT_ACTION_SELECT: return "select";
        case GB_INPUT_ACTION_START: return "start";
        case GB_INPUT_ACTION_FAST_FORWARD: return "fast_forward";
        case GB_INPUT_ACTION_TOGGLE_MAX_SPEED: return "toggle_max_speed";
        case GB_INPUT_ACTION_SAVE_STATE: return "save_state";
        case GB_INPUT_ACTION_LOAD_STATE: return "load_state";
        case GB_INPUT_ACTION_PREVIOUS_STATE_SLOT: return "previous_state_slot";
        case GB_INPUT_ACTION_NEXT_STATE_SLOT: return "next_state_slot";
        case GB_INPUT_ACTION_TOGGLE_OVERLAY: return "toggle_overlay";
        case GB_INPUT_ACTION_TOGGLE_MUTE: return "toggle_mute";
        case GB_INPUT_ACTION_TOGGLE_MENU: return "toggle_menu";
        case GB_INPUT_ACTION_TOGGLE_PORT_UI: return "toggle_port_ui";
        case GB_INPUT_ACTION_COUNT:
        default:
            return "unknown";
    }
}

static bool parse_input_action_name(const char* text, GBInputAction* out_action) {
    if (!text || !out_action) {
        return false;
    }

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        if (strcmp(text, input_action_config_name((GBInputAction)action)) == 0) {
            *out_action = (GBInputAction)action;
            return true;
        }
    }

    return false;
}

static void clear_all_binding_pressed_state(void) {
    memset(g_keyboard_binding_pressed, 0, sizeof(g_keyboard_binding_pressed));
    memset(g_controller_button_binding_pressed, 0, sizeof(g_controller_button_binding_pressed));
    memset(g_controller_axis_binding_pressed, 0, sizeof(g_controller_axis_binding_pressed));
    memset(g_runtime_action_pressed, 0, sizeof(g_runtime_action_pressed));
    memset(g_controller_axis_values, 0, sizeof(g_controller_axis_values));
    g_fast_forward_active = false;
}

static bool binding_is_valid(const GBInputBinding& binding) {
    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY:
            return binding.code > SDL_SCANCODE_UNKNOWN && binding.code < SDL_NUM_SCANCODES;

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            return binding.code >= 0 && binding.code < SDL_CONTROLLER_BUTTON_MAX;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            return binding.code >= 0 && binding.code < SDL_CONTROLLER_AXIS_MAX;

        case GB_INPUT_BINDING_NONE:
        default:
            return false;
    }
}

static bool binding_matches_scancode(const GBInputBinding& binding, SDL_Scancode scancode) {
    return binding.kind == GB_INPUT_BINDING_KEY && binding.code == (int)scancode;
}

static bool binding_matches_controller_button(const GBInputBinding& binding, uint8_t button) {
    return binding.kind == GB_INPUT_BINDING_CONTROLLER_BUTTON && binding.code == (int)button;
}

static bool binding_active_for_axis_value(const GBInputBinding& binding, Sint16 value) {
    const Sint16 threshold = 16000;
    if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE) {
        return value >= threshold;
    }
    if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE) {
        return value <= -threshold;
    }
    return false;
}

static void set_default_audio_preferences(void) {
    g_audio_output_enabled = true;
    g_audio_muted.store(false, std::memory_order_relaxed);
    g_audio_latency_ms = 80;
    g_audio_volume_percent.store(100, std::memory_order_relaxed);
    g_audio_gb_volume_percent.store(100, std::memory_order_relaxed); /* passo 6e */
    g_audio_target_device_name.clear();
}

static void set_default_input_bindings(void) {
    memset(g_keyboard_bindings, 0, sizeof(g_keyboard_bindings));
    memset(g_controller_bindings, 0, sizeof(g_controller_bindings));

    g_keyboard_bindings[GB_INPUT_ACTION_UP][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_UP);
    g_keyboard_bindings[GB_INPUT_ACTION_UP][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_W);
    g_keyboard_bindings[GB_INPUT_ACTION_DOWN][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_DOWN);
    g_keyboard_bindings[GB_INPUT_ACTION_DOWN][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_S);
    g_keyboard_bindings[GB_INPUT_ACTION_LEFT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_LEFT);
    g_keyboard_bindings[GB_INPUT_ACTION_LEFT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_A);
    g_keyboard_bindings[GB_INPUT_ACTION_RIGHT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RIGHT);
    g_keyboard_bindings[GB_INPUT_ACTION_RIGHT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_D);
    g_keyboard_bindings[GB_INPUT_ACTION_A][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_Z);
    g_keyboard_bindings[GB_INPUT_ACTION_A][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_J);
    g_keyboard_bindings[GB_INPUT_ACTION_B][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_X);
    g_keyboard_bindings[GB_INPUT_ACTION_B][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_K);
    g_keyboard_bindings[GB_INPUT_ACTION_SELECT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_BACKSPACE);
    g_keyboard_bindings[GB_INPUT_ACTION_SELECT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RSHIFT);
    g_keyboard_bindings[GB_INPUT_ACTION_START][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RETURN);
    g_keyboard_bindings[GB_INPUT_ACTION_FAST_FORWARD][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_TAB);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MAX_SPEED][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_GRAVE);
    g_keyboard_bindings[GB_INPUT_ACTION_SAVE_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F5);
    g_keyboard_bindings[GB_INPUT_ACTION_LOAD_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F8);
    g_keyboard_bindings[GB_INPUT_ACTION_PREVIOUS_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F6);
    g_keyboard_bindings[GB_INPUT_ACTION_NEXT_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F7);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_OVERLAY][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F1);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MUTE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_M);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MENU][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F10);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_PORT_UI][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F2);

    g_controller_bindings[GB_INPUT_ACTION_UP][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_UP);
    g_controller_bindings[GB_INPUT_ACTION_UP][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, SDL_CONTROLLER_AXIS_LEFTY);
    g_controller_bindings[GB_INPUT_ACTION_DOWN][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    g_controller_bindings[GB_INPUT_ACTION_DOWN][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_LEFTY);
    g_controller_bindings[GB_INPUT_ACTION_LEFT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    g_controller_bindings[GB_INPUT_ACTION_LEFT][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, SDL_CONTROLLER_AXIS_LEFTX);
    g_controller_bindings[GB_INPUT_ACTION_RIGHT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    g_controller_bindings[GB_INPUT_ACTION_RIGHT][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_LEFTX);
    g_controller_bindings[GB_INPUT_ACTION_A][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_B);
    g_controller_bindings[GB_INPUT_ACTION_A][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    g_controller_bindings[GB_INPUT_ACTION_B][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_A);
    g_controller_bindings[GB_INPUT_ACTION_B][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    g_controller_bindings[GB_INPUT_ACTION_SELECT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_BACK);
    g_controller_bindings[GB_INPUT_ACTION_START][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_START);
    g_controller_bindings[GB_INPUT_ACTION_FAST_FORWARD][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MAX_SPEED][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    g_controller_bindings[GB_INPUT_ACTION_SAVE_STATE][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_X);
    g_controller_bindings[GB_INPUT_ACTION_LOAD_STATE][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_Y);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_PORT_UI][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSTICK);

    clear_all_binding_pressed_state();
}

static std::string runtime_preferences_path(void) {
    const std::string pref_dir = make_pref_storage_dir("runtime");
    if (!pref_dir.empty()) {
        fs::path resolved = fs::path(pref_dir) / "runtime_prefs.ini";
        ensure_parent_directory(resolved);
        return resolved.lexically_normal().string();
    }
    return fs::path("runtime_prefs.ini").lexically_normal().string();
}

static void load_runtime_preferences(void) {
    set_default_audio_preferences();
    set_default_input_bindings();
    touch_controls::set_enabled(touch_controls::enabled_by_default());   /* [android-port] */
    hd_pack_io_clear_disabled_names();
    g_savestate_slot = 0;
    g_savestate_status.clear();
    g_max_speed_mode = false;
    bool saw_controller_port_ui_binding = false;

    const std::string path = runtime_preferences_path();
    if (!path.empty()) {
        FILE* file = fopen(path.c_str(), "r");
        if (file) {
            char line[256];
            while (fgets(line, sizeof(line), file)) {
                char* text = trim_ascii(line);
                if (!text || !text[0] || text[0] == '#') {
                    continue;
                }

                char* equals = strchr(text, '=');
                if (!equals) {
                    continue;
                }

                *equals = '\0';
                char* key = trim_ascii(text);
                char* value = trim_ascii(equals + 1);
                if (!key || !value || !key[0]) {
                    continue;
                }

                if (strcmp(key, "audio.enabled") == 0) {
                    g_audio_output_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "audio.muted") == 0) {
                    g_audio_muted.store(strcmp(value, "0") != 0, std::memory_order_relaxed);
                    continue;
                }
                if (strcmp(key, "audio.latency_ms") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed > 0) {
                        g_audio_latency_ms = (uint32_t)parsed;
                    }
                    continue;
                }
if (strcmp(key, "display.widescreen_fill_mode") == 0) {   /* passo 8a */
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= WS_FILL_BLACK && parsed <= WS_FILL_CENTER) {
                        g_ws_fill_mode = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "display.widescreen_fill_dim") == 0) {   /* passo 8a */
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 90) {
                        g_ws_fill_dim_percent = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "audio.gb_volume_percent") == 0) {   /* passo 6e */
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 200) {
                        g_audio_gb_volume_percent.store((uint32_t)parsed, std::memory_order_relaxed);
                    }
                    continue;
                }
                if (strcmp(key, "audio.volume_percent") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0) {
                        if (parsed > 200) parsed = 200;
                        g_audio_volume_percent.store((uint32_t)parsed, std::memory_order_relaxed);
                    }
                    continue;
                }
                if (strcmp(key, "audio.device_name") == 0) {
                    g_audio_target_device_name = value;
                    continue;
                }
                if (strcmp(key, "savestate.slot") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < GB_SAVESTATE_SLOT_COUNT) {
                        g_savestate_slot = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "display.fullscreen") == 0) {
                    g_fullscreen = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "display.widescreen_v2") == 0) {   /* [fix-v2] */
                    g_widescreen_mode = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "display.widescreen_fade_enabled") == 0) {
                    g_widescreen_fade_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "display.widescreen_fade_percent") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed < 0) parsed = 0;
                    if (parsed > 100) parsed = 100;
                    g_widescreen_fade_percent = (int)parsed;
                    continue;
                }
                if (strcmp(key, "display.internal_scale") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 1 && parsed <= IM_ARRAYSIZE(g_internal_res_names)) {
                        g_internal_res_scale = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "display.scale_filter") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < IM_ARRAYSIZE(g_render_filter_names)) {
                        g_render_filter_mode = (GBRenderFilterMode)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "display.post_filter") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < GB_POST_FILTER_COUNT) {
                        g_post_filter_mode = (GBPostFilterMode)parsed;
                    }
                    continue;
                }                
                if (strcmp(key, "mods.disabled") == 0) {
                    hd_pack_io_add_disabled_name(value);
                    continue;
                }
                /* Passo 6d: trilhas .ogg e registro das musicas */
                if (strcmp(key, "music.enabled") == 0) {
                    music_replace_set_enabled(strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "music.volume_percent") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 200) {
                        music_replace_set_volume((float)parsed / 100.0f);
                    }
                    continue;
                }
                if (strcmp(key, "music.log") == 0) {
                    g_music_log = (strcmp(value, "0") != 0);
                    music_replace_set_log(g_music_log);
                    continue;
                }
                if (strcmp(key, "input.touch_controls") == 0) {   /* [android-port] */
                    touch_controls::set_enabled(strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "display.hd_tiles") == 0) {
                    ppu_native_hd_set_tiles_enabled(strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "display.video_backend") == 0) {   /* passo 14a */
                    g_video_backend_pref = (strtol(value, NULL, 10) == 1) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.mode3d") == 0) {   /* passo 3D-1 */
                    g_3d_mode = (strtol(value, NULL, 10) == 1) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.tilt3d") == 0) {
                    long v = strtol(value, NULL, 10);
                    g_3d_tilt = (int)(v < -35 ? -35 : (v > 35 ? 35 : v));   /* 3D-4t: negativo = por baixo */
                    continue;
                }
                if (strcmp(key, "ui.language") == 0) {   /* passo 15a */
                    g_ui_lang = (strtol(value, NULL, 10) == 1) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "ui.menu_page") == 0) {   /* passo 15a */
                    long v = strtol(value, NULL, 10);
                    g_menu_page = (int)(v < 0 ? 0 : (v > 6 ? 6 : v));
                    continue;
                }
                if (strcmp(key, "display.follow3d") == 0) {   /* passo 3D-4q */
                    g_3d_follow = (strtol(value, NULL, 10) != 0) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.stick3d") == 0) {   /* passo 3D-5a */
                    g_3d_stick = (strtol(value, NULL, 10) != 0) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.front3d") == 0) {   /* passo 3D-6a */
                    long v = strtol(value, NULL, 10);
                    g_3d_front = (int)(v < 0 ? 0 : (v > 300 ? 300 : v));
                    continue;
                }
                if (strcmp(key, "display.spr3d") == 0) {   /* passo 3D-5e */
                    long v = strtol(value, NULL, 10);
                    g_3d_spr = (int)(v < 5 ? 5 : (v > 100 ? 100 : v));
                    continue;
                }
                if (strcmp(key, "display.yaw3d") == 0) {   /* passo 3D-3e */
                    long v = strtol(value, NULL, 10);
                    g_3d_yaw = (int)(v < -35 ? -35 : (v > 35 ? 35 : v));
                    continue;
                }
                if (strcmp(key, "display.depth3d") == 0) {
                    long v = strtol(value, NULL, 10);
                    g_3d_depth = (int)(v < 0 ? 0 : (v > 200 ? 200 : v));
                    continue;
                }
                if (strcmp(key, "display.gpu_screen") == 0) {   /* passo 14c-2 */
                    g_gpu_screen_pref = (strtol(value, NULL, 10) == 1) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.fps_mode") == 0) {   /* passo 10a */
                    g_fps_mode = (strtol(value, NULL, 10) == 1) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "display.renderer") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < PPU_RENDERER_COUNT) {
                        ppu_native_set_mode((PPURendererMode)parsed);
                    }
                    continue;
                }
                bool is_keyboard = strncmp(key, "keyboard.", 9) == 0;
                bool is_controller = strncmp(key, "controller.", 11) == 0;
                if (!is_keyboard && !is_controller) {
                    continue;
                }

                char* section = key + (is_keyboard ? 9 : 11);
                char* dot = strrchr(section, '.');
                if (!dot) {
                    continue;
                }
                *dot = '\0';
                char* slot_text = dot + 1;
                long slot = strtol(slot_text, NULL, 10);
                if (slot < 0 || slot >= 2) {
                    continue;
                }

                GBInputAction action = GB_INPUT_ACTION_RIGHT;
                if (!parse_input_action_name(section, &action)) {
                    continue;
                }

                GBInputBinding binding = {};
                if (strcmp(value, "none") == 0) {
                    binding = make_binding(GB_INPUT_BINDING_NONE, 0);
                } else if (is_keyboard) {
                    if (strncmp(value, "key:", 4) != 0) {
                        continue;
                    }
                    long code = strtol(value + 4, NULL, 10);
                    binding = make_binding(GB_INPUT_BINDING_KEY, (int)code);
                } else if (strncmp(value, "button:", 7) == 0) {
                    long code = strtol(value + 7, NULL, 10);
                    binding = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, (int)code);
                } else if (strncmp(value, "axis:", 5) == 0) {
                    char* value_copy = strdup(value + 5);
                    if (!value_copy) {
                        continue;
                    }
                    char* axis_text = strtok(value_copy, ":");
                    char* direction_text = strtok(NULL, ":");
                    if (axis_text && direction_text) {
                        long axis = strtol(axis_text, NULL, 10);
                        binding = make_binding(
                            (strcmp(direction_text, "+") == 0) ? GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE
                                                                 : GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE,
                            (int)axis);
                    }
                    free(value_copy);
                } else {
                    continue;
                }

                if (!binding_is_valid(binding) && binding.kind != GB_INPUT_BINDING_NONE) {
                    continue;
                }

                if (is_keyboard) {
                    g_keyboard_bindings[action][slot] = binding;
                } else {
                    g_controller_bindings[action][slot] = binding;
                    if (action == GB_INPUT_ACTION_TOGGLE_PORT_UI) {
                        saw_controller_port_ui_binding = true;
                    }
                }
            }
            fclose(file);
        }
    }

    /*
     * Runtime preferences written before the game-panel action existed stored
     * the old default R3 binding as Toggle Menu slot 1. Preserve every custom
     * binding, but migrate that one recognizable legacy default so an upgrade
     * cannot open both the settings menu and the native game panel at once.
     * Once a file contains any explicit game-panel controller binding (even
     * "none"), the user's current choices are authoritative.
     */
    const GBInputBinding& legacy_menu_binding =
        g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][1];
    if (!saw_controller_port_ui_binding &&
        legacy_menu_binding.kind == GB_INPUT_BINDING_CONTROLLER_BUTTON &&
        legacy_menu_binding.code == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
        g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][1] =
            make_binding(GB_INPUT_BINDING_NONE, 0);
    }
    update_effective_joypad_state();
}

static void binding_to_config_value(const GBInputBinding& binding, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY:
            snprintf(out, out_size, "key:%d", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            snprintf(out, out_size, "button:%d", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
            snprintf(out, out_size, "axis:%d:+", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            snprintf(out, out_size, "axis:%d:-", (int)binding.code);
            break;

        case GB_INPUT_BINDING_NONE:
        default:
            snprintf(out, out_size, "none");
            break;
    }
}

static void save_runtime_preferences(void) {
    const std::string path = runtime_preferences_path();
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "w");
    if (!file) {
        fprintf(stderr, "[SDL] Failed to save runtime prefs to %s\n", path.c_str());
        return;
    }

    fprintf(file, "audio.enabled=%d\n", g_audio_output_enabled ? 1 : 0);
    fprintf(file, "audio.muted=%d\n",
            g_audio_muted.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(file, "audio.latency_ms=%u\n", g_audio_latency_ms);
    fprintf(file, "audio.volume_percent=%u\n",
            g_audio_volume_percent.load(std::memory_order_relaxed));
    fprintf(file, "audio.gb_volume_percent=%u\n",   /* passo 6e */
            g_audio_gb_volume_percent.load(std::memory_order_relaxed));
    fprintf(file, "audio.device_name=%s\n", g_audio_target_device_name.c_str());
    fprintf(file, "savestate.slot=%d\n", g_savestate_slot);
    fprintf(file, "display.fullscreen=%d\n", g_fullscreen ? 1 : 0);
    fprintf(file, "display.widescreen_v2=%d\n", g_widescreen_mode ? 1 : 0);   /* [fix-v2] */
    fprintf(file, "display.widescreen_fade_enabled=%d\n", g_widescreen_fade_enabled ? 1 : 0);
    fprintf(file, "display.widescreen_fill_mode=%d\n", g_ws_fill_mode);          /* passo 8a */
    fprintf(file, "display.widescreen_fill_dim=%d\n", g_ws_fill_dim_percent);    /* passo 8a */
    fprintf(file, "display.widescreen_fade_percent=%d\n", g_widescreen_fade_percent);
    fprintf(file, "display.internal_scale=%d\n", g_internal_res_scale);
    fprintf(file, "display.scale_filter=%d\n", (int)g_render_filter_mode);
    fprintf(file, "display.post_filter=%d\n", (int)g_post_filter_mode);
    fprintf(file, "display.renderer=%d\n", (int)ppu_native_get_mode());
    fprintf(file, "display.fps_mode=%d\n", g_fps_mode);   /* passo 10a */
    fprintf(file, "display.gpu_screen=%d\n", g_gpu_screen_pref);   /* passo 14c-2 */
    fprintf(file, "display.mode3d=%d\n", g_3d_mode);                /* passo 3D-1 */
    fprintf(file, "display.tilt3d=%d\n", g_3d_tilt);
    fprintf(file, "display.depth3d=%d\n", g_3d_depth);
    fprintf(file, "display.yaw3d=%d\n", g_3d_yaw);
    fprintf(file, "display.spr3d=%d\n", g_3d_spr);
    fprintf(file, "display.front3d=%d\n", g_3d_front);
    fprintf(file, "display.stick3d=%d\n", g_3d_stick);
    fprintf(file, "display.follow3d=%d\n", g_3d_follow);
    fprintf(file, "ui.language=%d\n", g_ui_lang);     /* passo 15a */
    fprintf(file, "ui.menu_page=%d\n", g_menu_page);
    fprintf(file, "display.video_backend=%d\n", g_video_backend_pref);   /* passo 14a */
    fprintf(file, "display.hd_tiles=%d\n", ppu_native_hd_get_tiles_enabled() ? 1 : 0);
    fprintf(file, "input.touch_controls=%d\n", touch_controls::enabled() ? 1 : 0);   /* [android-port] */
    for (int i = 0; i < hd_pack_io_disabled_count(); ++i) {
        fprintf(file, "mods.disabled=%s\n", hd_pack_io_disabled_name(i));
    }
    fprintf(file, "music.enabled=%d\n", music_replace_get_enabled() ? 1 : 0);   /* passo 6d */
    fprintf(file, "music.volume_percent=%d\n", (int)(music_replace_get_volume() * 100.0f + 0.5f));
    fprintf(file, "music.log=%d\n", g_music_log ? 1 : 0);

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            char value[64];
            binding_to_config_value(g_keyboard_bindings[action][slot], value, sizeof(value));
            fprintf(file, "keyboard.%s.%d=%s\n", input_action_config_name((GBInputAction)action), slot, value);
        }
    }

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            char value[64];
            binding_to_config_value(g_controller_bindings[action][slot], value, sizeof(value));
            fprintf(file, "controller.%s.%d=%s\n", input_action_config_name((GBInputAction)action), slot, value);
        }
    }

    fclose(file);
}

/*static void set_savestate_status(const char* action, int slot, bool success, const char* detail) {
    char message[512];
    snprintf(message,
             sizeof(message),
             "%s slot %d %s%s%s",
             action ? action : "Savestate",
             slot + 1,
             success ? "succeeded" : "failed",
             (detail && detail[0]) ? ": " : "",
             (detail && detail[0]) ? detail : "");
    g_savestate_status = message;
}
*/
static void set_savestate_status(const char* action, int slot, bool success, const char* detail) {
    std::string msg = action ? action : "Savestate";
    msg += " slot " + std::to_string(slot + 1) + " ";
    msg += success ? "succeeded" : "failed";
    if (detail && detail[0]) {
        msg += ": ";
        msg += detail;
    }
    g_savestate_status = msg;
}

static bool recreate_streaming_texture(void);
static void set_app_suspended(bool suspended);

static bool frame_is_selected_for_dump(const uint32_t* frames, int count, uint32_t frame) {
    for (int i = 0; i < count; i++) {
        if (frames[i] == frame) {
            return true;
        }
    }
    return false;
}

/* Helper to parse button string "U,D,L,R,A,B,S,T" */
static bool parse_buttons(const char* btn_str, uint8_t* dpad, uint8_t* buttons) {
    *dpad = 0xFF;
    *buttons = 0xFF;
    if (!btn_str || !*btn_str) {
        return false;
    }
    for (const char* cursor = btn_str; *cursor; ++cursor) {
        switch (*cursor) {
            case 'U': *dpad &= (uint8_t)~0x04; break;
            case 'D': *dpad &= (uint8_t)~0x08; break;
            case 'L': *dpad &= (uint8_t)~0x02; break;
            case 'R': *dpad &= (uint8_t)~0x01; break;
            case 'A': *buttons &= (uint8_t)~0x01; break;
            case 'B': *buttons &= (uint8_t)~0x02; break;
            case 'S': *buttons &= (uint8_t)~0x08; break; /* Start */
            case 'T': *buttons &= (uint8_t)~0x04; break; /* Select */
            default: return false;
        }
    }
    return true;
}

static bool input_action_is_pressed(GBInputAction action) {
    /* [android-port] gamepad na tela */
    if (!g_show_menu && (int)action < GB_JOYPAD_ACTION_COUNT && touch_controls::pressed((int)action)) {
        return true;
    }
    for (int slot = 0; slot < 2; slot++) {
        if (g_keyboard_binding_pressed[action][slot] ||
            g_controller_button_binding_pressed[action][slot] ||
            g_controller_axis_binding_pressed[action][slot]) {
            return true;
        }
    }
    return false;
}

static void update_runtime_action_state(GBContext* ctx) {
    const int previous_effective_speed = effective_speed_percent();

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        if (!input_action_is_runtime((GBInputAction)action)) {
            continue;
        }

        const bool pressed = input_action_is_pressed((GBInputAction)action);
        const bool was_pressed = g_runtime_action_pressed[action];
        if (pressed && !was_pressed) {
            switch ((GBInputAction)action) {
                case GB_INPUT_ACTION_TOGGLE_MAX_SPEED:
                    g_max_speed_mode = !g_max_speed_mode;
                    break;

                case GB_INPUT_ACTION_SAVE_STATE:
                    save_savestate_slot(ctx, g_savestate_slot);
                    break;

                case GB_INPUT_ACTION_LOAD_STATE:
                    load_savestate_slot(ctx, g_savestate_slot);
                    break;

                case GB_INPUT_ACTION_PREVIOUS_STATE_SLOT:
                    g_savestate_slot = (g_savestate_slot + GB_SAVESTATE_SLOT_COUNT - 1) % GB_SAVESTATE_SLOT_COUNT;
                    g_savestate_status = "Selected slot " + std::to_string(g_savestate_slot + 1);
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_NEXT_STATE_SLOT:
                    g_savestate_slot = (g_savestate_slot + 1) % GB_SAVESTATE_SLOT_COUNT;
                    g_savestate_status = "Selected slot " + std::to_string(g_savestate_slot + 1);
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_OVERLAY:
                    g_show_overlay = !g_show_overlay;
                    break;

                case GB_INPUT_ACTION_TOGGLE_MUTE:
                    g_audio_muted.store(!g_audio_muted.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_MENU:
                    if (menu_toggle_allowed()) g_show_menu = !g_show_menu;
                    break;

                case GB_INPUT_ACTION_TOGGLE_PORT_UI:
                    if (ctx != NULL) {
                        const GBPortInputEvent event = {
                            GB_PORT_INPUT_TOGGLE_UI, true};
                        gbrt_port_input(ctx, &event);
                    }
                    break;

                case GB_INPUT_ACTION_FAST_FORWARD:
                case GB_INPUT_ACTION_RIGHT:
                case GB_INPUT_ACTION_LEFT:
                case GB_INPUT_ACTION_UP:
                case GB_INPUT_ACTION_DOWN:
                case GB_INPUT_ACTION_A:
                case GB_INPUT_ACTION_B:
                case GB_INPUT_ACTION_SELECT:
                case GB_INPUT_ACTION_START:
                case GB_INPUT_ACTION_COUNT:
                default:
                    break;
            }
        }

        g_runtime_action_pressed[action] = pressed;
    }

    g_fast_forward_active = g_runtime_action_pressed[GB_INPUT_ACTION_FAST_FORWARD];

    if (effective_speed_percent() != previous_effective_speed) {
        reset_audio_output_buffer(true);
    }
}

static void rebuild_manual_joypad_state_from_bindings(void) {
    g_manual_joypad_dpad = 0xFF;
    g_manual_joypad_buttons = 0xFF;

    if (input_action_is_pressed(GB_INPUT_ACTION_RIGHT)) g_manual_joypad_dpad &= (uint8_t)~0x01;
    if (input_action_is_pressed(GB_INPUT_ACTION_LEFT)) g_manual_joypad_dpad &= (uint8_t)~0x02;
    if (input_action_is_pressed(GB_INPUT_ACTION_UP)) g_manual_joypad_dpad &= (uint8_t)~0x04;
    if (input_action_is_pressed(GB_INPUT_ACTION_DOWN)) g_manual_joypad_dpad &= (uint8_t)~0x08;
    if (input_action_is_pressed(GB_INPUT_ACTION_A)) g_manual_joypad_buttons &= (uint8_t)~0x01;
    if (input_action_is_pressed(GB_INPUT_ACTION_B)) g_manual_joypad_buttons &= (uint8_t)~0x02;
    if (input_action_is_pressed(GB_INPUT_ACTION_SELECT)) g_manual_joypad_buttons &= (uint8_t)~0x04;
    if (input_action_is_pressed(GB_INPUT_ACTION_START)) g_manual_joypad_buttons &= (uint8_t)~0x08;
}

static void update_effective_joypad_state(void) {
    rebuild_manual_joypad_state_from_bindings();
    g_joypad_dpad = g_manual_joypad_dpad & g_script_joypad_dpad;
    g_joypad_buttons = g_manual_joypad_buttons & g_script_joypad_buttons;
}

static void request_joypad_interrupt(GBContext* ctx) {
    if (!ctx) return;
    ctx->io[0x0F] |= 0x10;
    if (ctx->halted) ctx->halted = 0;
}

static bool input_state_has_press(uint8_t dpad, uint8_t buttons) {
    return dpad != 0xFF || buttons != 0xFF;
}

static void write_buttons(FILE* file, uint8_t dpad, uint8_t buttons) {
    if (!(dpad & 0x04)) fputc('U', file);
    if (!(dpad & 0x08)) fputc('D', file);
    if (!(dpad & 0x02)) fputc('L', file);
    if (!(dpad & 0x01)) fputc('R', file);
    if (!(buttons & 0x01)) fputc('A', file);
    if (!(buttons & 0x02)) fputc('B', file);
    if (!(buttons & 0x08)) fputc('S', file);
    if (!(buttons & 0x04)) fputc('T', file);
}

static char* trim_ascii(char* text) {
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    if (!text || !*text) {
        return text;
    }

    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool parse_script_u64(char* text, uint64_t* value) {
    if (!text || !*text || !value) {
        return false;
    }
    for (const char* cursor = text; *cursor; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_script_token(char* token, ScriptEntry* entry, char* button_buf, size_t button_buf_size) {
    char* start_text = trim_ascii(token);
    if (!start_text || !*start_text) {
        return false;
    }

    char* first_colon = strchr(start_text, ':');
    if (!first_colon) {
        return false;
    }
    *first_colon = '\0';

    char* buttons_text = trim_ascii(first_colon + 1);
    char* second_colon = strchr(buttons_text, ':');
    if (!second_colon) {
        return false;
    }
    *second_colon = '\0';

    char* duration_text = trim_ascii(second_colon + 1);
    start_text = trim_ascii(start_text);
    buttons_text = trim_ascii(buttons_text);
    duration_text = trim_ascii(duration_text);
    if (!*start_text || !*duration_text) {
        return false;
    }

    entry->anchor = SCRIPT_ANCHOR_FRAME;
    bool periodic = false;
    if (*start_text == 'p' || *start_text == 'P') {
        entry->anchor = SCRIPT_ANCHOR_CYCLE;
        periodic = true;
        start_text++;
    } else if (*start_text == 'c' || *start_text == 'C') {
        entry->anchor = SCRIPT_ANCHOR_CYCLE;
        start_text++;
    } else if (*start_text == 'f' || *start_text == 'F') {
        start_text++;
    }
    start_text = trim_ascii(start_text);
    if (!*start_text) {
        return false;
    }

    if (!parse_script_u64(duration_text, &entry->duration)) {
        return false;
    }
    if (entry->duration == 0) {
        return false;
    }
    if (periodic) {
        char* dash = strchr(start_text, '-');
        char* slash = dash ? strchr(dash + 1, '/') : NULL;
        if (!dash || !slash || strchr(dash + 1, '-') || strchr(slash + 1, '/')) {
            return false;
        }
        *dash = '\0';
        *slash = '\0';
        if (!parse_script_u64(trim_ascii(start_text), &entry->start) ||
            !parse_script_u64(trim_ascii(dash + 1), &entry->end) ||
            !parse_script_u64(trim_ascii(slash + 1), &entry->period) ||
            entry->end < entry->start ||
            entry->period <= entry->duration ||
            entry->end > UINT64_MAX - entry->duration) {
            return false;
        }
    } else {
        if (!parse_script_u64(start_text, &entry->start) ||
            entry->start > UINT64_MAX - entry->duration) {
            return false;
        }
        entry->end = entry->start;
        entry->period = 0;
    }

    if (!*buttons_text || strlen(buttons_text) >= button_buf_size) {
        return false;
    }
    snprintf(button_buf, button_buf_size, "%s", buttons_text);
    return true;
}

static void flush_input_record_segment(void) {
    if (!g_input_record_file || !g_input_record_has_segment) return;

    uint64_t duration_cycles = 0;
    if (g_input_record_end_cycle > g_input_record_start_cycle) {
        duration_cycles = g_input_record_end_cycle - g_input_record_start_cycle;
    }

    if (duration_cycles > 0 && input_state_has_press(g_input_record_dpad, g_input_record_buttons)) {
        if (g_input_record_wrote_entry) {
            fputc(',', g_input_record_file);
        }
        fprintf(g_input_record_file, "c%llu:", (unsigned long long)g_input_record_start_cycle);
        write_buttons(g_input_record_file, g_input_record_dpad, g_input_record_buttons);
        fprintf(g_input_record_file, ":%llu", (unsigned long long)duration_cycles);
        fflush(g_input_record_file);
        g_input_record_wrote_entry = true;
    }

    g_input_record_has_segment = false;
    g_input_record_start_cycle = 0;
    g_input_record_end_cycle = 0;
    g_input_record_dpad = 0xFF;
    g_input_record_buttons = 0xFF;
}

static void close_input_record_file(void) {
    if (!g_input_record_file) return;
    flush_input_record_segment();
    fputc('\n', g_input_record_file);
    fclose(g_input_record_file);
    g_input_record_file = NULL;
    g_input_record_wrote_entry = false;
}

static void record_manual_input_state(uint64_t cycle_count) {
    if (!g_input_record_file) return;

    if (!g_input_record_has_segment) {
        g_input_record_has_segment = true;
        g_input_record_start_cycle = cycle_count;
        g_input_record_end_cycle = cycle_count;
        g_input_record_dpad = g_manual_joypad_dpad;
        g_input_record_buttons = g_manual_joypad_buttons;
        return;
    }

    if (g_input_record_dpad == g_manual_joypad_dpad && g_input_record_buttons == g_manual_joypad_buttons) {
        g_input_record_end_cycle = cycle_count;
        return;
    }

    g_input_record_end_cycle = cycle_count;
    flush_input_record_segment();
    g_input_record_has_segment = true;
    g_input_record_start_cycle = cycle_count;
    g_input_record_end_cycle = cycle_count;
    g_input_record_dpad = g_manual_joypad_dpad;
    g_input_record_buttons = g_manual_joypad_buttons;
}

bool gb_platform_set_input_script(const char* script) {
    // Formats: frame:buttons:duration, ccycle:buttons:duration, or
    // pstart-end/period:buttons:duration for periodic cycle pulses.
    g_script_count = 0;
    g_script_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;
    update_effective_joypad_state();

    if (!script) return true;
    if (!*script || script[0] == ',' || script[strlen(script) - 1] == ',' ||
        strstr(script, ",,") != NULL) {
        fprintf(stderr, "[AUTO] Invalid input script: empty token\n");
        return false;
    }

    char* copy = strdup(script);
    if (!copy) {
        fprintf(stderr, "[AUTO] Invalid input script: allocation failed\n");
        return false;
    }
    ScriptEntry parsed_entries[MAX_SCRIPT_ENTRIES] = {};
    int parsed_count = 0;
    char* token = strtok(copy, ",");

    while (token) {
        if (parsed_count >= MAX_SCRIPT_ENTRIES) {
            fprintf(stderr,
                    "[AUTO] Invalid input script: exceeds %u entries\n",
                    (unsigned)MAX_SCRIPT_ENTRIES);
            free(copy);
            return false;
        }
        char btn_buf[16] = {0};
        ScriptEntry parsed = {};

        if (!parse_script_token(token, &parsed, btn_buf, sizeof(btn_buf)) ||
            !parse_buttons(btn_buf, &parsed.dpad, &parsed.buttons)) {
            fprintf(stderr, "[AUTO] Invalid input token '%s'\n", token);
            free(copy);
            return false;
        }
        parsed_entries[parsed_count++] = parsed;
        token = strtok(NULL, ",");
    }
    free(copy);

    memcpy(g_input_script, parsed_entries, (size_t)parsed_count * sizeof(ScriptEntry));
    g_script_count = parsed_count;
    printf("[AUTO] Installed %d input entries\n", g_script_count);
    return true;
}

void gb_platform_set_input_record_file(const char* path) {
    if (!g_input_record_exit_handler_registered) {
        atexit(close_input_record_file);
        g_input_record_exit_handler_registered = true;
    }

    close_input_record_file();
    g_input_record_has_segment = false;
    g_input_record_start_cycle = 0;
    g_input_record_end_cycle = 0;
    g_input_record_dpad = 0xFF;
    g_input_record_buttons = 0xFF;

    if (!path || !path[0]) return;

    const std::string resolved_path = resolve_writable_path(path, "artifacts");
    g_input_record_file = fopen(resolved_path.c_str(), "w");
    if (!g_input_record_file) {
        fprintf(stderr, "[AUTO] Failed to open input record file '%s'\n", resolved_path.c_str());
        return;
    }

    fprintf(stderr, "[AUTO] Recording live input to %s (cycle anchored)\n", resolved_path.c_str());
}

void gb_platform_set_dump_frames(const char* frames) {
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_count = 0;
    while (token && g_dump_count < MAX_DUMP_FRAMES) {
        g_dump_frames[g_dump_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_dump_present_frames(const char* frames) {
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_present_count = 0;
    while (token && g_dump_present_count < MAX_DUMP_FRAMES) {
        g_dump_present_frames[g_dump_present_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_screenshot_prefix(const char* prefix) {
    if (prefix) g_screenshot_prefix = prefix;
}

static void save_ppm(const char* filename, const uint32_t* fb, int width, int height, int frame_count) {
    const std::string resolved_filename = resolve_writable_path(filename, "artifacts");
    // Calculate simple hash
    uint32_t hash = 0;
    for (int k = 0; k < width * height; k++) {
        hash = (hash * 33) ^ fb[k];
    }
    printf("[AUTO] Frame %d hash: %08X\n", frame_count, hash);

    FILE* f = fopen(resolved_filename.c_str(), "wb");
    if (!f) return;
    
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    uint8_t* row = (uint8_t*)malloc(width * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t p = fb[y * width + x];
            row[x*3+0] = (p >> 16) & 0xFF; // R
            row[x*3+1] = (p >> 8) & 0xFF;  // G
            row[x*3+2] = (p >> 0) & 0xFF;  // B
        }
        fwrite(row, 1, width * 3, f);
    }
    
    free(row);
    fclose(f);
    printf("[AUTO] Saved screenshot: %s\n", resolved_filename.c_str());
}


static int g_frame_count = 0;

static double sdl_now_ms(void) {
    uint64_t ticks = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    return freq ? ((double)ticks * 1000.0) / (double)freq : 0.0;
}

static int round_to_int(double value) {
    return (int)(value + 0.5);
}

static void update_render_filter(void) {
    if (!vid_ready()) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (g_texture) {
        vid_tex_set_linear(g_texture, g_render_filter_mode == GB_RENDER_FILTER_LINEAR);
    }
#else
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                g_render_filter_mode == GB_RENDER_FILTER_LINEAR ? "linear" : "nearest");
#endif
}

static void update_game_viewport(void) {
    if (!g_window || !vid_ready()) {
        g_game_viewport.x = 0;
        g_game_viewport.y = 0;
        g_game_viewport.w = GB_SCREEN_WIDTH * g_scale;
        g_game_viewport.h = GB_SCREEN_HEIGHT * g_scale;
        return;
    }

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(g_window, &window_w, &window_h);
    if (window_w <= 0) window_w = GB_SCREEN_WIDTH;
    if (window_h <= 0) window_h = GB_SCREEN_HEIGHT;

    const int source_w = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;

    // Mantém o aspecto proporcional (Aspect Fit) dentro da resolução real da janela
    double scale_x = (double)window_w / (double)source_w;
    double scale_y = (double)window_h / (double)GB_SCREEN_HEIGHT;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale <= 0.0) scale = 1.0;

    int viewport_w = round_to_int((double)source_w * scale);
    int viewport_h = round_to_int((double)GB_SCREEN_HEIGHT * scale);

    g_game_viewport.w = viewport_w;
    g_game_viewport.h = viewport_h;
    g_game_viewport.x = (window_w - viewport_w) / 2;
    g_game_viewport.y = (window_h - viewport_h) / 2;

    /*
    // Define o Logical Size no próprio SDL Renderer para que o Gamescope lide com a escala e o viewport
    SDL_RenderSetLogicalSize(g_renderer, source_w, GB_SCREEN_HEIGHT);

    // No modo Logical Size do SDL, a textura é desenhada na coordenada (0, 0, source_w, GB_SCREEN_HEIGHT)
    g_game_viewport.x = 0;
    g_game_viewport.y = 0;
    g_game_viewport.w = source_w;
    g_game_viewport.h = GB_SCREEN_HEIGHT;   
    */


    /*
    int viewport_w = window_w;
    int viewport_h = window_h;
    
    switch (g_render_scaling_mode) {
        case GB_RENDER_SCALING_PIXEL_PERFECT: {
            int scale_x = window_w / source_w;
            int scale_y = window_h / GB_SCREEN_HEIGHT;
            int integer_scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (integer_scale < 1) {
                integer_scale = 1;
            }
            viewport_w = source_w * integer_scale;
            viewport_h = GB_SCREEN_HEIGHT * integer_scale;
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FIT: {
            double scale_x = (double)window_w / (double)source_w;
            double scale_y = (double)window_h / (double)GB_SCREEN_HEIGHT;
            double scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)source_w * scale);
            viewport_h = round_to_int((double)GB_SCREEN_HEIGHT * scale);
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FILL: {
            double scale_x = (double)window_w / (double)source_w;
            double scale_y = (double)window_h / (double)GB_SCREEN_HEIGHT;
            double scale = (scale_x > scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)source_w * scale);
            viewport_h = round_to_int((double)GB_SCREEN_HEIGHT * scale);
            break;
        }
        case GB_RENDER_SCALING_STRETCH:
        default:
            viewport_w = window_w;
            viewport_h = window_h;
            break;
    }

    if (viewport_w < 1) viewport_w = 1;
    if (viewport_h < 1) viewport_h = 1;

    g_game_viewport.w = viewport_w;
    g_game_viewport.h = viewport_h;
    g_game_viewport.x = (window_w - viewport_w) / 2;
    g_game_viewport.y = (window_h - viewport_h) / 2;
    */
}

static void apply_window_scale_preset(void) {
    g_windowed_width = GB_SCREEN_WIDTH * g_scale;
    g_windowed_height = GB_SCREEN_HEIGHT * g_scale;

    if (g_window && !g_fullscreen) {
        SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_game_viewport();
}

static void set_fullscreen_enabled(bool enabled) {
    if (!g_window || g_fullscreen == enabled) {
        return;
    }

    if (enabled) {
        SDL_GetWindowSize(g_window, &g_windowed_width, &g_windowed_height);
        if (SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
            g_fullscreen = true;
        }
    } else {
        if (SDL_SetWindowFullscreen(g_window, 0) == 0) {
            g_fullscreen = false;
            SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }
    // Garante que o renderer e a textura sejam marcados para reinicialização no próximo frame
    g_renderer_reset_pending = true;
    update_game_viewport();
    save_runtime_preferences();
}

static bool recreate_streaming_texture(void) {
    if (!vid_ready() || g_benchmark_mode) {
        return true;
    }
    ip_release_textures();   /* passo 10a */

    if (g_texture) {
        vid_tex_destroy(g_texture);
        g_texture = NULL;
    }

    //const int texture_width = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int base_width = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int base_height = GB_SCREEN_HEIGHT;
    //const int texture_width = base_width * g_internal_res_scale;
    //const int texture_height = base_height * g_internal_res_scale;
    g_texture_scale = effective_internal_scale();
    const int texture_width = base_width * g_texture_scale;
    const int texture_height = base_height * g_texture_scale;

    g_texture = vid_tex_create(texture_width, texture_height);   /* passo 14a */
    if (!g_texture) {
        fprintf(stderr, "[SDL] Failed to recreate texture: %s\n", SDL_GetError());
        return false;
    }

    update_render_filter();
    g_renderer_reset_pending = false;
    return true;
}

static void set_app_suspended(bool suspended) {
    if (g_app_suspended == suspended) {
        return;
    }

    g_app_suspended = suspended;
    refresh_audio_device_pause_state();

    if (!suspended) {
        g_last_frame_time = SDL_GetTicks();
        if (g_window) {
            update_game_viewport();
        }
        g_renderer_reset_pending = true;
    }
}

static void reset_runtime_display_defaults(void) {
    g_scale = 5;
    g_speed_percent = 100;
    //g_palette_idx = 0;
    g_smooth_lcd_transitions = true;
    g_vsync = false;
    g_show_overlay = false;
    g_max_speed_mode = false;
    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
    g_render_filter_mode = GB_RENDER_FILTER_LINEAR;   /* passo 14p */
    g_internal_res_scale = 4;                         /* passo 14p */
    g_video_backend_pref = 1;                         /* passo 14p: OpenGL (vale ao abrir de novo) */
    g_gpu_screen_pref = 1;                            /* passo 14c-3: a GPU monta a tela */
    g_3d_mode = 0;                                    /* passo 3D-1 */
    g_3d_tilt = 10;    /* 3D-6d: padrões escolhidos pelo usuário */
    g_3d_depth = 60;
    g_3d_yaw = -5;
    g_3d_follow = 1;
    g_3d_front = 90;
    g_3d_spr = 90;
    g_3d_stick = 1;
    g_gpu_screen_error.clear();
    g_renderer_reset_pending = true;
    update_render_filter();

    vid_set_vsync(false);   /* passo 14a */

    const bool want_fullscreen = platform_default_fullscreen();
    if (g_window) {
        if (g_fullscreen != want_fullscreen) {
            set_fullscreen_enabled(want_fullscreen);
        }
        if (!g_fullscreen) {
            apply_window_scale_preset();
        } else {
            update_game_viewport();
        }
    } else {
        g_fullscreen = want_fullscreen;
    }

    reset_audio_output_buffer(true);
}

static void reset_runtime_audio_defaults(void) {
    set_default_audio_preferences();
    refresh_audio_output_devices();
    reopen_audio_output_device(true);
    save_runtime_preferences();
}

static void reset_runtime_control_defaults(void) {
    set_default_input_bindings();
    update_effective_joypad_state();
    save_runtime_preferences();
}

static void update_controller_axis_binding_state(void) {
    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            const GBInputBinding& binding = g_controller_bindings[action][slot];
            if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE ||
                binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE) {
                Sint16 value = 0;
                if (binding.code >= 0 && binding.code < SDL_CONTROLLER_AXIS_MAX) {
                    value = g_controller_axis_values[binding.code];
                }
                g_controller_axis_binding_pressed[action][slot] = binding_active_for_axis_value(binding, value);
            } else {
                g_controller_axis_binding_pressed[action][slot] = false;
            }
        }
    }
}

static bool input_transition_creates_press(uint8_t previous_dpad,
                                           uint8_t previous_buttons,
                                           uint8_t next_dpad,
                                           uint8_t next_buttons,
                                           bool dpad_selected,
                                           bool buttons_selected) {
    if (dpad_selected && ((previous_dpad & ~next_dpad) != 0)) {
        return true;
    }
    if (buttons_selected && ((previous_buttons & ~next_buttons) != 0)) {
        return true;
    }
    return false;
}

static bool dispatch_captured_port_input(
    GBContext* ctx,
    uint8_t previous_dpad,
    uint8_t previous_buttons) {
    if (ctx == NULL || !gbrt_port_snapshot(ctx).input_captured) return false;
    static const GBPortInputAction dpad_actions[4] = {
        GB_PORT_INPUT_RIGHT,
        GB_PORT_INPUT_LEFT,
        GB_PORT_INPUT_UP,
        GB_PORT_INPUT_DOWN,
    };
    for (unsigned bit = 0; bit < 4; ++bit) {
        const uint8_t mask = (uint8_t)(1u << bit);
        if ((previous_dpad & mask) != 0u && (g_joypad_dpad & mask) == 0u) {
            const GBPortInputEvent event = {dpad_actions[bit], true};
            gbrt_port_input(ctx, &event);
        }
    }
    static const GBPortInputAction button_actions[2] = {
        GB_PORT_INPUT_ACCEPT,
        GB_PORT_INPUT_BACK,
    };
    for (unsigned bit = 0; bit < 2; ++bit) {
        const uint8_t mask = (uint8_t)(1u << bit);
        if ((previous_buttons & mask) != 0u &&
            (g_joypad_buttons & mask) == 0u) {
            const GBPortInputEvent event = {button_actions[bit], true};
            gbrt_port_input(ctx, &event);
        }
    }
    g_joypad_dpad = 0xffu;
    g_joypad_buttons = 0xffu;
    return true;
}

static void render_binding_editor(const char* section_label,
                                  GBBindingCaptureDevice device,
                                  GBInputBinding bindings[GB_INPUT_ACTION_COUNT][2],
                                  GBInputAction action_begin,
                                  GBInputAction action_end) {
    ImGui::TextDisabled("%s", section_label);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float content_width = ImGui::GetContentRegionAvail().x;
    const bool compact_layout = content_width < 720.0f;

    for (int action = (int)action_begin; action < (int)action_end; action++) {
        ImGui::PushID(section_label);
        ImGui::PushID(action);
        ImGui::Text("%s", action_name_ui(action));

        const float clear_width = ImGui::CalcTextSize(TR("Clear", "Limpar")).x + style.FramePadding.x * 2.0f + 12.0f;
        if (!compact_layout) {
            const float action_column_width = content_width * 0.26f;
            float binding_width =
                (content_width - action_column_width - clear_width * 2.0f - style.ItemSpacing.x * 4.0f) / 2.0f;
            if (binding_width < 120.0f) {
                binding_width = 120.0f;
            }
            ImGui::SameLine(action_column_width);

            for (int slot = 0; slot < 2; slot++) {
                std::string label;
                if (g_binding_capture_active &&
                    g_binding_capture_device == device &&
                    g_binding_capture_action == (GBInputAction)action &&
                    g_binding_capture_slot == slot) {
                    label = TR("Press a key...", "Aperte...");
                } else {
                    label = binding_display_label(bindings[action][slot]);
                }

                ImGui::PushID(slot);
                if (ImGui::Button(label.c_str(), ImVec2(binding_width, 0.0f))) {
                    start_binding_capture(device, (GBInputAction)action, slot);
                }
                ImGui::SameLine();
                if (ImGui::Button(TR("Clear", "Limpar"), ImVec2(clear_width, 0.0f))) {
                    assign_binding_slot(device, (GBInputAction)action, slot, make_binding(GB_INPUT_BINDING_NONE, 0));
                }
                ImGui::PopID();
                if (slot == 0) {
                    ImGui::SameLine();
                }
            }
        } else {
            float compact_button_width = content_width - clear_width - style.ItemSpacing.x;
            if (compact_button_width < 120.0f) {
                compact_button_width = 120.0f;
            }
            for (int slot = 0; slot < 2; slot++) {
                std::string label;
                if (g_binding_capture_active &&
                    g_binding_capture_device == device &&
                    g_binding_capture_action == (GBInputAction)action &&
                    g_binding_capture_slot == slot) {
                    label = TR("Press a key...", "Aperte...");
                } else {
                    label = binding_display_label(bindings[action][slot]);
                }

                ImGui::PushID(slot);
                if (ImGui::Button(label.c_str(), ImVec2(compact_button_width, 0.0f))) {
                    start_binding_capture(device, (GBInputAction)action, slot);
                }
                ImGui::SameLine();
                if (ImGui::Button(TR("Clear", "Limpar"), ImVec2(clear_width, 0.0f))) {
                    assign_binding_slot(device, (GBInputAction)action, slot, make_binding(GB_INPUT_BINDING_NONE, 0));
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}

/* ============================================================================
 * Passo 10a: 120 fps com quadros intermediarios.
 *
 * O jogo continua rodando a ~60 quadros por segundo: fisica, musica e tudo o
 * que conta quadros do jogo ficam na mesma velocidade. Entre dois quadros do
 * jogo a tela mostra um quadro do meio, montado a partir do ultimo quadro:
 *   - cenario: cada linha anda metade do que o SCX/SCY andou no ultimo quadro
 *     (linhas da window, como o HUD, ficam paradas; parallax por linha vale);
 *   - sprites: cada sprite anda metade do que andou na tela no ultimo quadro
 *     (procura o mesmo sprite no quadro anterior pelo tile e pelo slot do OAM);
 *   - por baixo dos sprites o cenario e preenchido com os pontos vizinhos.
 * E uma previsao (meio passo a frente), sem atraso a mais. Saltos grandes
 * (porta, troca de sala, LCD desligado) nao sao interpolados. So liga com
 * monitor de 100 Hz ou mais e velocidade em 100%.
 * ========================================================================== */
static const char* g_fps_mode_names[] = {
    "60 fps (original)",
    "120 fps (quadros intermediarios)",
};

#define IP_MAX_SHIFT 6   /* andou mais que isso num quadro = salto, nao interpola */

struct IpSprite {
    bool on;
    int x, y, h;    /* na resolucao base da textura (com a margem do widescreen) */
    int vx, vy;     /* quanto andou na tela no ultimo quadro */
};

static VidTex* g_ip_tex_bg = NULL;    /* cenario sem os sprites */
static VidTex* g_ip_tex_obj = NULL;   /* so os sprites (alfa) */
/* Passo 14d-1: as camadas podem ser texturas da GPU embrulhadas */
static bool g_ip_tex_gpu = false;
static unsigned g_ip_gpu_bg_id = 0, g_ip_gpu_obj_id = 0;
static SDL_Rect g_ip_obj_rect = {0, 0, 0, 0};   /* onde a CPU pôs a camada de sprites */
static uint16_t g_ip_runs[GB_SCREEN_HEIGHT * GB_WIDESCREEN_WIDTH * 2];

static int g_ip_tex_w = 0;
static int g_ip_tex_h = 0;
static std::vector<uint32_t> g_ip_comp;    /* quadro montado (o mesmo da tela) */
static std::vector<uint32_t> g_ip_bg;
static std::vector<uint32_t> g_ip_obj;
static uint8_t g_ip_owner[GB_WIDESCREEN_FRAMEBUFFER_SIZE];   /* indice OAM + 1 */
static bool g_ip_valid = false;            /* quadro atual capturado */
static bool g_ip_prev_valid = false;       /* o anterior tambem (e nada invalidou) */
static bool g_ip_have_prev = false;
static int g_ip_base_w = GB_SCREEN_WIDTH;
static int g_ip_scale = 1;
static bool g_ip_black_fill = false;
static int g_ip_xmin = 0;
static int g_ip_xmax = GB_SCREEN_WIDTH;
static uint8_t g_ip_oam_cur[OAM_SIZE];
static uint8_t g_ip_oam_prev[OAM_SIZE];
static PPUNativeLineState g_ip_lines_cur[GB_SCREEN_HEIGHT];
static PPUNativeLineState g_ip_lines_prev[GB_SCREEN_HEIGHT];
static bool g_ip_lines_ok_cur = false;
static bool g_ip_lines_ok_prev = false;
static int8_t g_ip_line_dx[GB_SCREEN_HEIGHT];
static int8_t g_ip_line_dy[GB_SCREEN_HEIGHT];
static IpSprite g_ip_spr[40];
static uint64_t g_ip_last_present = 0;     /* contador de desempenho do ultimo quadro do jogo */
static uint32_t g_ip_mid_count = 0;
static uint32_t g_ip_mid_per_s = 0;
static uint32_t g_ip_mid_window_start = 0;
static int g_ip_refresh_hz = 0;
/* Passo 10b: por que o quadro do meio nao saiu (por segundo) */
enum { IP_SKIP_NO_FRAME = 0, IP_SKIP_NO_PREV, IP_SKIP_AUDIO, IP_SKIP_OFF, IP_SKIP_LATE, IP_SKIP_COUNT };
static uint32_t g_ip_skip_count[IP_SKIP_COUNT] = {};
static uint32_t g_ip_skip_shown[IP_SKIP_COUNT] = {};
static uint32_t g_ip_skip_window_start = 0;
static double g_ip_last_gap_ms = 0.0;   /* do quadro do jogo ate o alvo do proximo */
/* Passo 10c: o quadro do meio sai primeiro e o quadro do jogo fica para
 * meio quadro depois (interpolacao de verdade, atraso de ~8 ms). */
static bool g_ip_pending_main = false;
/* Passo 10f: com o quadro do meio, a textura da tela nao recebe o quadro
 * (economiza uma copia grande); o quadro do jogo e desenhado pelas camadas. */
static bool g_ip_use_mid = false;
static bool g_ip_tex_stale = false;
static float g_ip_draw_t = -0.5f;
static uint32_t g_ip_missed = 0, g_ip_missed_shown = 0, g_ip_missed_window = 0;
static uint64_t g_ip_prev_present = 0;

/* Intervalo entre presents maior que 1,5 refresh = refresh perdido. */
static void ip_note_present_interval(int hz) {
    const uint64_t now = SDL_GetPerformanceCounter();
    const uint32_t ms = SDL_GetTicks();
    if (ms - g_ip_missed_window >= 1000) {
        g_ip_missed_shown = g_ip_missed;
        g_ip_missed = 0;
        g_ip_missed_window = ms;
    }
    if (g_ip_prev_present && hz > 0) {
        const double dt = (double)(now - g_ip_prev_present) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        if (dt > 1.5 * 1000.0 / (double)hz && dt < 100.0) g_ip_missed++;
    }
    g_ip_prev_present = now;
}
/* Passo 10d: se o quadro do jogo sai atrasado, desliga o quadro do meio por um tempo */
static int g_ip_late_streak = 0;
/* Passo 10e: quanto o present do quadro do jogo demora (media). Acima de
 * 1,5 ms o present esta esperando o monitor (V-Sync do driver/DWM). */
static double g_ip_present_avg_ms = 0.0;
static int g_ip_cooldown = 0;
static bool audio_output_should_run(void);

static void ip_note_skip(int reason) {
    const uint32_t now = SDL_GetTicks();
    if (now - g_ip_skip_window_start >= 1000) {
        memcpy(g_ip_skip_shown, g_ip_skip_count, sizeof(g_ip_skip_shown));
        memset(g_ip_skip_count, 0, sizeof(g_ip_skip_count));
        g_ip_skip_window_start = now;
    }
    if (reason >= 0 && reason < IP_SKIP_COUNT) g_ip_skip_count[reason]++;
}

static void ip_skip_text(char* out, size_t n) {
    if (SDL_GetTicks() - g_ip_skip_window_start > 2000) {
        snprintf(out, n, "Pulados/s: nenhum");
        return;
    }
    snprintf(out, n, "Pulados/s: sem quadro %u, sem anterior %u, desligado %u, audio %u, sem tempo %u (folga %.1f ms)",
             g_ip_skip_shown[IP_SKIP_NO_FRAME], g_ip_skip_shown[IP_SKIP_NO_PREV],
             g_ip_skip_shown[IP_SKIP_OFF], g_ip_skip_shown[IP_SKIP_AUDIO],
             g_ip_skip_shown[IP_SKIP_LATE], g_ip_last_gap_ms);
    const size_t used = strlen(out);   /* passo 10e */
    if (used + 1 < n) {
        snprintf(out + used, n - used, " | present %.1f ms%s | refresh perdidos %u/s", g_ip_present_avg_ms,
                 g_ip_present_avg_ms > 1.5 ? " (espera o monitor)" : "",
                 (SDL_GetTicks() - g_ip_missed_window > 2000) ? 0u : g_ip_missed_shown);
    }
}
static uint32_t g_ip_refresh_checked = 0;

static void ip_invalidate(void) {
    g_ip_valid = false;
    g_ip_prev_valid = false;
    g_ip_have_prev = false;
}

static void ip_drop_textures(void);

static void ip_release_textures(void) {
    ip_drop_textures();   /* passo 14d-1 */
    ip_invalidate();
}

static int ip_display_refresh_hz(void) {
    const uint32_t now = SDL_GetTicks();
    if (g_window && (g_ip_refresh_checked == 0 || now - g_ip_refresh_checked > 1000)) {
        g_ip_refresh_checked = now ? now : 1;
        const int idx = SDL_GetWindowDisplayIndex(g_window);
        SDL_DisplayMode mode;
        if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &mode) == 0) {
            g_ip_refresh_hz = mode.refresh_rate;
        }
    }
    return g_ip_refresh_hz;
}

static bool ip_refresh_ok(void) {
    const int hz = ip_display_refresh_hz();
    return hz >= 100 || (hz == 0 && !g_vsync);
}

static uint32_t ip_mid_per_second(void) {
    if (SDL_GetTicks() - g_ip_mid_window_start > 2000) {
        return 0;
    }
    return g_ip_mid_per_s;
}

static bool ip_test_mid(void);

static bool ip_mid_wanted(void) {
    return g_fps_mode == 1 && g_ip_valid && g_ip_have_prev && vid_ready() &&
           g_ip_tex_bg && g_ip_tex_obj && !g_benchmark_mode && !g_app_suspended &&
           effective_speed_percent() == 100 && (ip_refresh_ok() || ip_test_mid());
}

/* Passo 14d-1: BIGODAO_TESTE_MEIO=1 (testes) sempre usa o quadro do meio e
 * não mostra o quadro do jogo depois dele: a última imagem na janela fica
 * sendo o quadro do meio, para fotografar. */
static bool ip_test_mid(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = SDL_getenv("BIGODAO_TESTE_MEIO");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on == 1;
}

/* Passo 10c */
static bool ip_mid_ready_note(void) {
    if (!g_ip_valid) { ip_note_skip(IP_SKIP_NO_FRAME); return false; }
    if (!g_ip_have_prev) { ip_note_skip(IP_SKIP_NO_PREV); return false; }
    if (!ip_mid_wanted()) { ip_note_skip(IP_SKIP_OFF); return false; }
    if (ip_test_mid()) return true;   /* passo 14d-1: teste */
    /* Passo 10d: buffer de audio baixo ou quadros atrasando: so o quadro do jogo */
    if (audio_output_should_run() && g_audio_started &&
        audio_ring_fill_samples() < g_audio_low_watermark) {
        ip_note_skip(IP_SKIP_AUDIO);
        return false;
    }
    if (g_ip_cooldown > 0) {
        g_ip_cooldown--;
        ip_note_skip(IP_SKIP_LATE);
        return false;
    }
    ip_note_skip(-1);
    return true;
}

static bool ip_capture_wanted(bool count_guest_frame) {
    return g_fps_mode == 1 && count_guest_frame && !g_ip_mid_draw && vid_ready() &&
           !g3d_active();   /* passo 3D-1: no 3D só o quadro do jogo */
}

/* Buffer onde o quadro e montado (no lugar da textura travada). */
static void* ip_comp_buffer(int* out_pitch) {
    const int base_w = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int w = base_w * g_texture_scale;
    const int h = GB_SCREEN_HEIGHT * g_texture_scale;
    g_ip_comp.resize((size_t)w * (size_t)h);
    *out_pitch = w * (int)sizeof(uint32_t);
    return g_ip_comp.data();
}

static inline int ip_wrap(int d) {
    d &= 0xFF;
    return d >= 128 ? d - 256 : d;
}

/* Posicao do sprite na tela (x ajustado como nas laterais do widescreen). */
static bool ip_oam_pos(const uint8_t* o, int* out_x, int* out_y) {
    if (o[0] == 0 || o[0] >= 160 || o[1] == 0) {
        return false;
    }
    const int center = GB_SCREEN_WIDTH / 2;
    int sx = (int)o[1] - 8;
    int best = sx;
    if (abs(sx - 256 - center) < abs(best - center)) best = sx - 256;
    if (abs(sx + 256 - center) < abs(best - center)) best = sx + 256;
    *out_x = best;
    *out_y = (int)o[0] - 16;
    return true;
}

static void ip_build_sprites(const GBContext* ctx) {
    memset(g_ip_owner, 0, (size_t)g_ip_base_w * GB_SCREEN_HEIGHT);
    for (int i = 0; i < 40; i++) {
        g_ip_spr[i].on = false;
    }
    if (!ctx || !ctx->oam || !ctx->vram) {
        return;
    }
    const bool cgb = ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
    const int off = (g_ip_base_w == GB_WIDESCREEN_WIDTH) ? GB_WIDESCREEN_MARGIN : 0;

    /* Do ultimo para o primeiro: o de indice menor fica por cima. */
    for (int i = 39; i >= 0; --i) {
        const uint8_t* o = &g_ip_oam_cur[i * 4];
        int sx = 0, sy = 0;
        if (!ip_oam_pos(o, &sx, &sy)) continue;
        if (o[3] & OAM_PRIORITY) continue;   /* atras do cenario: anda junto com ele */
        const int ly0 = sy < 0 ? 0 : (sy > GB_SCREEN_HEIGHT - 1 ? GB_SCREEN_HEIGHT - 1 : sy);
        const uint8_t lcdc = g_ip_lines_ok_cur ? g_ip_lines_cur[ly0].lcdc : ctx->io[0x40];
        const int h = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
        const uint8_t tile = (h == 16) ? (uint8_t)(o[2] & 0xFE) : o[2];
        const size_t bank_base = (cgb && (o[3] & OAM_CGB_BANK)) ? (size_t)VRAM_SIZE : 0u;
        bool any = false;
        for (int r = 0; r < h; r++) {
            const int y = sy + r;
            if (y < 0 || y >= GB_SCREEN_HEIGHT) continue;
            if (g_ip_lines_ok_cur && !(g_ip_lines_cur[y].lcdc & LCDC_OBJ_ENABLE)) continue;
            const int line = (o[3] & OAM_FLIP_Y) ? (h - 1 - r) : r;
            const size_t addr = bank_base + (size_t)tile * 16u + (size_t)line * 2u;
            const uint8_t lo = ctx->vram[addr];
            const uint8_t hi = ctx->vram[addr + 1];
            uint8_t* row = g_ip_owner + (size_t)y * g_ip_base_w;
            for (int c = 0; c < 8; c++) {
                const int bit = (o[3] & OAM_FLIP_X) ? c : 7 - c;
                if ((((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)) == 0) continue;
                const int x = sx + off + c;
                if (x < g_ip_xmin || x >= g_ip_xmax) continue;
                row[x] = (uint8_t)(i + 1);
                any = true;
            }
        }
        if (!any) continue;
        IpSprite& s = g_ip_spr[i];
        s.on = true;
        s.x = sx + off;
        s.y = sy;
        s.h = h;
        s.vx = 0;
        s.vy = 0;
        if (!g_ip_have_prev) continue;

        /* Mesmo sprite no quadro anterior: 1) mesmo slot e mesmo tile;
         * 2) mesmo tile em outro slot, o mais perto; 3) mesmo slot (o
         * desenho mudou na animacao). */
        int best_d = 1 << 30, bvx = 0, bvy = 0;
        bool found = false;
        int px = 0, py = 0;
        const uint8_t* p = &g_ip_oam_prev[i * 4];
        if (ip_oam_pos(p, &px, &py) && p[2] == o[2] && (p[3] & 0x60) == (o[3] & 0x60) &&
            abs(sx - px) <= IP_MAX_SHIFT && abs(sy - py) <= IP_MAX_SHIFT) {
            bvx = sx - px; bvy = sy - py; found = true;
        }
        if (!found) {
            for (int j = 0; j < 40; j++) {
                const uint8_t* q = &g_ip_oam_prev[j * 4];
                if (q[2] != o[2] || (q[3] & 0x60) != (o[3] & 0x60)) continue;
                if (!ip_oam_pos(q, &px, &py)) continue;
                const int dx = sx - px, dy = sy - py;
                if (abs(dx) > IP_MAX_SHIFT || abs(dy) > IP_MAX_SHIFT) continue;
                const int d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; bvx = dx; bvy = dy; found = true; }
            }
        }
        if (!found && ip_oam_pos(p, &px, &py) &&
            abs(sx - px) <= IP_MAX_SHIFT && abs(sy - py) <= IP_MAX_SHIFT) {
            bvx = sx - px; bvy = sy - py; found = true;
        }
        if (found) {
            s.vx = bvx;
            s.vy = bvy;
        }
    }
}

static void ip_build_line_shifts(void) {
    memset(g_ip_line_dx, 0, sizeof(g_ip_line_dx));
    memset(g_ip_line_dy, 0, sizeof(g_ip_line_dy));
    if (!g_ip_have_prev || !g_ip_lines_ok_cur || !g_ip_lines_ok_prev) {
        return;
    }
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        const PPUNativeLineState& a = g_ip_lines_cur[y];
        const PPUNativeLineState& b = g_ip_lines_prev[y];
        const bool win_a = (a.flags & PPU_NATIVE_LINE_WINDOW) && a.wx <= 7;
        const bool win_b = (b.flags & PPU_NATIVE_LINE_WINDOW) && b.wx <= 7;
        if (win_a || win_b || ((a.lcdc ^ b.lcdc) & (LCDC_BG_TILEMAP | LCDC_TILE_DATA))) {
            continue;
        }
        const int dx = ip_wrap((int)a.scx - (int)b.scx);
        const int dy = ip_wrap((int)a.scy - (int)b.scy);
        if (abs(dx) > IP_MAX_SHIFT || abs(dy) > IP_MAX_SHIFT) {
            continue;
        }
        g_ip_line_dx[y] = (int8_t)dx;
        g_ip_line_dy[y] = (int8_t)dy;
    }
}

static bool ip_ensure_textures(int w, int h) {
    if (g_ip_tex_gpu) {   /* passo 14d-1: estavam embrulhando as da GPU */
        ip_drop_textures();
    }
    if (g_ip_tex_bg && g_ip_tex_obj && g_ip_tex_w == w && g_ip_tex_h == h) {
        return true;
    }
    if (g_ip_tex_bg) vid_tex_destroy(g_ip_tex_bg);
    if (g_ip_tex_obj) vid_tex_destroy(g_ip_tex_obj);
    g_ip_tex_bg = vid_tex_create(w, h);
    g_ip_tex_obj = vid_tex_create(w, h);
    if (!g_ip_tex_bg || !g_ip_tex_obj) {
        if (g_ip_tex_bg) vid_tex_destroy(g_ip_tex_bg);
        if (g_ip_tex_obj) vid_tex_destroy(g_ip_tex_obj);
        g_ip_tex_bg = NULL;
        g_ip_tex_obj = NULL;
        g_ip_tex_w = 0;
        g_ip_tex_h = 0;
        return false;
    }
    vid_tex_set_blend(g_ip_tex_bg, false);
    vid_tex_set_blend(g_ip_tex_obj, true);
    g_ip_tex_w = w;
    g_ip_tex_h = h;
    return true;
}

/* Chamado com o quadro do jogo ja montado em g_ip_comp: copia para a textura
 * da tela e prepara o cenario sem sprites e a camada de sprites. */
/* Passo 14d-1: as camadas do 120 fps podem vir da GPU (texturas dela
 * embrulhadas) ou da CPU (texturas próprias). */

/* Solta as texturas das camadas sem esquecer os quadros (ip_release_textures esquece). */
static void ip_drop_textures(void) {
    if (g_ip_tex_bg) vid_tex_destroy(g_ip_tex_bg);
    if (g_ip_tex_obj) vid_tex_destroy(g_ip_tex_obj);
    g_ip_tex_bg = NULL;
    g_ip_tex_obj = NULL;
    g_ip_tex_w = 0;
    g_ip_tex_h = 0;
    g_ip_tex_gpu = false;
    g_ip_gpu_bg_id = g_ip_gpu_obj_id = 0;
}

/* Trechos de pontos de sprite (g_ip_owner) por linha, para a GPU. */
static void ip_build_runs(int base_w) {
    memset(g_ip_runs, 0, sizeof(g_ip_runs));
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        const uint8_t* ow = g_ip_owner + (size_t)y * base_w;
        uint16_t* r = g_ip_runs + (size_t)y * GB_WIDESCREEN_WIDTH * 2;
        int x = 0;
        while (x < base_w) {
            if (!ow[x]) { x++; continue; }
            const int a = x;
            while (x < base_w && ow[x]) x++;
            for (int k = a; k < x; k++) {
                r[k * 2] = (uint16_t)(a + 1);
                r[k * 2 + 1] = (uint16_t)x;
            }
        }
    }
}

/* O quadro atual vira o anterior; sprites e deslocamentos do quadro novo. */
static void ip_capture_state(int scale, int base_w, bool black_fill) {
    /* O quadro atual vira o anterior. */
    memcpy(g_ip_oam_prev, g_ip_oam_cur, sizeof(g_ip_oam_prev));
    memcpy(g_ip_lines_prev, g_ip_lines_cur, sizeof(g_ip_lines_prev));
    g_ip_lines_ok_prev = g_ip_lines_ok_cur;
    g_ip_have_prev = g_ip_prev_valid && base_w == g_ip_base_w && scale == g_ip_scale &&
                     black_fill == g_ip_black_fill;

    const GBContext* ctx = g_registered_ctx;
    if (ctx && ctx->oam) {
        memcpy(g_ip_oam_cur, ctx->oam, sizeof(g_ip_oam_cur));
    } else {
        memset(g_ip_oam_cur, 0, sizeof(g_ip_oam_cur));
    }
    const PPUNativeLineState* ls =
        (ppu_native_get_mode() != PPU_RENDERER_EMULATED) ? ppu_native_line_states() : NULL;
    if (ls) {
        memcpy(g_ip_lines_cur, ls, sizeof(g_ip_lines_cur));
    }
    g_ip_lines_ok_cur = (ls != NULL);

    g_ip_base_w = base_w;
    g_ip_scale = scale;
    g_ip_black_fill = black_fill;
    const int off = (base_w == GB_WIDESCREEN_WIDTH) ? GB_WIDESCREEN_MARGIN : 0;
    g_ip_xmin = black_fill ? off : 0;
    g_ip_xmax = black_fill ? off + GB_SCREEN_WIDTH : base_w;

    ip_build_sprites(ctx);
    ip_build_line_shifts();
}

/* Chamado com o quadro do jogo ja montado em g_ip_comp: copia para a textura
 * da tela e prepara o cenario sem sprites e a camada de sprites. */
static void ip_finish_capture(void* tex_pixels, int tex_pitch, int tex_w, int tex_h,
                              int scale, int base_w, bool black_fill) {
    /* Passo 10f: a copia para a textura da tela vai mais abaixo, so se
     * este quadro nao usar o quadro do meio. */
    g_ip_use_mid = false;
    g_ip_tex_stale = false;
    g_ip_obj_rect = SDL_Rect{0, 0, 0, 0};
    ip_capture_state(scale, base_w, black_fill);   /* passo 14d-1: separado */

    const bool ip_tex_ok = ip_ensure_textures(tex_w, tex_h);
    if (ip_tex_ok) {
        g_ip_valid = true;   /* para a decisao abaixo */
        g_ip_use_mid = (g_fps_mode == 1) && ip_mid_ready_note();
        g_ip_valid = false;
    }
    if (!g_ip_use_mid) {
        for (int y = 0; y < tex_h; y++) {
            memcpy((uint8_t*)tex_pixels + (size_t)y * tex_pitch,
                   g_ip_comp.data() + (size_t)y * tex_w, (size_t)tex_w * sizeof(uint32_t));
        }
    }
    g_ip_tex_stale = g_ip_use_mid;
    if (!ip_tex_ok) {
        return;
    }
    /* Passo 10d: menos copias. A camada de sprites so na caixa que contem
     * todos os sprites; o cenario sem sprites e feito no proprio buffer (o
     * quadro composto ja foi para a textura da tela). */
    int ux0 = base_w, uy0 = GB_SCREEN_HEIGHT, ux1 = 0, uy1 = 0;
    for (int i = 0; i < 40; i++) {
        const IpSprite& sp = g_ip_spr[i];
        if (!sp.on) continue;
        const int x0 = sp.x < g_ip_xmin ? g_ip_xmin : sp.x;
        const int x1 = (sp.x + 8) > g_ip_xmax ? g_ip_xmax : sp.x + 8;
        const int y0 = sp.y < 0 ? 0 : sp.y;
        const int y1 = (sp.y + sp.h) > GB_SCREEN_HEIGHT ? GB_SCREEN_HEIGHT : sp.y + sp.h;
        if (x1 <= x0 || y1 <= y0) continue;
        if (x0 < ux0) ux0 = x0;
        if (y0 < uy0) uy0 = y0;
        if (x1 > ux1) ux1 = x1;
        if (y1 > uy1) uy1 = y1;
    }
    if (ux1 > ux0 && uy1 > uy0) {
        const int rx = ux0 * scale, ry = uy0 * scale;
        const int rw = (ux1 - ux0) * scale, rh = (uy1 - uy0) * scale;
        g_ip_obj.assign((size_t)rw * (size_t)rh, 0u);
        for (int ty = ry; ty < ry + rh; ty++) {
            const uint8_t* ow = g_ip_owner + (size_t)(ty / scale) * base_w;
            const uint32_t* comp = g_ip_comp.data() + (size_t)ty * tex_w;
            uint32_t* obj = g_ip_obj.data() + (size_t)(ty - ry) * rw;
            for (int x = ux0; x < ux1; x++) {
                if (!ow[x]) continue;
                for (int tx = x * scale; tx < (x + 1) * scale; tx++) {
                    obj[tx - rx] = comp[tx] | 0xFF000000u;
                }
            }
        }
        const SDL_Rect r = {rx, ry, rw, rh};
        g_ip_obj_rect = r;   /* passo 14d-1 */
        vid_tex_update(g_ip_tex_obj, &r, g_ip_obj.data(), rw * (int)sizeof(uint32_t));

        for (int ty = ry; ty < ry + rh; ty++) {
            const uint8_t* ow = g_ip_owner + (size_t)(ty / scale) * base_w;
            uint32_t* comp = g_ip_comp.data() + (size_t)ty * tex_w;
            int x = ux0;
            while (x < ux1) {
                if (!ow[x]) { x++; continue; }
                const int a = x;
                while (x < ux1 && ow[x]) x++;
                const int b = x;
                const int ta = a * scale, tb = b * scale, tm = (ta + tb) / 2;
                /* Cenario por baixo do sprite: metade com o ponto da esquerda,
                 * metade com o da direita (os dois sao cenario). */
                const uint32_t cl = (ta > 0) ? comp[ta - 1] : (tb < tex_w ? comp[tb] : 0xFF000000u);
                const uint32_t cr = (tb < tex_w) ? comp[tb] : cl;
                for (int tx = ta; tx < tm; tx++) comp[tx] = cl;
                for (int tx = tm; tx < tb; tx++) comp[tx] = cr;
            }
        }
    }
    vid_tex_update(g_ip_tex_bg, NULL, g_ip_comp.data(), tex_w * (int)sizeof(uint32_t));
    {
        const bool linear = (g_render_filter_mode == GB_RENDER_FILTER_LINEAR);
        vid_tex_set_linear(g_ip_tex_bg, linear);
        vid_tex_set_linear(g_ip_tex_obj, linear);
    }
    g_ip_valid = true;
}

/* Desenha o quadro do meio no lugar da textura da tela. */
static void ip_draw_mid(void) {
    const SDL_Rect vp = g_game_viewport;
    if (!g_ip_tex_bg || !g_ip_tex_obj || vp.w <= 0 || vp.h <= 0) {
        vid_copy(g_texture, NULL, &vp);
        return;
    }
    const float t = g_ip_draw_t;   /* passo 10c/10f: -0,5 = meio do caminho desde o anterior; 0 = o proprio quadro */
    const float kx = (float)vp.w / (float)g_ip_base_w;
    const float ky = (float)vp.h / (float)GB_SCREEN_HEIGHT;
    const int s = g_ip_scale;

    vid_copy(g_ip_tex_bg, NULL, &vp);

    SDL_Rect clip = vp;
    if (g_ip_xmin > 0 || g_ip_xmax < g_ip_base_w) {
        clip.x = vp.x + (int)((float)g_ip_xmin * kx + 0.5f);
        clip.w = (int)((float)(g_ip_xmax - g_ip_xmin) * kx + 0.5f);
    }
    vid_set_clip(&clip);

    /* Cenario: primeiro as faixas que andam, depois as paradas por cima
     * (HUD fica firme). */
    bool any_moving = false;
    for (int pass = 0; pass < 2; pass++) {
        int y = 0;
        while (y < GB_SCREEN_HEIGHT) {
            const int dx = g_ip_line_dx[y], dy = g_ip_line_dy[y];
            int y1 = y + 1;
            while (y1 < GB_SCREEN_HEIGHT && g_ip_line_dx[y1] == dx && g_ip_line_dy[y1] == dy) y1++;
            const bool still = (dx == 0 && dy == 0);
            if (!still) any_moving = true;
            if ((pass == 0 && !still) || (pass == 1 && still && any_moving)) {
                const SDL_Rect src = {0, y * s, g_ip_tex_w, (y1 - y) * s};
                const SDL_FRect dst = {(float)vp.x - (float)dx * t * kx,
                                       (float)vp.y + ((float)y - (float)dy * t) * ky,
                                       (float)vp.w, (float)(y1 - y) * ky};
                vid_copy_f(g_ip_tex_bg, &src, &dst);
            }
            y = y1;
        }
    }

    /* Sprites: o de indice menor por ultimo (fica por cima). */
    for (int i = 39; i >= 0; --i) {
        const IpSprite& sp = g_ip_spr[i];
        if (!sp.on) continue;
        const int x0 = sp.x < g_ip_xmin ? g_ip_xmin : sp.x;
        const int x1 = (sp.x + 8) > g_ip_xmax ? g_ip_xmax : sp.x + 8;
        const int y0 = sp.y < 0 ? 0 : sp.y;
        const int y1 = (sp.y + sp.h) > GB_SCREEN_HEIGHT ? GB_SCREEN_HEIGHT : sp.y + sp.h;
        if (x1 <= x0 || y1 <= y0) continue;
        const SDL_Rect src = {x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s};
        const SDL_FRect dst = {(float)vp.x + ((float)x0 + (float)sp.vx * t) * kx,
                               (float)vp.y + ((float)y0 + (float)sp.vy * t) * ky,
                               (float)(x1 - x0) * kx, (float)(y1 - y0) * ky};
        vid_copy_f(g_ip_tex_obj, &src, &dst);
    }
    vid_set_clip(NULL);
}

static void ensure_lcd_off_framebuffer(void) {
    if (g_lcd_off_framebuffer_initialized) {
        return;
    }

    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        g_lcd_off_framebuffer[i] = 0xFFE0F8D0;
    }

    g_lcd_off_framebuffer_initialized = true;
}

/* Passo 14c-1: a tela inteira montada pela GPU contra a textura que a CPU
 * acabou de montar (antes do filtro de pós-processamento). */
static uint32_t g_scr_base[GB_SCREEN_HEIGHT * GB_WIDESCREEN_WIDTH];
static std::vector<uint32_t> g_gpu_read_s;
static bool g_gpu_last_screen_ok = false;   /* passo 14c-2: a GPU montou a tela neste quadro */

static void gpu_compare_screen(const void* cpu_pixels, int cpu_pitch, int w, int h, int base_width,
                               int scale, bool hd_center, bool hd_margins, bool amb_hd) {
    GpuCompareStats& st = g_gpu_stats;
    g_gpu_last_screen_ok = false;
    if ((hd_center && !g_gpu_center_ok) || (hd_margins && !g_gpu_margins_ok)) {
        st.s_skipped++;   /* a GPU não montou o HD deste quadro */
        return;
    }
    GpuScreenInput in = {};
    in.base = g_scr_base;
    in.wide = base_width == GB_WIDESCREEN_WIDTH;
    in.scale = scale;
    in.hd_center = hd_center;
    in.hd_margins = hd_margins;
    in.amb_q = amb_hd ? &g_ws_amb_q[0][0][0] : NULL;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    g_gpu_read_s.resize((size_t)w * h);
    int gw = 0, gh = 0;
    if (!gpu_ppu_compose_screen(&in) || !gpu_ppu_read_screen(g_gpu_read_s.data(), &gw, &gh) ||
        gw != w || gh != h) {
        g_gpu_error = "a GPU nao conseguiu montar a tela inteira";
        return;
    }
    g_gpu_last_screen_ok = true;
    st.s_ms_total += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                     (double)SDL_GetPerformanceFrequency();
    uint32_t diff = 0;
    int fx = -1, fy = -1;
    for (int y = 0; y < h; ++y) {
        const uint32_t* c = (const uint32_t*)((const uint8_t*)cpu_pixels + (size_t)y * cpu_pitch);
        const uint32_t* g = &g_gpu_read_s[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            if (((c[x] ^ g[x]) & 0x00FFFFFFu) != 0) {
                if (!diff) { fx = x; fy = y; }
                diff++;
            }
        }
    }
    st.s_compared++;
    st.s_w = w;
    st.s_h = h;
    if (!diff) {
        st.s_equal++;
        return;
    }
    st.s_last_diff = diff;
    st.s_x = fx;
    st.s_y = fy;
    st.s_cpu = ((const uint32_t*)((const uint8_t*)cpu_pixels + (size_t)fy * cpu_pitch))[fx];
    st.s_gpu = g_gpu_read_s[(size_t)fy * w + fx];
    if (st.s_compared - st.s_equal <= 20u) {
        fprintf(stderr, "[GPU-TELA] %dx%d: %u pontos diferentes, primeiro (%d,%d) cpu %08X gpu %08X "
                "(centro HD %d, laterais HD %d, ambiente %d)\n",
                w, h, diff, fx, fy, st.s_cpu, st.s_gpu, hd_center ? 1 : 0, hd_margins ? 1 : 0,
                amb_hd ? 1 : 0);
    }
}

/* Passo 14e-1: o filtro de pós-processamento da GPU (sobre a tela que ela
 * acabou de montar) contra o da CPU. Sem filtro, só deixa a tela sem filtro
 * valendo (a comparação da tela inteira já cobre). */
static std::vector<uint32_t> g_gpu_read_f;
static void gpu_compare_filter(const void* cpu_pixels, int cpu_pitch, int w, int h) {
    const int mode = (int)g_post_filter_mode;
    if (!gpu_ppu_apply_filter(mode)) {
        g_gpu_error = "a GPU nao conseguiu aplicar o filtro";
        g_gpu_last_screen_ok = false;
        return;
    }
    if (mode == GB_POST_FILTER_NONE) return;
    g_gpu_read_f.resize((size_t)w * h);
    if (!gpu_ppu_read_final(g_gpu_read_f.data())) {
        g_gpu_error = "a GPU nao conseguiu ler a tela filtrada";
        return;
    }
    GpuCompareStats& st = g_gpu_stats;
    uint32_t diff = 0;
    int fx = -1, fy = -1;
    for (int y = 0; y < h; ++y) {
        const uint32_t* c = (const uint32_t*)((const uint8_t*)cpu_pixels + (size_t)y * cpu_pitch);
        const uint32_t* g = &g_gpu_read_f[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            if (((c[x] ^ g[x]) & 0x00FFFFFFu) != 0) {
                if (!diff) { fx = x; fy = y; }
                diff++;
            }
        }
    }
    st.f_compared++;
    st.f_mode = mode;
    if (!diff) {
        st.f_equal++;
        return;
    }
    st.f_last_diff = diff;
    st.f_x = fx;
    st.f_y = fy;
    st.f_cpu = ((const uint32_t*)((const uint8_t*)cpu_pixels + (size_t)fy * cpu_pitch))[fx];
    st.f_gpu = g_gpu_read_f[(size_t)fy * w + fx];
    if (st.f_compared - st.f_equal <= 20u) {
        fprintf(stderr, "[GPU-FILTRO] %s: %u pontos diferentes, primeiro (%d,%d) cpu %08X gpu %08X\n",
                post_processing_get_names()[mode], diff, fx, fy, st.f_cpu, st.f_gpu);
    }
}

/* Passo 14d-1: 120 fps com a tela da GPU: mesma preparação da CPU
 * (ip_capture_state), camadas feitas na GPU e embrulhadas para o ip_draw_mid. */
static bool ip_gpu_capture(int scale, int base_w, bool black_fill) {
    g_ip_use_mid = false;
    g_ip_tex_stale = false;   /* a tela da GPU já tem o quadro do jogo */
    ip_capture_state(scale, base_w, black_fill);
    ip_build_runs(base_w);
    if (!gpu_ppu_compose_ip_layers(g_ip_runs, scale)) {
        return false;
    }
    int w = 0, h = 0;
    const unsigned bg = gpu_ppu_ip_texture(false, &w, &h);
    const unsigned obj = gpu_ppu_ip_texture(true, NULL, NULL);
    if (!bg || !obj) return false;
    if (!g_ip_tex_gpu || g_ip_gpu_bg_id != bg || g_ip_gpu_obj_id != obj) {
        ip_drop_textures();
        g_ip_tex_bg = new VidTex();
        g_ip_tex_bg->gl = vgl_tex_wrap(bg, w, h);
        g_ip_tex_obj = new VidTex();
        g_ip_tex_obj->gl = vgl_tex_wrap(obj, w, h);
        vid_tex_set_blend(g_ip_tex_bg, false);
        vid_tex_set_blend(g_ip_tex_obj, true);
        g_ip_tex_gpu = true;
        g_ip_gpu_bg_id = bg;
        g_ip_gpu_obj_id = obj;
        g_ip_tex_w = w;
        g_ip_tex_h = h;
    }
    const bool linear = (g_render_filter_mode == GB_RENDER_FILTER_LINEAR);
    vid_tex_set_linear(g_ip_tex_bg, linear);
    vid_tex_set_linear(g_ip_tex_obj, linear);
    g_ip_valid = true;
    g_ip_use_mid = (g_fps_mode == 1) && ip_mid_ready_note();
    return true;
}

/* Passo 14d-1: com a comparação ligada, as camadas da CPU (ip_finish_capture,
 * já feitas) contra as da GPU feitas a partir da mesma tela. */
static std::vector<uint32_t> g_gpu_read_ibg, g_gpu_read_iobj;
static void ip_gpu_compare_layers(int tex_w, int tex_h, int scale) {
    if (!g_ip_valid || g_ip_tex_gpu || !g_gpu_last_screen_ok) return;
    int w = 0, h = 0;
    if (!gpu_ppu_screen_texture(&w, &h) || w != tex_w || h != tex_h) return;
    ip_build_runs(g_ip_base_w);
    g_gpu_read_ibg.resize((size_t)w * h);
    g_gpu_read_iobj.resize((size_t)w * h);
    if (!gpu_ppu_compose_ip_layers(g_ip_runs, scale) || !gpu_ppu_read_ip_layer(false, g_gpu_read_ibg.data()) ||
        !gpu_ppu_read_ip_layer(true, g_gpu_read_iobj.data())) {
        g_gpu_error = "a GPU nao conseguiu montar as camadas do 120 fps";
        return;
    }
    GpuCompareStats& st = g_gpu_stats;
    uint32_t dbg = 0, dobj = 0;
    int fx = -1, fy = -1;
    for (int i = 0; i < w * h; ++i) {
        if (((g_gpu_read_ibg[(size_t)i] ^ g_ip_comp[(size_t)i]) & 0x00FFFFFFu) != 0) {
            if (!dbg && !dobj) { fx = i % w; fy = i / w; }
            dbg++;
        }
    }
    const SDL_Rect& r = g_ip_obj_rect;
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const uint32_t c = g_ip_obj[(size_t)y * r.w + x];
            const uint32_t g = g_gpu_read_iobj[(size_t)(r.y + y) * w + r.x + x];
            const bool same = (c >> 24) == 0 ? (g >> 24) == 0 : c == g;
            if (!same) {
                if (!dbg && !dobj) { fx = r.x + x; fy = r.y + y; }
                dobj++;
            }
        }
    }
    st.ip_compared++;
    if (!dbg && !dobj) {
        st.ip_equal++;
        return;
    }
    st.ip_last_bg = dbg;
    st.ip_last_obj = dobj;
    st.ip_x = fx;
    st.ip_y = fy;
    if (st.ip_compared - st.ip_equal <= 20u) {
        fprintf(stderr, "[GPU-120] camadas diferentes: cenario %u pontos, sprites %u pontos, primeiro (%d,%d)\n",
                dbg, dobj, fx, fy);
    }
}

/* Linha base 1x da tela (render_frame_internal, paleta original): o quadro
 * do jogo (esticando as bordas se vier com 160 px), com as laterais pretas ou
 * o fundo ambiente 1x nas telas sem cenário. Passo 14c-2: separada para a
 * GPU usar a mesma. */
static void build_base_row(int y, const uint32_t* src, bool src_is_wide, int base_width,
                           bool black_fill, bool ws_ambient, uint32_t* base_row) {
    if (base_width == GB_WIDESCREEN_WIDTH) {
        if (src_is_wide) {
            memcpy(base_row, src + (size_t)y * GB_WIDESCREEN_WIDTH,
                   GB_WIDESCREEN_WIDTH * sizeof(uint32_t));
        } else {
            /* Frame 160 px numa textura wide: estica as bordas. */
            const uint32_t* s = src + (size_t)y * GB_SCREEN_WIDTH;
            for (int x = 0; x < GB_WIDESCREEN_MARGIN; x++) base_row[x] = s[0];
            memcpy(&base_row[GB_WIDESCREEN_MARGIN], s, GB_SCREEN_WIDTH * sizeof(uint32_t));
            for (int x = GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH; x < GB_WIDESCREEN_WIDTH; x++) {
                base_row[x] = s[GB_SCREEN_WIDTH - 1];
            }
        }
        if (black_fill) {
            for (int x = 0; x < GB_WIDESCREEN_MARGIN; x++) {
                base_row[x] = 0xFF000000;
                base_row[GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH + x] = 0xFF000000;
            }
            if (ws_ambient) {
                ws_ambient_row_1x(y, base_row);   /* passo 8a */
            }
        }
    } else {
        memcpy(base_row, src + (size_t)y * GB_SCREEN_WIDTH,
               GB_SCREEN_WIDTH * sizeof(uint32_t));
    }
}

/* Passo 14e-2: linha base 1x com a paleta alternativa, igual ao caminho
 * "Palette remap" de render_frame_internal (que não usa HD nem o fundo
 * ambiente): o quadro (esticando as bordas se vier com 160 px; preto nas
 * laterais sem cenário só quando vem com 256) e as 4 cores do DMG trocadas
 * pelas da paleta escolhida. */
static void remap_base_row(int y, const uint32_t* src, int src_pixel_count, int base_width,
                           bool black_fill, uint32_t* row) {
    uint32_t source_palette[4];
    for (int shade = 0; shade < 4; ++shade) {
        source_palette[shade] = ppu_get_dmg_shade_rgb((uint8_t)shade);
    }
    for (int x = 0; x < base_width; x++) {
        uint32_t src_pixel = 0;
        if (base_width == GB_WIDESCREEN_WIDTH && src_pixel_count == GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT) {
            if (x < GB_WIDESCREEN_MARGIN) {
                src_pixel = src[y * GB_SCREEN_WIDTH + 0];
            } else if (x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH) {
                src_pixel = src[y * GB_SCREEN_WIDTH + (GB_SCREEN_WIDTH - 1)];
            } else {
                src_pixel = src[y * GB_SCREEN_WIDTH + (x - GB_WIDESCREEN_MARGIN)];
            }
        } else if (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE) {
            if (black_fill && (x < GB_WIDESCREEN_MARGIN || x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH)) {
                src_pixel = 0xFF000000;
            } else {
                src_pixel = src[y * base_width + x];
            }
        } else {
            src_pixel = src[y * base_width + x];
        }
        int color_idx = -1;
        for (int shade = 0; shade < 4; ++shade) {
            if (src_pixel == source_palette[shade] || src_pixel == g_palettes[0][shade]) {
                color_idx = shade;
                break;
            }
        }
        row[x] = color_idx >= 0 ? g_palettes[g_palette_idx][color_idx] : src_pixel;
    }
}

/* Passo 14c-2: a GPU monta a tela quando a opção está ligada e nada que só a
 * CPU faz está em uso (paleta alternativa, filtro de pós-processamento,
 * 120 fps). */
static const char* gpu_screen_blocker(void) {
    if (!g_use_gl) return TR("needs the OpenGL video", "precisa do Video OpenGL");
    if (ppu_native_get_mode() == PPU_RENDERER_EMULATED) return TR("Emulated renderer in use", "renderizador Emulado em uso");
    if (!g_gpu_screen_error.empty()) return g_gpu_screen_error.c_str();
    return NULL;
}

static bool gpu_screen_active(void) {
    return g_gpu_screen_pref == 1 && !g_benchmark_mode && gpu_screen_blocker() == NULL;
}

/* Passo 3D-1: por que o 3D não está valendo (NULL = está). */
static const char* g3d_blocker(void) {
    if (g_gpu_screen_pref != 1) return TR("needs Screen composition: GPU", "precisa de Montagem da tela: GPU");
    if (const char* w = gpu_screen_blocker()) return w;
    if (g_gpu_compare) return TR("the GPU comparison is on (the screen stays 2D)", "a comparacao da GPU esta ligada (a tela fica em 2D)");
    if (g_palette_idx != 0) return TR("alternative palette in use", "paleta alternativa em uso");
    return NULL;
}

static bool g3d_active(void) {
    return g_3d_mode == 1 && !g_benchmark_mode && g3d_blocker() == NULL;
}

/* Embrulha a textura da tela da GPU para o vid_copy (refeito se ela mudar). */
static VidTex* gpu_screen_vtex(void) {
    int w = 0, h = 0;
    const unsigned id = gpu_ppu_screen_texture(&w, &h);
    if (!id) return NULL;
    if (!g_gpu_vtex || g_gpu_vtex_id != id) {
        if (g_gpu_vtex) vid_tex_destroy(g_gpu_vtex);
        g_gpu_vtex = new VidTex();
        g_gpu_vtex->gl = vgl_tex_wrap(id, w, h);
        g_gpu_vtex_id = id;
    }
    vid_tex_set_linear(g_gpu_vtex, g_render_filter_mode == GB_RENDER_FILTER_LINEAR);
    return g_gpu_vtex;
}

/* Monta a tela pela GPU: a CPU só faz a linha base 1x; o HD (centro e
 * laterais) já foi montado na GPU no vsync. Quadro repetido (pausa, LCD
 * desligado): mostra a última de novo. Devolve false para a CPU montar. */
static bool gpu_present_frame(const uint32_t* framebuffer, bool count_guest_frame,
                              bool framebuffer_is_widescreen) {
    const int scale = g_texture_scale;
    const int base_width = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int w = base_width * scale, h = GB_SCREEN_HEIGHT * scale;
    if (!count_guest_frame) {
        if (g3d_active()) return g_3d_valid && g_3d_w == w && g_3d_h == h;   /* passo 3D-1 */
        return g_gpu_screen_valid && g_gpu_screen_w == w && g_gpu_screen_h == h;
    }
    g_ip_tex_stale = false;   /* a tela da GPU sempre tem o quadro do jogo */
    g_ip_use_mid = false;     /* passo 14d-1: ip_gpu_capture decide */
    /* mesma escolha da fonte que o caminho da CPU */
    const uint32_t* src = framebuffer;
    int src_pixel_count = GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT;
    if (g_widescreen_mode) {
        GBPPU* ppu = active_ppu();
        const uint32_t* widesrc = ppu ? ppu_get_widescreen_framebuffer(ppu) : NULL;
        if (widesrc) {
            src = widesrc;
            src_pixel_count = GB_WIDESCREEN_FRAMEBUFFER_SIZE;
        }
    }
    (void)framebuffer_is_widescreen;
    const bool src_is_wide = (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE);
    const bool black_fill = g_widescreen_mode && gb_platform_should_black_fill_non_side_scrolling(g_registered_ctx);
    const bool remap = g_palette_idx != 0;   /* passo 14e-2: paleta alternativa (sem HD, como na CPU) */
    const bool ws_ambient = !remap && black_fill && g_ws_fill_mode != WS_FILL_BLACK &&
                            base_width == GB_WIDESCREEN_WIDTH;
    if (ws_ambient) {
        ws_ambient_update(src, src_is_wide, true, g_frame_count);
    }
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        if (remap) {
            remap_base_row(y, src, src_pixel_count, base_width, black_fill, &g_scr_base[(size_t)y * base_width]);
        } else {
            build_base_row(y, src, src_is_wide, base_width, black_fill, ws_ambient,
                           &g_scr_base[(size_t)y * base_width]);
        }
    }
    GpuScreenInput in = {};
    in.base = g_scr_base;
    in.wide = base_width == GB_WIDESCREEN_WIDTH;
    in.scale = scale;
    in.hd_center = !remap && g_gpu_center_ok && g_use_hd_frame;
    in.hd_margins = in.hd_center && g_gpu_margins_ok && in.wide && src_is_wide && !black_fill;
    in.amb_q = (ws_ambient && scale > 1) ? &g_ws_amb_q[0][0][0] : NULL;
    in.amb_on = ws_ambient;   /* passo 3D-2i */
    in.black_fill = black_fill;
    if (g3d_active()) {   /* passo 3D-1: camadas (o filtro de pós-processamento não vale aqui) */
        GpuScreenInput in3 = in;
        in3.hd_margins = in.wide && src_is_wide && !black_fill;
        marks3d_load();   /* passo 3D-2b */
        marks3d_table(g_registered_ctx);
        g_3d_wide = in3.wide;
        /* passo 3D-2g: no g00 (título/slots) a janela são as ondas do mar, não o HUD;
         * 3D-2o: no tempo esgotado (g84) a janela é o Wario: vai para o 3D, não fica fixa */
        gpu_ppu_set_window_floor(ppu_native_hd_context() == (PPU_NATIVE_CTX_MODE | 0x00) ||
                                 marks3d_context(g_registered_ctx) == (PPU_NATIVE_CTX_MODE | 0x84));
        g_3d_valid = g_frame3d && gpu_ppu_compose_3d(g_frame3d, &in3);
        g_frame3d = NULL;
        if (g_3d_valid) {
            g_3d_w = w;
            g_3d_h = h;
            return true;
        }
    } else {
        g_3d_valid = false;
    }
    if (!gpu_ppu_compose_screen(&in) || !gpu_ppu_apply_filter((int)g_post_filter_mode)) {   /* passo 14e-1: filtro */
        g_gpu_screen_valid = false;
        return false;
    }
    g_gpu_screen_valid = true;
    g_gpu_screen_w = w;
    g_gpu_screen_h = h;
    if (ip_capture_wanted(true)) {   /* passo 14d-1: camadas do 120 fps na GPU */
        ip_gpu_capture(scale, base_width, black_fill);
    }
    return true;
}

static void render_frame_internal(const uint32_t* framebuffer,
                                   bool count_guest_frame,
                                   bool framebuffer_is_widescreen = false,
                                   bool reuse_hd_frame = false) {
    if (!framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: texture=%d, renderer=%d, fb=%d)",
                  g_texture == NULL, g_renderer == NULL, framebuffer == NULL);
        return;
    }

    g_present_count++;
    if (!g_ip_mid_draw) {   /* passo 10a */
        if (count_guest_frame) {
            g_ip_prev_valid = g_ip_valid;   /* o anterior serve de base para a velocidade */
            g_ip_valid = false;
        } else {
            ip_invalidate();   /* frame repetido (LCD desligado, pausa) */
        }
    }
    if (count_guest_frame && g_last_vsync_end_ms > 0.0) {
        timing_window_add(g_emu_window, sdl_now_ms() - g_last_vsync_end_ms);
    }
    if (count_guest_frame) {
        g_frame_count++;
        /* Copy the incoming framebuffer into the appropriate stable buffer
         * so presentation can use a fully-composed frame for widescreen. */
        if (g_widescreen_mode) {
            GBPPU* ppu = active_ppu();
            const uint32_t* widesrc = ppu ? ppu_get_widescreen_framebuffer(ppu) : NULL;
            if (widesrc) {
                memcpy(g_last_guest_widescreen_framebuffer, widesrc, sizeof(g_last_guest_widescreen_framebuffer));
                g_last_guest_widescreen_framebuffer_valid = true;
                g_last_guest_framebuffer_valid = false;
            } else {
                memcpy(g_last_guest_framebuffer, framebuffer, sizeof(g_last_guest_framebuffer));
                g_last_guest_framebuffer_valid = true;
                g_last_guest_widescreen_framebuffer_valid = false;
            }
        } else {
            memcpy(g_last_guest_framebuffer, framebuffer, sizeof(g_last_guest_framebuffer));
            g_last_guest_framebuffer_valid = true;
            g_last_guest_widescreen_framebuffer_valid = false;
        }
    }

    if (count_guest_frame) {
        /* Handle Screenshot Dumping */
        if (frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count)) {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), "_%05d.ppm", g_frame_count);
            const std::string filename = g_screenshot_prefix + suffix;
            save_ppm(filename.c_str(), framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
        }
    }

    if (frame_is_selected_for_dump(g_dump_present_frames, g_dump_present_count, (uint32_t)g_frame_count)) {
        char suffix[80];
        snprintf(suffix,
                 sizeof(suffix),
                 "_guest_%05d_present_%06llu.ppm",
                 g_frame_count,
                 (unsigned long long)g_present_count);
        const std::string filename = g_screenshot_prefix + suffix;
        const uint32_t* present_framebuffer = framebuffer;
        int present_width = GB_SCREEN_WIDTH;
        if (g_widescreen_mode) {
            GBPPU* ppu = active_ppu();
            const uint32_t* widescreen =
                ppu ? ppu_get_widescreen_framebuffer(ppu) : NULL;
            if (widescreen) {
                present_framebuffer = widescreen;
                present_width = GB_WIDESCREEN_WIDTH;
            }
        }
        save_ppm(
            filename.c_str(),
            present_framebuffer,
            present_width,
            GB_SCREEN_HEIGHT,
            g_frame_count);
    }

    if (g_benchmark_mode) {
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    if (g_benchmark_mode || g_app_suspended || !vid_ready()) {
        DBG_FRAME("Platform render_frame: host render skipped (benchmark=%d texture=%d renderer=%d)",
                  g_benchmark_mode ? 1 : 0,
                  g_texture == NULL,
                  g_renderer == NULL);
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    //if ((!g_texture || g_renderer_reset_pending) && !recreate_streaming_texture()) {
    if ((!g_texture || g_renderer_reset_pending ||
         effective_internal_scale() != g_texture_scale) &&
        !recreate_streaming_texture()) {
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    double total_render_start_ms = sdl_now_ms();

    /* Debug: check framebuffer content on first few guest frames */
    if (count_guest_frame && g_frame_count <= 3) {
        bool has_content = false;
        uint32_t white = 0xFFE0F8D0;
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }

    if (count_guest_frame && (g_frame_count % 60) == 0) {
        char title[64];
        snprintf(title, sizeof(title), "Bigodão - Wario Land: Super Mario Land 3 - Frame %d", g_frame_count);
        SDL_SetWindowTitle(g_window, title);
    } else if (!count_guest_frame && !g_ip_mid_draw && (g_present_count % 30) == 0) {
        char title[96];
        if (g_frame_count == 0) {
            snprintf(title, sizeof(title), "Bigodão - Wario Land: Super Mario Land 3 - Starting... (%llu)",
                     (unsigned long long)(g_present_count / 30));
        } else {
            snprintf(title, sizeof(title), "Bigodão - Wario Land: Super Mario Land 3 - Frame %d (Working...)",
                     g_frame_count);
        }
        SDL_SetWindowTitle(g_window, title);
    }

    /* Passo 14c-2: a GPU monta a tela. Com a comparação ligada a CPU também
     * monta a dela (para conferir), mas a que aparece é a da GPU. */
    bool gpu_shown = false;
    if (!g_ip_mid_draw && gpu_screen_active()) {
        if (g_gpu_compare) {
            gpu_shown = true;   /* decidido depois que a CPU montar e conferir */
        } else {
            const double t0 = sdl_now_ms();
            gpu_shown = gpu_present_frame(framebuffer, count_guest_frame, framebuffer_is_widescreen);
            if (gpu_shown) {
                g_last_timing.upload_ms = sdl_now_ms() - t0;
            }
        }
    }
    if (!g_ip_mid_draw && !(gpu_shown && !g_gpu_compare)) {   /* passo 10a: o quadro do meio usa o que ja foi montado */
    /* Update texture */
    double upload_start_ms = sdl_now_ms();
    void* pixels;
    int pitch;
    vid_tex_lock(g_texture, &pixels, &pitch);   /* passo 14a */
    /* Passo 10a: com 120 fps o quadro e montado num buffer proprio, copiado
     * para a textura no fim; o buffer vira a base do quadro do meio. */
    void* ip_tex_pixels = pixels;
    const int ip_tex_pitch = pitch;
    const bool ip_capture = ip_capture_wanted(count_guest_frame);
    if (count_guest_frame) g_gpu_last_screen_ok = false;   /* passo 14e-2: vale só o deste quadro */
    if (ip_capture) {
        pixels = ip_comp_buffer(&pitch);
    } else {
        g_ip_tex_stale = false;   /* passo 10f */
        g_ip_use_mid = false;
    }

    const uint32_t* src = framebuffer;
    int src_pixel_count = GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT;

    /* If this is a live guest frame, prefer the PPU-provided widescreen
     * framebuffer when widescreen mode is enabled. For present/fallback
     * render paths, honor the supplied stable framebuffer instead. */
    if (count_guest_frame && g_widescreen_mode) {
        GBPPU* ppu = active_ppu();
        const uint32_t* widesrc = ppu ? ppu_get_widescreen_framebuffer(ppu) : NULL;
        if (widesrc) {
            src = widesrc;
            src_pixel_count = GB_WIDESCREEN_FRAMEBUFFER_SIZE;
        }
    } else if (!count_guest_frame && framebuffer_is_widescreen && g_widescreen_mode) {
        /* Passo 6a.3: frame repetido (LCD desligado/pausa) ja vem com 256 px.
         * Antes era lido como se tivesse 160 px: linhas embaralhadas. */
        src_pixel_count = GB_WIDESCREEN_FRAMEBUFFER_SIZE;
    }

    // --- DIMENSÕES BASE E COM SUPERSAMPLING ---
    /*const int base_width = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int base_height = GB_SCREEN_HEIGHT;

    const int texture_width = base_width * g_internal_res_scale;
    const int texture_height = base_height * g_internal_res_scale;
    const bool black_fill_non_side_scrolling = g_widescreen_mode && gb_platform_should_black_fill_non_side_scrolling(g_registered_ctx);

    if (g_palette_idx == 0) {
        if (g_internal_res_scale > 1) {
            // Caminho com Supersampling Ativo (>1x) 
            for (int y = 0; y < texture_height; y++) {
                uint32_t* dst_row = (uint32_t*)((uint8_t*)pixels + y * pitch);
                int src_y = y / g_internal_res_scale;

                for (int x = 0; x < texture_width; x++) {
                    int src_x = x / g_internal_res_scale;

                    if (black_fill_non_side_scrolling && (src_x < GB_WIDESCREEN_MARGIN || src_x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH)) {
                        dst_row[x] = 0xFF000000;
                    } else if (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE) {
                        dst_row[x] = src[src_y * base_width + src_x];
                    } else {
                        dst_row[x] = src[src_y * GB_SCREEN_WIDTH + src_x];
                    }
                }
            }
        } else {
            // Caminho nativo 1x
            for (int y = 0; y < texture_height; y++) {
                uint32_t* dst_row_ptr = (uint32_t*)((uint8_t*)pixels + y * pitch);
                if (texture_width == GB_WIDESCREEN_WIDTH && src_pixel_count == GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT) {
                    const uint32_t* src_row = src + y * GB_SCREEN_WIDTH;
                    if (black_fill_non_side_scrolling) {
                        for (int x = 0; x < GB_WIDESCREEN_WIDTH; x++) {
                            if (x < GB_WIDESCREEN_MARGIN || x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH) {
                                dst_row_ptr[x] = 0xFF000000;
                            } else {
                                dst_row_ptr[x] = src_row[x - GB_WIDESCREEN_MARGIN];
                            }
                        }
                    } else {
                        for (int x = 0; x < GB_WIDESCREEN_MARGIN; x++) dst_row_ptr[x] = src_row[0];
                        memcpy(&dst_row_ptr[GB_WIDESCREEN_MARGIN], src_row, GB_SCREEN_WIDTH * sizeof(uint32_t));
                        uint32_t last = src_row[GB_SCREEN_WIDTH - 1];
                        for (int x = GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH; x < texture_width; x++) dst_row_ptr[x] = last;
                    }
                } else if (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE) {
                    const uint32_t* src_row = src + y * texture_width;
                    if (black_fill_non_side_scrolling) {
                        for (int x = 0; x < GB_WIDESCREEN_WIDTH; x++) {
                            if (x < GB_WIDESCREEN_MARGIN || x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH) {
                                dst_row_ptr[x] = 0xFF000000;
                            } else {
                                dst_row_ptr[x] = src_row[x];
                            }
                        }
                    } else {
                        memcpy(dst_row_ptr, src_row, (size_t)texture_width * sizeof(uint32_t));
                    }
                } else {
                    const uint32_t* src_row = src + y * texture_width;
                    memcpy(dst_row_ptr, src_row, texture_width * sizeof(uint32_t));
                }
            }
        }
    } else {*/

    // --- DIMENSÕES BASE E COM SUPERSAMPLING ---
    const int scale = g_texture_scale;
    const int base_width = g_widescreen_mode ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    const int base_height = GB_SCREEN_HEIGHT;

    const int texture_width = base_width * scale;
    const int texture_height = base_height * scale;
    const bool black_fill_non_side_scrolling = g_widescreen_mode && gb_platform_should_black_fill_non_side_scrolling(g_registered_ctx);
    const bool src_is_wide = (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE);
    /* Passo 8a: fundo ambiente nas laterais que seriam pretas. */
    const bool ws_ambient = black_fill_non_side_scrolling && g_ws_fill_mode != WS_FILL_BLACK &&
                            base_width == GB_WIDESCREEN_WIDTH && g_palette_idx == 0;
    if (ws_ambient) {
        ws_ambient_update(src, src_is_wide, count_guest_frame, g_frame_count);
    }

    if (g_palette_idx == 0) {
        /* Monta cada linha uma vez na resolução base, amplia na horizontal
         * repetindo o pixel e replica a linha pronta na vertical com memcpy.
         * Sem divisão nem if por pixel de destino. */
        uint32_t base_row[GB_WIDESCREEN_WIDTH];
        /* Passo 4b: centro em alta resolucao vindo do ppu_native. So para
         * frames novos do jogo e com a mesma escala da textura. */
        int hd_scale = 0;
        const uint32_t* hd_frame = NULL;
        if ((count_guest_frame || reuse_hd_frame) && g_use_hd_frame &&
            ppu_native_get_mode() != PPU_RENDERER_EMULATED) {
            hd_frame = ppu_native_hd_frame(&hd_scale);
            if (hd_scale != scale) {
                hd_frame = NULL;
            }
        }
        const int hd_dst_x = (base_width == GB_WIDESCREEN_WIDTH) ? GB_WIDESCREEN_MARGIN * scale : 0;
        /* Passo 5d: laterais em alta resolucao (widescreen ligado e tela com
         * laterais; nas telas com laterais pretas fica o preto). */
        int hd_m_scale = 0;
        const uint32_t* hd_margins = NULL;
        if (hd_frame && base_width == GB_WIDESCREEN_WIDTH && src_is_wide &&
            !black_fill_non_side_scrolling) {
            hd_margins = ppu_native_hd_margin_frame(&hd_m_scale);
            if (hd_m_scale != scale) {
                hd_margins = NULL;
            }
        }

        /* Passo 14c-1: conferir a tela inteira montada pela GPU (quadro novo). */
        const bool gpu_screen = g_gpu_compare && g_use_gl && count_guest_frame && !g_ip_mid_draw;
        for (int y = 0; y < base_height; y++) {
            build_base_row(y, src, src_is_wide, base_width, black_fill_non_side_scrolling,
                           ws_ambient, base_row);   /* passo 14c-2: função separada */
            if (gpu_screen) {   /* passo 14c-1: a linha base vai para a GPU */
                memcpy(&g_scr_base[(size_t)y * base_width], base_row, (size_t)base_width * sizeof(uint32_t));
            }

            uint32_t* first = (uint32_t*)((uint8_t*)pixels + (size_t)(y * scale) * pitch);
            if (scale == 1) {
                memcpy(first, base_row, (size_t)base_width * sizeof(uint32_t));
            } else {
                uint32_t* d = first;
                for (int x = 0; x < base_width; x++) {
                    const uint32_t c = base_row[x];
                    for (int k = 0; k < scale; k++) *d++ = c;
                }
                for (int k = 1; k < scale; k++) {
                    memcpy((uint8_t*)pixels + (size_t)(y * scale + k) * pitch,
                           first, (size_t)texture_width * sizeof(uint32_t));
                }
            }
            if (hd_frame) {
                const size_t hd_stride = (size_t)GB_SCREEN_WIDTH * (size_t)scale;
                for (int k = 0; k < scale; k++) {
                    memcpy((uint8_t*)pixels + (size_t)(y * scale + k) * pitch +
                               (size_t)hd_dst_x * sizeof(uint32_t),
                           hd_frame + ((size_t)y * scale + k) * hd_stride,
                           hd_stride * sizeof(uint32_t));
                }
            }
            if (hd_margins) {
                const size_t m_w = (size_t)GB_WIDESCREEN_MARGIN * (size_t)scale;
                const size_t right_x = (size_t)(GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH) * (size_t)scale;
                for (int k = 0; k < scale; k++) {
                    uint32_t* dst = (uint32_t*)((uint8_t*)pixels + (size_t)(y * scale + k) * pitch);
                    const uint32_t* m_src = hd_margins + ((size_t)y * scale + k) * 2u * m_w;
                    memcpy(dst, m_src, m_w * sizeof(uint32_t));
                    memcpy(dst + right_x, m_src + m_w, m_w * sizeof(uint32_t));
                }
            }
            if (ws_ambient && scale > 1) {
                ws_ambient_rows_hd(pixels, pitch, y, scale);   /* passo 8a: suave no HD */
            }
        }
        if (gpu_screen) {
            gpu_compare_screen(pixels, pitch, texture_width, texture_height, base_width, scale,
                               hd_frame != NULL, hd_margins != NULL, ws_ambient && scale > 1);
        }
    } else {    
        /* Palette remap path */
        uint32_t source_palette[4];
        for (int shade = 0; shade < 4; ++shade) {
            source_palette[shade] = ppu_get_dmg_shade_rgb((uint8_t)shade);
        }
        for (int y = 0; y < texture_height; y++) {
            uint32_t* dst_row_ptr = (uint32_t*)((uint8_t*)pixels + y * pitch);
            int src_y = y / scale;// y / g_internal_res_scale;

            for (int x = 0; x < texture_width; x++) {
                int src_x = x / scale;// x / g_internal_res_scale;
                uint32_t src_pixel = 0;

                if (base_width == GB_WIDESCREEN_WIDTH && src_pixel_count == GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT) {
                    if (src_x < GB_WIDESCREEN_MARGIN) {
                        src_pixel = src[src_y * GB_SCREEN_WIDTH + 0];
                    } else if (src_x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH) {
                        src_pixel = src[src_y * GB_SCREEN_WIDTH + (GB_SCREEN_WIDTH - 1)];
                    } else {
                        src_pixel = src[src_y * GB_SCREEN_WIDTH + (src_x - GB_WIDESCREEN_MARGIN)];
                    }
                } else if (src_pixel_count == GB_WIDESCREEN_FRAMEBUFFER_SIZE) {
                    if (black_fill_non_side_scrolling && (src_x < GB_WIDESCREEN_MARGIN || src_x >= GB_WIDESCREEN_MARGIN + GB_SCREEN_WIDTH)) {
                        src_pixel = 0xFF000000;
                    } else {
                        src_pixel = src[src_y * base_width + src_x];
                    }
                } else {
                    src_pixel = src[src_y * base_width + src_x];
                }

                int color_idx = -1;
                for (int shade = 0; shade < 4; ++shade) {
                    if (src_pixel == source_palette[shade] || src_pixel == g_palettes[0][shade]) {
                        color_idx = shade;
                        break;
                    }
                }

                if (color_idx >= 0) {
                    dst_row_ptr[x] = g_palettes[g_palette_idx][color_idx];
                } else {
                    dst_row_ptr[x] = src_pixel;
                }
            }
        }
        /* Passo 14e-2: conferir a tela com a paleta alternativa montada pela GPU. */
        if (g_gpu_compare && g_use_gl && count_guest_frame) {
            for (int y = 0; y < base_height; y++) {
                remap_base_row(y, src, src_pixel_count, base_width, black_fill_non_side_scrolling,
                               &g_scr_base[(size_t)y * base_width]);
            }
            gpu_compare_screen(pixels, pitch, texture_width, texture_height, base_width, scale,
                               false, false, false);
        }
    }

    if (gpu_shown && g_gpu_compare) {   /* passo 14c-2: mostra a da GPU se ela montou este quadro */
        if (count_guest_frame) {
            g_gpu_screen_valid = g_gpu_last_screen_ok;
            g_gpu_screen_w = texture_width;
            g_gpu_screen_h = texture_height;
        }
        gpu_shown = g_gpu_screen_valid && g_gpu_screen_w == texture_width &&
                    g_gpu_screen_h == texture_height;
    }

    // Aplica o filtro de pós-processamento ativo diretamente no buffer escalado
    post_processing_apply(g_post_filter_mode, (uint32_t*)pixels, texture_width, texture_height, pitch);
    if (g_gpu_compare && g_use_gl && count_guest_frame && g_gpu_last_screen_ok) {
        gpu_compare_filter(pixels, pitch, texture_width, texture_height);   /* passo 14e-1 */
    }

    if (ip_capture) {   /* passo 10a */
        ip_finish_capture(ip_tex_pixels, ip_tex_pitch, texture_width, texture_height,
                          scale, base_width, black_fill_non_side_scrolling);
        if (g_gpu_compare && g_use_gl) {   /* passo 14d-1 (14e-1: com filtro; 14e-2: com paleta) */
            ip_gpu_compare_layers(texture_width, texture_height, scale);
        }
    }
    vid_tex_unlock(g_texture);   /* passo 14a */
    g_last_timing.upload_ms = sdl_now_ms() - upload_start_ms;
    }   /* passo 10a: fim do if (!g_ip_mid_draw) */

    if (!g_ip_mid_draw && count_guest_frame) {
        g_gpu_frame_on_gpu = gpu_shown;   /* passo 14d-1: de quem é o quadro do jogo atual */
    }

    /* Clear and render */
    double compose_start_ms = sdl_now_ms();

    // Força o reset do viewport nativo do SDL antes do Clear
    vid_begin_frame();   /* passo 14a */

    // Desenha a textura do jogo no viewport proporcional correto
    if (g_ip_mid_draw) {
        /* passo 10c: chamada do ritmo = o quadro do jogo, meio quadro depois */
        if (g_ip_tex_stale) {
            g_ip_draw_t = 0.0f;   /* passo 10f: pelas camadas, sem deslocar */
            ip_draw_mid();
            g_ip_draw_t = -0.5f;
        } else {
            VidTex* gt = g_gpu_frame_on_gpu ? gpu_screen_vtex() : NULL;   /* passo 14d-1 */
            vid_copy(gt ? gt : g_texture, NULL, &g_game_viewport);
        }
    } else if (count_guest_frame && g_ip_use_mid) {
        g_ip_use_mid = false;
        ip_draw_mid();   /* passo 10c: primeiro o meio do caminho entre o anterior e este */
        g_ip_pending_main = true;
    } else {
        bool drawn3d = false;
        if (gpu_shown && g3d_active() && g_3d_valid) {   /* passo 3D-1 */
            Gpu3DView v = {};
            vgl_rect_to_fb(&g_game_viewport, &v.vp_x, &v.vp_y, &v.vp_w, &v.vp_h);
            {   /* passo 3D-5a: analógico direito mexe a câmera; soltando, ela volta */
                float ty = 0.0f, ty2 = 0.0f;
                static const char* fake = getenv("BIGODAO_TESTE_ANALOGICO");   /* testes: "x,y" de -32767 a 32767 */
                if (fake) {
                    int fx = 0, fy = 0;
                    if (sscanf(fake, "%d,%d", &fx, &fy) == 2) {
                        g_controller_axis_values[SDL_CONTROLLER_AXIS_RIGHTX] = (Sint16)fx;
                        g_controller_axis_values[SDL_CONTROLLER_AXIS_RIGHTY] = (Sint16)fy;
                    }
                }
                if (g_3d_stick && (g_controller || fake) && !g_show_menu) {
                    auto axis = [](int a) {
                        float f = (float)g_controller_axis_values[a] / 32767.0f;
                        const float dz = 0.2f;
                        if (f > dz) return (f - dz) / (1.0f - dz);
                        if (f < -dz) return (f + dz) / (1.0f - dz);
                        return 0.0f;
                    };
                    ty = axis(SDL_CONTROLLER_AXIS_RIGHTX) * 40.0f;    /* giro */
                    ty2 = -axis(SDL_CONTROLLER_AXIS_RIGHTY) * 30.0f;  /* inclinação: para cima = mais de cima */
                }
                g_3d_cam_yaw += (ty - g_3d_cam_yaw) * 0.15f;
                g_3d_cam_tilt += (ty2 - g_3d_cam_tilt) * 0.15f;
                if (std::fabs(g_3d_cam_yaw) < 0.01f) g_3d_cam_yaw = 0.0f;
                if (std::fabs(g_3d_cam_tilt) < 0.01f) g_3d_cam_tilt = 0.0f;
            }
            v.tilt_deg = std::min(60.0f, std::max(-60.0f, (float)g_3d_tilt + g_3d_cam_tilt));   /* 3D-4t */
            v.yaw_deg = std::min(60.0f, std::max(-60.0f, (float)g_3d_yaw + g_3d_cam_yaw));
            v.depth = (float)g_3d_depth / 100.0f;
            v.spr_depth = (float)g_3d_spr / 100.0f;   /* 3D-5e */
            v.front = (float)g_3d_front / 100.0f;      /* 3D-6a */
            v.map_sprites = g_registered_ctx && g_registered_ctx->eram && g_registered_ctx->eram_size > 0x08C3 &&
                            g_registered_ctx->eram[0x08C3] == 1;   /* 3D-2h: mapa geral / regional */
            {   /* passo 3D-4q: ponto do Wario na tela (sPlXRel/sPlYRel, ERAM $A91D/$A91C,
                 * coordenadas do OAM: x + 8, y + 16, origem nos pés), suavizado */
                static float s_fx = 80.0f, s_fy = 72.0f, s_f = 0.0f;
                float tx = 80.0f, ty = 72.0f, tf = 0.0f;
                const GBContext* c = g_registered_ctx;
                static const char* fake_w = getenv("BIGODAO_TESTE_WARIO");   /* testes: "x,y" na tela */
                int fwx = 0, fwy = 0;
                if (g_3d_follow && fake_w && sscanf(fake_w, "%d,%d", &fwx, &fwy) == 2) {
                    tx = (float)fwx; ty = (float)fwy; tf = 1.0f;
                } else if (g_3d_follow && c && c->eram && c->eram_size > 0x091D &&
                           (c->eram[0x08C3] == 2 || c->eram[0x08C3] == 3)) {
                    /* 3D-4r: o byte dá a volta quando o Wario passa do topo da tela (pulo
                     * alto, céu): lido com sinal, senão parecia fora e a câmera dava uma
                     * guinada de volta ao centro, deixando o Wario no céu. O alvo fica
                     * preso dentro da tela (a fase acima/abaixo não continua) e, sem
                     * leitura boa, a câmera fica onde estava em vez de voltar. */
                    int rx = c->eram[0x091D], ry = c->eram[0x091C];
                    if (rx >= 0xC8) rx -= 256;
                    if (ry >= 0xC8) ry -= 256;
                    tx = std::min(144.0f, std::max(16.0f, (float)rx - 8.0f));
                    ty = std::min(104.0f, std::max(48.0f, (float)ry - 16.0f - 12.0f));   /* meio do corpo; na vertical segue pouco */
                    tf = 1.0f;
                }
                if (tf > 0.0f) {   /* 3D-4r: sem Wario (fora da fase) só a força volta a 0 */
                    s_fx += (tx - s_fx) * 0.12f;
                    s_fy += (ty - s_fy) * 0.12f;
                }
                s_f += (tf - s_f) * 0.08f;
                v.follow = s_f;
                v.follow_x = s_fx;
                v.follow_y = s_fy;
                /* 3D-4s: o giro do menu acompanha o lado para onde o Wario anda: indo
                 * para a esquerda vira o mesmo ângulo espelhado (antes, indo para a
                 * esquerda, a câmera chegava no máximo de frente para a tela). Lado
                 * pelo X do Wario no mundo (sPlX, ERAM $A913/$A914); parado, mantém
                 * o último; troca devagar para não dar tranco. */
                static float s_dir = 1.0f;
                static int s_last_x = -1;
                static float s_goal = 1.0f;
                const char* fake_d = getenv("BIGODAO_TESTE_LADO");   /* testes: 1 direita, -1 esquerda */
                if (g_3d_follow && fake_d) {
                    s_goal = atoi(fake_d) < 0 ? -1.0f : 1.0f;
                    s_dir = s_goal;
                } else if (g_3d_follow && tf > 0.0f && c && c->eram && c->eram_size > 0x0914) {
                    const int x = (c->eram[0x0913] << 8) | c->eram[0x0914];
                    if (s_last_x >= 0) {
                        const int dx = x - s_last_x;
                        if (dx > 0 && dx < 16) s_goal = 1.0f;
                        else if (dx < 0 && dx > -16) s_goal = -1.0f;
                    }
                    s_last_x = x;
                } else {
                    s_last_x = -1;
                    s_goal = 1.0f;   /* fora da fase: o giro do menu como está */
                }
                s_dir += (s_goal - s_dir) * 0.04f;
                v.yaw_deg = std::min(60.0f, std::max(-60.0f, (float)g_3d_yaw * s_dir + g_3d_cam_yaw));
            }
            v.linear = false;   /* 3D-6a: no 3D sempre Nearest (o Linear dava artefatos) */
            v.debug = g_3d_debug || g_3d_mark || getenv("BIGODAO_3D_CLASSES") != NULL;   /* env: testes; 3D-2b: marcando */
            if (g_3d_mark) { v.tilt_deg = 0.0f; v.yaw_deg = 0.0f; }   /* 3D-2b: vista reta para o clique bater com o tile */
            v.hide = g_3d_hide | (getenv("BIGODAO_3D_ESCONDER") ? atoi(getenv("BIGODAO_3D_ESCONDER")) : 0);
            drawn3d = gpu_ppu_draw_3d(&v);
            if (drawn3d) {   /* passo 3D-3: desenhado fora da tela, com profundidade */
                int tw = 0, th = 0;
                const unsigned id = gpu_ppu_3d_texture(&tw, &th);
                if (id && (!g_3d_vtex || g_3d_vtex_id != id || g_3d_vtex->gl == NULL)) {
                    if (g_3d_vtex) vid_tex_destroy(g_3d_vtex);
                    g_3d_vtex = new VidTex();
                    g_3d_vtex->gl = vgl_tex_wrap(id, tw, th);
                    g_3d_vtex_id = id;
                }
                drawn3d = id && g_3d_vtex && g_3d_vtex->gl;
                if (drawn3d) vid_copy(g_3d_vtex, NULL, &g_game_viewport);
            }
        }
        g_3d_shown = drawn3d;
        if (!drawn3d) {
            VidTex* gt = gpu_shown ? gpu_screen_vtex() : NULL;   /* passo 14c-2 */
            vid_copy(gt ? gt : g_texture, NULL, &g_game_viewport);
            g_gpu_screen_shown = gt != NULL;
        } else {
            g_gpu_screen_shown = true;
        }
    }

    vid_imgui_new_frame();   /* passo 14a */
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& imgui_io = ImGui::GetIO();
    imgui_io.FontGlobalScale = settings_ui_scale_for_size(imgui_io.DisplaySize);
    /* Passo 7a: retangulo do gravador de poses e aviso "gravando". */
    if (hd_pack_io_sprite_rec_active() || (g_show_menu && g_rec_rect_enabled)) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float base_w = g_widescreen_mode ? (float)GB_WIDESCREEN_WIDTH : (float)GB_SCREEN_WIDTH;
        const float off = g_widescreen_mode ? (float)GB_WIDESCREEN_MARGIN : 0.0f;
        const float s = (float)g_game_viewport.w / base_w;
        if (g_rec_rect_enabled) {
            const ImVec2 a((float)g_game_viewport.x + (off + (float)g_rec_rect[0]) * s,
                           (float)g_game_viewport.y + (float)g_rec_rect[1] * s);
            const ImVec2 b(a.x + (float)g_rec_rect[2] * s, a.y + (float)g_rec_rect[3] * s);
            fg->AddRect(a, b, IM_COL32(255, 60, 60, 220), 0.0f, 0, 2.0f);
        }
        if (hd_pack_io_sprite_rec_active()) {
            char rec_text[64];
            snprintf(rec_text, sizeof(rec_text), "REC poses: %u", hd_pack_io_sprite_rec_pose_count());
            const ImVec2 t((float)g_game_viewport.x + (float)g_game_viewport.w - 150.0f,
                           (float)g_game_viewport.y + 8.0f);
            fg->AddRectFilled(ImVec2(t.x - 6.0f, t.y - 3.0f), ImVec2(t.x + 140.0f, t.y + 18.0f),
                              IM_COL32(0, 0, 0, 160), 4.0f);
            fg->AddCircleFilled(ImVec2(t.x + 4.0f, t.y + 7.0f), 5.0f, IM_COL32(255, 40, 40, 255));
            fg->AddText(ImVec2(t.x + 14.0f, t.y), IM_COL32(255, 255, 255, 255), rec_text);
        }
    }

    if (g_show_menu) {
        const float ui_scale = imgui_io.FontGlobalScale;
        const float footer_height = ImGui::GetFrameHeightWithSpacing() *
                                    (g_launcher_return_enabled ? 3.8f : 3.0f);

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(imgui_io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * ui_scale, 16.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * ui_scale, 10.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * ui_scale, 10.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 18.0f * ui_scale);
        ImGui::Begin("GameBoy Recompiled",
                     &g_show_menu,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Bigodão - Wario Land: Super Mario Land 3");
        {   /* passo 15a: idioma do menu, no canto de cima */
            const char* kLangs[] = {"English", "Portugues"};
            const float lw = 170.0f * ui_scale;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lw - ImGui::CalcTextSize("Language / Idioma").x -
                            ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(lw);
            int lang = g_ui_lang;
            if (ImGui::Combo("Language / Idioma", &lang, kLangs, 2)) {
                g_ui_lang = lang;
                save_runtime_preferences();
            }
        }
        if (ImGui::Button(TR("Resume", "Continuar"))) {
            g_show_menu = false;
        }
        ImGui::SameLine();   /* passo 14p: reiniciar o jogo (com confirmação) */
        if (ImGui::Button(TR("Reset game", "Reiniciar jogo"))) {
            ImGui::OpenPopup("##reiniciar");
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("##reiniciar", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(TR("Reset the game?", "Reiniciar o jogo?"));
            ImGui::TextUnformatted(TR("The game goes back to the title screen.", "O jogo volta para a tela de titulo."));
            ImGui::TextUnformatted(TR("The cartridge save stays; anything not saved is lost.",
                                      "O save do cartucho continua; o que nao foi salvo se perde."));
            if (ImGui::Button(TR("Reset", "Reiniciar"))) {
                g_game_reset_pending = true;
                g_show_menu = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Cancel", "Cancelar"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
#if defined(__ANDROID__)
        ImGui::TextDisabled("   %s / %s", controller_menu_hint_text(), TR("Back: open or close the menu", "Voltar: abre ou fecha o menu"));
#else
        ImGui::TextDisabled("   %s / %s", controller_menu_hint_text(), TR("Esc: open or close the menu", "Esc: abre ou fecha o menu"));
#endif
        ImGui::Separator();
        /* Passo 15a: menu com a lista de categorias à esquerda (opção B escolhida
         * pelo usuário) e a página da categoria à direita */
        static const char* const kMenuCatsEn[] = {"Display", "3D", "Audio", "Controls", "Emulation", "Mods", "Developer"};
        static const char* const kMenuCatsPt[] = {"Video", "3D", "Som", "Controles", "Emulacao", "Mods", "Desenvolvedor"};
        const float side_w = 210.0f * ui_scale;
        ImGui::BeginChild("Categorias", ImVec2(side_w, -footer_height), true);
        for (int c = 0; c < 7; ++c) {
            if (c == 6) ImGui::Separator();
            if (ImGui::Selectable(g_ui_lang == 1 ? kMenuCatsPt[c] : kMenuCatsEn[c], g_menu_page == c, 0,
                                  ImVec2(0.0f, ImGui::GetFrameHeight()))) {
                g_menu_page = c;
                save_runtime_preferences();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), true);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);   /* 15a: sobra espaço para o nome à direita */
        if (g_menu_page == 0) {   /* Display */
            ImGui::SeparatorText(TR("Window", "Janela"));
            bool fullscreen = g_fullscreen;
            if (ImGui::Checkbox(TR("Fullscreen", "Tela cheia"), &fullscreen)) {
                set_fullscreen_enabled(fullscreen);
            }

            if (!g_fullscreen) {
                int scale_idx = g_scale - 1;
                if (ImGui::Combo(TR("Window size", "Tamanho da janela"),
                                 &scale_idx,
                                 g_scale_names,
                                 IM_ARRAYSIZE(g_scale_names))) {
                    g_scale = scale_idx + 1;
                    apply_window_scale_preset();
                }
            } else {
                ImGui::TextDisabled(TR("Window size is disabled in fullscreen.", "Tamanho da janela nao vale em tela cheia."));
            }

            if (ImGui::Checkbox("V-Sync", &g_vsync)) {
                vid_set_vsync(g_vsync);   /* passo 14a */
            }
            ImGui::SeparatorText(TR("Image", "Imagem"));
            int current_internal_scale = g_internal_res_scale - 1;
            const char* res_names[] = {TR("1x (native, 160x144)", "1x (nativo, 160x144)"), "2x (320x288)", "3x (480x432)", "4x (640x576)"};   /* 15a */
            if (ImGui::Combo(TR("Internal resolution", "Resolucao interna"), &current_internal_scale, res_names, IM_ARRAYSIZE(res_names))) {
                g_internal_res_scale = current_internal_scale + 1;
                g_renderer_reset_pending = true;
                save_runtime_preferences();
            }

            if (effective_internal_scale() == 1 && g_internal_res_scale > 1) {
                ImGui::TextDisabled(TR("No effect with the Nearest filter and no post-processing: the GPU already scales up.", "Sem efeito com filtro Nearest e sem pos-processamento: a GPU ja amplia."));
            }


            int filter_mode = (int)g_render_filter_mode;
            if (ImGui::Combo(TR("Scale filter", "Filtro de escala"),
                             &filter_mode,
                             g_render_filter_names,
                             IM_ARRAYSIZE(g_render_filter_names))) {
                g_render_filter_mode = (GBRenderFilterMode)filter_mode;
                update_render_filter();
                save_runtime_preferences();
            }
            if (g3d_active() && g_render_filter_mode == GB_RENDER_FILTER_LINEAR) {   /* 3D-6a */
                ImGui::TextDisabled(TR("In 3D the scale filter is always Nearest.", "No 3D o filtro de escala fica sempre em Nearest."));
            }

            int current_post_filter = (int)g_post_filter_mode;
            if (ImGui::Combo(TR("Post-processing filter", "Filtro de pos-processamento"), &current_post_filter, post_processing_get_names(), GB_POST_FILTER_COUNT)) {
                g_post_filter_mode = (GBPostFilterMode)current_post_filter;
                save_runtime_preferences();
            }

            ImGui::SeparatorText("Widescreen");
            bool widescreen = g_widescreen_mode;
            if (ImGui::Checkbox("Widescreen (16:9)", &widescreen)) {
                GBPPU* ppu = active_ppu();
                if (widescreen && !ppu) {
                    widescreen = false;
                }
                g_widescreen_mode = widescreen;
                if (ppu) {
                    ppu_set_widescreen_enabled(ppu, g_widescreen_mode);
                }
                g_renderer_reset_pending = true;
                update_game_viewport();
                save_runtime_preferences();
            }
            if (g_widescreen_mode) {
                ImGui::TextDisabled(TR("Extends the background at the sides; may reveal unused tiles.", "Estende o cenario nas laterais; pode mostrar tiles nao usados."));

                /* Passo 8a */
                int fill_mode = g_ws_fill_mode;
                const char* fill_names[] = {TR("Black", "Preto"), TR("Blurred edge", "Borda desfocada"), TR("Blurred center", "Centro desfocado")};
                if (ImGui::Combo(TR("Sides without scenery", "Laterais sem cenario"), &fill_mode, fill_names, IM_ARRAYSIZE(fill_names))) {
                    g_ws_fill_mode = fill_mode;
                    g_ws_amb_valid = false;
                    save_runtime_preferences();
                }
                if (g_ws_fill_mode != WS_FILL_BLACK) {
                    int dim = g_ws_fill_dim_percent;
                    if (ImGui::SliderInt(TR("Darken sides (%)", "Escurecer laterais (%)"), &dim, 0, 90)) {
                        g_ws_fill_dim_percent = dim;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        save_runtime_preferences();
                    }
                }
                ImGui::TextDisabled(TR("Title, COURSE, boss, transitions. Room walls stay black.", "Titulo, COURSE, chefe, transicoes. Paredes das salas ficam pretas."));

                bool fade_enabled = g_widescreen_fade_enabled;
                if (ImGui::Checkbox(TR("Edge fade", "Esmaecer bordas"), &fade_enabled)) {
                    g_widescreen_fade_enabled = fade_enabled;
                    GBPPU* ppu = active_ppu();
                    if (ppu) {
                        ppu_set_widescreen_fade_enabled(ppu, g_widescreen_fade_enabled);
                    }
                    save_runtime_preferences();
                }

                if (g_widescreen_fade_enabled) {
                    int fade_percent = g_widescreen_fade_percent;
                    if (ImGui::SliderInt(TR("Edge fade amount (%)", "Intensidade do esmaecimento (%)"), &fade_percent, 0, 100)) {
                        g_widescreen_fade_percent = fade_percent;
                        GBPPU* ppu = active_ppu();
                        if (ppu) {
                            ppu_set_widescreen_fade_amount(ppu, (float)g_widescreen_fade_percent / 100.0f);
                        }
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        save_runtime_preferences();
                    }
                }
            }


            ImGui::SeparatorText(TR("Renderer", "Renderizador"));
            {   /* Passo 14a: caminho de vídeo (vale ao abrir o jogo de novo). */
                const char* kVideoNames[] = {TR("SDL (default)", "SDL (padrao)"), "OpenGL"};
                int video = g_video_backend_pref;
                if (ImGui::Combo("Video", &video, kVideoNames, IM_ARRAYSIZE(kVideoNames))) {
                    g_video_backend_pref = video;
                    save_runtime_preferences();
                }
                if (g_video_backend_pref != (g_use_gl ? 1 : 0)) {
                    ImGui::TextDisabled(TR("Close and reopen the game to switch.", "Feche e abra o jogo para trocar."));
                }
                ImGui::TextDisabled(TR("In use: %s", "Em uso: %s"), g_video_status.c_str());
            }
            {   /* Passo 14c-2: quem monta a tela */
                const char* kScreenNames[] = {"CPU", TR("GPU (OpenGL, default)", "GPU (OpenGL, padrao)")};
                int scr = g_gpu_screen_pref;
                if (ImGui::Combo(TR("Screen composition", "Montagem da tela"), &scr, kScreenNames, IM_ARRAYSIZE(kScreenNames))) {
                    g_gpu_screen_pref = scr;
                    g_gpu_screen_error.clear();   /* tenta de novo */
                    save_runtime_preferences();
                }
                if (g_gpu_screen_pref == 1) {
                    const char* why = gpu_screen_blocker();
                    if (why) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), TR("Composing on the CPU: %s.", "Montando na CPU: %s."), why);
                    } else {
                        ImGui::TextDisabled(TR("In use: %s", "Em uso: %s"), g_gpu_screen_shown ? "GPU" : TR("CPU (waiting for the GPU)", "CPU (esperando a GPU)"));
                    }
                }
            }
            {   /* passo 10a */
                int fps_mode = g_fps_mode;
                const char* fps_names[] = {TR("60 fps (original)", "60 fps (original)"), TR("120 fps (in-between frames)", "120 fps (quadros intermediarios)")};   /* 15a */
                if (ImGui::Combo(TR("Frame rate", "Quadros por segundo"), &fps_mode, fps_names, IM_ARRAYSIZE(fps_names))) {
                    g_fps_mode = fps_mode;
                    ip_invalidate();
                    save_runtime_preferences();
                }
                if (g_fps_mode == 1) {
                    ImGui::TextDisabled(TR("The game still runs at 60: physics and music at the same speed.", "O jogo continua a 60: fisica e musica na mesma velocidade."));
                    ImGui::TextDisabled(TR("In-between frame = halfway between the previous and the current one (~8 ms delay).", "Quadro do meio = meio do caminho entre o anterior e o atual (atraso ~8 ms)."));
                    const int hz = ip_display_refresh_hz();
                    if (!ip_refresh_ok()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                           TR("Monitor at %d Hz: needs 100 Hz or more. In-between frame off.", "Monitor a %d Hz: precisa de 100 Hz ou mais. Quadro do meio desligado."), hz);
                    } else if (effective_speed_percent() != 100) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                           TR("Off while the speed is not 100%%.", "Desligado enquanto a velocidade nao for 100%%."));
                    } else if (hz > 0) {
                        ImGui::TextDisabled(TR("Monitor: %d Hz. In-between frames: %u per second.", "Monitor: %d Hz. Quadros do meio: %u por segundo."), hz, ip_mid_per_second());
                        char ip_why[256];
                        ip_skip_text(ip_why, sizeof(ip_why));
                        ImGui::TextDisabled("%s", ip_why);   /* passo 10b */
                    } else {
                        ImGui::TextDisabled(TR("Monitor: unknown refresh rate. In-between frames: %u per second.", "Monitor: frequencia desconhecida. Quadros do meio: %u por segundo."), ip_mid_per_second());
                    }
                }
            }
            ImGui::SeparatorText(TR("Info", "Informacoes"));
            {
                int window_w = 0;
                int window_h = 0;
                SDL_GetWindowSize(g_window, &window_w, &window_h);
                ImGui::TextDisabled(TR("Performance: %.1f FPS", "Desempenho: %.1f FPS"), ImGui::GetIO().Framerate);
                ImGui::TextDisabled(TR("Window: %d x %d", "Janela: %d x %d"), window_w, window_h);
                ImGui::TextDisabled(TR("Game area: %d x %d (%.2fx)", "Area do jogo: %d x %d (%.2fx)"),
                                    g_game_viewport.w,
                                    g_game_viewport.h,
                                    (double)g_game_viewport.w / (double)GB_SCREEN_WIDTH);
            }
        } else if (g_menu_page == 1) {   /* 3D */
            {   /* Passo 3D-1: modo 3D em camadas; 15a: em grupos */
                const char* k3DNames[] = {TR("Off", "Desligado"), TR("Layers", "Camadas")};
                ImGui::SeparatorText(TR("3D mode", "Modo 3D"));
                int m3 = g_3d_mode;
                if (ImGui::Combo(TR("3D mode", "Modo 3D"), &m3, k3DNames, IM_ARRAYSIZE(k3DNames))) {
                    g_3d_mode = m3;
                    save_runtime_preferences();
                }
                if (g_3d_mode == 1) {
                    const char* why = g3d_blocker();
                    if (why) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), TR("3D stopped: %s.", "3D parado: %s."), why);
                    } else {
                        ImGui::TextDisabled(TR("In use: %s", "Em uso: %s"),
                                            g_3d_shown ? TR("3D (layers)", "3D (camadas)") : TR("2D (waiting for a frame)", "2D (esperando o quadro)"));
                        ImGui::TextDisabled("%s", TR("In 3D the scale filter is always Nearest.", "No 3D o filtro de escala fica sempre em Nearest."));
                        if (g_post_filter_mode != GB_POST_FILTER_NONE || g_fps_mode == 1) {
                            ImGui::TextDisabled("%s", TR("In 3D the post-processing filter and the in-between frame (120 fps) are off.",
                                                         "No 3D o filtro de pos-processamento e o quadro do meio (120 fps) ficam desligados."));
                        }
                    }
                    ImGui::SeparatorText(TR("Camera", "Camera"));
                    if (ImGui::SliderInt(TR("Tilt (degrees)", "Inclinacao (graus)"), &g_3d_tilt, -35, 35)) save_runtime_preferences();
                    if (ImGui::SliderInt(TR("Turn (degrees)", "Giro (graus)"), &g_3d_yaw, -35, 35)) save_runtime_preferences();
                    {   /* passo 3D-5a / 3D-4q */
                        bool st = g_3d_stick != 0;
                        if (ImGui::Checkbox(TR("Right stick moves the camera", "Analogico direito mexe a camera"), &st)) { g_3d_stick = st ? 1 : 0; save_runtime_preferences(); }
                        if (g_3d_stick) ImGui::TextDisabled("%s", TR("Right stick: tilts and turns; let go and it comes back.", "Analogico direito: inclina e gira; soltando, volta ao padrao."));
                        bool fw = g_3d_follow != 0;
                        if (ImGui::Checkbox(TR("Camera follows Wario", "Camera segue o Wario"), &fw)) { g_3d_follow = fw ? 1 : 0; save_runtime_preferences(); }
                        if (g_3d_follow) ImGui::TextDisabled("%s", TR("In levels the turn faces the side Wario is walking to.", "Nas fases o giro vira para o lado em que o Wario anda."));
                    }
                    ImGui::SeparatorText(TR("Depth", "Profundidade"));
                    if (ImGui::SliderInt(TR("Depth (%)", "Profundidade (%)"), &g_3d_depth, 0, 200)) save_runtime_preferences();
                    if (ImGui::SliderInt(TR("Sprite thickness (%)", "Espessura dos sprites (%)"), &g_3d_spr, 5, 100)) save_runtime_preferences();   /* 3D-5e */
                    if (ImGui::SliderInt(TR("Front layer (% of a block, 0 = off)", "Plano da frente (% de bloco, 0 = sem)"), &g_3d_front, 0, 300)) save_runtime_preferences();   /* 3D-6a */
                }
            }

            ImGui::SeparatorText(TR("Tools", "Ferramentas"));
            /* Passo 3D-3f: diagnóstico do 3D */
            ImGui::Checkbox(TR("Show classes", "Mostrar classes"), &g_3d_debug);
            {   /* passo 3D-2b */
                ImGui::Checkbox(TR("Mark tiles (outside levels)", "Marcar tiles (fora das fases)"), &g_3d_mark);
                ImGui::SameLine();   /* passo 3D-2c */
                if (ImGui::Button(g_freeze ? TR("Unfreeze the game", "Descongelar o jogo") : TR("Freeze the game", "Congelar o jogo"))) g_freeze = !g_freeze;
                if (g_3d_mark) {
                    ImGui::TextWrapped("%s", TR("Close the menu and click on the game: left click cycles background > floor > half floor > water > door/ladder; "
                                          "right click goes back to background. A mark is for that tile drawing on this screen only (g00, m44...), "
                                          "also on the window layer (waves, bars). While marking, the view is straight and the classes are painted.",
                                          "Feche o menu e clique no jogo: esquerdo troca fundo > chao > meio chao > agua > porta/escada; "
                                          "direito volta a fundo. Vale para o mesmo desenho de tile so nesta tela (g00, m44...), "
                                          "tambem na janela (ondas, faixas). Enquanto marca, a vista fica reta e com as classes pintadas."));
                    marks3d_load();
                    ImGui::TextDisabled(TR("%u tiles marked - %s", "%u tiles marcados - %s"), (unsigned)g_3d_marks.size(), marks3d_path().c_str());
                    {   /* passo 3D-2j: a tela onde as marcas valem agora */
                        const uint16_t cx = marks3d_context(g_registered_ctx);
                        char sfx[8];
                        ppu_native_hd_context_suffix(cx, sfx);
                        const char* nm = marks3d_context_name(cx);
                        ImGui::TextDisabled(TR("Current screen: %s%s%s%s", "Tela atual: %s%s%s%s"), sfx[0] ? sfx : TR("(none)", "(nenhuma)"), nm[0] ? " - " : "", nm, "");
                    }
                    if (!g_3d_mark_info.empty()) ImGui::TextDisabled(TR("Last: %s", "Ultimo: %s"), g_3d_mark_info.c_str());
                }
            }
            if (ImGui::TreeNode(TR("Hide parts (diagnostics)", "Esconder partes (diagnostico)"))) {   /* passo 3D-4f */
                const struct { const char* name; int bit; } kParts[] = {
                    {TR("Far", "Longe"), 0}, {TR("Background", "Fundo"), 1}, {TR("Water (back wall)", "Agua (parede do fundo)"), 5}, {TR("Door / coins / ladder", "Porta / moedas / escada"), 6},
                    {TR("Floor (front)", "Chao (frente)"), 4}, {TR("Waves (surface copies)", "Ondas (copias da superficie)"), 8}, {TR("Water surface floor", "Chao da superficie da agua"), 12},
                    {TR("Block and door sides", "Lados dos blocos e da porta"), 11}, {"Sprites", 3}, {"HUD", 2}};
                for (const auto& part : kParts) {
                    bool hid = (g_3d_hide >> part.bit) & 1;
                    if (ImGui::Checkbox(part.name, &hid)) g_3d_hide = hid ? (g_3d_hide | (1 << part.bit)) : (g_3d_hide & ~(1 << part.bit));
                }
                ImGui::TreePop();
            }
            if (g_3d_debug) {
                ImGui::TextWrapped("%s", TR("grey background, blue water, cyan wave band, light blue sky inside water, "
                                              "brown floor, yellow half floor, orange door/ladder/coin, magenta sprite, white HUD, black far",
                                              "cinza fundo, azul agua, ciano faixa das ondas, azul-claro ceu dentro da agua, "
                                              "marrom chao, amarelo meio chao, laranja porta/escada/moeda, magenta sprite, branco HUD, preto longe"));
            }
        } else if (g_menu_page == 2) {   /* Audio */
            if (ImGui::Checkbox(TR("Enable audio", "Ligar o som"), &g_audio_output_enabled)) {
                reset_audio_output_buffer(true);
                save_runtime_preferences();
            }
            bool audio_muted = g_audio_muted.load(std::memory_order_relaxed);
            if (ImGui::Checkbox(TR("Mute", "Mudo"), &audio_muted)) {
                g_audio_muted.store(audio_muted, std::memory_order_relaxed);
                save_runtime_preferences();
            }
            int audio_volume_percent =
                (int)g_audio_volume_percent.load(std::memory_order_relaxed);
            if (ImGui::SliderInt(TR("Master volume (%)", "Volume geral (%)"), &audio_volume_percent, 0, 200)) {
                g_audio_volume_percent.store((uint32_t)audio_volume_percent,
                                             std::memory_order_relaxed);
                save_runtime_preferences();
            }
            {   /* Passo 6e */
                int gb_volume_percent = (int)g_audio_gb_volume_percent.load(std::memory_order_relaxed);
                if (ImGui::SliderInt(TR("Game Boy volume (%)", "Volume do Game Boy (%)"), &gb_volume_percent, 0, 200)) {
                    g_audio_gb_volume_percent.store((uint32_t)gb_volume_percent, std::memory_order_relaxed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    save_runtime_preferences();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", TR("Only the Game Boy sound (GB effects and music).\nThe .ogg tracks have their own volume in Mods.",
                                              "So o som do Game Boy (efeitos e musica do GB).\nAs trilhas .ogg tem o volume delas em Mods."));
                }
            }
            if (ImGui::BeginCombo(TR("Output device", "Dispositivo de saida"), current_audio_output_device_label())) {
                bool selected_default = g_audio_target_device_name.empty();
                if (ImGui::Selectable(TR("System default", "Padrao do sistema"), selected_default)) {
                    g_audio_target_device_name.clear();
                    save_runtime_preferences();
                    reopen_audio_output_device(true);
                }
                if (selected_default) {
                    ImGui::SetItemDefaultFocus();
                }

                for (size_t i = 0; i < g_audio_output_devices.size(); i++) {
                    const bool selected = g_audio_output_devices[i] == g_audio_target_device_name;
                    if (ImGui::Selectable(g_audio_output_devices[i].c_str(), selected)) {
                        g_audio_target_device_name = g_audio_output_devices[i];
                        save_runtime_preferences();
                        reopen_audio_output_device(true);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            int audio_latency_ms = (int)g_audio_latency_ms;
            if (ImGui::SliderInt(TR("Target latency (ms)", "Latencia alvo (ms)"), &audio_latency_ms, 20, 250)) {
                g_audio_latency_ms = (uint32_t)audio_latency_ms;
                recompute_audio_targets();
                reset_audio_output_buffer(true);
                save_runtime_preferences();
            }
            if (ImGui::Button(TR("Refresh devices", "Atualizar dispositivos"))) {
                refresh_audio_output_devices();
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Reconnect", "Reconectar"))) {
                reopen_audio_output_device(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Clear buffer", "Limpar buffer"))) {
                reset_audio_output_buffer(true);
            }
            ImGui::Text(TR("Requested output: %s", "Saida pedida: %s"), current_audio_output_device_label());
            ImGui::Text(TR("Active output: %s", "Saida em uso: %s"), g_audio_active_device_name.empty() ? TR("Unavailable", "Indisponivel") : g_audio_active_device_name.c_str());
            if (!current_audio_output_device_available()) {
                ImGui::TextDisabled(TR("Requested device is unavailable. Using the default output when possible.", "O dispositivo pedido nao esta disponivel. Usando a saida padrao quando possivel."));
            }
            ImGui::Text(TR("Ring buffer: %u / %u samples", "Buffer: %u / %u amostras"), current_audio_ring_fill_samples(), current_audio_ring_capacity());
            ImGui::Text(TR("Device: %u Hz, device buffer %u, start %u, low-water %u", "Dispositivo: %u Hz, buffer %u, inicio %u, minimo %u"),
                        g_audio_device_sample_rate,
                        g_audio_device_buffer_samples,
                        g_audio_start_threshold,
                        g_audio_low_watermark);
            if (effective_speed_percent() != 100) {
                ImGui::TextDisabled(TR("Audio pauses while the speed is not 100%%.", "O som pausa enquanto a velocidade nao for 100%%."));
            }
            if (ImGui::Button(TR("Reset audio", "Restaurar som"))) {
                reset_runtime_audio_defaults();
            }

        } else if (g_menu_page == 3) {   /* Controls */
            if (!g_controller_name.empty()) {
                ImGui::Text(TR("Detected controller: %s", "Controle detectado: %s"), g_controller_name.c_str());
                ImGui::TextDisabled(TR("Profile: %s", "Perfil: %s"), controller_type_name(g_controller_type));
            } else {
    #if defined(__ANDROID__)
                ImGui::TextDisabled(TR("No controller detected. Controller mappings will apply when one is connected.", "Nenhum controle detectado. Os mapeamentos valem quando um for conectado."));
    #else
                ImGui::TextDisabled(TR("No controller detected.", "Nenhum controle detectado."));
    #endif
            }
            render_binding_editor(TR("Keyboard - game", "Teclado - jogo"),
                                  GB_CAPTURE_DEVICE_KEYBOARD,
                                  g_keyboard_bindings,
                                  GB_INPUT_ACTION_RIGHT,
                                  GB_INPUT_ACTION_FAST_FORWARD);
            ImGui::Spacing();
            render_binding_editor(TR("Controller - game", "Controle - jogo"),
                                  GB_CAPTURE_DEVICE_CONTROLLER,
                                  g_controller_bindings,
                                  GB_INPUT_ACTION_RIGHT,
                                  GB_INPUT_ACTION_FAST_FORWARD);
            ImGui::Spacing();
            render_binding_editor(TR("Keyboard - shortcuts", "Teclado - atalhos"),
                                  GB_CAPTURE_DEVICE_KEYBOARD,
                                  g_keyboard_bindings,
                                  GB_INPUT_ACTION_FAST_FORWARD,
                                  GB_INPUT_ACTION_COUNT);
            ImGui::Spacing();
            render_binding_editor(TR("Controller - shortcuts", "Controle - atalhos"),
                                  GB_CAPTURE_DEVICE_CONTROLLER,
                                  g_controller_bindings,
                                  GB_INPUT_ACTION_FAST_FORWARD,
                                  GB_INPUT_ACTION_COUNT);
            if (g_binding_capture_active) {
                ImGui::Separator();
                ImGui::TextDisabled(TR("Waiting for %s input for %s slot %d", "Esperando entrada do %s para %s, posicao %d"),
                                    g_binding_capture_device == GB_CAPTURE_DEVICE_KEYBOARD ? TR("keyboard", "teclado") : TR("controller", "controle"),
                                    action_name_ui(g_binding_capture_action),
                                    g_binding_capture_slot + 1);
                if (ImGui::Button(TR("Cancel", "Cancelar"))) {
                    cancel_binding_capture();
                }
            }
            {   /* [android-port] */
                bool touch_on = touch_controls::enabled();
                if (ImGui::Checkbox(TR("Touch controls (on-screen gamepad)", "Controles na tela (toque)"), &touch_on)) {
                    touch_controls::set_enabled(touch_on);
                    update_effective_joypad_state();
                    save_runtime_preferences();
                }
            }
            if (ImGui::Button(TR("Reset controls", "Restaurar controles"))) {
                reset_runtime_control_defaults();
            }
            ImGui::TextDisabled(TR("Shortcuts can also control fast forward, max speed, save states, overlay, mute and the menu.", "Os atalhos tambem controlam avanco rapido, velocidade maxima, estados salvos, overlay, mudo e o menu."));
            ImGui::BulletText(TR("%s: settings menu", "%s: menu de configuracoes"), controller_menu_hint_text());
    #if defined(__ANDROID__)
            ImGui::BulletText(TR("Android Back / Escape: settings menu", "Voltar do Android / Escape: menu de configuracoes"));
    #else
            ImGui::BulletText(TR("Escape: settings menu", "Escape: menu de configuracoes"));
    #endif

        } else if (g_menu_page == 4) {   /* Emulation */
            ImGui::SeparatorText(TR("Speed", "Velocidade"));
            ImGui::Checkbox(TR("Show overlay", "Mostrar overlay"), &g_show_overlay);
            ImGui::Checkbox(TR("Smooth slow frames", "Suavizar quadros lentos"), &g_smooth_lcd_transitions);
            if (ImGui::SliderInt(TR("Speed (%)", "Velocidade (%)"), &g_speed_percent, 10, 500)) {
                reset_audio_output_buffer(true);
            }
            if (ImGui::Button(TR("Reset speed", "Velocidade padrao"))) {
                g_speed_percent = 100;
                g_max_speed_mode = false;
                reset_audio_output_buffer(true);
            }
            ImGui::Text(TR("Effective speed: %d%%", "Velocidade efetiva: %d%%"), effective_speed_percent());
            if (g_fast_forward_active) {
                ImGui::TextDisabled(TR("Fast forward shortcut is active.", "Atalho de avanco rapido ativo."));
            } else if (g_max_speed_mode) {
                ImGui::TextDisabled(TR("Max speed shortcut is active.", "Atalho de velocidade maxima ativo."));
            }

            ImGui::SeparatorText(TR("Save states", "Estados salvos"));
            int savestate_slot = g_savestate_slot + 1;
            if (ImGui::SliderInt(TR("Slot", "Posicao"), &savestate_slot, 1, GB_SAVESTATE_SLOT_COUNT)) {
                g_savestate_slot = savestate_slot - 1;
                save_runtime_preferences();
            }
            if (ImGui::Button(TR("Save state (F5)", "Salvar estado (F5)"))) {
                save_savestate_slot(g_registered_ctx, g_savestate_slot);
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Load state (F8)", "Carregar estado (F8)"))) {
                load_savestate_slot(g_registered_ctx, g_savestate_slot);
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Delete slot", "Apagar posicao"))) {
                delete_savestate_slot(g_registered_ctx, g_savestate_slot);
            }
            {
                std::string savestate_path;
                const bool slot_exists = savestate_slot_exists(g_registered_ctx, g_savestate_slot, &savestate_path);
                ImGui::Text(TR("File: %s", "Arquivo: %s"), savestate_path.empty() ? TR("(unavailable)", "(indisponivel)") : savestate_path.c_str());
                ImGui::Text(TR("Status: %s", "Situacao: %s"), slot_exists ? TR("Saved", "Gravado") : TR("Empty", "Vazio"));
            }
            if (!g_savestate_status.empty()) {
                ImGui::TextWrapped("%s", g_savestate_status.c_str());
            }

        } else if (g_menu_page == 5) {   /* Mods */
            bool hd_tiles = ppu_native_hd_get_tiles_enabled();
            if (ImGui::Checkbox(TR("HD tiles", "Tiles HD"), &hd_tiles)) {
                ppu_native_hd_set_tiles_enabled(hd_tiles);
                g_renderer_reset_pending = true;
                save_runtime_preferences();
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Reload", "Recarregar"))) {
                hd_pack_io_load_pack();
                g_renderer_reset_pending = true;
            }
            ImGui::TextDisabled("%s", hd_pack_io_pack_status());
            {
                /* Passo 11a: contexto atual, que vira o sufixo _fXX/_mXX dos tiles. */
                char hd_ctx[8];
                ppu_native_hd_context_suffix(ppu_native_hd_context(), hd_ctx);
                if (hd_ctx[0]) {
                    ImGui::TextDisabled(TR("Current context: %s (suffix _%s)", "Contexto agora: %s (sufixo _%s)"),
                                        hd_ctx[0] == 'f' ? TR("level", "fase") : hd_ctx[0] == 'm' ? TR("map", "mapa") : TR("screen", "tela"),
                                        hd_ctx);
                } else {
                    ImGui::TextDisabled(TR("Current context: none (only _pN and generic)", "Contexto agora: nenhum (so _pN e genericos)"));
                }
                if (ppu_native_hd_variant_count() > 0) {
                    ImGui::TextDisabled(TR("Tiles per level/palette: %u", "Tiles por fase/paleta: %u"),
                                        ppu_native_hd_variant_count());
                }
            }
            {   /* Passo 6c: trilhas .ogg */
                bool music_on = music_replace_get_enabled();
                if (ImGui::Checkbox(TR(".ogg tracks", "Trilhas .ogg"), &music_on)) {
                    music_replace_set_enabled(music_on);
                    save_runtime_preferences();
                }
                ImGui::SameLine();
                float music_vol = music_replace_get_volume() * 100.0f;
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Volume##trilhas", &music_vol, 0.0f, 200.0f, "%.0f%%")) {
                    music_replace_set_volume(music_vol / 100.0f);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    save_runtime_preferences();
                }
                if (music_replace_track_count() > 0) {
                    /* Passo 6e: volume do GB aqui tambem, para equilibrar. */
                    int gb_vol = (int)g_audio_gb_volume_percent.load(std::memory_order_relaxed);
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::SliderInt(TR("Game Boy volume##mods", "Volume do GB##mods"), &gb_vol, 0, 200, "%d%%")) {
                        g_audio_gb_volume_percent.store((uint32_t)gb_vol, std::memory_order_relaxed);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        save_runtime_preferences();
                    }
                    ImGui::TextDisabled(TR("%d track(s): %s", "%d trilha(s): %s"), music_replace_track_count(),
                                        music_replace_track_list());
                } else {
                    ImGui::TextDisabled(TR("No tracks (mods/music/XX.ogg or music/ inside a .o2r)", "Nenhuma trilha (mods/music/XX.ogg ou music/ dentro de um .o2r)"));
                }
            }
            if (hd_tiles && ppu_native_hd_tile_count() > 0 && g_internal_res_scale < 2) {
                ImGui::TextDisabled(TR("Choose internal resolution 2x or more to see the HD tiles.", "Escolha resolucao interna 2x ou mais para ver os tiles HD."));
            }
            const int mod_count = hd_pack_io_mod_count();
            if (mod_count == 0) {
                ImGui::TextWrapped(TR("Put .o2r files in %s and click Reload.", "Coloque arquivos .o2r em %s e clique em Recarregar."),
                                   hd_pack_io_mods_dir());
            } else {
                ImGui::TextDisabled(TR("When the same tile is in more than one mod, the lower one wins.", "Quando o mesmo tile esta em mais de um mod, vale o de baixo."));
                for (int i = 0; i < mod_count; ++i) {
                    ImGui::PushID(i);
                    bool mod_on = hd_pack_io_mod_enabled(i);
                    std::string mod_label = hd_pack_io_mod_name(i);
                    if (hd_pack_io_mod_is_folder(i)) {
                        mod_label += TR(" (folder)", " (pasta)");
                    }
                    if (ImGui::Checkbox(mod_label.c_str(), &mod_on)) {
                        hd_pack_io_set_mod_enabled(i, mod_on);
                        g_renderer_reset_pending = true;
                        save_runtime_preferences();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", hd_pack_io_mod_info(i));
                    ImGui::PopID();
                }
                ImGui::TextWrapped(TR("Folder: %s", "Pasta: %s"), hd_pack_io_mods_dir());
            }
        } else {   /* Developer */
            int renderer_mode = (int)ppu_native_get_mode();
            const char* renderer_names[PPU_RENDERER_COUNT];
            for (int i = 0; i < PPU_RENDERER_COUNT; ++i) {
                renderer_names[i] = ppu_native_mode_name((PPURendererMode)i);
            }
            if (ImGui::Combo(TR("Renderer", "Renderizador"), &renderer_mode, renderer_names, PPU_RENDERER_COUNT)) {
                ppu_native_set_mode((PPURendererMode)renderer_mode);
                ppu_native_reset_compare_stats();
                save_runtime_preferences();
            }
            bool hd_check = ppu_native_get_hd_check();
            if (ImGui::Checkbox(TR("Check HD sampling", "Conferir amostragem HD"), &hd_check)) {
                ppu_native_set_hd_check(hd_check);
                ppu_native_reset_hd_stats();
            }
            ImGui::TextDisabled(TR("HD scale: %dx (follows the texture scale)", "Escala HD: %dx (segue a escala da textura)"),
                                ppu_native_get_hd_scale());
            ImGui::Checkbox(TR("Use the HD frame on screen", "Usar frame HD na tela"), &g_use_hd_frame);
            {   /* Passo 7h */
                bool attr_dbg = ppu_get_widescreen_attr_debug();
                if (ImGui::Checkbox(TR("Sides: show color source", "Laterais: mostrar origem da cor"), &attr_dbg)) {
                    ppu_set_widescreen_attr_debug(attr_dbg);
                }
                if (attr_dbg) {
                    ImGui::TextDisabled(TR("no tint = VRAM | green = cache | blue = calibrated DX table", "sem tinta = VRAM | verde = cache | azul = tabela DX calibrada"));
                    ImGui::TextDisabled(TR("yellow = learned block | magenta = table by level", "amarelo = bloco aprendido | magenta = tabela pela fase"));
                    ImGui::TextDisabled(TR("cyan = color of the same tile on screen (7j/7k)", "ciano = cor do mesmo tile na tela (7j/7k)"));
                    ImGui::TextDisabled(TR("red = tile guess | grey = nothing", "vermelho = palpite do tile | cinza = nada"));
                    const int page = ppu_get_dx_page();
                    if (page >= 0) ImGui::TextDisabled(TR("DX table: page %02X", "Tabela DX: pagina %02X"), page);
                    else ImGui::TextDisabled(TR("DX table: not calibrated", "Tabela DX: sem calibracao"));
                    if (g_registered_ctx) {   /* passo 7r */
                        ImGui::TextDisabled(TR("game_mode_2: %u (21 level, 22 boss, 23 loading, 13 map preparing, 14 COURSE)", "game_mode_2: %u (21 fase, 22 chefe, 23 carregando, 13 mapa se preparando, 14 COURSE)"),
                                            (unsigned)gb_wl_game_mode_2(g_registered_ctx));
                        ImGui::Checkbox(TR("Log state to the terminal ([Estado] ...)", "Registrar estado no terminal ([Estado] ...)"), &g_state_log);   /* passo 7t */
                    }
                }
            }
            bool rate_control = g_audio_rate_control.load(std::memory_order_relaxed);
            if (ImGui::Checkbox(TR("Audio: rate control (mixer)", "Audio: controle de taxa (mixer)"), &rate_control)) {
                g_audio_rate_control.store(rate_control, std::memory_order_relaxed);
            }
            /* Passo 6b: musicas vistas (mais nova em cima). */
            if (ImGui::TreeNode(TR("Songs seen", "Musicas vistas"))) {
                if (ImGui::Checkbox(TR("Log to the terminal ([Musica] ...)", "Registrar no terminal ([Musica] ...)"), &g_music_log)) {
                    music_replace_set_log(g_music_log);
                    save_runtime_preferences();
                }
                ImGui::TextDisabled(TR("Playing: %s%02X   ($A61D; requested at $A61C)", "Tocando: %s%02X   ($A61D; pedido em $A61C)"),
                                    g_music_current < 0 ? "-- " : "",
                                    (unsigned)(g_music_current < 0 ? 0 : g_music_current));
                const uint32_t shown = g_music_history_total < MUSIC_HISTORY_MAX
                    ? g_music_history_total : MUSIC_HISTORY_MAX;
                for (uint32_t i = 0; i < shown; i++) {
                    const MusicHistoryEntry& e =
                        g_music_history[(g_music_history_total - 1u - i) % MUSIC_HISTORY_MAX];
                    ImGui::Text(TR("%02X   mode %02X   frame %u", "%02X   modo %02X   frame %u"),
                                (unsigned)e.song, (unsigned)e.game_mode, e.frame);
                }
                if (shown == 0) {
                    ImGui::TextDisabled(TR("(none yet)", "(nenhuma ainda)"));
                }
                if (ImGui::Button(TR("Clear list", "Limpar lista"))) {
                    g_music_history_total = 0;
                }
                ImGui::TreePop();
            }
            /* Passo 6c.1: laboratorio de audio. */
            if (ImGui::TreeNode(TR("Audio lab", "Laboratorio de audio"))) {
                const char* kChannelNames[4] = {TR("CH1 pulse", "CH1 pulso"), TR("CH2 pulse", "CH2 pulso"), TR("CH3 wave", "CH3 onda"), TR("CH4 noise", "CH4 ruido")};
                uint8_t mute = gb_audio_get_channel_mute_mask();
                for (int ch = 0; ch < 4; ch++) {
                    bool m = (mute >> ch) & 1;
                    char label[32];
                    snprintf(label, sizeof(label), TR("Mute %d", "Mudo %d"), ch + 1);
                    if (ch > 0) ImGui::SameLine();
                    if (ImGui::Checkbox(label, &m)) {
                        mute = (uint8_t)(m ? (mute | (1u << ch)) : (mute & ~(1u << ch)));
                        gb_audio_set_channel_mute_mask(mute);
                    }
                }
                GBAudioChannelInfo info[4];
                gb_audio_get_channel_info(g_registered_ctx ? g_registered_ctx->apu : NULL, info);
                for (int ch = 0; ch < 4; ch++) {
                    const char* pan = info[ch].panning == 3 ? "E+D" : info[ch].panning == 2 ? "E" :
                                      info[ch].panning == 1 ? "D" : "-";
                    ImGui::TextColored(info[ch].enabled ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f)
                                                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                       TR("%s  %s  vol %2d  %s %04X  out %-3s  triggers %u", "%s  %s  vol %2d  %s %04X  saida %-3s  disparos %u"),
                                       kChannelNames[ch], info[ch].enabled ? "ON " : "off",
                                       info[ch].volume, ch == 3 ? "nr43" : "freq",
                                       (unsigned)info[ch].freq, pan, info[ch].triggers);
                }
                /* Passo 6c.2: silenciar a musica do GB deixando os efeitos. */
                ImGui::Separator();
                bool gate = g_audio_lab_gate;
                if (ImGui::Checkbox(TR("Silence GB music (keep only effects)", "Silenciar musica do GB (deixar so efeitos)"), &gate)) {
                    g_audio_lab_gate = gate;
                }
                if (music_replace_active()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(TR("(on because of the .ogg track)", "(ligado pela trilha .ogg)"));
                }
                gate = gb_audio_get_gate_enabled();
                {
                    const char* kGateModes[3] = {TR("always play", "tocar sempre"), TR("muted", "mudo"), TR("only with effect", "so com efeito")};
                    const uint8_t open_mask = gb_audio_get_gate_open_mask();
                    for (int ch = 0; ch < 4; ch++) {
                        int mode = 0;
                        uint16_t addr = 0;
                        gb_audio_get_channel_gate(ch, &mode, &addr);
                        ImGui::PushID(1000 + ch);
                        ImGui::Text("CH%d", ch + 1);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(130.0f);
                        bool changed = ImGui::Combo("##modo", &mode, kGateModes, 3);
                        if (mode == 2) {
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(70.0f);
                            changed |= ImGui::InputScalar("##addr", ImGuiDataType_U16, &addr, NULL, NULL,
                                                          "%04X", ImGuiInputTextFlags_CharsHexadecimal);
                            const int v = read_cart_ram_byte(addr);
                            ImGui::SameLine();
                            ImGui::Text("= %02X", (unsigned)(v < 0 ? 0 : v));
                        }
                        if (gate) {
                            ImGui::SameLine();
                            const bool open = (open_mask >> ch) & 1;
                            ImGui::TextColored(open ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f)
                                                    : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               open ? TR("[open]", "[passa]") : TR("[closed]", "[fechado]"));
                        }
                        if (changed) {
                            gb_audio_set_channel_gate(ch, mode, addr);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::Separator();
                uint16_t base = g_audio_lab_base;
                const uint16_t step = 0x40, step_fast = 0x100;
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputScalar(TR("Start (hex)", "Inicio (hex)"), ImGuiDataType_U16, &base, &step, &step_fast,
                                       "%04X", ImGuiInputTextFlags_CharsHexadecimal)) {
                    if (base < 0xA000) base = 0xA000;
                    if (base > 0xBFFF - AUDIO_LAB_BYTES + 1) base = (uint16_t)(0xBFFF - AUDIO_LAB_BYTES + 1);
                    g_audio_lab_base = base;
                }
                ImGui::TextDisabled(TR("Yellow = changed now; orange = in the last second.", "Amarelo = mudou agora; laranja = no ultimo segundo."));
                for (int row = 0; row < AUDIO_LAB_BYTES / 16; row++) {
                    ImGui::Text("%04X:", (unsigned)(g_audio_lab_base + row * 16));
                    for (int col = 0; col < 16; col++) {
                        const int i = row * 16 + col;
                        ImGui::SameLine(0.0f, col == 8 ? 10.0f : 4.0f);
                        const uint32_t age = g_audio_lab_changed_at[i] == 0
                            ? 0xFFFFFFFFu : g_audio_lab_frames - g_audio_lab_changed_at[i];
                        ImVec4 color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                        if (age < 8) color = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                        else if (age < 60) color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                        else if (g_audio_lab_change_count[i] == 0) color = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
                        if (g_audio_lab_value[i] < 0) {
                            ImGui::TextColored(color, "--");
                        } else {
                            ImGui::TextColored(color, "%02X", (unsigned)g_audio_lab_value[i]);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(TR("$%04X = %02X  (changed %u times)", "$%04X = %02X  (mudou %u vezes)"),
                                              (unsigned)(g_audio_lab_base + i),
                                              (unsigned)(g_audio_lab_value[i] < 0 ? 0 : g_audio_lab_value[i]),
                                              g_audio_lab_change_count[i]);
                        }
                    }
                }
                if (ImGui::Button(TR("Reset counts", "Zerar contagens"))) {
                    g_audio_lab_base_seen = 0; /* forca releitura sem destaque */
                }
                ImGui::TreePop();
            }
            bool tile_dump = ppu_native_get_tile_dump();
            if (ImGui::Checkbox(TR("Dump tiles (PNG)", "Despejar tiles (PNG)"), &tile_dump)) {
                ppu_native_set_tile_dump(tile_dump);
            }
            if (tile_dump || hd_pack_io_dumped_total() > 0) {
                ImGui::TextDisabled(TR("Seen: %u  Saved: %u", "Vistos: %u  Gravados: %u"),
                                    ppu_native_tile_dump_seen(),
                                    hd_pack_io_dumped_total());
                ImGui::TextWrapped(TR("Folder: %s", "Pasta: %s"), hd_pack_io_dump_dir());
                if (ImGui::Button(TR("Forget seen tiles", "Esquecer tiles vistos"))) {
                    ppu_native_reset_tile_dump();
                }
            }
            /* Passo 14b-1: centro pela GPU x CPU. */
            if (ImGui::TreeNode(TR("Compare GPU (14b)", "Comparar GPU (14b)"))) {
                if (!g_use_gl) {
                    ImGui::TextWrapped(TR("Needs the OpenGL video (Display > Video).", "Precisa do Video OpenGL (Video > Video)."));
                } else {
                    bool on = g_gpu_compare;
                    if (ImGui::Checkbox(TR("Also compose the center on the GPU and compare", "Montar o centro tambem na GPU e comparar"), &on)) {
                        char err[256] = "";
                        if (on && !gpu_ppu_init(err, sizeof(err))) {
                            g_gpu_error = err;
                            on = false;
                        }
                        g_gpu_compare = on;
                        ppu_native_gpu_capture(on);
                    }
                    if (!g_gpu_error.empty()) {
                        ImGui::TextWrapped(TR("Error: %s", "Erro: %s"), g_gpu_error.c_str());
                    }
                    const GpuCompareStats& st = g_gpu_stats;
                    ImGui::Text(TR("Checked: %llu  equal: %llu  different: %llu", "Conferidos: %llu  iguais: %llu  com diferenca: %llu"),
                                (unsigned long long)st.compared, (unsigned long long)st.equal,
                                (unsigned long long)(st.compared - st.equal));
                    ImGui::Text(TR("No mid-frame writes: checked above. With mid-frame writes: %llu (below)", "Sem escrita no meio: conferidos acima. Com escrita no meio: %llu (abaixo)"),
                                (unsigned long long)st.skipped_mid);
                    ImGui::Text(TR("Skipped: write log full %llu, not CGB %llu", "Pulados: registro de escritas cheio %llu, nao CGB %llu"),
                                (unsigned long long)st.skipped_overflow, (unsigned long long)st.skipped_mode);
                    if (st.skipped_mid > 0) {   /* passo 14b-2a: diagnóstico */
                        const double n = (double)st.skipped_mid;
                        ImGui::Text(TR("  Checked anyway: %llu, equal: %llu, different: %llu", "  Conferidos mesmo assim: %llu, iguais: %llu, diferentes: %llu"),
                                    (unsigned long long)st.mid_compared, (unsigned long long)st.mid_equal,
                                    (unsigned long long)(st.mid_compared - st.mid_equal));
                        ImGui::Text(TR("  Different with tile writes only: %llu, map only: %llu", "  Diferentes so com escrita em tiles: %llu, so no mapa: %llu"),
                                    (unsigned long long)st.mid_bad_tile_frames,
                                    (unsigned long long)st.mid_bad_map_frames);
                        ImGui::Text(TR("  Per frame: %.1f writes (tiles %.1f, map %.1f) on %.1f lines", "  Por quadro: %.1f escritas (tiles %.1f, mapa %.1f) em %.1f linhas"),
                                    (double)st.mid_writes / n, (double)st.mid_tile / n,
                                    (double)st.mid_map / n, (double)st.mid_lines / n);
                        ImGui::Text(TR("  Last: lines %d to %d", "  Ultimo: linhas %d a %d"), st.last_mid_first, st.last_mid_last);
                        if (st.last_mid_x >= 0) {
                            ImGui::Text(TR("  Last difference: %u pixels, first (%d,%d)", "  Ultima diferenca: %u pontos, primeiro (%d,%d)"),
                                        st.last_mid_diff, st.last_mid_x, st.last_mid_y);
                        }
                    }
                    if (st.compared > 0) {
                        ImGui::Text(TR("GPU (compose + read back): %.2f ms per frame", "GPU (montar + ler de volta): %.2f ms por quadro"),
                                    st.ms_total / (double)st.compared);
                    }
                    if (st.first_x >= 0) {
                        ImGui::TextWrapped(TR("Last difference: %u pixels, first (%d,%d) CPU %08X GPU %08X", "Ultima diferenca: %u pontos, primeiro (%d,%d) CPU %08X GPU %08X"),
                                           st.last_diff, st.first_x, st.first_y, st.first_cpu, st.first_gpu);
                    }
                    /* Passo 14b-3: HD */
                    ImGui::Separator();
                    if (st.hd_compared + st.hd_mid_compared == 0) {
                        ImGui::TextWrapped(TR("HD: only checked with internal resolution above 1x.", "HD: so confere com resolucao interna acima de 1x."));
                    } else {
                        ImGui::Text(TR("HD %dx: checked %llu  equal: %llu  different: %llu", "HD %dx: conferidos %llu  iguais: %llu  com diferenca: %llu"),
                                    st.hd_scale, (unsigned long long)st.hd_compared,
                                    (unsigned long long)st.hd_equal,
                                    (unsigned long long)(st.hd_compared - st.hd_equal));
                        ImGui::Text(TR("  With HD tiles on screen: %llu", "  Com tiles HD na tela: %llu"), (unsigned long long)st.hd_with_tiles);
                        ImGui::Text(TR("  Tile written mid-frame: %llu, equal: %llu, different: %llu", "  Tile escrito no meio: %llu, iguais: %llu, diferentes: %llu"),
                                    (unsigned long long)st.hd_mid_compared,
                                    (unsigned long long)st.hd_mid_equal,
                                    (unsigned long long)(st.hd_mid_compared - st.hd_mid_equal));
                        ImGui::Text(TR("  GPU HD (compose + read back): %.2f ms per frame", "  GPU HD (montar + ler de volta): %.2f ms por quadro"),
                                    st.hd_ms_total / (double)(st.hd_compared + st.hd_mid_compared));
                        if (st.hd_x >= 0) {
                            ImGui::TextWrapped(TR("  Last HD difference: %u pixels, first (%d,%d) CPU %08X GPU %08X", "  Ultima diferenca HD: %u pontos, primeiro (%d,%d) CPU %08X GPU %08X"),
                                               st.hd_last_diff, st.hd_x, st.hd_y, st.hd_cpu, st.hd_gpu);
                        }
                    }
                    /* Passo 14b-4: laterais */
                    if (st.m_compared == 0) {
                        ImGui::TextWrapped("%s", TR("HD sides: only checked with widescreen, internal resolution above 1x and HD tiles on.",
                                                      "Laterais HD: so conferem com widescreen, resolucao interna acima de 1x e tiles HD ligados."));
                    } else {
                        ImGui::Text(TR("HD sides: checked %llu  equal: %llu  different: %llu", "Laterais HD: conferidas %llu  iguais: %llu  com diferenca: %llu"),
                                    (unsigned long long)st.m_compared, (unsigned long long)st.m_equal,
                                    (unsigned long long)(st.m_compared - st.m_equal));
                        ImGui::Text(TR("  GPU sides (compose + read back): %.2f ms per frame", "  GPU laterais (montar + ler de volta): %.2f ms por quadro"),
                                    st.m_ms_total / (double)st.m_compared);
                        if (st.m_fade_conflict) {
                            ImGui::Text(TR("  Fade with two values in the column: %llu frames", "  Escurecimento com dois valores na coluna: %llu quadros"),
                                        (unsigned long long)st.m_fade_conflict);
                        }
                        if (st.m_x >= 0) {
                            ImGui::TextWrapped(TR("  Last difference: %u pixels, first (%d,%d) CPU %08X GPU %08X", "  Ultima diferenca: %u pontos, primeiro (%d,%d) CPU %08X GPU %08X"),
                                               st.m_last_diff, st.m_x, st.m_y, st.m_cpu, st.m_gpu);
                        }
                    }
                    /* Passo 14c-1: tela inteira */
                    if (st.s_compared + st.s_skipped == 0) {
                        ImGui::TextWrapped(TR("Whole screen: checked on a new game frame.", "Tela inteira: confere em quadro novo do jogo."));
                    } else {
                        ImGui::Text(TR("Whole screen %dx%d: checked %llu  equal: %llu  different: %llu", "Tela inteira %dx%d: conferidas %llu  iguais: %llu  com diferenca: %llu"),
                                    st.s_w, st.s_h, (unsigned long long)st.s_compared,
                                    (unsigned long long)st.s_equal,
                                    (unsigned long long)(st.s_compared - st.s_equal));
                        ImGui::Text(TR("  Skipped (GPU without the frame HD): %llu", "  Puladas (GPU sem o HD do quadro): %llu"), (unsigned long long)st.s_skipped);
                        if (st.s_compared > 0) {
                            ImGui::Text(TR("  GPU screen (compose + read back): %.2f ms per frame", "  GPU tela (montar + ler de volta): %.2f ms por quadro"),
                                        st.s_ms_total / (double)st.s_compared);
                        }
                        if (st.s_x >= 0) {
                            ImGui::TextWrapped(TR("  Last difference: %u pixels, first (%d,%d) CPU %08X GPU %08X", "  Ultima diferenca: %u pontos, primeiro (%d,%d) CPU %08X GPU %08X"),
                                               st.s_last_diff, st.s_x, st.s_y, st.s_cpu, st.s_gpu);
                        }
                    }
                    if (st.f_compared > 0) {   /* passo 14e-1 */
                        ImGui::Text(TR("Filter (%s): checked %llu  equal: %llu  different: %llu", "Filtro (%s): conferidas %llu  iguais: %llu  com diferenca: %llu"),
                                    post_processing_get_names()[st.f_mode], (unsigned long long)st.f_compared,
                                    (unsigned long long)st.f_equal,
                                    (unsigned long long)(st.f_compared - st.f_equal));
                        if (st.f_x >= 0) {
                            ImGui::TextWrapped(TR("  Last difference: %u pixels, first (%d,%d) CPU %08X GPU %08X", "  Ultima diferenca: %u pontos, primeiro (%d,%d) CPU %08X GPU %08X"),
                                               st.f_last_diff, st.f_x, st.f_y, st.f_cpu, st.f_gpu);
                        }
                    }
                    if (st.ip_compared > 0) {   /* passo 14d-1 */
                        ImGui::Text(TR("120 fps layers: checked %llu  equal: %llu  different: %llu", "Camadas 120 fps: conferidas %llu  iguais: %llu  com diferenca: %llu"),
                                    (unsigned long long)st.ip_compared, (unsigned long long)st.ip_equal,
                                    (unsigned long long)(st.ip_compared - st.ip_equal));
                        if (st.ip_x >= 0) {
                            ImGui::TextWrapped(TR("  Last difference: scenery %u, sprites %u pixels, first (%d,%d)", "  Ultima diferenca: cenario %u, sprites %u pontos, primeiro (%d,%d)"),
                                               st.ip_last_bg, st.ip_last_obj, st.ip_x, st.ip_y);
                        }
                    } else if (g_fps_mode == 1) {
                        ImGui::TextWrapped(TR("120 fps layers: show up when the in-between frame is on.", "Camadas 120 fps: aparecem quando o quadro do meio estiver ligado."));
                    }
                    ImGui::TextWrapped("Atlas: %s", gpu_ppu_atlas_info());
                    if (ImGui::Button(TR("Reset count", "Zerar contagem"))) {
                        g_gpu_stats = GpuCompareStats();
                        g_gpu_error.clear();
                    }
                }
                ImGui::TreePop();
            }
            if (ImGui::Button(TR("Capture scene (F9)", "Capturar cena (F9)")) && !hd_pack_io_sprite_rec_active()) {
                ppu_native_request_scene_capture();
            }
            /* Passo 12a: gabarito do renderizador. */
            if (ImGui::TreeNode(TR("Renderer reference frames", "Gabarito do renderizador"))) {
                ImGui::TextWrapped("%s", TR("Records the game state and the frame the renderer draws from it. After changing the "
                                              "renderer, Check replays each one and compares pixel by pixel.",
                                              "Grava o estado do jogo e o quadro que o renderizador desenha a partir dele. Depois de "
                                              "mudar o desenho, Conferir repete cada um e compara ponto a ponto."));
                if (ImGui::Button(TR("Record reference (F4)", "Gravar gabarito (F4)"))) {
                    gabarito_request_capture();
                }
                ImGui::SameLine();
                if (ImGui::Button(TR("Check references", "Conferir gabaritos"))) {
                    gabarito_request_check();
                }
                if (gabarito_busy()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(TR("running...", "rodando..."));
                }
                ImGui::TextWrapped("%s", gabarito_status());
                for (int i = 0; i < gabarito_result_count(); ++i) {
                    const ImVec4 color = gabarito_result_ok(i) ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                                               : ImVec4(1.0f, 0.45f, 0.40f, 1.0f);
                    ImGui::TextColored(color, "%s", gabarito_result_text(i));
                }
                ImGui::TextWrapped(TR("Folder: %s", "Pasta: %s"), gabarito_dir());
                static bool confirm_rewrite = false;
                ImGui::Checkbox(TR("Allow re-recording", "Liberar regravar"), &confirm_rewrite);
                if (confirm_rewrite) {
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Re-record all with the current result", "Regravar todos com o resultado atual"))) {
                        gabarito_request_rewrite();
                        confirm_rewrite = false;
                    }
                }
                ImGui::TreePop();
            }
            /* Passo 7a: gravador de poses de sprites. */
            if (ImGui::TreeNode(TR("Record sprite poses", "Gravar poses de sprites"))) {
                if (!hd_pack_io_sprite_rec_active()) {
                    if (ImGui::Button(TR("Start recording", "Comecar a gravar"))) {
                        hd_pack_io_sprite_rec_start();
                    }
                } else {
                    if (ImGui::Button(TR("Stop and save", "Parar e salvar"))) {
                        hd_pack_io_sprite_rec_stop();
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), TR("recording", "gravando"));
                }
                ImGui::Text(TR("Poses: %u   frames: %u", "Poses: %u   quadros: %u"), hd_pack_io_sprite_rec_pose_count(),
                            hd_pack_io_sprite_rec_frames());
                ImGui::Checkbox(TR("Only inside the rectangle", "So dentro do retangulo"), &g_rec_rect_enabled);
                if (g_rec_rect_enabled) {
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::SliderInt2("X / Y", g_rec_rect, 0, 159);
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::SliderInt2(TR("Width / Height", "Largura / Altura"), g_rec_rect + 2, 8, 160);
                    if (g_rec_rect[1] > 143) g_rec_rect[1] = 143;
                    if (g_rec_rect[3] > 144) g_rec_rect[3] = 144;
                    ImGui::TextDisabled(TR("Poses with their center inside the red rectangle are recorded.", "Entram as poses com o centro dentro do retangulo vermelho."));
                } else {
                    ImGui::TextDisabled(TR("All sprites on screen are recorded (sides included).", "Entram todos os sprites da tela (inclusive laterais)."));
                }
                if (hd_pack_io_sprite_rec_last()[0]) {
                    ImGui::TextWrapped(TR("Last sheet: %s_obj_4x.png", "Ultima folha: %s_obj_4x.png"), hd_pack_io_sprite_rec_last());
                }
                ImGui::TreePop();
            }
            if (ppu_native_scene_capture_pending()) {
                ImGui::SameLine();
                ImGui::TextDisabled(TR("capturing...", "capturando..."));
            }
            if (hd_pack_io_last_scene()[0]) {
                ImGui::TextWrapped(TR("Last scene: %s", "Ultima cena: %s"), hd_pack_io_last_scene());
            } else {
                ImGui::TextWrapped(TR("Scenes: %s", "Cenas: %s"), hd_pack_io_scene_dir());
            }
            if (has_interpreter_activity(g_registered_ctx)) {
                const GBInterpreterHotspot* hotspot = &g_registered_ctx->interpreter_hotspots[0];
                ImGui::Separator();
                ImGui::TextDisabled(TR("Interpreter fallback", "Interpretador (fallback)"));
                ImGui::Text("Frame Fallbacks: %u", g_registered_ctx->frame_dispatch_fallbacks);
                ImGui::Text("Interp Entries: %llu",
                            (unsigned long long)g_registered_ctx->total_interpreter_entries);
                ImGui::Text("Total Fallbacks: %llu",
                            (unsigned long long)g_registered_ctx->total_dispatch_fallbacks);
                ImGui::Text("Interp Instr: frame %llu total %llu",
                            (unsigned long long)g_registered_ctx->frame_interpreter_instructions,
                            (unsigned long long)g_registered_ctx->total_interpreter_instructions);
                ImGui::Text("Interp Cycles: frame %llu total %llu",
                            (unsigned long long)g_registered_ctx->frame_interpreter_cycles,
                            (unsigned long long)g_registered_ctx->total_interpreter_cycles);
                if (hotspot->valid && hotspot->entries > 0) {
                    ImGui::Text("Top Hotspot: %03X:%04X", hotspot->bank, hotspot->addr);
                    ImGui::Text("Hits %llu Instr %llu Cycles %llu",
                                (unsigned long long)hotspot->entries,
                                (unsigned long long)hotspot->instructions,
                                (unsigned long long)hotspot->cycles);
                }
                if (g_registered_ctx->has_unimplemented_interpreter_opcode) {
                    ImGui::Text("Coverage Gap: %02X at %03X:%04X",
                                g_registered_ctx->last_unimplemented_opcode,
                                g_registered_ctx->last_unimplemented_bank,
                                g_registered_ctx->last_unimplemented_addr);
                }
            }

        }
        ImGui::PopItemWidth();
        imgui_drag_to_scroll();   /* passo 14p */
        ImGui::EndChild();
        ImGui::Separator();

        const int footer_button_count = g_launcher_return_enabled ? 3 : 2;
        const float footer_button_width =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (footer_button_count - 1)) /
            (float)footer_button_count;
        if (ImGui::Button(TR("Reset display", "Restaurar video"), ImVec2(footer_button_width, 0.0f))) {
            reset_runtime_display_defaults();
            save_runtime_preferences();   /* passo 14p: os padrões ficam gravados */
        }

        if (g_launcher_return_enabled) {
            ImGui::SameLine();
            if (ImGui::Button(TR("Return to launcher", "Voltar ao launcher"), ImVec2(footer_button_width, 0.0f))) {
                g_exit_action = GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER;
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(TR("Quit", "Sair"), ImVec2(footer_button_width, 0.0f))) {
            g_exit_action = GB_PLATFORM_EXIT_QUIT;
            SDL_Event quit_event;
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
        }
        ImGui::End();
        ImGui::PopStyleVar(6);
    } else if (g_show_overlay) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("Overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            update_audio_stats_from_ring();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            if (g_fps_mode == 1) {
                ImGui::Text(TR("In-between frames: %u/s", "Quadros do meio: %u/s"), ip_mid_per_second());   /* passo 10a */
                char ip_why[256];
                ip_skip_text(ip_why, sizeof(ip_why));
                ImGui::TextUnformatted(ip_why);   /* passo 10b */
            }
            {
                const double budget_ms = 1000.0 * 70224.0 / 4194304.0; /* 16,74 ms */
                ImGui::Separator();
                ImGui::Text(TR("Budget: %.2f ms/frame", "Orcamento: %.2f ms/frame"), budget_ms);
                ImGui::Text(TR("Emu:    avg %.2f  peak %.2f ms", "Emu:    med %.2f  pico %.2f ms"),
                            g_emu_window.shown_avg, g_emu_window.shown_max);
                ImGui::Text(TR("Render: avg %.2f  peak %.2f ms", "Render: med %.2f  pico %.2f ms"),
                            g_render_window.shown_avg, g_render_window.shown_max);
                ImGui::Text(TR("Texture: scale %dx (requested %dx)", "Textura: escala %dx (pedida %dx)"),
                            g_texture_scale, g_internal_res_scale);
                ImGui::Text("Underruns: %u", current_audio_underruns());
                if (g_audio_rate_control.load(std::memory_order_relaxed)) {
                    ImGui::Text(TR("Mixer: read %+.3f%%  pace %+.3f%%", "Mixer: leitura %+.3f%%  ritmo %+.3f%%"),
                                (double)g_audio_rate_ppm.load(std::memory_order_relaxed) / 10000.0,
                                (double)g_audio_pace_ppm.load(std::memory_order_relaxed) / 10000.0);
                    ImGui::Text(TR("       level %u (target %u)", "       nivel %u (alvo %u)"),
                                current_audio_ring_fill_samples(),
                                g_audio_target_fill.load(std::memory_order_relaxed));
                } else {
                    ImGui::Text("%s", TR("Mixer: direct copy", "Mixer: copia direta"));
                }
                if (g_music_current >= 0) {
                    ImGui::Text(TR("Music: %02X  (last request %02X)%s", "Musica: %02X  (ultimo pedido %02X)%s"),
                                (unsigned)g_music_current,
                                (unsigned)(g_music_request_seen < 0 ? 0 : g_music_request_seen),
                                music_replace_active() ? "  [ogg]" : "");
                } else {
                    ImGui::Text("%s", TR("Music: --", "Musica: --"));
                }
                ImGui::Text("Renderer: %s", ppu_native_mode_name(ppu_native_get_mode()));
                {
                    int hd_scale = 0;
                    ppu_native_hd_frame(&hd_scale);
                    int hd_m_scale = 0;
                    ppu_native_hd_margin_frame(&hd_m_scale);
                    ImGui::Text("HD: %dx%s%s", hd_scale,
                                hd_m_scale > 1 ? TR(" + sides", " + laterais") : "",
                                ppu_native_get_hd_check() ? TR(" (checking)", " (conferindo)") : "");
                    if (ppu_native_get_hd_check()) {
                        const PPUNativeHdStats* hs = ppu_native_hd_stats();
                        ImGui::Text(TR("HD cmp: %llu frames, %llu different", "HD cmp: %llu frames, %llu com diferenca"),
                                    (unsigned long long)hs->frames_checked,
                                    (unsigned long long)hs->frames_with_mismatch);
                    }
                }
                if (ppu_native_get_mode() == PPU_RENDERER_COMPARE) {
                    const PPUNativeCompareStats* cmp = ppu_native_compare_stats();
                    ImGui::Text(TR("Compare: %llu frames, %llu different", "Comparar: %llu frames, %llu com diferenca"),
                                (unsigned long long)cmp->frames_compared,
                                (unsigned long long)cmp->frames_with_mismatch);
                    ImGui::Text(TR("Last frame: %u px different", "Ultimo frame: %u px diferentes"),
                                cmp->last_frame_mismatched_pixels);
                    if (cmp->frames_with_mismatch > 0) {
                        ImGui::Text(TR("Last diff.: frame %llu, x=%d y=%d", "Ultima dif.: frame %llu, x=%d y=%d"),
                                    (unsigned long long)cmp->first_mismatch_frame,
                                    cmp->first_mismatch_x, cmp->first_mismatch_y);
                    }
                    ImGui::Text(TR("VRAM without hook: %llu tiles", "VRAM sem hook: %llu tiles"),
                                (unsigned long long)cmp->unhooked_tile_writes);
                }
                ImGui::Separator();
            }
            ImGui::Text("%s", overlay_menu_hint_text());
            ImGui::Text("%s", overlay_visibility_hint_text());
            ImGui::Text("Viewport: %d x %d",
                        g_game_viewport.w,
                        g_game_viewport.h);
            if (g_timing_frame_count > 0) {
                float avg_render = (float)(g_timing_render_total / g_timing_frame_count);
                float avg_vsync = (float)(g_timing_vsync_total / g_timing_frame_count);
                ImGui::Text("Render: %.1fms, VSync: %.1fms", avg_render, avg_vsync);
                ImGui::Text("Upload: %.2f Compose: %.2f Present: %.2f",
                            (float)g_last_timing.upload_ms,
                            (float)g_last_timing.compose_ms,
                            (float)g_last_timing.present_ms);
                ImGui::Text("AudioBuf: %u/%u, Underruns:%u",
                            current_audio_ring_fill_samples(),
                            current_audio_ring_capacity(),
                            current_audio_underruns());
                ImGui::Text("Smooth Slow Frames: %s", g_smooth_lcd_transitions ? "On" : "Off");
                ImGui::TextUnformatted(audio_stats_get_summary());
                if (has_interpreter_activity(g_registered_ctx)) {
                    const GBInterpreterHotspot* hotspot = &g_registered_ctx->interpreter_hotspots[0];
                    ImGui::Separator();
                    ImGui::Text("Interp: frame %u total %llu entries %llu",
                                g_registered_ctx->frame_dispatch_fallbacks,
                                (unsigned long long)g_registered_ctx->total_dispatch_fallbacks,
                                (unsigned long long)g_registered_ctx->total_interpreter_entries);
                    if (hotspot->valid && hotspot->entries > 0) {
                        ImGui::Text("Hotspot: %03X:%04X (%llu hits)",
                                    hotspot->bank,
                                    hotspot->addr,
                                    (unsigned long long)hotspot->entries);
                    }
                }
            }
            ImGui::End();
        }
    }

    if (!g_show_menu) {   /* [android-port] gamepad na tela */
        touch_controls::draw(ImGui::GetForegroundDrawList(),
                             imgui_io.DisplaySize.x,
                             imgui_io.DisplaySize.y,
                             g_controller != NULL);
    }

    if (g_port_frame_valid &&
        g_port_frame.abi_version == GB_PORT_ABI_VERSION &&
        g_port_frame.canvas_width > 0 &&
        g_port_frame.canvas_height > 0) {
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        const float scale_x =
            imgui_io.DisplaySize.x / (float)g_port_frame.canvas_width;
        const float scale_y =
            imgui_io.DisplaySize.y / (float)g_port_frame.canvas_height;
        for (size_t index = 0;
             index < g_port_frame.command_count &&
             index < GB_PORT_MAX_DRAW_COMMANDS;
             ++index) {
            const GBPortDrawCommand& command = g_port_frame.commands[index];
            const ImU32 color = IM_COL32(
                (command.color_rgba >> 24u) & 0xffu,
                (command.color_rgba >> 16u) & 0xffu,
                (command.color_rgba >> 8u) & 0xffu,
                command.color_rgba & 0xffu);
            const ImVec2 start(
                command.x * scale_x,
                command.y * scale_y);
            if (command.type == GB_PORT_DRAW_PANEL) {
                const ImVec2 end(
                    (command.x + command.width) * scale_x,
                    (command.y + command.height) * scale_y);
                draw_list->AddRectFilled(start, end, color, 10.0f);
            } else if (command.type == GB_PORT_DRAW_TEXT) {
                draw_list->AddText(start, color, command.text);
            }
        }
    }
    if (!g_use_gl) SDL_RenderSetViewport(g_renderer, NULL);
    ImGui::Render();
    vid_imgui_render();   /* passo 14a */
    g_last_timing.compose_ms = sdl_now_ms() - compose_start_ms;

    double present_start_ms = sdl_now_ms();
    vid_present();   /* passo 14a */
    if (count_guest_frame) {
        g_ip_last_present = SDL_GetPerformanceCounter();   /* passo 10a */
    }
    if (g_fps_mode == 1) {
        ip_note_present_interval(g_ip_refresh_hz);   /* passo 10f */
    }

    g_last_timing.present_ms = sdl_now_ms() - present_start_ms;
    g_last_timing.total_render_ms = sdl_now_ms() - total_render_start_ms;
    if (!g_ip_mid_draw) {   /* passo 10a: a medicao e so do quadro do jogo */
        g_timing_render_total += g_last_timing.total_render_ms;
        timing_window_add(g_render_window, g_last_timing.total_render_ms);
    }
}
////////////////

/* Passo 10a: espera ate o instante pedido (mesmo jeito do ritmo dos quadros). */
static void ip_wait_until(uint64_t target) {
    const uint64_t freq = SDL_GetPerformanceFrequency();
    for (;;) {
        const uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target) return;
        const uint32_t wait_us = (uint32_t)(((target - now) * 1000000ull) / freq);
        if (wait_us > 4000) {
            SDL_Delay((wait_us / 1000) - 2);
        } else if (wait_us > 1500) {
            SDL_Delay(1);
        } else {
            break;
        }
    }
    while (SDL_GetPerformanceCounter() < target) {
        /* spin */
    }
}

/* Passo 10a: mostra o quadro do meio (mesmo menu/overlay, sem mexer no jogo). */
static void ip_present_mid(void) {
    if (!g_texture || g_renderer_reset_pending || effective_internal_scale() != g_texture_scale) {
        return;
    }
    const bool wide = g_widescreen_mode && g_last_guest_widescreen_framebuffer_valid;
    const uint32_t* fb = wide ? g_last_guest_widescreen_framebuffer : g_last_guest_framebuffer;
    g_ip_mid_draw = true;
    render_frame_internal(fb, false, wide);
    g_ip_mid_draw = false;

    const uint32_t now = SDL_GetTicks();
    if (now - g_ip_mid_window_start >= 1000) {
        g_ip_mid_per_s = g_ip_mid_count;
        g_ip_mid_count = 0;
        g_ip_mid_window_start = now;
    }
    g_ip_mid_count++;
}

/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
    close_input_record_file();
    close_audio_output_device();
    g_audio_output_devices.clear();
    g_port_frame = {};
    g_port_frame_valid = false;

    clear_controller_state();
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    
    if (ImGui::GetCurrentContext() != NULL) {
        if (g_use_gl) ImGui_ImplOpenGL3_Shutdown();   /* passo 14a */
        else ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    ip_release_textures();   /* passo 10a */
    if (g_texture) {
        vid_tex_destroy(g_texture);
        g_texture = NULL;
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_use_gl) {   /* passo 14a */
        if (g_gpu_compare) {   /* passo 14c-1: resumo no terminal */
            const GpuCompareStats& st = g_gpu_stats;
            fprintf(stderr, "[GPU] resumo: 1x %llu/%llu (meio %llu/%llu) | HD %llu/%llu (meio %llu/%llu) | "
                    "laterais %llu/%llu | tela %llu/%llu (puladas %llu) | 120fps %llu/%llu | filtro %llu/%llu\n",
                    (unsigned long long)st.equal, (unsigned long long)st.compared,
                    (unsigned long long)st.mid_equal, (unsigned long long)st.mid_compared,
                    (unsigned long long)st.hd_equal, (unsigned long long)st.hd_compared,
                    (unsigned long long)st.hd_mid_equal, (unsigned long long)st.hd_mid_compared,
                    (unsigned long long)st.m_equal, (unsigned long long)st.m_compared,
                    (unsigned long long)st.s_equal, (unsigned long long)st.s_compared,
                    (unsigned long long)st.s_skipped,
                    (unsigned long long)st.ip_equal, (unsigned long long)st.ip_compared,
                    (unsigned long long)st.f_equal, (unsigned long long)st.f_compared);
        }
        if (g_3d_vtex) {   /* passo 3D-3 */
            vid_tex_destroy(g_3d_vtex);
            g_3d_vtex = NULL;
            g_3d_vtex_id = 0;
        }
        if (g_gpu_vtex) {   /* passo 14c-2 */
            vid_tex_destroy(g_gpu_vtex);
            g_gpu_vtex = NULL;
            g_gpu_vtex_id = 0;
        }
        gpu_ppu_shutdown();   /* passo 14b */
        vgl_shutdown();
        g_use_gl = false;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    g_registered_ctx = NULL;
    g_app_suspended = false;
    g_renderer_reset_pending = false;
    SDL_Quit();
}

void gb_platform_set_benchmark_mode(bool enabled) {
    g_benchmark_mode = enabled;
}

void gb_platform_set_launcher_return_enabled(bool enabled) {
    g_launcher_return_enabled = enabled;
}

GBPlatformExitAction gb_platform_get_exit_action(void) {
    return g_exit_action;
}

/* ============================================================================
 * Audio - Simple Push Mode with SDL_QueueAudio
 * ========================================================================== */

#define AUDIO_SAMPLE_RATE 44100

/* 
 * Audio: Simple circular buffer with SDL callback
 * The callback pulls samples at exactly 44100 Hz.
 * The emulator pushes samples as they're generated.
 * A large buffer provides tolerance for timing variations.
 */
#define AUDIO_RING_SIZE 16384  /* ~370ms buffer - plenty of headroom */
#ifndef AUDIO_WRITE_BATCH_FRAMES
#define AUDIO_WRITE_BATCH_FRAMES 32
#endif
static int16_t g_audio_ring[AUDIO_RING_SIZE * 2];  /* Stereo */
static std::atomic<uint32_t> g_audio_write_pos{0};
static std::atomic<uint32_t> g_audio_read_pos{0};
static uint32_t g_audio_producer_write_pos = 0;
static uint32_t g_audio_pending_write_frames = 0;

/* Debug counters */
static uint64_t g_audio_samples_written = 0;
static uint64_t g_audio_write_publications = 0;
static std::atomic<uint64_t> g_audio_underruns{0};
static uint64_t g_audio_reported_underruns = 0;

static bool audio_subsystem_available(void) {
    return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
}

static void refresh_audio_output_devices(void) {
    g_audio_output_devices.clear();
    if (!audio_subsystem_available()) {
        return;
    }

    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(i, 0);
        if (name && name[0]) {
            g_audio_output_devices.emplace_back(name);
        }
    }
}

static int current_audio_output_device_index(void) {
    if (g_audio_target_device_name.empty()) {
        return 0;
    }

    for (size_t i = 0; i < g_audio_output_devices.size(); i++) {
        if (g_audio_output_devices[i] == g_audio_target_device_name) {
            return (int)i + 1;
        }
    }

    return 0;
}

static const char* current_audio_output_device_label(void) {
    if (!g_audio_target_device_name.empty()) {
        return g_audio_target_device_name.c_str();
    }
    return TR("System default", "Padrao do sistema");   /* 15a */
}

static bool current_audio_output_device_available(void) {
    return g_audio_target_device_name.empty() || current_audio_output_device_index() != 0;
}

static void close_audio_output_device(void) {
    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    g_audio_started = false;
    g_audio_start_threshold = 0;
    g_audio_low_watermark = 0;
    g_audio_device_sample_rate = AUDIO_SAMPLE_RATE;
    g_audio_device_buffer_samples = 0;
    g_audio_active_device_name.clear();
}

static bool open_audio_output_device(const char* device_name, bool preserve_stats) {
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 2048;
    want.callback = sdl_audio_callback;
    want.userdata = NULL;

    g_audio_device = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        return false;
    }

    g_audio_device_sample_rate = (uint32_t)have.freq;
    g_audio_device_buffer_samples = (uint32_t)have.samples;
    g_audio_active_device_name = (device_name && device_name[0]) ? device_name : "System Default";
    recompute_audio_targets();
    reset_audio_output_buffer(preserve_stats);
    fprintf(stderr,
            "[SDL] Audio initialized: %d Hz, %d channels, device '%s', buffer %d samples, target latency %u ms\n",
            have.freq,
            have.channels,
            g_audio_active_device_name.c_str(),
            have.samples,
            g_audio_latency_ms);
    return true;
}

static bool reopen_audio_output_device(bool preserve_stats) {
    if (!audio_subsystem_available()) {
        return false;
    }

    const std::string requested_device = g_audio_target_device_name;
    close_audio_output_device();

    if (!requested_device.empty() && open_audio_output_device(requested_device.c_str(), preserve_stats)) {
        return true;
    }

    if (!requested_device.empty()) {
        fprintf(stderr,
                "[SDL] Failed to open requested audio device '%s': %s. Falling back to default output.\n",
                requested_device.c_str(),
                SDL_GetError());
    }

    if (open_audio_output_device(NULL, preserve_stats)) {
        return true;
    }

    fprintf(stderr, "[SDL] Failed to open audio: %s\n", SDL_GetError());
    return false;
}

static uint32_t current_audio_underruns(void) {
    const uint64_t underruns = g_audio_underruns.load(std::memory_order_relaxed);
    return underruns > UINT32_MAX ? UINT32_MAX : (uint32_t)underruns;
}

static uint32_t current_audio_ring_fill_samples(void) {
    return audio_ring_fill_samples();
}

static uint32_t current_audio_ring_capacity(void) {
    return AUDIO_RING_SIZE;
}

static bool audio_output_should_run(void) {
    return g_audio_device != 0 &&
           g_audio_output_enabled &&
           !g_benchmark_mode &&
           !g_app_suspended &&
           effective_speed_percent() == 100;
}

static uint32_t audio_ring_fill_samples(void) {
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_acquire);
    return (write_pos >= read_pos) ? (write_pos - read_pos) : (AUDIO_RING_SIZE - read_pos + write_pos);
}

static void update_audio_stats_from_ring(void) {
    const uint64_t total_underruns = g_audio_underruns.load(std::memory_order_relaxed);
    const uint64_t pending_underruns = total_underruns - g_audio_reported_underruns;
    if (pending_underruns > 0) {
        audio_stats_underruns(pending_underruns > UINT32_MAX
                                  ? UINT32_MAX
                                  : (uint32_t)pending_underruns);
        g_audio_reported_underruns = total_underruns;
    }
    audio_stats_update_buffer(audio_ring_fill_samples(), AUDIO_RING_SIZE, g_audio_device_sample_rate);
}

static void recompute_audio_targets(void) {
    const uint32_t sample_rate = g_audio_device_sample_rate ? g_audio_device_sample_rate : AUDIO_SAMPLE_RATE;
    const uint32_t device_buffer = g_audio_device_buffer_samples ? g_audio_device_buffer_samples : 512u;
    uint32_t target = (uint32_t)(((uint64_t)sample_rate * (uint64_t)g_audio_latency_ms + 999ull) / 1000ull);

    if (target < device_buffer) {
        target = device_buffer;
    }
    if (target == 0) {
        target = 1;
    }
    if (target > (AUDIO_RING_SIZE / 2)) {
        target = AUDIO_RING_SIZE / 2;
    }

    /* O piso precisa ficar acima de uma puxada inteira do dispositivo,
     * senão o buffer pode estacionar abaixo dela e dar underrun em toda puxada. */
    const uint32_t min_low = device_buffer + device_buffer / 2;  /* 1,5 puxada */
    if (target < min_low * 2) {
        target = min_low * 2;
    }
    if (target > (AUDIO_RING_SIZE / 2)) {
        target = AUDIO_RING_SIZE / 2;
    }
    g_audio_start_threshold = target;
    g_audio_target_fill.store(target, std::memory_order_relaxed);
    g_audio_low_watermark = target / 2;
    if (g_audio_low_watermark < min_low) {
        g_audio_low_watermark = min_low;
    }
}

static void clear_audio_ring_buffer_locked(void) {
    g_audio_write_pos.store(0, std::memory_order_relaxed);
    g_audio_read_pos.store(0, std::memory_order_relaxed);
    g_audio_producer_write_pos = 0;
    g_audio_pending_write_frames = 0;
    memset(g_audio_ring, 0, sizeof(g_audio_ring));
    g_audio_started = false;
    update_audio_stats_from_ring();
    g_resample_frac = 0;
    g_rate_adjust = 0.0;
}

static void refresh_audio_device_pause_state(void) {
    if (!g_audio_device) {
        return;
    }

    const bool paused = !audio_output_should_run() || !g_audio_started;
    SDL_PauseAudioDevice(g_audio_device, paused ? 1 : 0);
}

static void reset_audio_output_buffer(bool preserve_stats) {
    if (g_audio_device) {
        SDL_LockAudioDevice(g_audio_device);
        clear_audio_ring_buffer_locked();
        refresh_audio_device_pause_state();
        SDL_UnlockAudioDevice(g_audio_device);
    } else {
        clear_audio_ring_buffer_locked();
    }

    if (!preserve_stats) {
        g_audio_underruns.store(0, std::memory_order_relaxed);
        g_audio_reported_underruns = 0;
        g_audio_samples_written = 0;
        g_audio_write_publications = 0;
    }
}

/* Copia direta (antes do passo 6a); usada com o controle de taxa desligado. */
static void sdl_audio_callback_direct(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    const uint32_t samples_needed = (uint32_t)(len / 4);
    const bool muted = g_audio_muted.load(std::memory_order_relaxed);
    const uint32_t volume_percent = gb_effective_volume_percent(); /* passo 6e */
    const uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_relaxed);
    const uint32_t available = (write_pos >= read_pos)
        ? (write_pos - read_pos)
        : (AUDIO_RING_SIZE - read_pos + write_pos);
    const uint32_t samples_to_copy = available < samples_needed ? available : samples_needed;
    uint32_t copied = 0;

    while (copied < samples_to_copy) {
        uint32_t contiguous = AUDIO_RING_SIZE - read_pos;
        const uint32_t remaining = samples_to_copy - copied;
        if (contiguous > remaining) contiguous = remaining;

        if (muted || volume_percent == 0) {
            memset(out + copied * 2, 0, (size_t)contiguous * 2u * sizeof(int16_t));
        } else if (volume_percent == 100) {
            memcpy(out + copied * 2,
                   g_audio_ring + read_pos * 2,
                   (size_t)contiguous * 2u * sizeof(int16_t));
        } else {
            for (uint32_t i = 0; i < contiguous; ++i) {
                int32_t left = g_audio_ring[(read_pos + i) * 2];
                int32_t right = g_audio_ring[(read_pos + i) * 2 + 1];
                left = (left * (int32_t)volume_percent) / 100;
                right = (right * (int32_t)volume_percent) / 100;
                if (left < -32768) left = -32768;
                if (left > 32767) left = 32767;
                if (right < -32768) right = -32768;
                if (right > 32767) right = 32767;
                out[(copied + i) * 2] = (int16_t)left;
                out[(copied + i) * 2 + 1] = (int16_t)right;
            }
        }

        copied += contiguous;
        read_pos = (read_pos + contiguous) % AUDIO_RING_SIZE;
    }

    const uint32_t underruns = samples_needed - copied;
    if (underruns > 0) {
        memset(out + copied * 2, 0, (size_t)underruns * 2u * sizeof(int16_t));
        g_audio_underruns.fetch_add(underruns, std::memory_order_relaxed);
    }

    g_audio_read_pos.store(read_pos, std::memory_order_release);
}

/* ============================================================================
 * Passo 6a: mixer com controle de taxa
 *
 * O jogo gera exatamente 44100 amostras por segundo de tempo emulado, e o
 * ritmo dos frames segue o relógio do PC. O relógio da placa de som nunca bate
 * exatamente com esse, então o buffer tende a encher ou esvaziar devagar.
 * Antes isso era corrigido deixando o frame ~2% mais lento quando o buffer
 * passava do alvo (aos trancos). Agora:
 *  - a diferença constante entre os relógios (que numa VM pode passar de 1%)
 *    é corrigida aos poucos no ritmo dos frames (parte integral, até ±3%);
 *  - as variações rápidas são absorvidas lendo o buffer do jogo um pouco
 *    mais rápido ou mais devagar (parte proporcional, até ±0,5%, inaudível),
 *    com interpolação linear.
 * As fontes são somadas aqui: por enquanto só existe a do Game Boy (a trilha
 * externa entra no passo 6c). Com o controle desligado (menu Desenvolvedor)
 * volta a cópia direta de antes.
 * ========================================================================== */
#define AUDIO_RATE_MAX_ADJUST 0.005   /* ±0,5% */
#define AUDIO_RATE_SMOOTHING 0.1      /* fração do erro corrigida por puxada */
#define AUDIO_RATE_INTEGRAL_GAIN 0.0003
#define AUDIO_RATE_INTEGRAL_ERR_CAP 0.25
#define AUDIO_PACE_PROPORTIONAL 0.01   /* ritmo: 1% com o erro máximo */
#define AUDIO_PACE_MAX_ADJUST 0.03     /* ritmo dos frames: até ±3% */

static inline int16_t audio_apply_volume(int32_t sample, bool muted, uint32_t volume_percent) {
    if (muted || volume_percent == 0) {
        return 0;
    }
    if (volume_percent != 100) {
        sample = (sample * (int32_t)volume_percent) / 100;
    }
    if (sample < -32768) sample = -32768;
    if (sample > 32767) sample = 32767;
    return (int16_t)sample;
}

static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    if (!g_audio_rate_control.load(std::memory_order_relaxed)) {
        g_audio_rate_ppm.store(0, std::memory_order_relaxed);
        const bool gb_running = audio_ring_fill_samples() > 0; /* passo 6c */
        sdl_audio_callback_direct(userdata, stream, len);
        if (gb_running) {
            const float master = g_audio_muted.load(std::memory_order_relaxed)
                ? 0.0f : (float)g_audio_volume_percent.load(std::memory_order_relaxed) / 100.0f;
            music_replace_mix((int16_t*)stream, (uint32_t)(len / 4),
                              g_audio_device_sample_rate ? g_audio_device_sample_rate : AUDIO_SAMPLE_RATE,
                              master);
        }
        return;
    }
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    const uint32_t frames = (uint32_t)(len / 4);
    const bool muted = g_audio_muted.load(std::memory_order_relaxed);
    const uint32_t volume_percent = gb_effective_volume_percent(); /* passo 6e: so o GB */
    const uint32_t master_percent = g_audio_volume_percent.load(std::memory_order_relaxed);
    const uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_relaxed);
    uint32_t available = (write_pos >= read_pos)
        ? (write_pos - read_pos)
        : (AUDIO_RING_SIZE - read_pos + write_pos);

    /* Controle de taxa: erro relativo do nível em relação ao alvo. */
    const uint32_t target = g_audio_target_fill.load(std::memory_order_relaxed);
    if (target > 0) {
        double err = ((double)available - (double)target) / (double)target;
        if (err > 1.0) err = 1.0;
        if (err < -1.0) err = -1.0;
        /* Parte integral: aprende a diferença constante entre os relógios e
         * corrige no ritmo dos frames (gb_platform_vsync), não no tom. O erro
         * entra limitado, para um evento isolado (porta, LCD desligado, pico de
         * CPU) não "ensinar" um desvio falso que depois vira underrun. */
        double err_i = err;
        if (err_i > AUDIO_RATE_INTEGRAL_ERR_CAP) err_i = AUDIO_RATE_INTEGRAL_ERR_CAP;
        if (err_i < -AUDIO_RATE_INTEGRAL_ERR_CAP) err_i = -AUDIO_RATE_INTEGRAL_ERR_CAP;
        g_rate_integral += err_i * AUDIO_RATE_INTEGRAL_GAIN;
        if (g_rate_integral > AUDIO_PACE_MAX_ADJUST) g_rate_integral = AUDIO_PACE_MAX_ADJUST;
        if (g_rate_integral < -AUDIO_PACE_MAX_ADJUST) g_rate_integral = -AUDIO_PACE_MAX_ADJUST;
        /* Ritmo = aprendido + proporcional ao erro atual (drena ou enche o
         * buffer depois de um evento e volta sozinho a zero). */
        double pace = g_rate_integral + err * AUDIO_PACE_PROPORTIONAL;
        if (pace > AUDIO_PACE_MAX_ADJUST) pace = AUDIO_PACE_MAX_ADJUST;
        if (pace < -AUDIO_PACE_MAX_ADJUST) pace = -AUDIO_PACE_MAX_ADJUST;
        g_audio_pace_ppm.store((int32_t)(pace * 1000000.0), std::memory_order_relaxed);
        /* Parte proporcional: correções rápidas, na leitura do buffer. */
        double desired = err * AUDIO_RATE_MAX_ADJUST;
        g_rate_adjust += (desired - g_rate_adjust) * AUDIO_RATE_SMOOTHING;
    }
    g_audio_rate_ppm.store((int32_t)(g_rate_adjust * 1000000.0), std::memory_order_relaxed);
    const uint32_t device_rate = g_audio_device_sample_rate ? g_audio_device_sample_rate : AUDIO_SAMPLE_RATE;
    const double step = ((double)AUDIO_SAMPLE_RATE / (double)device_rate) * (1.0 + g_rate_adjust);
    const uint64_t step_fx = (uint64_t)(step * 4294967296.0);

    uint64_t frac = g_resample_frac;
    uint32_t produced = 0;
    for (; produced < frames; ++produced) {
        if (available < 2u) {
            break;
        }
        const uint32_t i0 = read_pos;
        const uint32_t i1 = (read_pos + 1u) % AUDIO_RING_SIZE;
        const int32_t f = (int32_t)(frac >> 16);   /* 0..65535 */
        const int32_t l0 = g_audio_ring[i0 * 2], l1 = g_audio_ring[i1 * 2];
        const int32_t r0 = g_audio_ring[i0 * 2 + 1], r1 = g_audio_ring[i1 * 2 + 1];
        const int32_t left = l0 + (int32_t)(((int64_t)(l1 - l0) * f) >> 16);
        const int32_t right = r0 + (int32_t)(((int64_t)(r1 - r0) * f) >> 16);
        /* Aqui entram as outras fontes do mixer (passo 6c). */
        out[produced * 2] = audio_apply_volume(left, muted, volume_percent);
        out[produced * 2 + 1] = audio_apply_volume(right, muted, volume_percent);

        frac += step_fx;
        uint32_t advance = (uint32_t)(frac >> 32);
        frac &= 0xFFFFFFFFull;
        if (advance > available) advance = available;
        read_pos = (read_pos + advance) % AUDIO_RING_SIZE;
        available -= advance;
    }
    g_resample_frac = frac;

    const uint32_t underruns = frames - produced;
    if (underruns > 0) {
        memset(out + produced * 2, 0, (size_t)underruns * 2u * sizeof(int16_t));
        g_audio_underruns.fetch_add(underruns, std::memory_order_relaxed);
    }

    /* Passo 6c: trilhas .ogg. So andam enquanto o jogo gera audio (pausado,
     * a trilha pausa junto). */
    if (produced > 0) {
        music_replace_mix(out, frames, device_rate, muted ? 0.0f : (float)master_percent / 100.0f);
    }

    g_audio_read_pos.store(read_pos, std::memory_order_release);
}





static void publish_audio_write_batch(void) {
    if (g_audio_pending_write_frames == 0) return;

    g_audio_write_pos.store(g_audio_producer_write_pos, std::memory_order_release);
    g_audio_write_publications++;
    g_audio_samples_written += g_audio_pending_write_frames;
    audio_stats_samples_queued(g_audio_pending_write_frames);
    g_audio_pending_write_frames = 0;

    if (!g_audio_started && audio_ring_fill_samples() >= g_audio_start_threshold) {
        g_audio_started = true;
        refresh_audio_device_pause_state();
    }
}

static bool enqueue_audio_sample(int16_t left, int16_t right) {
    const uint32_t next_write = (g_audio_producer_write_pos + 1) % AUDIO_RING_SIZE;
    
    /* If buffer is full, drop this sample (prevents blocking) */
    if (next_write == g_audio_read_pos.load(std::memory_order_acquire)) {
        publish_audio_write_batch();
        audio_stats_samples_dropped(1);
        return false;
    }
    
    g_audio_ring[g_audio_producer_write_pos * 2] = left;
    g_audio_ring[g_audio_producer_write_pos * 2 + 1] = right;
    g_audio_producer_write_pos = next_write;
    g_audio_pending_write_frames++;
    if (g_audio_pending_write_frames >= AUDIO_WRITE_BATCH_FRAMES) {
        publish_audio_write_batch();
    }
    return true;
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    if (!audio_output_should_run()) return;
    (void)enqueue_audio_sample(left, right);
}

#ifdef GBRT_ENABLE_TEST_HOOKS
static void reset_audio_test_ring(void) {
    g_audio_write_pos.store(0, std::memory_order_relaxed);
    g_audio_read_pos.store(0, std::memory_order_relaxed);
    g_audio_producer_write_pos = 0;
    g_audio_pending_write_frames = 0;
    g_audio_started = false;
    g_audio_start_threshold = AUDIO_RING_SIZE;
    g_audio_low_watermark = 1;
    g_audio_rate_control.store(false, std::memory_order_relaxed);
    g_audio_samples_written = 0;
    g_audio_write_publications = 0;
    g_audio_underruns.store(0, std::memory_order_relaxed);
    g_audio_reported_underruns = 0;
    memset(g_audio_ring, 0, sizeof(g_audio_ring));
    audio_stats_init();
}

bool gb_platform_test_audio_concurrency(uint32_t frames,
                                        GBAudioStressResult* out_result) {
    if (!out_result || frames < AUDIO_WRITE_BATCH_FRAMES * 4u) return false;

    reset_audio_test_ring();
    g_audio_muted.store(false, std::memory_order_relaxed);
    g_audio_volume_percent.store(100, std::memory_order_relaxed);

    /* First prove
    g_audio_gb_volume_percent.store(100, std::memory_order_relaxed); /* passo 6e */ the block-copy fast path preserves exact stereo PCM. */
    enum { PCM_TEST_FRAMES = 128 };
    int16_t expected[PCM_TEST_FRAMES * 2];
    int16_t actual[PCM_TEST_FRAMES * 2];
    for (uint32_t i = 0; i < PCM_TEST_FRAMES; ++i) {
        expected[i * 2] = (int16_t)(i * 193u);
        expected[i * 2 + 1] = (int16_t)~expected[i * 2];
        if (!enqueue_audio_sample(expected[i * 2], expected[i * 2 + 1])) return false;
    }
    publish_audio_write_batch();
    sdl_audio_callback(NULL, (Uint8*)actual, (int)sizeof(actual));
    if (memcmp(expected, actual, sizeof(expected)) != 0) return false;

    reset_audio_test_ring();
    std::atomic<bool> producer_done{false};
    std::thread consumer([&producer_done]() {
        int16_t output[256 * 2];
        while (!producer_done.load(std::memory_order_acquire) ||
               audio_ring_fill_samples() > 0) {
            sdl_audio_callback(NULL, (Uint8*)output, (int)sizeof(output));
            std::this_thread::yield();
        }
    });

    uint64_t frames_enqueued = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if ((i & 0xFFu) == 0) {
            g_audio_muted.store((i & 0x100u) != 0, std::memory_order_relaxed);
            g_audio_volume_percent.store((i % 201u), std::memory_order_relaxed);
            update_audio_stats_from_ring();
        }
        if (enqueue_audio_sample((int16_t)i, (int16_t)~i)) {
            frames_enqueued++;
        }
    }
    publish_audio_write_batch();
    producer_done.store(true, std::memory_order_release);
    consumer.join();
    update_audio_stats_from_ring();

    out_result->frames_enqueued = frames_enqueued;
    out_result->write_publications = g_audio_write_publications;
    out_result->underruns = g_audio_underruns.load(std::memory_order_relaxed);
    return frames_enqueued > 0 &&
           g_audio_stats.total_samples_queued == frames_enqueued &&
           g_audio_stats.total_buffer_underruns == out_result->underruns;
}
#endif

bool gb_platform_init(int scale) {
    g_benchmark_mode = g_benchmark_mode || env_flag_enabled("GBRECOMP_BENCHMARK");
    g_scale = scale;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 20) g_scale = 20;
    g_windowed_width = GB_SCREEN_WIDTH * g_scale;
    g_windowed_height = GB_SCREEN_HEIGHT * g_scale;
    g_game_viewport = {0, 0, g_windowed_width, g_windowed_height};
    g_exit_action = GB_PLATFORM_EXIT_QUIT;
    g_frame_count = 0;
    g_manual_joypad_buttons = 0xFF;
    g_manual_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;
    g_script_joypad_dpad = 0xFF;
    g_fullscreen = platform_default_fullscreen();
    g_app_suspended = false;
    g_renderer_reset_pending = false;
    g_show_overlay = false;
    g_max_speed_mode = false;
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    update_effective_joypad_state();
    g_last_guest_framebuffer_valid = false;
    g_present_count = 0;
    g_last_timing = {};

    if (g_benchmark_mode) {
        if (!SDL_getenv("SDL_VIDEODRIVER")) {
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
        }
        if (!SDL_getenv("SDL_AUDIODRIVER")) {
            SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
        }
        if (SDL_Init(SDL_INIT_TIMER) < 0) {
            fprintf(stderr, "[SDL] SDL_Init failed in benchmark mode: %s\n", SDL_GetError());
            return false;
        }
        load_runtime_preferences();
        g_last_frame_time = SDL_GetTicks();
        return true;
    }
    
    fprintf(stderr, "[SDL] Initializing SDL...\n");
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    load_runtime_preferences();
    fprintf(stderr, "[SDL] SDL initialized.\n");
    {
#if defined(__ANDROID__)   /* [android-port] Android/data/<pacote>/files/ */
        char* hd_base = NULL;
        hd_pack_io_set_base_dir(android_bridge::user_dir().c_str());
#else
        char* hd_base = SDL_GetBasePath();
        hd_pack_io_set_base_dir(hd_base ? hd_base : "");
#endif
        if (hd_base) {
            SDL_free(hd_base);
        }
        hd_pack_io_load_pack(); /* passo 5b */
    }

#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif

    install_supplemental_controller_mappings();
    clear_controller_state();
    open_first_available_controller();
    refresh_audio_output_devices();
    
    reopen_audio_output_device(false);
    
    fprintf(stderr, "[SDL] Creating window...\n");
    /* Passo 14a: com OpenGL a janela nasce com contexto de GL; se não der,
     * volta para a janela de sempre com o SDL_Renderer. */
    const bool want_gl = (g_video_backend_pref == 1);
    if (want_gl) {
        vgl_set_context_attributes();
    }
    for (int attempt = want_gl ? 0 : 1; attempt < 2 && !g_window; ++attempt) {
        g_window = SDL_CreateWindow(
            "Bigodão - Wario Land: Super Mario Land 3",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            g_windowed_width,
            g_windowed_height,
            SDL_WINDOW_SHOWN | (attempt == 0 ? SDL_WINDOW_OPENGL : 0) |
            (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE)
        );
        if (!g_window && attempt == 0) {
            fprintf(stderr, "[GL] Janela com OpenGL nao abriu (%s); usando o SDL_Renderer.\n",
                    SDL_GetError());
            g_video_status = std::string("SDL (OpenGL nao abriu: ") + SDL_GetError() + ")";
        }
        if (g_window && attempt == 0) {
            char gl_err[256] = "";
            if (vgl_init(g_window, gl_err, sizeof(gl_err))) {
                g_use_gl = true;
                g_video_status = std::string("OpenGL ") + vgl_description();
            } else {
                fprintf(stderr, "[GL] Nao abriu (%s); usando o SDL_Renderer.\n", gl_err);
                g_video_status = std::string("SDL (OpenGL nao abriu: ") + gl_err + ")";
                SDL_DestroyWindow(g_window);
                g_window = NULL;
            }
        }
    }

    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    app_icon_apply(g_window);   /* [fix-v3] ícone da janela do jogo */
    SDL_SetWindowMinimumSize(g_window, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    
    fprintf(stderr, "[SDL] Creating renderer...\n");
    /* 
     * NO VSync - we use wall-clock timing to run at exactly 59.7 FPS.
     * This is essential for non-60Hz monitors (like 100Hz).
     */
    if (!g_use_gl) {   /* passo 14a: com OpenGL não há SDL_Renderer */
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);


    if (!g_renderer) {
        fprintf(stderr, "[SDL] Hardware renderer failed, trying software fallback...\n");
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (g_renderer && g_video_status.empty()) {
        SDL_RendererInfo info;
        g_video_status = (SDL_GetRendererInfo(g_renderer, &info) == 0)
            ? std::string("SDL (") + info.name + ")" : std::string("SDL");
    } else if (g_renderer) {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(g_renderer, &info) == 0) {
            g_video_status += std::string(" - ") + info.name;
        }
    }
    }

    if (!g_use_gl && !g_renderer) {
        fprintf(stderr, "[SDL] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    update_render_filter();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    if (g_use_gl) {   /* passo 14a */
        ImGui_ImplSDL2_InitForOpenGL(g_window, vgl_context());
        ImGui_ImplOpenGL3_Init(vgl_glsl_version());
        vgl_set_vsync(g_vsync);
    } else {
        ImGui_ImplSDL2_InitForSDLRenderer(g_window, g_renderer);
        ImGui_ImplSDLRenderer2_Init(g_renderer);
    }

    if (!recreate_streaming_texture()) {
        if (g_renderer) SDL_DestroyRenderer(g_renderer);
        if (g_use_gl) vgl_shutdown();
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    update_game_viewport();
    
    g_last_frame_time = SDL_GetTicks();
    /* passo 3D-5a: descarta teclas/botões que ficaram na fila de quem abriu o
     * jogo e protege o primeiro segundo contra abrir o menu */
    SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYUP);
    SDL_FlushEvents(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLERBUTTONUP);
    g_show_menu = getenv("BIGODAO_TESTE_MENU") != NULL;   /* testes: abre com o menu na tela (passo 15a) */
    g_menu_guard_until = SDL_GetTicks() + 1000;
    if (g_menu_guard_until == 0) g_menu_guard_until = 1;
    
    return true;
}

static bool handle_binding_capture_event(const SDL_Event* event) {
    if (!g_binding_capture_active || !event) {
        return false;
    }

    switch (event->type) {
        case SDL_KEYDOWN:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_KEYBOARD || event->key.repeat != 0) {
                return true;
            }
            if (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                event->key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                cancel_binding_capture();
                return true;
            }
            commit_binding_capture(make_binding(GB_INPUT_BINDING_KEY, event->key.keysym.scancode));
            return true;

        case SDL_CONTROLLERBUTTONDOWN:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_CONTROLLER) {
                return true;
            }
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                cancel_binding_capture();
                return true;
            }
            commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, event->cbutton.button));
            return true;

        case SDL_CONTROLLERAXISMOTION:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_CONTROLLER) {
                return true;
            }
            if (event->caxis.value >= 16000) {
                commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, event->caxis.axis));
            } else if (event->caxis.value <= -16000) {
                commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, event->caxis.axis));
            }
            return true;

        case SDL_KEYUP:
        case SDL_CONTROLLERBUTTONUP:
            return true;

        default:
            return false;
    }
}

static bool handle_runtime_event(const SDL_Event* event, GBContext* ctx) {
    if (!event) {
        return true;
    }

    const uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    const bool dpad_selected = !(joyp & 0x10);
    const bool buttons_selected = !(joyp & 0x20);

    if (event->type == SDL_QUIT) {
        return false;
    }
    if (event->type == SDL_WINDOWEVENT &&
        event->window.event == SDL_WINDOWEVENT_CLOSE &&
        (!g_window || event->window.windowID == SDL_GetWindowID(g_window))) {
        return false;
    }
    if (handle_binding_capture_event(event)) {
        return true;
    }

    switch (event->type) {
        case SDL_APP_WILLENTERBACKGROUND:
            touch_controls::reset();   /* [android-port] */
            update_effective_joypad_state();
            set_app_suspended(true);
            break;

        case SDL_APP_DIDENTERFOREGROUND:
            set_app_suspended(false);
            break;

        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:
            g_renderer_reset_pending = true;
            recreate_streaming_texture();
            break;

        case SDL_CONTROLLERDEVICEADDED:
            if (!g_controller) {
                open_controller_index(event->cdevice.which);
            }
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_controller && active_controller_instance_id() == event->cdevice.which) {
                clear_controller_state();
                open_first_available_controller();
            }
            break;

        case SDL_CONTROLLERDEVICEREMAPPED:
            if (g_controller && active_controller_instance_id() == event->cdevice.which) {
                refresh_controller_profile();
            }
            break;

        case SDL_AUDIODEVICEADDED:
            if (!event->adevice.iscapture) {
                refresh_audio_output_devices();
            }
            break;

        case SDL_AUDIODEVICEREMOVED:
            if (!event->adevice.iscapture) {
                refresh_audio_output_devices();
                if (g_audio_device != 0 && event->adevice.which == g_audio_device) {
                    reopen_audio_output_device(true);
                }
            }
            break;

        case SDL_FINGERDOWN:      /* [android-port] gamepad na tela */
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: {
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;
            if (!touch_controls::handle_event(*event, g_window, g_show_menu)) {
                break;
            }
            if (touch_controls::take_menu_request()) {
                g_show_menu = !g_show_menu;
                touch_controls::reset();
            }
            update_effective_joypad_state();
            const bool port_captured = dispatch_captured_port_input(
                ctx, previous_dpad, previous_buttons);
            if (ctx && !port_captured &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_CONTROLLERAXISMOTION: {
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            if (event->caxis.axis >= 0 && event->caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
                g_controller_axis_values[event->caxis.axis] = event->caxis.value;
            }
            update_controller_axis_binding_state();
            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            const bool port_captured = dispatch_captured_port_input(
                ctx, previous_dpad, previous_buttons);
            if (ctx && !port_captured &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool pressed = (event->type == SDL_CONTROLLERBUTTONDOWN);
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                if (pressed && menu_toggle_allowed()) {
                    g_show_menu = !g_show_menu;
                }
                return true;
            }

            for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
                for (int slot = 0; slot < 2; slot++) {
                    if (binding_matches_controller_button(g_controller_bindings[action][slot], event->cbutton.button)) {
                        g_controller_button_binding_pressed[action][slot] = pressed;
                    }
                }
            }

            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            const bool port_captured = dispatch_captured_port_input(
                ctx, previous_dpad, previous_buttons);
            if (ctx && !port_captured &&
                pressed &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool pressed = (event->type == SDL_KEYDOWN);
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            switch (event->key.keysym.scancode) {
                case SDL_SCANCODE_F2:
                    if (ctx && pressed && event->key.repeat == 0) {
                        const GBPortInputEvent port_event = {
                            GB_PORT_INPUT_TOGGLE_UI, true};
                        gbrt_port_input(ctx, &port_event);
                    }
                    return true;

                case SDL_SCANCODE_F3:
                    if (ctx && pressed && event->key.repeat == 0) {
                        const GBPortInputEvent port_event = {
                            GB_PORT_INPUT_TOGGLE_ENCOUNTERS, true};
                        gbrt_port_input(ctx, &port_event);
                    }
                    return true;
                case SDL_SCANCODE_F9:
                    if (pressed && event->key.repeat == 0 && !hd_pack_io_sprite_rec_active()) {
                        ppu_native_request_scene_capture(); /* passo 5a.2 */
                    }
                    return true;
                case SDL_SCANCODE_F4:   /* passo 12a: gravar gabarito */
                    if (pressed && event->key.repeat == 0) {
                        gabarito_request_capture();
                    }
                    return true;
                case SDL_SCANCODE_F11:
                    if (pressed && event->key.repeat == 0) {
                        set_fullscreen_enabled(!g_fullscreen);
                    }
                    return true;
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_AC_BACK:
                    if (pressed && event->key.repeat == 0 && menu_toggle_allowed()) {
                        g_show_menu = !g_show_menu;
                    }
                    return true;

                default:
                    break;
            }

            for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
                for (int slot = 0; slot < 2; slot++) {
                    if (binding_matches_scancode(g_keyboard_bindings[action][slot], event->key.keysym.scancode)) {
                        g_keyboard_binding_pressed[action][slot] = pressed;
                    }
                }
            }

            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            const bool port_captured = dispatch_captured_port_input(
                ctx, previous_dpad, previous_buttons);
            if (ctx && !port_captured &&
                pressed &&
                event->key.repeat == 0 &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_RESIZED ||
                event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event->window.event == SDL_WINDOWEVENT_EXPOSED) {
                    
                // Atualiza as dimensões baseadas no tamanho real informado pelo evento
                if (!g_fullscreen) {
                    g_windowed_width = event->window.data1;
                    g_windowed_height = event->window.data2;
                }
        
                // Força a recriação da textura streaming e reinicia o viewport para preencher o Gamescope
                g_renderer_reset_pending = true;
                update_game_viewport();
            }
            break;
    }

    return true;
}

/* Passo 3D-2b: marcas dos tiles fora das fases */
static std::string marks3d_path(void) {
    std::string d = hd_pack_io_dump_dir();   /* .../mods/dump */
    const size_t c = d.find_last_of("/\\");
    return (c == std::string::npos ? std::string("mods") : d.substr(0, c)) + "/3d_marcas.txt";
}
/* uma linha "hash tipo [tela]" do arquivo de marcas */
static void marks3d_parse(const char* line, std::map<std::pair<uint16_t, uint64_t>, uint8_t>& out) {
    {
        unsigned long long h = 0;
        unsigned k = 0;
        char cx[16] = "";
        const int n = sscanf(line, "%llx %u %15s", &h, &k, cx);
        if (n < 2 || k > 4) return;   /* 3D-2k: 4 = meio chão */
        uint16_t c = 0;   /* 3D-2f: contexto "f07"/"m44"/"g00" (sem = todas as telas) */
        if (n == 3) {
            const unsigned v = (unsigned)strtoul(cx + 1, NULL, 16) & 0xFFu;
            if (cx[0] == 'f') c = (uint16_t)(PPU_NATIVE_CTX_LEVEL | v);
            else if (cx[0] == 'm') c = (uint16_t)(PPU_NATIVE_CTX_MAP | v);
            else if (cx[0] == 'g') c = (uint16_t)(PPU_NATIVE_CTX_MODE | v);
        }
        if (c == 0 && k == 0) return;
        out[{c, (uint64_t)h}] = (uint8_t)k;
    }
}
static void marks3d_load(void) {
    if (g_3d_marks_loaded) return;
    g_3d_marks_loaded = true;
    /* 3D-2q: primeiro as padrão; o arquivo do usuário vale por cima */
    for (const char* l : k_marcas3d_padrao) marks3d_parse(l, g_3d_marks_def);
    g_3d_marks = g_3d_marks_def;
    FILE* f = fopen(marks3d_path().c_str(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) marks3d_parse(line, g_3d_marks);
    fclose(f);
}
static void marks3d_save(void) {
    const std::string p = marks3d_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    FILE* f = fopen(p.c_str(), "w");
    if (!f) { g_3d_mark_info = "nao consegui gravar " + p; return; }
    fprintf(f, "# Bigodao 3D-2b/2f: hash do tile (FNV-1a dos 16 bytes), tipo (0 fundo, 1 chao, 2 agua, 3 porta/escada, 4 meio chao) e tela (g00, m44...; sem = todas)\n");
    for (const auto& e : g_3d_marks) {
        const auto d = g_3d_marks_def.find(e.first);   /* 3D-2q: só o que difere das padrão */
        if (d != g_3d_marks_def.end() && d->second == e.second) continue;
        char cx[8];
        ppu_native_hd_context_suffix(e.first.first, cx);
        fprintf(f, "%016llx %u%s%s\n", (unsigned long long)e.first.second, (unsigned)e.second, cx[0] ? " " : "", cx);
    }
    fclose(f);
}
static uint64_t marks3d_hash(const uint8_t* src) {
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64, como o ppu_native */
    for (int i = 0; i < 16; ++i) { h ^= src[i]; h *= 1099511628211ull; }
    return h;
}
/* Passo 3D-2j: tela das marcas. Igual ao contexto do pacote HD, menos as telas
 * depois da fase, que lá contam como a fase (f07...) e aqui ficam separadas (disassembly:
 * sGameMode $A8C3 = 4/6, sSubMode $A8C4, sCourseClrMode $A9AD):
 *   g80 sala do tesouro (submodo 5-9), g81 jogo das moedas (baldes, rotina 6),
 *   g82 jogo dos corações (A/B/C, rotina $0B), g83 tela do Course Clear;
 *   3D-2n: g84 tempo esgotado (modo 7, a tela do TIME UP);
 *   3D-2p: g85 sala do tesouro depois do game over (modo 8, submodo 2-3: GM_GAMEOVER_INITTRROOM/TRROOM),
 *   do grupo da g80: sem marca própria num tile, vale a marca da g80 */
static uint16_t marks3d_context(const GBContext* ctx) {
    const uint16_t c = ppu_native_hd_context();
    if (!ctx || !ctx->eram || ctx->eram_size <= 0x09AD) return c;
    const uint8_t mode = ctx->eram[0x08C3], sub = ctx->eram[0x08C4];
    if (mode == 0x07) return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x84);   /* 3D-2n */
    if (mode == 0x08 && sub >= 2) return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x85);   /* 3D-2p */
    if ((mode == 0x04 || mode == 0x06) && sub >= 3) {
        if (sub >= 5) return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x80);
        const uint8_t rtn = ctx->eram[0x09AD];
        if (rtn == 0x06) return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x81);
        if (rtn == 0x0B) return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x82);
        return (uint16_t)(PPU_NATIVE_CTX_MODE | 0x83);
    }
    return c;
}
static const char* marks3d_context_name(uint16_t c) {
    switch (c) {
        case PPU_NATIVE_CTX_MODE | 0x80: return TR("treasure room", "sala do tesouro");
        case PPU_NATIVE_CTX_MODE | 0x81: return TR("coin game", "jogo das moedas");
        case PPU_NATIVE_CTX_MODE | 0x82: return TR("heart game", "jogo dos coracoes");
        case PPU_NATIVE_CTX_MODE | 0x83: return "Course Clear";
        case PPU_NATIVE_CTX_MODE | 0x84: return TR("time up", "tempo esgotado (TIME UP)");
        case PPU_NATIVE_CTX_MODE | 0x85: return TR("game over treasure room (g80 group)", "sala do tesouro do game over (grupo da g80)");
        case PPU_NATIVE_CTX_MODE | 0x00: return TR("title / save slots", "titulo / slots");
        default: return "";
    }
}

/* 3D-2p: marca de um tile numa tela: a da tela, a do grupo (g85 -> g80) ou a geral */
static uint16_t marks3d_parent(uint16_t c) {
    return c == (uint16_t)(PPU_NATIVE_CTX_MODE | 0x85) ? (uint16_t)(PPU_NATIVE_CTX_MODE | 0x80) : (uint16_t)0;
}
static std::map<std::pair<uint16_t, uint64_t>, uint8_t>::iterator marks3d_find(uint16_t cx, uint64_t h) {
    auto it = g_3d_marks.find({cx, h});
    const uint16_t par = marks3d_parent(cx);
    if (it == g_3d_marks.end() && par != 0) it = g_3d_marks.find({par, h});
    if (it == g_3d_marks.end()) it = g_3d_marks.find({(uint16_t)0, h});
    return it;
}

/* tabela por tile da VRAM (banco * 384 + tile) para a GPU */
static void marks3d_table(const GBContext* ctx) {
    static uint8_t tab[768];
    bool any = false;
    memset(tab, 0, sizeof(tab));
    if (ctx && ctx->vram && !g_3d_marks.empty()) {
        const uint16_t cx = marks3d_context(ctx);   /* 3D-2f: a tela atual; 3D-2j: telas depois da fase separadas */
        for (int b = 0; b < 2; ++b) {
            for (int t = 0; t < 384; ++t) {
                const uint64_t h = marks3d_hash(ctx->vram + b * 0x2000 + t * 16);
                auto it = marks3d_find(cx, h);   /* 3D-2p */
                if (it != g_3d_marks.end() && it->second != 0) { tab[b * 384 + t] = it->second; any = true; }
            }
        }
    }
    gpu_ppu_set_tile_marks(tab, any);
}
/* clique no jogo: tile do fundo/janela debaixo do ponteiro */
static void marks3d_click(GBContext* ctx, int mx, int my, bool clear) {
    if (!ctx || !ctx->vram) return;
    const SDL_Rect& vp = g_game_viewport;
    if (vp.w <= 0 || vp.h <= 0 || mx < vp.x || my < vp.y || mx >= vp.x + vp.w || my >= vp.y + vp.h) return;
    const int bw = g_3d_wide ? 256 : 160;
    int sx = (mx - vp.x) * bw / vp.w - (bw == 256 ? 48 : 0);
    const int sy = (my - vp.y) * 144 / vp.h;
    if (sx < 0 || sx >= 160) { g_3d_mark_info = "clique no meio da tela (nas laterais nao vale)"; return; }
    const uint8_t lcdc = gb_read8(ctx, 0xFF40), scy = gb_read8(ctx, 0xFF42), scx = gb_read8(ctx, 0xFF43);
    const uint8_t wy = gb_read8(ctx, 0xFF4A), wx = gb_read8(ctx, 0xFF4B);
    int bx, by, map;
    if ((lcdc & 0x20) && sy >= wy && sx >= wx - 7) {
        bx = sx - (wx - 7); by = sy - wy; map = (lcdc & 0x40) ? 0x1C00 : 0x1800;
    } else {
        bx = (sx + scx) & 255; by = (sy + scy) & 255; map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    }
    const int entry = map + ((by >> 3) & 31) * 32 + ((bx >> 3) & 31);
    const int num = ctx->vram[entry], attr = ctx->vram[0x2000 + entry];
    const int tidx = (lcdc & 0x10) ? num : (num < 128 ? 256 + num : num);
    const int bank = (attr & 0x08) ? 1 : 0;
    const uint64_t h = marks3d_hash(ctx->vram + bank * 0x2000 + tidx * 16);
    marks3d_load();
    /* 3D-2f: a marca vale só para esta tela */
    const uint16_t cx = marks3d_context(ctx);   /* 3D-2j */
    int cur = 0;
    auto it = marks3d_find(cx, h);   /* 3D-2p */
    if (it != g_3d_marks.end()) cur = it->second;
    /* 3D-2k: ordem fundo -> chão -> meio chão -> água -> porta/escada -> fundo */
    static const int kNext[5] = {1, 4, 3, 0, 2};
    const int k = clear ? 0 : kNext[cur < 0 || cur > 4 ? 0 : cur];
    const bool general = g_3d_marks.count({(uint16_t)0, h}) != 0 || g_3d_marks_def.count({cx, h}) != 0 ||   /* 3D-2q */
                         (marks3d_parent(cx) != 0 && g_3d_marks.count({marks3d_parent(cx), h}) != 0);   /* 3D-2p */
    if (cx == 0) {
        if (k == 0) g_3d_marks.erase({(uint16_t)0, h}); else g_3d_marks[{(uint16_t)0, h}] = (uint8_t)k;
    } else if (k == 0 && !general) {
        g_3d_marks.erase({cx, h});
    } else {
        g_3d_marks[{cx, h}] = (uint8_t)k;
    }
    const char* kNames[] = {TR("background", "fundo"), TR("floor", "chao"), TR("water", "agua"), TR("door/ladder", "porta/escada"), TR("half floor", "meio chao")};
    char sfx[8];
    ppu_native_hd_context_suffix(cx, sfx);
    char buf[128];
    const char* nm = marks3d_context_name(cx);
    snprintf(buf, sizeof(buf), TR("tile %03X (bank %d) on screen %s%s%s%s: %s", "tile %03X (banco %d) na tela %s%s%s%s: %s"), tidx, bank, sfx[0] ? sfx : TR("(all)", "(todas)"),
             nm[0] ? " (" : "", nm, nm[0] ? ")" : "", kNames[k]);
    g_3d_mark_info = buf;
    marks3d_save();
}

bool gb_platform_poll_events(GBContext* ctx) {
    SDL_Event event;

    if (g_freeze_quit) return false;   /* passo 3D-2c: fechou com o jogo congelado */
    if (!g_benchmark_mode) {
        while (SDL_PollEvent(&event)) {
            if (ImGui::GetCurrentContext() != NULL) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            if (g_3d_mark && !g_show_menu && event.type == SDL_MOUSEBUTTONDOWN &&
                (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {   /* passo 3D-2b */
                marks3d_click(ctx, event.button.x, event.button.y, event.button.button == SDL_BUTTON_RIGHT);
                continue;
            }
            if (!handle_runtime_event(&event, ctx)) {
                return false;
            }
        }

        while (g_app_suspended) {
            if (!SDL_WaitEvent(&event)) {
                continue;
            }
            if (ImGui::GetCurrentContext() != NULL) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            if (!handle_runtime_event(&event, ctx)) {
                return false;
            }
        }
    }

    /* Handle Automation Inputs */
    uint8_t previous_script_dpad = g_script_joypad_dpad;
    uint8_t previous_script_buttons = g_script_joypad_buttons;
    g_script_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;

    uint64_t current_cycles = ctx ? ctx->total_cycles : 0;
    for (int i = 0; i < g_script_count; i++) {
        ScriptEntry* e = &g_input_script[i];
        bool active = false;
        if (e->anchor == SCRIPT_ANCHOR_CYCLE) {
            if (e->period > 0) {
                active = current_cycles >= e->start &&
                         current_cycles < (e->end + e->duration) &&
                         ((current_cycles - e->start) % e->period) < e->duration;
            } else {
                active = current_cycles >= e->start &&
                         current_cycles < (e->start + e->duration);
            }
        } else {
            uint64_t current_frame = (uint64_t)g_frame_count;
            active = current_frame >= e->start &&
                     current_frame < (e->start + e->duration);
        }
        if (active) {
            g_script_joypad_dpad &= e->dpad;
            g_script_joypad_buttons &= e->buttons;
        }
    }

    uint8_t new_script_dpad = (uint8_t)(previous_script_dpad & (uint8_t)(~g_script_joypad_dpad) & 0x0F);
    uint8_t new_script_buttons = (uint8_t)(previous_script_buttons & (uint8_t)(~g_script_joypad_buttons) & 0x0F);
    uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    bool dpad_selected = !(joyp & 0x10);
    bool buttons_selected = !(joyp & 0x20);
    if (ctx && ((new_script_dpad && dpad_selected) || (new_script_buttons && buttons_selected))) {
        request_joypad_interrupt(ctx);
    }

    update_effective_joypad_state();
    record_manual_input_state(current_cycles);

    return true;
}

void gb_platform_submit_port_frame(void* user, const GBPortFrame* frame) {
    (void)user;
    if (frame == NULL || frame->abi_version != GB_PORT_ABI_VERSION ||
        frame->command_count > GB_PORT_MAX_DRAW_COMMANDS) {
        g_port_frame = {};
        g_port_frame_valid = false;
        return;
    }
    g_port_frame = *frame;
    g_port_frame_valid = true;
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    if (!g_freeze_redraw) g_freeze_fb = framebuffer;   /* passo 3D-2c */
    ppu_native_set_hd_scale(effective_internal_scale()); /* passo 4b: segue a textura */
    music_monitor_update(); /* passo 6b */
    hd_pack_io_flush_dumps(128); /* passo 5a */
    {
        const PPUNativeScene* scene = ppu_native_take_scene(); /* passo 5a.2 */
        if (scene) {
            if (g_scene_for_recorder) {
                hd_pack_io_sprite_rec_feed(scene);   /* passo 7a */
            } else {
                hd_pack_io_write_scene(scene);
            }
        }
        g_scene_for_recorder = false;
        if (hd_pack_io_sprite_rec_active()) {   /* passo 7a: uma cena por frame */
            hd_pack_io_sprite_rec_set_rect(g_rec_rect_enabled, g_rec_rect[0], g_rec_rect[1],
                                           g_rec_rect[2], g_rec_rect[3]);
            ppu_native_request_scene_capture();
            g_scene_for_recorder = true;
        }
    }
    {   /* passo 14c-1: BIGODAO_COMPARAR_GPU=1 liga a comparação sem o menu (testes) */
        static bool env_checked = false;
        if (!env_checked && g_use_gl) {
            env_checked = true;
            const char* e = SDL_getenv("BIGODAO_COMPARAR_GPU");
            char err[256] = "";
            if (e && e[0] == '1' && !g_gpu_compare && gpu_ppu_init(err, sizeof(err))) {
                g_gpu_compare = true;
                ppu_native_gpu_capture(true);
            }
        }
    }
    bool gpu_scr = false;   /* passo 14c-2 */
    if (g_gpu_screen_pref == 1 && g_use_gl && !gpu_ppu_ready() && g_gpu_screen_error.empty()) {
        char err[256] = "";
        if (!gpu_ppu_init(err, sizeof(err))) {
            g_gpu_screen_error = std::string("a GPU nao abriu: ") + err;
            fprintf(stderr, "[GPU] %s; a CPU continua montando a tela\n", g_gpu_screen_error.c_str());
        }
    }
    gpu_scr = gpu_screen_active() && gpu_ppu_ready();
    if (g_use_gl) {
        const bool want_capture = g_gpu_compare || gpu_scr;
        if (ppu_native_gpu_capture_enabled() != want_capture) {   /* ligar zera o quadro pronto */
            ppu_native_gpu_capture(want_capture);
        }
        ppu_native_set_cpu_hd(!gpu_scr || g_gpu_compare);   /* a comparação precisa do HD da CPU */
        ppu_native_set_world_ext(gpu_scr && g3d_active());   /* passo 3D-4a */
    }
    if ((g_gpu_compare || gpu_scr) && g_use_gl) {   /* passo 14b-1 / 14c-2 */
        gpu_compare_frame();
    }
    //render_frame_internal(framebuffer, true, false);
    // Utilize o framebuffer gerado nativamente no VBlank
    GBPPU* ppu = active_ppu();
    const uint32_t* native_fb = (ppu && ppu_get_widescreen_enabled(ppu)) 
                                ? ppu_get_widescreen_framebuffer(ppu) 
                                : framebuffer;

    render_frame_internal(native_fb, true, ppu_get_widescreen_enabled(ppu));
}

void gb_platform_present_framebuffer(const uint32_t* framebuffer) {
    const uint32_t* stable_framebuffer = framebuffer;
    bool stable_framebuffer_is_widescreen = false;
    if (g_widescreen_mode) {
        if (g_last_guest_widescreen_framebuffer_valid) {
            stable_framebuffer = g_last_guest_widescreen_framebuffer;
            stable_framebuffer_is_widescreen = true;
        } else if (g_last_guest_framebuffer_valid) {
            /* No widescreen stable frame available; fall back to normal frame
             * centered in the widescreen texture during upload. */
            stable_framebuffer = g_last_guest_framebuffer;
        }
    } else {
        if (g_last_guest_framebuffer_valid) {
            stable_framebuffer = g_last_guest_framebuffer;
        }
    }
    render_frame_internal(stable_framebuffer, false, stable_framebuffer_is_widescreen);
}

void gb_platform_render_lcd_off_frame(void) {
    ensure_lcd_off_framebuffer();
    const uint32_t* stable_framebuffer = g_lcd_off_framebuffer;
    bool is_wide = false;
    bool is_last_frame = false;   /* passo 6a.3 */
    if (g_widescreen_mode) {
        if (g_last_guest_widescreen_framebuffer_valid) {
            stable_framebuffer = g_last_guest_widescreen_framebuffer;
            is_wide = true;
            is_last_frame = true;
        } else if (g_last_guest_framebuffer_valid) {
            stable_framebuffer = g_last_guest_framebuffer;
            is_last_frame = true;
        }
    } else if (g_last_guest_framebuffer_valid) {
        stable_framebuffer = g_last_guest_framebuffer;
        is_last_frame = true;
    }
    /* Passo 6a.3: repete o ultimo frame com a largura certa e, se for o
     * caso, com o mesmo frame HD (o ppu_native nao desenha com o LCD
     * desligado, entao o frame HD ainda e o do ultimo frame mostrado; se o
     * LCD ja religou no meio da fatia, fica sem HD so neste frame). */
    const bool lcd_still_off = g_registered_ctx && g_registered_ctx->lcd_off_active;
    render_frame_internal(stable_framebuffer, false, is_wide, is_last_frame && lcd_still_off);
}

void gb_platform_get_timing_info(GBPlatformTimingInfo* out) {
    if (!out) return;
    *out = g_last_timing;
}

uint8_t gb_platform_get_joypad(void) {
    /* Return combined state based on P1 register selection */
    /* Caller should AND with the appropriate selection bits */
    return g_joypad_buttons & g_joypad_dpad;
}

void gb_platform_vsync(uint32_t frame_cycles) {
    /* Passo 12a: gabarito entre um quadro e outro. O jogo volta ao estado de
     * antes; o resto é o mesmo que depois de carregar um savestate. */
    /* Passo 14p: Reset do menu, entre um quadro e outro. Reinicia como na
     * abertura (gb_context_reset); a RAM do cartucho (save) fica. */
    if (g_game_reset_pending && g_registered_ctx) {
        g_game_reset_pending = false;
        gb_context_reset(g_registered_ctx, true);
        ppu_native_invalidate_all();
        ppu_reset_widescreen_heuristics(g_registered_ctx);
        ip_invalidate();
        reset_audio_output_buffer(true);
        g_last_guest_framebuffer_valid = false;
        g_last_guest_widescreen_framebuffer_valid = false;
        g_present_count = g_registered_ctx->completed_frames;
        g_last_frame_time = SDL_GetTicks();
        fprintf(stderr, "[SDL] Jogo reiniciado pelo menu.\n");
    }
    if (g_registered_ctx && gabarito_process(g_registered_ctx)) {
        reset_audio_output_buffer(true);
        g_last_guest_framebuffer_valid = false;
        g_last_guest_widescreen_framebuffer_valid = false;
        g_present_count = g_registered_ctx->completed_frames;
        g_last_frame_time = SDL_GetTicks();
    }
    static const char* s_test_freeze = getenv("BIGODAO_TESTE_CONGELAR");   /* testes: congela por N redesenhos no quadro 30 */
    if (s_test_freeze && g_registered_ctx && g_registered_ctx->completed_frames == 30) g_freeze = true;
    if (g_freeze && !g_benchmark_mode && g_registered_ctx) {   /* passo 3D-2c: jogo congelado */
        if (g_audio_device) SDL_PauseAudioDevice(g_audio_device, 1);
        int redraws = 0;
        while (g_freeze && !g_freeze_quit) {
            if (s_test_freeze && redraws++ >= atoi(s_test_freeze)) { g_freeze = false; break; }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ImGui::GetCurrentContext() != NULL) ImGui_ImplSDL2_ProcessEvent(&ev);
                if (g_3d_mark && !g_show_menu && ev.type == SDL_MOUSEBUTTONDOWN &&
                    (ev.button.button == SDL_BUTTON_LEFT || ev.button.button == SDL_BUTTON_RIGHT)) {
                    marks3d_click(g_registered_ctx, ev.button.x, ev.button.y, ev.button.button == SDL_BUTTON_RIGHT);
                    continue;
                }
                if (!handle_runtime_event(&ev, g_registered_ctx)) { g_freeze_quit = true; break; }
            }
            if (g_freeze_quit) break;
            if (g_freeze_fb) {
                g_freeze_redraw = true;
                gb_platform_render_frame(g_freeze_fb);
                g_freeze_redraw = false;
            }
            SDL_Delay(16);
        }
        reset_audio_output_buffer(true);
        g_last_frame_time = SDL_GetTicks();
        if (g_audio_device) SDL_PauseAudioDevice(g_audio_device, (audio_output_should_run() && g_audio_started) ? 0 : 1);
    }
    if (g_benchmark_mode || g_app_suspended) {
        g_last_timing.pacing_cycles = (frame_cycles > 0) ? frame_cycles : 70224u;
        g_last_timing.pacing_ms = 0.0;
        return;
    }
    /* 
     * Frame pacing: Run at the DMG frame cadence derived from 70224 cycles
     * at 4194304 Hz, and ease off sleeping when audio fill is too low.
     *
     * Each call accounts for the frame that just completed. Keep an
     * accumulated wall-clock target and advance it before waiting so the
     * current frame's cycle count is what determines the current sleep.
     */
    static uint64_t next_frame_time = 0;
    static uint64_t frame_remainder = 0;
    const uint64_t gb_frame_cycles = (frame_cycles > 0) ? (uint64_t)frame_cycles : 70224ull;
    const uint64_t gb_cpu_hz = 4194304;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint32_t speed_percent = (uint32_t)effective_speed_percent();
    uint64_t frame_ticks_num = (freq * gb_frame_cycles * 100ull) + frame_remainder;
    uint64_t frame_ticks_den = gb_cpu_hz * (uint64_t)speed_percent;
    uint64_t frame_ticks = frame_ticks_num / frame_ticks_den;
    double pacing_start_ms = sdl_now_ms();

    if (frame_ticks == 0) {
        frame_ticks = 1;
    }
    /* Passo 6a.1: o ritmo dos frames segue o relógio da placa de som
     * (parte integral do mixer, até ±3%). */
    if (g_audio_rate_control.load(std::memory_order_relaxed) && audio_output_should_run()) {
        const double pace = (double)g_audio_pace_ppm.load(std::memory_order_relaxed) / 1000000.0;
        frame_ticks = (uint64_t)((double)frame_ticks * (1.0 + pace));
        if (frame_ticks == 0) {
            frame_ticks = 1;
        }
    }

    if (next_frame_time == 0) {
        next_frame_time = now;
    }

    frame_remainder = frame_ticks_num % frame_ticks_den;
    next_frame_time += frame_ticks;
    //uint64_t target_frame_time = next_frame_time;

    uint32_t audio_fill = audio_ring_fill_samples();
    bool audio_starved = audio_output_should_run() && g_audio_started && audio_fill < g_audio_low_watermark;
    bool audio_overfull = audio_output_should_run() && g_audio_started &&
                      audio_fill > g_audio_start_threshold + g_audio_start_threshold / 4;
    if (audio_overfull && !g_audio_rate_control.load(std::memory_order_relaxed)) {
    next_frame_time += frame_ticks / 50; /* ~2% mais lento até drenar */
    }
    uint64_t target_frame_time = next_frame_time;

    /* Passo 10a: quadro do meio, meio quadro depois do ultimo quadro do jogo
     * (assim os dois ficam igualmente espacados). */
    /* Passo 10b: o quadro do meio vai no meio do intervalo do ritmo (o
     * horario do ultimo present podia chegar tarde quando o present espera
     * o monitor); se o jogo demorou mais que meio quadro, sai assim que der. */
    /* Passo 10c: o quadro do meio ja saiu no lugar do quadro do jogo; o
     * quadro do jogo sai meio quadro depois (sempre sai, mesmo atrasado). */
    if (g_ip_pending_main && ip_test_mid()) {
        g_ip_pending_main = false;   /* passo 14d-1: teste, fica o quadro do meio na janela */
    }
    if (g_ip_pending_main) {
        g_ip_pending_main = false;
        g_ip_last_gap_ms = (target_frame_time > now)
            ? (double)(target_frame_time - now) * 1000.0 / (double)freq : 0.0;
        uint64_t main_time = g_ip_last_present + frame_ticks / 2;
        const uint64_t latest = target_frame_time - frame_ticks / 16;
        if (main_time > latest) {
            main_time = latest;
        }
        /* Passo 10e: se o present espera o monitor, o proximo present ja cai
         * no proximo refresh (8,3 ms a 120 Hz): sai na hora, sem esperar.
         * Esperar aqui fazia perder o refresh e atrasar meio quadro. */
        g_ip_present_avg_ms = g_ip_present_avg_ms * 0.9 + g_last_timing.present_ms * 0.1;
        const bool present_waits = g_ip_present_avg_ms > 1.5;
        if (!audio_starved && !present_waits) {
            ip_wait_until(main_time);
        }
        ip_present_mid();
        now = SDL_GetPerformanceCounter();
        /* Passo 10d/10e: tres atrasos seguidos de mais de meio quadro = sem
         * folga; 2 s so com o quadro do jogo */
        if (now > target_frame_time + frame_ticks / 2) {
            if (++g_ip_late_streak >= 3) {
                g_ip_cooldown = 120;
                g_ip_late_streak = 0;
            }
        } else {
            g_ip_late_streak = 0;
        }
    }

    if (!audio_starved && now < target_frame_time) {
        for (;;) {
            uint64_t wait_ticks = target_frame_time - now;
            uint32_t wait_us = (uint32_t)((wait_ticks * 1000000) / freq);

            /*
             * SDL_Delay() can oversleep by several milliseconds on desktop OSes.
             * Sleep in small chunks, then busy-wait the tail for stable pacing.
             */
            if (wait_us > 4000) {
                SDL_Delay((wait_us / 1000) - 2);
            } else if (wait_us > 1500) {
                SDL_Delay(1);
            } else {
                break;
            }

            now = SDL_GetPerformanceCounter();
            if (now >= target_frame_time) {
                break;
            }
        }

        while (SDL_GetPerformanceCounter() < target_frame_time) {
            /* spin */
        }
        now = SDL_GetPerformanceCounter();
    }

    /* If we fell behind by more than 6 frames, reset (don't try to catch up) */
    uint64_t max_frame_lag = frame_ticks * 6;
    if (now > target_frame_time + max_frame_lag) {
        next_frame_time = now;
        frame_remainder = 0;
    }
    
    update_audio_stats_from_ring();
    audio_stats_tick(SDL_GetTicks64());
    g_last_timing.pacing_cycles = (uint32_t)gb_frame_cycles;
    g_last_timing.pacing_ms = sdl_now_ms() - pacing_start_ms;
    g_timing_vsync_total += g_last_timing.pacing_ms;
    g_timing_frame_count++;
    g_last_frame_time = SDL_GetTicks();
    g_last_vsync_end_ms = sdl_now_ms();
}

bool gb_platform_get_smooth_lcd_transitions(void) {
    return g_smooth_lcd_transitions;
}

void gb_platform_set_smooth_lcd_transitions(bool enabled) {
    g_smooth_lcd_transitions = enabled;
}

void gb_platform_set_title(const char* title) {
    if (g_window && !g_benchmark_mode) {
        SDL_SetWindowTitle(g_window, title);
    }
}

/* ============================================================================
 * Save Data
 * ========================================================================== */

static void sdl_get_persistent_path(char* buffer, size_t size, const char* rom_name, const char* extension) {
    const std::string base_name = extract_path_leaf(rom_name);
    const std::string suffix = (extension && extension[0]) ? extension : ".sav";
    const std::string filename = base_name + suffix;

    if (!g_persistence_dir.empty()) {
        const fs::path resolved = fs::path(g_persistence_dir) / filename;
        snprintf(buffer, size, "%s", resolved.lexically_normal().string().c_str());
        return;
    }

#if defined(__ANDROID__)
    const std::string resolved = resolve_writable_path(filename.c_str(), base_name.c_str());
    snprintf(buffer, size, "%s", resolved.c_str());
#else
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        fs::path resolved = fs::path(base_path) / filename;
        SDL_free(base_path);
        snprintf(buffer, size, "%s", resolved.lexically_normal().string().c_str());
    } else {
        const std::string resolved = resolve_writable_path(filename.c_str(), base_name.c_str());
        snprintf(buffer, size, "%s", resolved.c_str());
    }
#endif
}

bool gb_platform_set_persistence_dir(const char* path) {
    if (!path || !path[0]) {
        g_persistence_dir.clear();
        return true;
    }
    std::error_code error;
    const fs::path resolved = fs::absolute(fs::path(path), error).lexically_normal();
    if (error || !fs::is_directory(resolved, error) || error) {
        return false;
    }
    g_persistence_dir = resolved.string();
    return true;
}

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    sdl_get_persistent_path(buffer, size, rom_name, ".sav");
}

static void sdl_get_rtc_path(char* buffer, size_t size, const char* rom_name) {
    sdl_get_persistent_path(buffer, size, rom_name, ".rtc");
}

static void context_storage_name(const GBContext* ctx, char* buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (ctx && ctx->save_id[0]) {
        snprintf(buffer, size, "%s", ctx->save_id);
        return;
    }

    if (ctx && ctx->rom && ctx->rom_size > 0x143) {
        char title[17];
        memset(title, 0, sizeof(title));
        memcpy(title, &ctx->rom[0x134], 16);
        for (int i = 0; i < 16; i++) {
            if (title[i] == 0 || title[i] < 32 || title[i] > 126) {
                title[i] = 0;
            }
        }
        if (title[0]) {
            snprintf(buffer, size, "%s", title);
            return;
        }
    }

    snprintf(buffer, size, "game");
}

static void sdl_get_savestate_path(char* buffer, size_t size, const GBContext* ctx, int slot) {
    char storage_name[64];
    char extension[32];
    context_storage_name(ctx, storage_name, sizeof(storage_name));
    if (slot < 0) {
        slot = 0;
    }
    if (slot >= GB_SAVESTATE_SLOT_COUNT) {
        slot = GB_SAVESTATE_SLOT_COUNT - 1;
    }
    snprintf(extension, sizeof(extension), ".state%d", slot + 1);
    sdl_get_persistent_path(buffer, size, storage_name, extension);
}

static bool savestate_slot_exists(const GBContext* ctx, int slot, std::string* out_path) {
    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    if (out_path) {
        *out_path = filename;
    }

    std::error_code ec;
    return fs::exists(fs::path(filename), ec) && !ec;
}

static bool save_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Save", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    const bool success = gb_context_save_state_file(ctx, filename);
    set_savestate_status("Save", slot, success, filename);
    return success;
}

static bool load_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Load", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    const bool success = gb_context_load_state_file(ctx, filename);
    if (success) {
        reset_audio_output_buffer(true);
        g_last_guest_framebuffer_valid = false;
        g_present_count = ctx->completed_frames;
        g_last_frame_time = SDL_GetTicks();
    }
    set_savestate_status("Load", slot, success, filename);
    return success;
}

static bool delete_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Delete", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    std::error_code ec;
    const bool removed = fs::remove(fs::path(filename), ec);
    if (!removed && !ec) {
        set_savestate_status("Delete", slot, false, "Slot file does not exist");
        return false;
    }
    set_savestate_status("Delete", slot, removed && !ec, filename);
    return removed && !ec;
}

static const char* persistence_kind_name(GBPersistenceTestTarget target) {
    return target == GB_PERSISTENCE_TEST_TARGET_RTC ? "RTC data" : "battery RAM";
}

void gb_platform_test_inject_persistence_fault(
    GBPersistenceTestTarget target,
    GBPersistenceTestFault fault) {
    g_persistence_test_target = target;
    g_persistence_test_fault = fault;
}

static GBPersistenceTestFault consume_persistence_test_fault(
    GBPersistenceTestTarget target) {
    if (g_persistence_test_fault == GB_PERSISTENCE_TEST_FAULT_NONE ||
        g_persistence_test_target != target) {
        return GB_PERSISTENCE_TEST_FAULT_NONE;
    }
    const GBPersistenceTestFault fault = g_persistence_test_fault;
    g_persistence_test_fault = GB_PERSISTENCE_TEST_FAULT_NONE;
    return fault;
}

static bool sync_persistence_file(FILE* file) {
    if (fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool replace_persistence_file(
    const fs::path& staged,
    const fs::path& destination) {
#if defined(_WIN32)
    return MoveFileExA(
               staged.string().c_str(),
               destination.string().c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (rename(staged.string().c_str(), destination.string().c_str()) != 0) {
        return false;
    }
    const fs::path parent = destination.parent_path();
    const int directory_fd = open(parent.string().c_str(), O_RDONLY);
    if (directory_fd >= 0) {
        if (fsync(directory_fd) != 0) {
            fprintf(
                stderr,
                "[GBRT] Persistence transaction committed but directory sync "
                "failed for '%s': %s\n",
                parent.string().c_str(),
                strerror(errno));
        }
        close(directory_fd);
    }
    return true;
#endif
}

static bool write_persistence_transaction(
    const char* filename,
    const void* data,
    size_t size,
    GBPersistenceTestTarget target) {
    const fs::path destination(filename);
    const fs::path staged = destination.string() + ".tmp-v1";
    const GBPersistenceTestFault fault =
        consume_persistence_test_fault(target);
    const char* kind = persistence_kind_name(target);

    if (fault == GB_PERSISTENCE_TEST_FAULT_FULL_DISK) {
        fprintf(
            stderr,
            "[GBRT] %s transaction v1 failed before staging '%s': "
            "injected full-disk condition; previous file retained\n",
            kind,
            destination.string().c_str());
        return false;
    }

    FILE* file = fopen(staged.string().c_str(), "wb");
    if (!file) {
        fprintf(
            stderr,
            "[GBRT] %s transaction v1 could not stage '%s': %s; "
            "previous file retained\n",
            kind,
            staged.string().c_str(),
            strerror(errno));
        return false;
    }

    size_t requested = size;
    if (fault == GB_PERSISTENCE_TEST_FAULT_SHORT_WRITE && requested > 0) {
        requested--;
    } else if (fault == GB_PERSISTENCE_TEST_FAULT_TRUNCATION) {
        requested = size / 2;
    }
    const size_t written = fwrite(data, 1, requested, file);
    bool staged_ok = written == requested && requested == size;
    if (staged_ok) {
        staged_ok = sync_persistence_file(file);
    }
    const int close_result = fclose(file);
    staged_ok = staged_ok && close_result == 0;

    if (!staged_ok) {
        std::error_code remove_error;
        fs::remove(staged, remove_error);
        fprintf(
            stderr,
            "[GBRT] %s transaction v1 rejected staged write '%s' "
            "(expected %zu bytes, wrote %zu); previous file retained\n",
            kind,
            staged.string().c_str(),
            size,
            written);
        return false;
    }

    if (fault == GB_PERSISTENCE_TEST_FAULT_INTERRUPTION) {
        fprintf(
            stderr,
            "[GBRT] %s transaction v1 interrupted after staging '%s'; "
            "previous file retained and stage left for recovery\n",
            kind,
            staged.string().c_str());
        return false;
    }

    if (!replace_persistence_file(staged, destination)) {
        const int replace_error = errno;
        std::error_code remove_error;
        fs::remove(staged, remove_error);
        fprintf(
            stderr,
            "[GBRT] %s transaction v1 could not atomically replace '%s': "
            "%s; previous file retained\n",
            kind,
            destination.string().c_str(),
            strerror(replace_error));
        return false;
    }
    return true;
}

static bool sdl_load_battery_ram(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "rb");
    if (!f) return false;

    std::vector<uint8_t> loaded(size);
    size_t read = fread(loaded.data(), 1, size, f);
    const int trailing = fgetc(f);
    fclose(f);

    if (read != size || trailing != EOF) {
        if (ctx) {
            ctx->persistence_load_failed = true;
        }
        fprintf(stderr,
                "[GBRT] Rejected battery RAM '%s': expected exactly %zu bytes; "
                "automatic persistence overwrite suppressed\n",
                filename,
                size);
        return false;
    }

    memcpy(data, loaded.data(), size);
    return true;
}

static bool sdl_save_battery_ram(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    return write_persistence_transaction(
        filename,
        data,
        size,
        GB_PERSISTENCE_TEST_TARGET_BATTERY);
}

static bool sdl_load_rtc_data(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    char filename[512];
    sdl_get_rtc_path(filename, sizeof(filename), rom_name);

    FILE* f = fopen(filename, "rb");
    if (!f) return false;

    std::vector<uint8_t> loaded(size);
    size_t read = fread(loaded.data(), 1, size, f);
    const int trailing = fgetc(f);
    fclose(f);

    if (read != size || trailing != EOF) {
        if (ctx) {
            ctx->persistence_load_failed = true;
        }
        fprintf(stderr,
                "[GBRT] Rejected RTC data '%s': expected exactly %zu bytes; "
                "automatic persistence overwrite suppressed\n",
                filename,
                size);
        return false;
    }

    memcpy(data, loaded.data(), size);
    return true;
}

static bool sdl_save_rtc_data(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_rtc_path(filename, sizeof(filename), rom_name);

    return write_persistence_transaction(
        filename,
        data,
        size,
        GB_PERSISTENCE_TEST_TARGET_RTC);
}

void gb_platform_register_context(GBContext* ctx) {
    g_registered_ctx = ctx;
    if (ctx && ctx->ppu) {
        ppu_set_widescreen_enabled((GBPPU*)ctx->ppu, g_widescreen_mode);
        ppu_set_widescreen_fade_enabled((GBPPU*)ctx->ppu, g_widescreen_fade_enabled);
        ppu_set_widescreen_fade_amount((GBPPU*)ctx->ppu, (float)g_widescreen_fade_percent / 100.0f);
    }
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram,
        .load_rtc_data = sdl_load_rtc_data,
        .save_rtc_data = sdl_save_rtc_data
    };
    gb_set_platform_callbacks(ctx, &callbacks);
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

bool gb_platform_init(int scale) {
    (void)scale;
    return false;
}

void gb_platform_shutdown(void) {}

void gb_platform_set_launcher_return_enabled(bool enabled) {
    (void)enabled;
}

GBPlatformExitAction gb_platform_get_exit_action(void) {
    return GB_PLATFORM_EXIT_QUIT;
}

bool gb_platform_poll_events(GBContext* ctx) {
    (void)ctx;
    return true;
}

bool gb_platform_set_input_script(const char* script) {
    return script == NULL || *script == '\0';
}

bool gb_platform_set_persistence_dir(const char* path) {
    return path == NULL || *path == '\0';
}

void gb_platform_test_inject_persistence_fault(
    GBPersistenceTestTarget target,
    GBPersistenceTestFault fault) {
    (void)target;
    (void)fault;
}

void gb_platform_set_input_record_file(const char* path) { (void)path; }
void gb_platform_set_benchmark_mode(bool enabled) { (void)enabled; }
void gb_platform_submit_port_frame(void* user, const GBPortFrame* frame) {
    (void)user;
    (void)frame;
}

void gb_platform_render_frame(const uint32_t* framebuffer) {
    (void)framebuffer;
}

void gb_platform_present_framebuffer(const uint32_t* framebuffer) {
    (void)framebuffer;
}

void gb_platform_render_lcd_off_frame(void) {}

void gb_platform_get_timing_info(GBPlatformTimingInfo* out) {
    if (!out) return;
    *out = GBPlatformTimingInfo{};
}

uint8_t gb_platform_get_joypad(void) {
    return 0xFF;
}

void gb_platform_vsync(uint32_t frame_cycles) { (void)frame_cycles; }

bool gb_platform_get_smooth_lcd_transitions(void) { return false; }

void gb_platform_set_smooth_lcd_transitions(bool enabled) { (void)enabled; }

void gb_platform_set_title(const char* title) {
    (void)title;
}

void gb_platform_set_dump_frames(const char* frames) { (void)frames; }

void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
