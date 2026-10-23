/**
 * @file gabarito.cpp
 * @brief Gabarito do renderizador (passo 12a). Veja gabarito.h.
 */
#include "gabarito.h"
#include "gbrt.h"
#include "ppu.h"
#include "ppu_native.h"
#include "hd_pack_io.h"
#include "platform_sdl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
/* Cópias static (como no hd_pack_io.cpp), para não colidir no link. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "third_party/stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace {

/* Quantos quadros (com LCD ligado) rodar depois de carregar; o último é o
 * comparado. Os primeiros refazem o que não está no savestate (cache de
 * atributos, página DX calibrada). */
constexpr int kWarmupFrames = 4;
/* Limite de voltas (LCD desligado não conta quadro): ~10 s de jogo. */
constexpr int kMaxIterations = 600;
constexpr int kHdScale = 4;
constexpr uint32_t kSlice = 70224u;   /* o mesmo do laço principal (main.cpp) */

enum class Pending { None, Capture, Check, Rewrite };
Pending g_pending = Pending::None;
std::string g_dir;
std::string g_status = "Nenhum gabarito conferido ainda.";
struct Result { std::string text; bool ok; };
std::vector<Result> g_results;

struct Frame {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   /* 0xAARRGGBB */
};

struct Shot {
    Frame f1x;
    Frame f4x;                  /* vazio sem tiles HD */
    std::string context;        /* "f07", "m44", "g00" ou "" */
    unsigned game_mode = 0xFF;
    bool lcd_never_on = false;
};

const std::string& dir() {
    if (g_dir.empty()) {
        g_dir = (fs::path(hd_pack_io_mods_dir()) / "gabarito").lexically_normal().string();
    }
    return g_dir;
}

std::string base_name(int n) {
    char b[32];
    snprintf(b, sizeof(b), "gab_%03d", n);
    return b;
}

std::string path_of(int n, const char* suffix) {
    return (fs::path(dir()) / (base_name(n) + suffix)).string();
}

/* Mods que mudam o quadro HD: nome + informação de cada mod ligado. */
std::string mods_signature() {
    if (!ppu_native_hd_get_tiles_enabled() || ppu_native_hd_tile_count() == 0) {
        return "";
    }
    std::string s;
    for (int i = 0; i < hd_pack_io_mod_count(); ++i) {
        if (!hd_pack_io_mod_enabled(i)) continue;
        if (!s.empty()) s += "; ";
        s += hd_pack_io_mod_name(i);
        s += " (";
        s += hd_pack_io_mod_info(i);
        s += ")";
    }
    return s;
}

bool write_png(const std::string& path, const Frame& f) {
    std::vector<unsigned char> rgba((size_t)f.w * f.h * 4u);
    for (size_t i = 0; i < f.px.size(); ++i) {
        const uint32_t c = f.px[i];
        rgba[i * 4 + 0] = (unsigned char)((c >> 16) & 0xFFu);
        rgba[i * 4 + 1] = (unsigned char)((c >> 8) & 0xFFu);
        rgba[i * 4 + 2] = (unsigned char)(c & 0xFFu);
        rgba[i * 4 + 3] = 0xFFu;   /* o quadro é opaco */
    }
    return stbi_write_png(path.c_str(), f.w, f.h, 4, rgba.data(), f.w * 4) != 0;
}

bool read_png(const std::string& path, Frame& f) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!data) return false;
    f.w = w;
    f.h = h;
    f.px.resize((size_t)w * h);
    for (size_t i = 0; i < f.px.size(); ++i) {
        const unsigned char* p = data + i * 4u;
        f.px[i] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    stbi_image_free(data);
    return true;
}

/* Lê um campo de texto/número simples do .json do gabarito. */
std::string json_field(const std::string& text, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos) return "";
    p = text.find(':', p);
    if (p == std::string::npos) return "";
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p < text.size() && text[p] == '"') {
        std::string out;
        for (++p; p < text.size() && text[p] != '"'; ++p) {
            if (text[p] == '\\' && p + 1 < text.size()) ++p;
            out += text[p];
        }
        return out;
    }
    size_t e = p;
    while (e < text.size() && text[e] != ',' && text[e] != '\n' && text[e] != '}') ++e;
    return text.substr(p, e - p);
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

