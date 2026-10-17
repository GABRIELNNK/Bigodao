#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

// stb_image (single-header, domínio público): decodifica a arte JPEG embutida e
// fornece o inflate usado para ler entradas comprimidas do wario.o2r.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "stb_image.h"

#include "asset_extractor.hpp"
#include "platform_sdl.h"
#include "ppu.h"
#include "wl_sm3_patch.hpp"
#include "gbrt.h"
#include "android_bridge.h"   /* [android-port] */
#include "app_icon.h"         /* [fix-v3] */

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#include "runtime/vendor/imgui/imgui.h"
#include "runtime/vendor/imgui/backends/imgui_impl_sdl2.h"
#include "runtime/vendor/imgui/backends/imgui_impl_sdlrenderer2.h"

// Arte + fontes embutidas no executável (gerado a partir do PNG/TTF).
#include "launcher_assets.h"

#include <cctype>
#include <cfloat>
#include <cmath>
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
static std::string calculate_file_sha256(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return "";

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

// =============================================================================
// LAUNCHER — interface ImGui + SDL2, tudo embutido no executável
// =============================================================================
namespace fs = std::filesystem;

// ---- Conversões de caminho (UTF-8 <-> fs::path, seguro para acentos) --------
static std::string path_to_utf8(const fs::path& p) {
    auto s = p.u8string();                       // std::string (C++17) ou std::u8string (C++20)
    return std::string(s.begin(), s.end());
}

static fs::path path_from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

// ---- Seletor de arquivos nativo ---------------------------------------------
struct DialogSpec {
    const wchar_t* win_title;
    const wchar_t* win_filter;     // formato Win32: "Nome\0*.a;*.b\0...\0"
    const char*    title;
    const char*    zenity_filter;  // ex.: "--file-filter='ROMs | *.gb *.gbc'"
    const char*    kdialog_filter; // ex.: "*.gb *.gbc|ROMs"
};

static fs::path open_file_dialog(SDL_Window* owner, const DialogSpec& spec) {
#ifdef _WIN32
    HWND hwnd = NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (owner && SDL_GetWindowWMInfo(owner, &info)) hwnd = info.info.win.window;

    wchar_t file[1024] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd;                 // diálogo modal sobre o launcher
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrFilter  = spec.win_filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle   = spec.win_title;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) == TRUE) return fs::path(file);
    return {};
#else
    (void)owner;
    std::string command;
    if (system("which zenity > /dev/null 2>&1") == 0) {
        command = std::string("zenity --file-selection --title=\"") + spec.title + "\" " +
                  spec.zenity_filter + " 2>/dev/null";
    } else if (system("which kdialog > /dev/null 2>&1") == 0) {
        command = std::string("kdialog --getopenfilename . \"") + spec.kdialog_filter +
                  "\" --title \"" + spec.title + "\" 2>/dev/null";
    }
    if (command.empty()) return {};

    std::string result;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[4096];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            result = buffer;
            result.erase(result.find_last_not_of("\r\n") + 1);
        }
        pclose(pipe);
    }
    return result.empty() ? fs::path() : path_from_utf8(result);
#endif
}

