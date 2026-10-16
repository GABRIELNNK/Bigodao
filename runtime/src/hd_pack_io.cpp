/**
 * @file hd_pack_io.cpp
 * @brief Arquivos dos tiles HD (passo 5). Veja hd_pack_io.h.
 */
#include "hd_pack_io.h"
#include "ppu_native.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <system_error>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "third_party/stb_image_write.h"
/* O main.cpp tem a sua propria implementacao do stb_image (JPEG/zlib):
 * esta fica static para nao colidir no link. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

static std::string g_base_dir;
static std::string g_dump_dir;
static bool g_dump_dir_ready = false;
static uint32_t g_dumped_total = 0;
static std::string g_scene_dir;
static std::string g_pack_dir;
static std::string g_pack_status;
static std::string g_last_scene;

void hd_pack_io_set_base_dir(const char* base_dir) {
    g_base_dir = (base_dir && base_dir[0]) ? base_dir : "";
    fs::path dump = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "dump";
    g_dump_dir = dump.lexically_normal().string();
    fs::path scenes = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "scenes";
    g_scene_dir = scenes.lexically_normal().string();
    fs::path pack = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "hd";
    g_pack_dir = pack.lexically_normal().string();
    g_dump_dir_ready = false;
}

const char* hd_pack_io_dump_dir(void) {
    if (g_dump_dir.empty()) {
        hd_pack_io_set_base_dir(NULL);
    }
    return g_dump_dir.c_str();
}

static bool ensure_dump_dir(void) {
    if (g_dump_dir_ready) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(fs::path(hd_pack_io_dump_dir()), ec);
    if (ec) {
        fprintf(stderr, "[HD] Nao consegui criar %s: %s\n", g_dump_dir.c_str(), ec.message().c_str());
        return false;
    }
    g_dump_dir_ready = true;
    return true;
}

int hd_pack_io_flush_dumps(int max) {
    PPUNativeTileDump items[64];
    int written = 0;
    while (max > 0) {
        const int want = max < 64 ? max : 64;
        const int n = ppu_native_take_tile_dumps(items, want);
        if (n <= 0) {
            break;
        }
        max -= n;
        if (!ensure_dump_dir()) {
            continue;   /* descarta: sem pasta não dá para gravar */
        }
        for (int i = 0; i < n; ++i) {
            const PPUNativeTileDump& d = items[i];
            char name[64];
            snprintf(name, sizeof(name), "%c_%016llx.png", d.kind,
                     (unsigned long long)d.tile_hash);
            const fs::path path = fs::path(g_dump_dir) / name;
            std::error_code ec;
            if (fs::exists(path, ec)) {
                continue;
            }
            unsigned char rgba[64 * 4];
            for (int p = 0; p < 64; ++p) {
                const uint32_t c = d.rgba[d.index[p] & 3u];
                rgba[p * 4 + 0] = (unsigned char)((c >> 16) & 0xFFu);
                rgba[p * 4 + 1] = (unsigned char)((c >> 8) & 0xFFu);
                rgba[p * 4 + 2] = (unsigned char)(c & 0xFFu);
                rgba[p * 4 + 3] = (unsigned char)((c >> 24) & 0xFFu);
            }
            if (stbi_write_png(path.string().c_str(), 8, 8, 4, rgba, 8 * 4)) {
                written++;
                g_dumped_total++;
            } else {
                fprintf(stderr, "[HD] Falha ao gravar %s\n", path.string().c_str());
            }
        }
    }
    return written;
}

uint32_t hd_pack_io_dumped_total(void) {
    return g_dumped_total;
}

/* ============================================================================
 * Captura de cena (passo 5a.2)
 * ========================================================================== */

const char* hd_pack_io_scene_dir(void) {
    if (g_scene_dir.empty()) {
        hd_pack_io_set_base_dir(NULL);
    }
    return g_scene_dir.c_str();
}

const char* hd_pack_io_last_scene(void) {
    return g_last_scene.c_str();
}

static bool write_rgba_png(const fs::path& path, const uint32_t* argb, int w, int h, int scale) {
    std::string bytes((size_t)w * scale * h * scale * 4u, '\0');
    unsigned char* out = (unsigned char*)&bytes[0];
    for (int y = 0; y < h * scale; ++y) {
        for (int x = 0; x < w * scale; ++x) {
            const uint32_t c = argb[(size_t)(y / scale) * w + (size_t)(x / scale)];
            unsigned char* d = out + ((size_t)y * w * scale + (size_t)x) * 4u;
            d[0] = (unsigned char)((c >> 16) & 0xFFu);
            d[1] = (unsigned char)((c >> 8) & 0xFFu);
            d[2] = (unsigned char)(c & 0xFFu);
            d[3] = (unsigned char)((c >> 24) & 0xFFu);
        }
    }
    return stbi_write_png(path.string().c_str(), w * scale, h * scale, 4, out, w * scale * 4) != 0;
}