/* Copia a saída do renderizador do último quadro. */
void grab(GBContext* ctx, Shot& shot) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    const bool wide = ppu_get_widescreen_enabled(ppu);
    const uint32_t* fb = wide ? ppu_get_widescreen_framebuffer(ppu) : ppu_get_framebuffer(ppu);
    shot.f1x.w = wide ? GB_WIDESCREEN_WIDTH : GB_SCREEN_WIDTH;
    shot.f1x.h = GB_SCREEN_HEIGHT;
    shot.f1x.px.assign(fb, fb + (size_t)shot.f1x.w * shot.f1x.h);
    for (uint32_t& c : shot.f1x.px) c |= 0xFF000000u;

    shot.f4x = Frame();
    if (mods_signature().empty()) return;
    int n = 0;
    const uint32_t* hd = ppu_native_hd_frame(&n);
    if (!hd || n != kHdScale) return;
    const int cw = GB_SCREEN_WIDTH * n, h = GB_SCREEN_HEIGHT * n;
    int mn = 0;
    const uint32_t* margins = wide ? ppu_native_hd_margin_frame(&mn) : nullptr;
    const bool with_margins = margins && mn == n;
    const int mw = GB_WIDESCREEN_MARGIN * n;
    shot.f4x.w = with_margins ? cw + 2 * mw : cw;
    shot.f4x.h = h;
    shot.f4x.px.assign((size_t)shot.f4x.w * h, 0xFF000000u);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = shot.f4x.px.data() + (size_t)y * shot.f4x.w;
        const int off = with_margins ? mw : 0;
        std::memcpy(row + off, hd + (size_t)y * cw, (size_t)cw * sizeof(uint32_t));
        if (with_margins) {
            const uint32_t* m = margins + (size_t)y * 2 * mw;
            std::memcpy(row, m, (size_t)mw * sizeof(uint32_t));
            std::memcpy(row + mw + cw, m + mw, (size_t)mw * sizeof(uint32_t));
        }
    }
    for (uint32_t& c : shot.f4x.px) c |= 0xFF000000u;
}

/* Carrega o savestate e roda kWarmupFrames quadros do jogo sem botões, como o
 * laço principal do main.cpp. */
bool replay(GBContext* ctx, const std::string& state_path, Shot& shot, std::string& err) {
    if (!gb_context_load_state_file(ctx, state_path.c_str())) {
        err = "savestate nao carregou (versao do savestate mudou?)";
        return false;
    }
    ppu_reset_widescreen_heuristics(ctx);
    int frames = 0;
    for (int it = 0; it < kMaxIterations && frames < kWarmupFrames; ++it) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done && !ctx->stopped) {
            gb_run_cycles(ctx, kSlice);
        }
        if (ctx->frame_done) ++frames;
    }
    if (frames < kWarmupFrames) {
        shot.lcd_never_on = true;
        err = "a tela ficou desligada (LCD) depois de carregar";
        return false;
    }
    grab(ctx, shot);
    char sfx[8];
    ppu_native_hd_context_suffix(ppu_native_hd_context(), sfx);
    shot.context = sfx;
    shot.game_mode = (ctx->eram && ctx->eram_size > 0x08C3u) ? ctx->eram[0x08C3] : 0xFFu;
    return true;
}

struct Diff {
    long count = 0;
    int first_x = -1, first_y = -1;
    bool size_mismatch = false;
};