namespace launcher {

/* [android-port] textos que mudam por plataforma */
#ifdef __ANDROID__
#define LAUNCHER_HINT_EMPTY "Tap the button to choose the file."
#define LAUNCHER_HINT_READY "Tap LOAD & PLAY."
#define LAUNCHER_HINT_TIP   "The files are copied into the app's own storage."
#else
#define LAUNCHER_HINT_EMPTY "Or drag the file into this window."
#define LAUNCHER_HINT_READY "Click LOAD & PLAY or press Enter."
#define LAUNCHER_HINT_TIP   "Tip: drag the ROM and the patch straight into this window."
#endif

// ---- Paleta (mesmas cores do launcher em HTML) ------------------------------
constexpr ImU32 COL_BG          = IM_COL32( 13,  15,  23, 255);
constexpr ImU32 COL_CARD        = IM_COL32( 22,  25,  37, 242);
constexpr ImU32 COL_CARD_HOVER  = IM_COL32( 27,  30,  44, 246);
constexpr ImU32 COL_BORDER      = IM_COL32(255, 255, 255,  20);
constexpr ImU32 COL_BORDER_GOLD = IM_COL32(255, 204,   0,  80);
constexpr ImU32 COL_TEXT        = IM_COL32(255, 255, 255, 255);
constexpr ImU32 COL_SUB         = IM_COL32(148, 163, 184, 255);
constexpr ImU32 COL_MUTED       = IM_COL32(100, 116, 139, 255);
constexpr ImU32 COL_DIM         = IM_COL32( 71,  85, 105, 255);
constexpr ImU32 COL_GOLD        = IM_COL32(255, 204,   0, 255);
constexpr ImU32 COL_GREEN       = IM_COL32( 74, 222, 128, 255);
constexpr ImU32 COL_RED         = IM_COL32(248, 113, 113, 255);
constexpr ImU32 COL_BLUE        = IM_COL32( 37,  99, 235, 255);
constexpr ImU32 COL_BLUE_HOVER  = IM_COL32( 59, 130, 246, 255);
constexpr ImU32 COL_BLUE_ACTIVE = IM_COL32( 29,  78, 216, 255);
constexpr ImU32 COL_PLAY        = IM_COL32(225,  29,  72, 255);
constexpr ImU32 COL_PLAY_HOVER  = IM_COL32(244,  63,  94, 255);
constexpr ImU32 COL_PLAY_ACTIVE = IM_COL32(190,  18,  60, 255);
constexpr ImU32 COL_OFF_BG      = IM_COL32( 39,  39,  42, 255);
constexpr ImU32 COL_OFF_TEXT    = IM_COL32( 82,  82,  91, 255);

// ---- Layout -----------------------------------------------------------------
constexpr int   WINDOW_W   = 960;
constexpr int   WINDOW_H   = 760;
constexpr float MARGIN     = 24.0f;
constexpr float GAP        = 16.0f;
constexpr float ART_BOTTOM = 506.0f;   // onde a arte termina (com o degradê)
constexpr float CARDS_Y    = 486.0f;   // cards sobem um pouco sobre o degradê
constexpr float CARD_H     = 180.0f;

// Tamanhos de fonte (cada um é rasterizado no tamanho exato para ficar nítido)
constexpr float FS_TITLE  = 18.0f;
constexpr float FS_BODY   = 14.0f;
constexpr float FS_SMALL  = 12.5f;
constexpr float FS_BUTTON = 14.0f;
constexpr float FS_PLAY   = 17.0f;
constexpr float FS_MONO   = 13.0f;

struct Fonts {
    ImFont* title  = nullptr;
    ImFont* body   = nullptr;
    ImFont* small  = nullptr;
    ImFont* button = nullptr;
    ImFont* play   = nullptr;
    ImFont* mono   = nullptr;
};

static ImFont* add_embedded_font(const unsigned char* data, unsigned int len, float px) {
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2022, 0x2022, 0x2026, 0x2026, 0 };
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // os dados são estáticos, o atlas não deve liberar
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), (int)len,
                                                      px, &cfg, ranges);
}

static Fonts load_fonts() {
    Fonts f;
    // A primeira fonte adicionada vira a padrão do ImGui.
    f.body   = add_embedded_font(LAUNCHER_FONT_SEMIBOLD,  LAUNCHER_FONT_SEMIBOLD_LEN,  FS_BODY);
    f.small  = add_embedded_font(LAUNCHER_FONT_SEMIBOLD,  LAUNCHER_FONT_SEMIBOLD_LEN,  FS_SMALL);
    f.title  = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_TITLE);
    f.button = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_BUTTON);
    f.play   = add_embedded_font(LAUNCHER_FONT_EXTRABOLD, LAUNCHER_FONT_EXTRABOLD_LEN, FS_PLAY);
    ImFontConfig mono_cfg;
    mono_cfg.SizePixels = FS_MONO;
    f.mono = ImGui::GetIO().Fonts->AddFontDefault(&mono_cfg);   // ProggyClean: monoespaçada, cara retrô
    return f;
}

static SDL_Texture* load_embedded_art(SDL_Renderer* renderer, int& w, int& h) {
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(LAUNCHER_ART_JPG, (int)LAUNCHER_ART_JPG_LEN,
                                                  &w, &h, &channels, 4);
    if (!pixels) return nullptr;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    stbi_image_free(pixels);
    return tex;
}