static void write_int_array(FILE* f, const char* name, const uint16_t* tile, int count, bool last) {
    fprintf(f, "  \"%s\": [", name);
    for (int i = 0; i < count; ++i) {
        const int v = (tile[i] == PPU_NATIVE_SCENE_NO_TILE) ? -1 : (int)tile[i];
        fprintf(f, "%s%d", i ? "," : "", v);
    }
    fprintf(f, "]%s\n", last ? "" : ",");
}

static void write_byte_array(FILE* f, const char* name, const uint8_t* v, int count, bool last) {
    fprintf(f, "  \"%s\": [", name);
    for (int i = 0; i < count; ++i) {
        fprintf(f, "%s%u", i ? "," : "", (unsigned)v[i]);
    }
    fprintf(f, "]%s\n", last ? "" : ",");
}

int hd_pack_io_write_scene(const PPUNativeScene* scene) {
    if (!scene) {
        return -1;
    }
    std::error_code ec;
    const fs::path dir(hd_pack_io_scene_dir());
    fs::create_directories(dir, ec);
    if (ec) {
        fprintf(stderr, "[HD] Nao consegui criar %s: %s\n", g_scene_dir.c_str(), ec.message().c_str());
        return -1;
    }
    int number = 1;
    char base[32];
    for (;; ++number) {
        snprintf(base, sizeof(base), "scene_%03d", number);
        if (!fs::exists(dir / (std::string(base) + ".json"), ec)) {
            break;
        }
    }
    const int w = GB_SCREEN_WIDTH;
    const int h = GB_SCREEN_HEIGHT;
    bool ok = true;
    ok = write_rgba_png(dir / (std::string(base) + "_bg.png"), scene->bg_rgba, w, h, 1) && ok;
    ok = write_rgba_png(dir / (std::string(base) + "_obj.png"), scene->obj_rgba, w, h, 1) && ok;
    ok = write_rgba_png(dir / (std::string(base) + "_bg_4x.png"), scene->bg_rgba, w, h, 4) && ok;
    ok = write_rgba_png(dir / (std::string(base) + "_obj_4x.png"), scene->obj_rgba, w, h, 4) && ok;

    const fs::path json_path = dir / (std::string(base) + ".json");
    FILE* f = fopen(json_path.string().c_str(), "w");
    if (!f) {
        fprintf(stderr, "[HD] Falha ao gravar %s\n", json_path.string().c_str());
        return -1;
    }
    fprintf(f, "{\n  \"version\": 1,\n  \"width\": %d,\n  \"height\": %d,\n  \"tiles\": [\n", w, h);
    for (uint32_t t = 0; t < scene->tile_count; ++t) {
        const PPUNativeTileDump& d = scene->tiles[t];
        char index[65];
        for (int i = 0; i < 64; ++i) {
            index[i] = (char)('0' + (d.index[i] & 3u));
        }
        index[64] = '\0';
        fprintf(f, "    {\"name\": \"%c_%016llx\", \"kind\": \"%c\", \"palette\": [%u, %u, %u, %u], \"index\": \"%s\"}%s\n",
                d.kind, (unsigned long long)d.tile_hash, d.kind,
                (unsigned)d.rgba[0], (unsigned)d.rgba[1], (unsigned)d.rgba[2], (unsigned)d.rgba[3],
                index, (t + 1 < scene->tile_count) ? "," : "");
    }
    fprintf(f, "  ],\n");
    write_int_array(f, "bg_tile", scene->bg_tile, w * h, false);
    write_byte_array(f, "bg_uvf", scene->bg_uvf, w * h, false);
    write_int_array(f, "obj_tile", scene->obj_tile, w * h, false);
    write_byte_array(f, "obj_uvf", scene->obj_uvf, w * h, true);
    fprintf(f, "}\n");
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        fprintf(stderr, "[HD] Cena %s gravada com erros\n", base);
    }
    g_last_scene = (dir / base).string();
    fprintf(stderr, "[HD] Cena gravada: %s (%u tiles)\n", g_last_scene.c_str(), (unsigned)scene->tile_count);
    return number;
}

