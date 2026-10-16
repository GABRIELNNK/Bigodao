/**
 * @file hd_pack_io.cpp
 * @brief Arquivos dos tiles HD (passo 5). Veja hd_pack_io.h.
 */
#include "hd_pack_io.h"
#include "ppu_native.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

static std::string g_base_dir;
static std::string g_dump_dir;
static bool g_dump_dir_ready = false;
static uint32_t g_dumped_total = 0;
static std::string g_scene_dir;
static std::string g_last_scene;

void hd_pack_io_set_base_dir(const char* base_dir) {
    g_base_dir = (base_dir && base_dir[0]) ? base_dir : "";
    fs::path dump = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "dump";
    g_dump_dir = dump.lexically_normal().string();
    fs::path scenes = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "scenes";
    g_scene_dir = scenes.lexically_normal().string();
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