// ---- Helpers de desenho -----------------------------------------------------
static ImU32 col_lerp(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                 ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

static ImU32 col_alpha(ImU32 c, float mul) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= mul;
    return ImGui::ColorConvertFloat4ToU32(v);
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float ease_out(float t) { t = clamp01(t); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// Valor 0..1 que anima suavemente em direção ao alvo (para hovers).
static float anim(const char* key, bool on, float speed = 10.0f) {
    ImGui::PushID(key);
    ImGuiID id = ImGui::GetID("##anim");
    ImGui::PopID();
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, 0.0f);
    v = clamp01(v + (on ? 1.0f : -1.0f) * ImGui::GetIO().DeltaTime * speed);
    st->SetFloat(id, v);
    return v;
}

static ImVec2 text_size(ImFont* f, float size, const char* text, float wrap = 0.0f) {
    return f->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

// Corta o texto com "…" se não couber na largura.
static std::string ellipsize(ImFont* f, float size, const std::string& s, float max_w) {
    if (text_size(f, size, s.c_str()).x <= max_w) return s;
    const char* ell = "\xE2\x80\xA6";
    const float ell_w = text_size(f, size, ell).x;
    std::string out = s;
    while (!out.empty()) {
        size_t i = out.size();
        do { --i; } while (i > 0 && (static_cast<unsigned char>(out[i]) & 0xC0) == 0x80);
        out.resize(i);
        if (text_size(f, size, out.c_str()).x + ell_w <= max_w) break;
    }
    return out + ell;
}

// Sombra suave: camadas translúcidas crescentes.
static void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col,
                        float strength, float spread = 3.0f, int layers = 6, float y_off = 4.0f) {
    for (int i = layers; i >= 1; --i) {
        const float e = spread * (float)i;
        const float alpha = strength * (1.0f - (float)i / (float)(layers + 1)) / (float)layers * 2.2f;
        dl->AddRectFilled(ImVec2(a.x - e, a.y - e + y_off), ImVec2(b.x + e, b.y + e + y_off),
                          col_alpha(col, alpha), rounding + e);
    }
}

// Botão "invisível" com hit-test do ImGui e desenho próprio.
struct ButtonState { bool pressed = false; bool hovered = false; bool held = false; float hover_t = 0.0f; };

static ButtonState hit_button(const char* id, ImVec2 pos, ImVec2 size, bool enabled, bool interactive) {
    ButtonState s;
    if (interactive) {
        ImGui::SetCursorScreenPos(pos);
        s.pressed = ImGui::InvisibleButton(id, size) && enabled;
        s.hovered = ImGui::IsItemHovered() && enabled;
        s.held    = ImGui::IsItemActive() && enabled;
        if (s.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    s.hover_t = anim(id, s.hovered);
    return s;
}

static bool blue_button(ImDrawList* dl, const char* id, const char* label, ImVec2 pos, float height,
                        const Fonts& f, bool interactive) {
    const ImVec2 ts = text_size(f.button, FS_BUTTON, label);
    const ImVec2 size(ts.x + 34.0f, height);
    ButtonState s = hit_button(id, pos, size, true, interactive);
    const float lift = s.held ? 0.0f : -1.0f * s.hover_t;
    const ImVec2 a(pos.x, pos.y + lift), b(pos.x + size.x, pos.y + size.y + lift);

    soft_shadow(dl, a, b, 8.0f, COL_BLUE, 0.25f + 0.25f * s.hover_t, 2.0f, 4, 2.0f);
    dl->AddRectFilled(a, b, s.held ? COL_BLUE_ACTIVE : col_lerp(COL_BLUE, COL_BLUE_HOVER, s.hover_t), 8.0f);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + size.y * 0.5f), IM_COL32(255, 255, 255, 18), 8.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddText(f.button, FS_BUTTON, ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f),
                COL_TEXT, label);
    return s.pressed;
}

// ---- Ícones vetoriais (as fontes não têm ✓ ✖ ▶) ------------------------------
static void icon_check(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th = 2.0f) {
    ImVec2 pts[3] = { ImVec2(p.x, p.y + s * 0.55f), ImVec2(p.x + s * 0.38f, p.y + s * 0.90f),
                      ImVec2(p.x + s, p.y + s * 0.12f) };
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, th);
}

static void icon_cross(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th = 2.0f) {
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + s, p.y + s), col, th);
    dl->AddLine(ImVec2(p.x + s, p.y), ImVec2(p.x, p.y + s), col, th);
}

static void icon_play(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    dl->AddTriangleFilled(ImVec2(p.x, p.y), ImVec2(p.x + s * 0.88f, p.y + s * 0.5f), ImVec2(p.x, p.y + s), col);
}

// ---- Estado de cada arquivo -------------------------------------------------
struct FileSlot {
    enum class State { Empty, Valid, Invalid, Missing };
    fs::path    path;
    std::string name;       // nome do arquivo em UTF-8 (para exibição)
    State       state = State::Empty;
    float       changed_at = -10.0f;   // para o "flash" de feedback

    // O hash só é calculado quando o arquivo muda (antes rodava a cada frame).
    void set(const fs::path& p, const std::string& expected, float now) {
        path = p;
        name = path_to_utf8(p.filename());
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))                 state = State::Missing;
        else if (calculate_file_sha256(p) == expected)  state = State::Valid;
        else                                             state = State::Invalid;
        changed_at = now;
    }
    bool valid() const { return state == State::Valid; }
};

static void draw_badge(ImDrawList* dl, ImVec2 right_top, FileSlot::State st, const Fonts& f) {
    const char* text;  ImU32 fg, bg, border;
    switch (st) {
        case FileSlot::State::Valid:   text = "Valid";          fg = COL_GREEN; bg = IM_COL32(34, 197, 94, 38); border = IM_COL32(34, 197, 94, 77); break;
        case FileSlot::State::Invalid: text = "Invalid";        fg = COL_RED;   bg = IM_COL32(239, 68, 68, 38); border = IM_COL32(239, 68, 68, 77); break;
        case FileSlot::State::Missing: text = "Not found";      fg = COL_RED;   bg = IM_COL32(239, 68, 68, 38); border = IM_COL32(239, 68, 68, 77); break;
        default:                       text = "Pending";        fg = COL_MUTED; bg = IM_COL32(148, 163, 184, 26); border = 0; break;
    }
    const ImVec2 ts = text_size(f.small, FS_SMALL, text);
    const float icon = 9.0f, pad = 9.0f, h = 23.0f;
    const float w = pad + icon + 6.0f + ts.x + pad;
    const ImVec2 a(right_top.x - w, right_top.y), b(right_top.x, right_top.y + h);
    dl->AddRectFilled(a, b, bg, 6.0f);
    if (border) dl->AddRect(a, b, border, 6.0f, 0, 1.0f);
    const ImVec2 ip(a.x + pad, a.y + (h - icon) * 0.5f);
    if (st == FileSlot::State::Valid)      icon_check(dl, ip, icon, fg, 1.8f);
    else if (st == FileSlot::State::Empty) dl->AddCircle(ImVec2(ip.x + icon * 0.5f, ip.y + icon * 0.5f), icon * 0.45f, fg, 12, 1.6f);
    else                                   icon_cross(dl, ip, icon * 0.9f, fg, 1.8f);
    dl->AddText(f.small, FS_SMALL, ImVec2(ip.x + icon + 6.0f, a.y + (h - ts.y) * 0.5f), fg, text);
}