/* ============================================================================
 * Pacote HD (passo 5b): mods/hd/b_<tile>.png, o_<tile>.png e pack.json
 * ========================================================================== */

const char* hd_pack_io_pack_dir(void) {
    if (g_pack_dir.empty()) {
        hd_pack_io_set_base_dir(NULL);
    }
    return g_pack_dir.c_str();
}

const char* hd_pack_io_pack_status(void) {
    return g_pack_status.c_str();
}

/* Nome no formato b_<16 hex> ou o_<16 hex>. */
static bool parse_tile_name(const std::string& name, char* kind, uint64_t* hash) {
    if (name.size() != 18 || (name[0] != 'b' && name[0] != 'o') || name[1] != '_') {
        return false;
    }
    uint64_t h = 0;
    for (size_t i = 2; i < 18; ++i) {
        const char c = (char)std::tolower((unsigned char)name[i]);
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        h = (h << 4) | (uint64_t)v;
    }
    *kind = name[0];
    *hash = h;
    return true;
}

/* Lê as paletas de referência do pack.json gravado pelo hd_slice.py:
 *   "b_0123...": { "palette": [n0, n1, n2, n3], ... }
 * Leitor simples: acha cada nome de tile e o primeiro "palette" depois dele. */
static std::unordered_map<std::string, std::vector<uint32_t>> read_pack_palettes(const fs::path& path) {
    std::unordered_map<std::string, std::vector<uint32_t>> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        const size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        const std::string key = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        char kind;
        uint64_t hash;
        if (!parse_tile_name(key, &kind, &hash)) continue;
        const size_t pal = text.find("\"palette\"", pos);
        const size_t next_tile = text.find("\"b_", pos) < text.find("\"o_", pos)
            ? text.find("\"b_", pos) : text.find("\"o_", pos);
        if (pal == std::string::npos || (next_tile != std::string::npos && pal > next_tile)) continue;
        size_t p = text.find('[', pal);
        const size_t close = (p == std::string::npos) ? p : text.find(']', p);
        if (p == std::string::npos || close == std::string::npos) continue;
        std::vector<uint32_t> values;
        ++p;
        while (p < close && values.size() < 4) {
            while (p < close && !std::isdigit((unsigned char)text[p])) ++p;
            if (p >= close) break;
            char* stop = nullptr;
            const unsigned long long v = std::strtoull(text.c_str() + p, &stop, 10);
            values.push_back((uint32_t)v);
            p = (size_t)(stop - text.c_str());
        }
        if (values.size() == 4) {
            out[key] = values;
        }
        pos = close;
    }
    return out;
}

int hd_pack_io_load_pack(void) {
    ppu_native_hd_clear_tiles();
    const fs::path dir(hd_pack_io_pack_dir());
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        g_pack_status = "sem pasta " + dir.string();
        return 0;
    }
    const auto palettes = read_pack_palettes(dir / "pack.json");
    int loaded = 0, failed = 0, no_ref = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".png") continue;
        const std::string stem = entry.path().stem().string();
        char kind;
        uint64_t hash;
        if (!parse_tile_name(stem, &kind, &hash)) continue;
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(entry.path().string().c_str(), &w, &h, &comp, 4);
        if (!data || w != h || w < 8 || (w % 8) != 0) {
            fprintf(stderr, "[HD] Ignorado %s (precisa ser quadrado, lado multiplo de 8)\n",
                    entry.path().filename().string().c_str());
            if (data) stbi_image_free(data);
            failed++;
            continue;
        }
        std::vector<uint32_t> argb((size_t)w * h);
        for (size_t i = 0; i < argb.size(); ++i) {
            const unsigned char* p = data + i * 4u;
            argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
        stbi_image_free(data);
        const auto it = palettes.find(stem);
        const uint32_t* ref = (it != palettes.end()) ? it->second.data() : nullptr;
        if (!ref) no_ref++;
        if (ppu_native_hd_add_tile(kind, hash, w, argb.data(), ref)) {
            loaded++;
        } else {
            failed++;
        }
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%d tiles HD carregados%s%s", loaded,
             failed ? ", alguns ignorados (veja o terminal)" : "",
             no_ref ? ", alguns sem paleta de referencia (sem recolorir)" : "");
    g_pack_status = buf;
    fprintf(stderr, "[HD] %s de %s\n", buf, dir.string().c_str());
    return loaded;
}
