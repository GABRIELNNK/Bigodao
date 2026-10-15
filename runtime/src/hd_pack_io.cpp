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

void hd_pack_io_set_base_dir(const char* base_dir) {
    g_base_dir = (base_dir && base_dir[0]) ? base_dir : "";
    fs::path dump = fs::path(g_base_dir.empty() ? "." : g_base_dir) / "mods" / "dump";
    g_dump_dir = dump.lexically_normal().string();
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
            snprintf(name, sizeof(name), "%c_%016llx_%08x.png", d.kind,
                     (unsigned long long)d.tile_hash, (unsigned)d.pal_hash);
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