struct CardSpec {
    const char* id;
    const char* title;
    const char* subtitle;
    const char* sha256;
    const char* button;
    const char* empty_hint;
    const char* invalid_msg;
};

// Desenha um card; devolve true se o botão "Buscar" foi clicado.
static bool draw_card(ImDrawList* dl, ImVec2 pos, ImVec2 size, const CardSpec& spec,
                      const FileSlot& slot, const Fonts& f, bool interactive, float now) {
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);
    const bool hovered = interactive && ImGui::IsMouseHoveringRect(a, b);
    const float ht = anim(spec.id, hovered, 8.0f);

    ImU32 accent = COL_GOLD;
    if (slot.state == FileSlot::State::Valid) accent = COL_GREEN;
    else if (slot.state != FileSlot::State::Empty) accent = COL_RED;

    // Moldura
    soft_shadow(dl, a, b, 14.0f, IM_COL32(0, 0, 0, 255), 0.55f, 3.0f, 6, 6.0f);
    dl->AddRectFilled(a, b, col_lerp(COL_CARD, COL_CARD_HOVER, ht), 14.0f);
    const float flash = 1.0f - clamp01((now - slot.changed_at) / 0.6f);   // brilho rápido ao validar
    dl->AddRect(a, b, col_lerp(col_lerp(COL_BORDER, COL_BORDER_GOLD, ht), col_alpha(accent, 0.9f), flash),
                14.0f, 0, 1.0f + flash);
    // Filete de destaque no topo (cor do estado)
    const float acc_a = (slot.state == FileSlot::State::Empty) ? 0.35f + 0.35f * ht : 0.9f;
    dl->AddRectFilledMultiColor(ImVec2(a.x + 14.0f, a.y), ImVec2(a.x + size.x * 0.55f, a.y + 2.0f),
                                col_alpha(accent, acc_a), col_alpha(accent, 0.0f),
                                col_alpha(accent, 0.0f), col_alpha(accent, acc_a));

    const float px = a.x + 18.0f, inner_w = size.x - 36.0f;
    float y = a.y + 16.0f;

    // Título + badge
    dl->AddText(f.title, FS_TITLE, ImVec2(px, y - 1.0f), COL_TEXT, spec.title);
    draw_badge(dl, ImVec2(b.x - 16.0f, y - 2.0f), slot.state, f);
    y += 24.0f;

    // Subtítulo
    dl->AddText(f.small, FS_SMALL, ImVec2(px, y), COL_SUB, spec.subtitle);
    y += 22.0f;

    // Caixa com o SHA-256 esperado (2 linhas, fonte monoespaçada)
    const float box_h = 38.0f;
    dl->AddRectFilled(ImVec2(px, y), ImVec2(px + inner_w, y + box_h), IM_COL32(0, 0, 0, 70), 7.0f);
    dl->AddRect(ImVec2(px, y), ImVec2(px + inner_w, y + box_h), IM_COL32(255, 255, 255, 10), 7.0f);
    dl->AddText(f.small, FS_SMALL, ImVec2(px + 12.0f, y + (box_h - FS_SMALL) * 0.5f - 1.0f), COL_DIM, "SHA-256");
    const std::string hash = spec.sha256;
    const float hx = px + 78.0f;
    dl->AddText(f.mono, FS_MONO, ImVec2(hx, y + 5.0f),  COL_MUTED, hash.substr(0, 32).c_str());
    dl->AddText(f.mono, FS_MONO, ImVec2(hx, y + 20.0f), COL_MUTED, hash.substr(32).c_str());
    y += box_h + 12.0f;

    // Botão + nome do arquivo
    const float btn_h = 34.0f;
    const bool clicked = blue_button(dl, spec.id, spec.button, ImVec2(px, y), btn_h, f, interactive);
    const float btn_w = text_size(f.button, FS_BUTTON, spec.button).x + 34.0f;
    const float name_x = px + btn_w + 14.0f;
    std::string shown = slot.state == FileSlot::State::Empty ? "No file selected" : slot.name;
    ImU32 name_col = COL_MUTED;
    if (slot.state == FileSlot::State::Valid) name_col = COL_GREEN;
    else if (slot.state != FileSlot::State::Empty) name_col = COL_RED;
    shown = ellipsize(f.body, FS_BODY, shown, (px + inner_w) - name_x);
    dl->AddText(f.body, FS_BODY, ImVec2(name_x, y + (btn_h - FS_BODY) * 0.5f - 1.0f), name_col, shown.c_str());
    y += btn_h + 10.0f;

    // Linha de feedback
    switch (slot.state) {
        case FileSlot::State::Valid:
            dl->AddText(f.small, FS_SMALL, ImVec2(px, y), col_alpha(COL_GREEN, 0.85f), "Hash matches. All good!");
            break;
        case FileSlot::State::Invalid:
            dl->AddText(f.small, FS_SMALL, ImVec2(px, y), COL_RED, spec.invalid_msg);
            break;
        case FileSlot::State::Missing:
            dl->AddText(f.small, FS_SMALL, ImVec2(px, y), COL_RED, "File not found.");
            break;
        default:
            dl->AddText(f.small, FS_SMALL, ImVec2(px, y), COL_DIM, spec.empty_hint);
            break;
    }
    return clicked;
}

} // namespace launcher