Diff compare(const Frame& ref, const Frame& got, const std::string& dif_path) {
    Diff d;
    std::error_code ec;
    if (ref.w != got.w || ref.h != got.h) {
        d.size_mismatch = true;
        fs::remove(dif_path, ec);
        return d;
    }
    Frame dif = got;
    for (size_t i = 0; i < ref.px.size(); ++i) {
        const uint32_t c = got.px[i];
        if (((ref.px[i] ^ c) & 0x00FFFFFFu) != 0) {
            if (d.count == 0) {
                d.first_x = (int)(i % (size_t)ref.w);
                d.first_y = (int)(i / (size_t)ref.w);
            }
            d.count++;
            dif.px[i] = 0xFFFF0000u;
        } else {
            const uint32_t g = (((c >> 16) & 0xFFu) + ((c >> 8) & 0xFFu) + (c & 0xFFu)) / 6u + 40u;
            dif.px[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
    }
    if (d.count) {
        write_png(dif_path, dif);
    } else {
        fs::remove(dif_path, ec);
    }
    return d;
}

std::string diff_text(const char* label, const Diff& d) {
    char b[160];
    if (d.size_mismatch) {
        snprintf(b, sizeof(b), "%s: tamanho diferente (widescreen ligado/desligado?)", label);
    } else if (d.count == 0) {
        snprintf(b, sizeof(b), "%s igual", label);
    } else {
        snprintf(b, sizeof(b), "%s: %ld pontos diferentes (primeiro em %d,%d)", label, d.count,
                 d.first_x, d.first_y);
    }
    return b;
}

std::vector<int> list_numbers() {
    std::vector<int> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir(), ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        int n = 0;
        if (std::sscanf(name.c_str(), "gab_%d.state", &n) == 1 &&
            name == base_name(n) + ".state") {
            out.push_back(n);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void write_meta(int n, const Shot& s, const std::string& mods) {
    std::ofstream f(path_of(n, ".json"));
    char when[32] = "";
    const std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << "{\n"
      << "  \"version\": 1,\n"
      << "  \"gravado\": \"" << when << "\",\n"
      << "  \"contexto\": \"" << s.context << "\",\n"
      << "  \"modo_jogo\": " << s.game_mode << ",\n"
      << "  \"renderizador\": \"" << json_escape(ppu_native_mode_name(ppu_native_get_mode())) << "\",\n"
      << "  \"quadros\": " << kWarmupFrames << ",\n"
      << "  \"largura_1x\": " << s.f1x.w << ",\n"
      << "  \"escala_hd\": " << (s.f4x.w ? kHdScale : 0) << ",\n"
      << "  \"mods\": \"" << json_escape(mods) << "\"\n"
      << "}\n";
}

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/* Configurações que mudam o quadro e não estão no savestate: valem as do
 * gabarito durante a repetição e voltam depois. */
struct Saved {
    uint8_t buttons, dpad;
    int hd_scale;
    bool attr_debug;
    bool cpu_hd;   /* passo 14c-2: o gabarito compara o HD da CPU */
};

Saved enter() {
    Saved s{g_joypad_buttons, g_joypad_dpad, ppu_native_get_hd_scale(),
            ppu_get_widescreen_attr_debug(), ppu_native_get_cpu_hd()};
    ppu_native_set_cpu_hd(true);
    g_joypad_buttons = 0xFF;   /* nenhum botão */
    g_joypad_dpad = 0xFF;
    ppu_native_set_hd_scale(kHdScale);
    ppu_set_widescreen_attr_debug(false);
    return s;
}

void leave(const Saved& s) {
    g_joypad_buttons = s.buttons;
    g_joypad_dpad = s.dpad;
    ppu_native_set_hd_scale(s.hd_scale);
    ppu_set_widescreen_attr_debug(s.attr_debug);
    ppu_native_set_cpu_hd(s.cpu_hd);
}

void do_capture(GBContext* ctx, const std::string& live_state) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    int n = 1;
    while (fs::exists(path_of(n, ".state"), ec)) ++n;
    const std::string state = path_of(n, ".state");
    std::error_code cec;
    fs::copy_file(live_state, state, fs::copy_options::overwrite_existing, cec);
    if (cec) {
        g_status = "Gabarito: nao consegui gravar " + state;
        return;
    }
    Shot a, b;
    std::string err;
    if (!replay(ctx, state, a, err)) {
        fs::remove(state, ec);
        g_status = "Gabarito nao gravado: " + err + ".";
        fprintf(stderr, "[Gabarito] %s\n", g_status.c_str());
        return;
    }
    const std::string mods = mods_signature();
    write_png(path_of(n, "_1x.png"), a.f1x);
    if (a.f4x.w) write_png(path_of(n, "_4x.png"), a.f4x);
    write_meta(n, a, mods);
    /* Conferência na hora: a repetição tem que dar o mesmo quadro duas vezes. */
    std::string note;
    if (replay(ctx, state, b, err)) {
        const Diff d1 = compare(a.f1x, b.f1x, path_of(n, "_dif_1x.png"));
        Diff d4;
        if (a.f4x.w) d4 = compare(a.f4x, b.f4x, path_of(n, "_dif_4x.png"));
        if (d1.count || d1.size_mismatch || d4.count || d4.size_mismatch) {
            note = " ATENCAO: repetindo deu outro quadro (nao deterministico); veja os _dif.";
        }
    }
    g_status = "Gabarito " + base_name(n) + " gravado (contexto " +
               (a.context.empty() ? std::string("nenhum") : a.context) +
               (a.f4x.w ? ", 1x e 4x)" : ", 1x; sem tiles HD ligados, sem 4x)") + "." + note;
    fprintf(stderr, "[Gabarito] %s\n", g_status.c_str());
}

void do_check(GBContext* ctx, bool rewrite) {
    g_results.clear();
    const std::vector<int> nums = list_numbers();
    if (nums.empty()) {
        g_status = "Nenhum gabarito em " + dir() + " (grave com F4).";
        return;
    }
    const std::string mods = mods_signature();
    int ok_count = 0;
    for (int n : nums) {
        Result r{base_name(n), false};
        const std::string meta = read_text(path_of(n, ".json"));
        const std::string ctx_name = json_field(meta, "contexto");
        if (!ctx_name.empty()) r.text += " (" + ctx_name + ")";
        Shot s;
        std::string err;
        if (!replay(ctx, path_of(n, ".state"), s, err)) {
            r.text += ": " + err;
            g_results.push_back(r);
            continue;
        }
        if (rewrite) {
            write_png(path_of(n, "_1x.png"), s.f1x);
            std::error_code ec;
            if (s.f4x.w) write_png(path_of(n, "_4x.png"), s.f4x);
            else fs::remove(path_of(n, "_4x.png"), ec);
            fs::remove(path_of(n, "_dif_1x.png"), ec);
            fs::remove(path_of(n, "_dif_4x.png"), ec);
            write_meta(n, s, mods);
            r.text += ": regravado";
            r.ok = true;
            ok_count++;
            g_results.push_back(r);
            continue;
        }
        Frame ref1;
        bool ok = true;
        if (!read_png(path_of(n, "_1x.png"), ref1)) {
            r.text += ": falta " + base_name(n) + "_1x.png";
            g_results.push_back(r);
            continue;
        }
        const Diff d1 = compare(ref1, s.f1x, path_of(n, "_dif_1x.png"));
        r.text += ": " + diff_text("1x", d1);
        ok = d1.count == 0 && !d1.size_mismatch;
        Frame ref4;
        if (read_png(path_of(n, "_4x.png"), ref4)) {
            const std::string ref_mods = json_field(meta, "mods");
            if (ref_mods != mods) {
                r.text += "; 4x pulado (mods ligados diferentes de quando gravou)";
            } else if (!s.f4x.w) {
                r.text += "; 4x pulado (sem quadro HD)";
            } else {
                const Diff d4 = compare(ref4, s.f4x, path_of(n, "_dif_4x.png"));
                r.text += "; " + diff_text("4x", d4);
                ok = ok && d4.count == 0 && !d4.size_mismatch;
            }
        }
        r.ok = ok;
        if (ok) ok_count++;
        g_results.push_back(r);
    }
    char b[160];
    snprintf(b, sizeof(b), "%s: %d de %d %s.", rewrite ? "Regravados" : "Conferidos", ok_count,
             (int)nums.size(), rewrite ? "regravados" : "iguais");
    g_status = b;
    fprintf(stderr, "[Gabarito] %s\n", g_status.c_str());
    for (const Result& r : g_results) {
        fprintf(stderr, "[Gabarito]   %s %s\n", r.ok ? "ok " : "DIF", r.text.c_str());
    }
}

}  // namespace

extern "C" {

void gabarito_request_capture(void) {
    if (g_pending == Pending::None) g_pending = Pending::Capture;
}

void gabarito_request_check(void) {
    if (g_pending == Pending::None) g_pending = Pending::Check;
}

void gabarito_request_rewrite(void) {
    if (g_pending == Pending::None) g_pending = Pending::Rewrite;
}

bool gabarito_busy(void) {
    return g_pending != Pending::None;
}

bool gabarito_process(GBContext* ctx) {
    if (g_pending == Pending::None) return false;
    const Pending what = g_pending;
    g_pending = Pending::None;
    if (!ctx || !ctx->ppu) {
        g_status = "Gabarito: jogo nao esta rodando.";
        return false;
    }
    /* Guarda o jogo como está, faz o trabalho e devolve. */
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const std::string live = (fs::path(dir()) / "atual.tmp").string();
    if (!gb_context_save_state_file(ctx, live.c_str())) {
        g_status = "Gabarito: nao consegui guardar o estado atual do jogo.";
        return false;
    }
    const Saved saved = enter();
    if (what == Pending::Capture) {
        do_capture(ctx, live);
    } else {
        do_check(ctx, what == Pending::Rewrite);
    }
    leave(saved);
    if (!gb_context_load_state_file(ctx, live.c_str())) {
        fprintf(stderr, "[Gabarito] ERRO: nao consegui voltar ao estado do jogo (%s)\n",
                live.c_str());
    }
    ppu_reset_widescreen_heuristics(ctx);
    fs::remove(live, ec);
    return true;
}

const char* gabarito_dir(void) {
    return dir().c_str();
}

const char* gabarito_status(void) {
    return g_status.c_str();
}

int gabarito_result_count(void) {
    return (int)g_results.size();
}

const char* gabarito_result_text(int index) {
    return (index >= 0 && index < (int)g_results.size()) ? g_results[(size_t)index].text.c_str() : "";
}

bool gabarito_result_ok(int index) {
    return index >= 0 && index < (int)g_results.size() && g_results[(size_t)index].ok;
}

}  // extern "C"