// Execute Launcher GUI via ImGui + SDL2
static bool run_imgui_launcher(std::string& out_rom_path, std::string& out_patch_path, const char* exec_path) {
    using namespace launcher;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Failed to initialize SDL2 for Launcher: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Bigodão Launcher - Wario Land DX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        SDL_Quit();
        return false;
    }

    app_icon_apply(window);   /* [fix-v3] ícone da janela do launcher */
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);   // fallback: renderer por software
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // não criar imgui.ini ao lado do jogo

    ImGui::StyleColorsDark();
    const Fonts fonts = load_fonts();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    int art_w = 0, art_h = 0;
    SDL_Texture* art = load_embedded_art(renderer, art_w, art_h);

    const CardSpec rom_card = {
        "rom",
        "Wario Land (World)",
        "Original ROM  \xE2\x80\xA2  Super Mario Land 3  \xE2\x80\xA2  .gb",
        EXPECTED_ROM_SHA256.c_str(),
        "Browse ROM",
        LAUNCHER_HINT_EMPTY,
        "SHA-256 mismatch. Please select the original ROM.",
    };
    const CardSpec patch_card = {
        "patch",
        "Patch Wario Land DX",
        "IPS patch  \xE2\x80\xA2  author: korxo  \xE2\x80\xA2  version 1.2",
        EXPECTED_PATCH_SHA256.c_str(),
        "Browse Patch",
        LAUNCHER_HINT_EMPTY,
        "SHA-256 mismatch. This is not the DX v1.2 patch.",
    };
    const DialogSpec rom_dialog = {
        L"Select the original ROM",
        L"Game Boy ROMs (*.gb, *.gbc)\0*.gb;*.gbc\0All files (*.*)\0*.*\0",
        "Select the original ROM",
        "--file-filter='ROMs (*.gb *.gbc) | *.gb *.gbc'",
        "*.gb *.gbc|Game Boy ROMs",
    };
    const DialogSpec patch_dialog = {
        L"Select the IPS patch",
        L"IPS patches (*.ips)\0*.ips\0All files (*.*)\0*.*\0",
        "Select the IPS patch",
        "--file-filter='Patches (*.ips) | *.ips'",
        "*.ips|IPS patches",
    };

    FileSlot rom, patch;
#ifdef __ANDROID__   /* [android-port] */
    int logical_w = 0, logical_h = 0;
    (void)rom_dialog; (void)patch_dialog;
#endif
    std::string modal_msg;
    bool ready_to_launch = false;
    bool done = false;
    const Uint64 t_start = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    auto now = [&]() { return (float)((SDL_GetPerformanceCounter() - t_start) / freq); };

    // Arquivo solto na janela: decide pelo hash; se não bater, pela extensão.
    auto handle_drop = [&](const fs::path& p) {
        const std::string h = calculate_file_sha256(p);
        std::string ext = path_to_utf8(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (h == EXPECTED_PATCH_SHA256 || (h != EXPECTED_ROM_SHA256 && ext == ".ips"))
            patch.set(p, EXPECTED_PATCH_SHA256, now());
        else
            rom.set(p, EXPECTED_ROM_SHA256, now());
    };

    auto try_launch = [&]() {
        // Revalida (o arquivo pode ter mudado desde a seleção).
        rom.set(rom.path, EXPECTED_ROM_SHA256, now());
        patch.set(patch.path, EXPECTED_PATCH_SHA256, now());
        if (!rom.valid() || !patch.valid()) return;
        try {
            find_patch_manifest(exec_path);
            out_rom_path = rom.path.string();
            out_patch_path = patch.path.string();
            ready_to_launch = true;
            done = true;
        } catch (const std::exception& e) {
            modal_msg = std::string("Failed to prepare the ROM:\n") + e.what();
        }
    };

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            else if (event.type == SDL_DROPFILE) {
                if (event.drop.file) {
                    if (modal_msg.empty()) handle_drop(path_from_utf8(event.drop.file));
                    SDL_free(event.drop.file);
                }
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode k = event.key.keysym.sym;
                if (!modal_msg.empty() && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE))
                    modal_msg.clear();
                else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && rom.valid() && patch.valid())
                    try_launch();
            }
        }
        if (done) break;

#ifdef __ANDROID__   /* [android-port] */
        {
            // Resposta do seletor de arquivos: decide ROM/patch pelo hash, como no drag-and-drop.
            int picked_kind = 0;
            std::string picked;
            if (android_bridge::poll_picked_file(picked_kind, picked) && !picked.empty() && modal_msg.empty())
                handle_drop(path_from_utf8(picked));

            // O layout foi feito para 960x760. Renderiza nessa escala lógica (altura fixa,
            // largura acompanha a proporção da tela) e o SDL amplia; toques são convertidos.
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
            if (out_w > 0 && out_h > 0) {
                int lw = (int)((float)WINDOW_H * (float)out_w / (float)out_h);
                int lh = WINDOW_H;
                if (lw < WINDOW_W) {   // tela em retrato: cresce na altura
                    lw = WINDOW_W;
                    lh = (int)((float)WINDOW_W * (float)out_h / (float)out_w);
                }
                if (lw != logical_w || lh != logical_h) {
                    logical_w = lw;
                    logical_h = lh;
                    SDL_RenderSetLogicalSize(renderer, logical_w, logical_h);
                }
            }
        }
#endif

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
#ifdef __ANDROID__   /* [android-port] */
        if (logical_w > 0) {
            io.DisplaySize = ImVec2((float)logical_w, (float)logical_h);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
#endif
        ImGui::NewFrame();

        const float t = now();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;
        const bool interactive = modal_msg.empty();
        const float appear = ease_out(t / 0.6f);   // animação de entrada

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::Begin("MainLauncher", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- Arte (banner em largura total, com degradê para o fundo) --------
        if (art) {
            const float full_h = W * (float)art_h / (float)art_w;
            const float crop = full_h - ART_BOTTOM;               // corta um pouco do céu no topo
            const float v0 = crop > 0.0f ? crop / full_h : 0.0f;
            const float zoom = 1.0f + 0.03f * (1.0f - appear);    // leve "zoom out" na abertura
            const float zx = W * (zoom - 1.0f) * 0.5f;
            dl->AddImage((ImTextureID)(intptr_t)art, ImVec2(-zx, -zx * 0.5f), ImVec2(W + zx, ART_BOTTOM + zx * 0.5f),
                         ImVec2(0.0f, v0), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, (int)(255 * appear)));
        }
        dl->AddRectFilledMultiColor(ImVec2(0, ART_BOTTOM - 80.0f), ImVec2(W, ART_BOTTOM),
                                    col_alpha(COL_BG, 0.0f), col_alpha(COL_BG, 0.0f), COL_BG, COL_BG);
        dl->AddRectFilled(ImVec2(0, ART_BOTTOM), ImVec2(W, H), COL_BG);

        // ---- Cards -----------------------------------------------------------
        const float slide = 18.0f * (1.0f - appear);
        const float card_w = (W - MARGIN * 2.0f - GAP) * 0.5f;
        const ImVec2 card_size(card_w, CARD_H);
        if (draw_card(dl, ImVec2(MARGIN, CARDS_Y + slide), card_size, rom_card, rom, fonts, interactive, t)) {
#ifdef __ANDROID__   /* [android-port] */
            android_bridge::request_file_pick(android_bridge::PICK_ROM);
#else
            const fs::path p = open_file_dialog(window, rom_dialog);
            if (!p.empty()) rom.set(p, EXPECTED_ROM_SHA256, now());
#endif
        }
        if (draw_card(dl, ImVec2(MARGIN + card_w + GAP, CARDS_Y + slide), card_size, patch_card, patch, fonts, interactive, t)) {
#ifdef __ANDROID__   /* [android-port] */
            android_bridge::request_file_pick(android_bridge::PICK_PATCH);
#else
            const fs::path p = open_file_dialog(window, patch_dialog);
            if (!p.empty()) patch.set(p, EXPECTED_PATCH_SHA256, now());
#endif
        }

        // ---- Rodapé ----------------------------------------------------------
        const bool can_play = rom.valid() && patch.valid();
        const float footer_y = CARDS_Y + CARD_H + 14.0f + slide;
        dl->AddLine(ImVec2(MARGIN, footer_y), ImVec2(W - MARGIN, footer_y), IM_COL32(255, 255, 255, 18));

        const ImVec2 play_size(212.0f, 46.0f);
        const ImVec2 play_pos(W - MARGIN - play_size.x, footer_y + 12.0f);

        // Status à esquerda
        const char* status;  ImU32 status_col;
        if (can_play)                                                       { status = "Ready to play!";              status_col = COL_GREEN; }
        else if (rom.state == FileSlot::State::Empty ||
                 patch.state == FileSlot::State::Empty)                     { status = "Select the ROM and the patch"; status_col = COL_SUB; }
        else                                                                { status = "Invalid files";               status_col = COL_RED; }
        const float sy = play_pos.y + 5.0f;
        const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f);
        const ImVec2 dot(MARGIN + 5.0f, sy + 8.0f);
        if (can_play) dl->AddCircleFilled(dot, 5.0f + 4.0f * pulse, col_alpha(COL_GREEN, 0.25f * (1.0f - pulse)), 20);
        dl->AddCircleFilled(dot, 4.0f, status_col, 16);
        dl->AddText(fonts.body, FS_BODY, ImVec2(MARGIN + 18.0f, sy), status_col, status);
        dl->AddText(fonts.small, FS_SMALL, ImVec2(MARGIN + 18.0f, sy + 20.0f), COL_DIM,
                    can_play ? LAUNCHER_HINT_READY
                             : LAUNCHER_HINT_TIP);

        // Botão LOAD & PLAY
        {
            ButtonState s = hit_button("##play", play_pos, play_size, can_play, interactive);
            const float grow = s.held ? -1.0f : 3.0f * s.hover_t;
            const ImVec2 a(play_pos.x - grow, play_pos.y - grow * 0.5f);
            const ImVec2 b(play_pos.x + play_size.x + grow, play_pos.y + play_size.y + grow * 0.5f);
            const float r = (b.y - a.y) * 0.5f;
            ImU32 fill = COL_OFF_BG, text_col = COL_OFF_TEXT;
            if (can_play) {
                soft_shadow(dl, a, b, r, COL_PLAY, 0.45f + 0.25f * pulse + 0.3f * s.hover_t, 3.0f, 6, 3.0f);
                fill = s.held ? COL_PLAY_ACTIVE : col_lerp(COL_PLAY, COL_PLAY_HOVER, s.hover_t);
                text_col = COL_TEXT;
            }
            dl->AddRectFilled(a, b, fill, r);
            if (can_play) {
                dl->AddRectFilled(a, ImVec2(b.x, (a.y + b.y) * 0.5f), IM_COL32(255, 255, 255, 22), r,
                                  ImDrawFlags_RoundCornersTop);
            } else {
                dl->AddRect(a, b, IM_COL32(255, 255, 255, 12), r);
            }
            const char* label = "LOAD & PLAY";
            const ImVec2 ts = text_size(fonts.play, FS_PLAY, label);
            const float icon = 13.0f, gap = 10.0f;
            const float cx = (a.x + b.x - (icon + gap + ts.x)) * 0.5f;
            const float cy = (a.y + b.y) * 0.5f;
            icon_play(dl, ImVec2(cx, cy - icon * 0.5f), icon, text_col);
            dl->AddText(fonts.play, FS_PLAY, ImVec2(cx + icon + gap, cy - ts.y * 0.5f), text_col, label);
            if (s.pressed) try_launch();
        }

        // ---- Modal de erro ---------------------------------------------------
        if (!modal_msg.empty()) {
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(5, 6, 10, 190));
            const float mw = 480.0f, pad = 24.0f;
            const ImVec2 msg_sz = text_size(fonts.body, FS_BODY, modal_msg.c_str(), mw - pad * 2.0f);
            const float mh = pad + 26.0f + 10.0f + msg_sz.y + 16.0f + 34.0f + pad;
            const ImVec2 a((W - mw) * 0.5f, (H - mh) * 0.5f), b(a.x + mw, a.y + mh);
            soft_shadow(dl, a, b, 16.0f, IM_COL32(0, 0, 0, 255), 0.8f, 4.0f, 6, 8.0f);
            dl->AddRectFilled(a, b, IM_COL32(24, 26, 38, 255), 16.0f);
            dl->AddRect(a, b, IM_COL32(239, 68, 68, 90), 16.0f, 0, 1.5f);
            icon_cross(dl, ImVec2(a.x + pad, a.y + pad + 5.0f), 12.0f, COL_RED, 2.5f);
            dl->AddText(fonts.title, FS_TITLE, ImVec2(a.x + pad + 24.0f, a.y + pad), COL_TEXT, "Could not start the game");
            dl->AddText(fonts.body, FS_BODY, ImVec2(a.x + pad, a.y + pad + 36.0f), COL_SUB,
                        modal_msg.c_str(), nullptr, mw - pad * 2.0f);
            const float ok_w = text_size(fonts.button, FS_BUTTON, "OK").x + 34.0f;
            if (blue_button(dl, "##modal_ok", "OK", ImVec2(b.x - pad - ok_w, b.y - pad - 34.0f), 34.0f, fonts, true))
                modal_msg.clear();
        }

        ImGui::SetCursorScreenPos(ImVec2(0, 0));
        ImGui::Dummy(ImVec2(0, 0));   // fecha o layout sem estender a janela
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 13, 15, 23, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData()
#if IMGUI_VERSION_NUM >= 19070   // backend passou a exigir o SDL_Renderer*
                                              , renderer
#endif
        );
        SDL_RenderPresent(renderer);
    }

    if (art) SDL_DestroyTexture(art);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#ifndef __ANDROID__   /* [android-port] no Android o SDL segue vivo até o fim do app */
    SDL_Quit();
#endif

    return ready_to_launch;
}

// Onde fica o wario.o2r: ao lado do executável, para não depender da pasta de
// trabalho (atalhos, abrir pelo Explorer etc.). Um wario.o2r na pasta atual ainda
// é aceito, para compatibilidade com a versão anterior.
static fs::path resolve_o2r_path() {
    const fs::path name = "wario.o2r";
    fs::path beside_exe = name;
    if (char* base = SDL_GetBasePath()) {
        beside_exe = path_from_utf8(base) / name;
        SDL_free(base);
    }
    std::error_code ec;
    if (fs::is_regular_file(beside_exe, ec)) return beside_exe;
    if (fs::is_regular_file(name, ec))       return fs::absolute(name, ec);
    return beside_exe;   // onde será criado
}

static int game_main(int argc, char* argv[]) {   /* [android-port] corpo original do main() */
    // ---------------------------------------------------------------------
    // 1. Fast path: assets already extracted (wario.o2r)
    //    The package holds the patched ROM, so no ROM, IPS or manifest is needed.
    // ---------------------------------------------------------------------
    AssetExtractor assets(resolve_o2r_path());
    std::string rom_input_path  = argc > 1 ? argv[1] : "";
    std::string custom_ips_path = argc > 2 ? argv[2] : "";

    std::vector<uint8_t> patched_rom;

    if (rom_input_path.empty() && assets.exists()) {
        std::string err;
        if (assets.load(patched_rom, err)) {
            std::cout << "[VFS] Loaded patched ROM from " << assets.path().filename().string()
                      << " (" << patched_rom.size() << " bytes).\n";
        } else {
            std::cerr << "[VFS] " << assets.path().filename().string() << " can't be used (" << err
                      << "). It will be rebuilt from the ROM.\n";
            patched_rom.clear();
        }
    }

    // ---------------------------------------------------------------------
    // 2. First run (or a ROM given on the command line):
    //    launcher -> IPS patch in memory -> build wario.o2r
    // ---------------------------------------------------------------------
    if (patched_rom.empty()) {
        if (rom_input_path.empty()) {
            std::cout << "[Launcher] No usable " << assets.path().filename().string() << ". Opening the launcher...\n";
            if (!run_imgui_launcher(rom_input_path, custom_ips_path, argv[0])) {
                std::cout << "[Launcher] Cancelled by user.\n";
                return 0;
            }
        }

        try {
            const fs::path manifest_path = find_patch_manifest(argv[0]);
            if (!custom_ips_path.empty()) {
                std::cout << "Using IPS patch: " << custom_ips_path << "\n";
                patched_rom = wl_sm3_patch::load_and_patch_rom_with_override(rom_input_path, manifest_path, custom_ips_path);
            } else {
                patched_rom = wl_sm3_patch::load_and_patch_rom(rom_input_path, manifest_path);
            }
            std::cout << "IPS patch applied in memory: " << rom_input_path << " -> "
                      << patched_rom.size() << " bytes\n";
        } catch (const std::exception& error) {
            std::cerr << "Failed to prepare ROM: " << error.what() << '\n'
                      << "Usage: " << argv[0] << " [rom.gb [patch.ips]]\n";
            return 1;
        }

        // ---------------------------------------------------------------------
        // 3. Asset extraction (.o2r) — from now on the game starts from it alone
        // ---------------------------------------------------------------------
        std::string err;
        if (!assets.build(patched_rom, err)) {
            std::cerr << "[VFS] Warning: could not create " << assets.path().string() << " (" << err
                      << "). The game will start, but the ROM will be needed again next time.\n";
        }
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


            // 3. Obter e enviar o framebuffer correto (Widescreen ou Standard)
            GBPPU* ppu = (GBPPU*)ctx->ppu;
            const uint32_t* fb = ppu_get_widescreen_enabled(ppu) 
                           ? ppu_get_widescreen_framebuffer(ppu) 
                           : ppu_get_framebuffer(ppu);

            if (fb) {
                gb_platform_render_frame(fb);
            }
            gb_platform_vsync(ctx->frame_cycles);
        } else if (ctx->frame_lcd_off_cycles > 0) {
            /* Passo 6a.2: com o LCD desligado (portas, troca de sala) a fatia
             * termina sem frame. Antes ela nao esperava nada: o jogo corria
             * sem freio ate o LCD religar e gerava audio demais de uma vez,
             * o que confundia o controle de taxa (underrun ~8 s depois).
             * Agora a fatia espera o tempo que ela representa. */
            gb_platform_render_lcd_off_frame();
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
/* [android-port] */
int main(int argc, char* argv[]) {
    /* [fix-v7] Android: pasta de trabalho, manifesto, log e saves.
     *          PC: pasta saves ao lado do executável. */
    android_bridge::prepare_storage();
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
