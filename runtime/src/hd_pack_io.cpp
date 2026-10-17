/**
 * @file hd_pack_io.cpp
 * @brief Arquivos dos tiles HD (passo 5). Veja hd_pack_io.h.
 */
#include "hd_pack_io.h"
#include "ppu_native.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set> /* passo 7a */
#include <array> /* passo 7b */
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
    /* Passo 5d: 256 de largura quando a captura incluiu as laterais. */
    const int w = scene->width ? scene->width : GB_SCREEN_WIDTH;
    const int h = scene->height ? scene->height : GB_SCREEN_HEIGHT;
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
 * Mods HD (passo 5c)
 *
 * Fontes, carregadas nesta ordem (a de baixo sobrepõe a de cima quando o
 * mesmo tile aparece em mais de uma):
 *   1. <base>/mods/<nome>.o2r em ordem alfabética (ZIP com outra extensão);
 *   2. a pasta <base>/mods/hd, se existir (útil enquanto pinta).
 * Em cada fonte valem os arquivos b_<16 hex>.png / o_<16 hex>.png em qualquer
 * subpasta e os pack.json (paleta de referência de cada tile).
 * ========================================================================== */

struct HdModSource {
    std::string name;      /* "meu_mod.o2r" ou "hd/" */
    fs::path path;
    bool is_folder = false;
    bool enabled = true;
    int tiles = 0;
    std::string info;
};

static std::vector<HdModSource> g_mods;
static uint32_t g_mods_generation = 0;   /* passo 6c */
static std::vector<std::string> g_disabled_names;   /* vem das preferências */

const char* hd_pack_io_pack_dir(void) {
    if (g_pack_dir.empty()) {
        hd_pack_io_set_base_dir(NULL);
    }
    return g_pack_dir.c_str();
}

const char* hd_pack_io_mods_dir(void) {
    static std::string dir;
    dir = fs::path(hd_pack_io_pack_dir()).parent_path().string();
    return dir.c_str();
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

typedef std::unordered_map<std::string, std::vector<uint32_t>> PaletteMap;

/* Lê as paletas de referência de um pack.json gravado pelo hd_slice.py:
 *   "b_0123...": { "palette": [n0, n1, n2, n3], ... }
 * Leitor simples: acha cada nome de tile e o primeiro "palette" depois dele. */
static void read_pack_palettes(const std::string& text, PaletteMap& out) {
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
        const size_t nb = text.find("\"b_", pos);
        const size_t no = text.find("\"o_", pos);
        const size_t next_tile = nb < no ? nb : no;
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
}

static bool read_whole_file(const fs::path& path, std::vector<uint8_t>& out, size_t max_bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0 || (uint64_t)size > max_bytes) return false;
    out.resize((size_t)size);
    in.seekg(0);
    return size == 0 || (bool)in.read((char*)out.data(), size);
}

/* --------------------------------------------------------------------------
 * Leitor de ZIP mínimo: entradas guardadas (método 0) ou Deflate (método 8).
 * Sem ZIP64 e sem criptografia (não são necessários para um pacote de tiles).
 * ------------------------------------------------------------------------ */
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t csize = 0;
    uint32_t usize = 0;
    uint32_t local_offset = 0;
};

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool zip_list(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out, std::string& err) {
    const size_t n = z.size();
    if (n < 22) { err = "arquivo pequeno demais para ser ZIP"; return false; }
    size_t eocd = std::string::npos;
    const size_t stop = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22 + 1; i-- > stop;) {
        if (rd32(&z[i]) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { err = "nao e um ZIP (fim do diretorio nao encontrado)"; return false; }
    const uint16_t count = rd16(&z[eocd + 10]);
    const uint32_t cd_size = rd32(&z[eocd + 12]);
    const uint32_t cd_off = rd32(&z[eocd + 16]);
    if (count == 0xFFFFu || cd_off == 0xFFFFFFFFu) { err = "ZIP64 nao suportado"; return false; }
    if ((uint64_t)cd_off + cd_size > n) { err = "diretorio do ZIP corrompido"; return false; }
    size_t p = cd_off;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > n || rd32(&z[p]) != 0x02014b50u) { err = "diretorio do ZIP corrompido"; return false; }
        ZipEntry e;
        e.flags = rd16(&z[p + 8]);
        e.method = rd16(&z[p + 10]);
        e.csize = rd32(&z[p + 20]);
        e.usize = rd32(&z[p + 24]);
        const uint16_t name_len = rd16(&z[p + 28]);
        const uint16_t extra_len = rd16(&z[p + 30]);
        const uint16_t comment_len = rd16(&z[p + 32]);
        e.local_offset = rd32(&z[p + 42]);
        if (p + 46 + name_len > n) { err = "diretorio do ZIP corrompido"; return false; }
        e.name.assign((const char*)&z[p + 46], name_len);
        p += 46u + name_len + extra_len + comment_len;
        out.push_back(e);
    }
    return true;
}

static bool zip_extract(const std::vector<uint8_t>& z, const ZipEntry& e, std::vector<uint8_t>& out) {
    if ((e.flags & 1u) != 0) return false;                       /* criptografado */
    if (e.csize == 0xFFFFFFFFu || e.usize == 0xFFFFFFFFu) return false;
    if (e.usize > (64u << 20)) return false;                     /* 64 MB por arquivo */
    const size_t lh = e.local_offset;
    if ((uint64_t)lh + 30 > z.size() || rd32(&z[lh]) != 0x04034b50u) return false;
    const size_t data = lh + 30u + rd16(&z[lh + 26]) + rd16(&z[lh + 28]);
    if ((uint64_t)data + e.csize > z.size()) return false;
    out.resize(e.usize);
    if (e.method == 0) {
        if (e.csize != e.usize) return false;
        if (e.usize) memcpy(out.data(), &z[data], e.usize);
        return true;
    }
    if (e.method == 8) {
        if (e.usize == 0) return true;
        const int got = stbi_zlib_decode_noheader_buffer(
            (char*)out.data(), (int)e.usize, (const char*)&z[data], (int)e.csize);
        return got == (int)e.usize;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Carregamento
 * ------------------------------------------------------------------------ */
struct PendingTile {
    std::string stem;
    std::vector<uint8_t> png;
};

static std::string lower_ascii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Nome do arquivo dentro do caminho (aceita / e \). */
static std::string leaf_of(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

/* Junta os PNGs de tile e os pack.json de uma fonte. */
static bool collect_source(const HdModSource& src, std::vector<PendingTile>& tiles,
                           PaletteMap& palettes, int& skipped, std::string& err) {
    std::error_code ec;
    if (src.is_folder) {
        for (auto it = fs::recursive_directory_iterator(src.path, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string leaf = lower_ascii(it->path().filename().string());
            if (leaf == "pack.json") {
                std::vector<uint8_t> bytes;
                if (read_whole_file(it->path(), bytes, 64u << 20)) {
                    read_pack_palettes(std::string(bytes.begin(), bytes.end()), palettes);
                }
                continue;
            }
            if (leaf.size() < 5 || leaf.compare(leaf.size() - 4, 4, ".png") != 0) continue;
            char kind;
            uint64_t hash;
            const std::string stem = leaf.substr(0, leaf.size() - 4);
            if (!parse_tile_name(stem, &kind, &hash)) continue;
            PendingTile t;
            t.stem = stem;
            if (!read_whole_file(it->path(), t.png, 64u << 20)) { skipped++; continue; }
            tiles.push_back(std::move(t));
        }
        return true;
    }

    std::vector<uint8_t> zip;
    if (!read_whole_file(src.path, zip, 1024u << 20)) {
        err = "nao consegui ler o arquivo";
        return false;
    }
    std::vector<ZipEntry> entries;
    if (!zip_list(zip, entries, err)) {
        return false;
    }
    for (const ZipEntry& e : entries) {
        if (e.name.empty() || e.name.back() == '/') continue;    /* pasta */
        const std::string leaf = lower_ascii(leaf_of(e.name));
        const bool is_pack = leaf == "pack.json";
        const bool is_png = leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".png") == 0;
        if (!is_pack && !is_png) continue;
        std::string stem;
        if (is_png) {
            char kind;
            uint64_t hash;
            stem = leaf.substr(0, leaf.size() - 4);
            if (!parse_tile_name(stem, &kind, &hash)) continue;
        }
        std::vector<uint8_t> bytes;
        if (!zip_extract(zip, e, bytes)) {
            fprintf(stderr, "[HD] %s: nao consegui extrair %s\n", src.name.c_str(), e.name.c_str());
            skipped++;
            continue;
        }
        if (is_pack) {
            read_pack_palettes(std::string(bytes.begin(), bytes.end()), palettes);
        } else {
            PendingTile t;
            t.stem = stem;
            t.png = std::move(bytes);
            tiles.push_back(std::move(t));
        }
    }
    return true;
}

static int load_source(HdModSource& src, int& no_ref_total) {
    std::vector<PendingTile> tiles;
    PaletteMap palettes;
    int skipped = 0;
    std::string err;
    if (!collect_source(src, tiles, palettes, skipped, err)) {
        src.tiles = 0;
        src.info = "erro: " + err;
        fprintf(stderr, "[HD] %s: %s\n", src.name.c_str(), err.c_str());
        return 0;
    }
    int loaded = 0, no_ref = 0;
    for (const PendingTile& t : tiles) {
        char kind;
        uint64_t hash;
        parse_tile_name(t.stem, &kind, &hash);
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load_from_memory(t.png.data(), (int)t.png.size(), &w, &h, &comp, 4);
        if (!data || w != h || w < 8 || (w % 8) != 0) {
            fprintf(stderr, "[HD] %s: ignorado %s.png (precisa ser PNG quadrado, lado multiplo de 8)\n",
                    src.name.c_str(), t.stem.c_str());
            if (data) stbi_image_free(data);
            skipped++;
            continue;
        }
        std::vector<uint32_t> argb((size_t)w * h);
        for (size_t i = 0; i < argb.size(); ++i) {
            const unsigned char* p = data + i * 4u;
            argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
        stbi_image_free(data);
        const auto it = palettes.find(t.stem);
        const uint32_t* ref = (it != palettes.end()) ? it->second.data() : nullptr;
        if (!ref) no_ref++;
        if (ppu_native_hd_add_tile(kind, hash, w, argb.data(), ref)) {
            loaded++;
        } else {
            skipped++;
        }
    }
    src.tiles = loaded;
    char buf[160];
    snprintf(buf, sizeof(buf), "%d tiles%s%s", loaded,
             skipped ? ", alguns ignorados (veja o terminal)" : "",
             no_ref ? ", alguns sem paleta de referencia" : "");
    src.info = buf;
    no_ref_total += no_ref;
    return loaded;
}

static bool name_disabled(const std::string& name) {
    for (const std::string& d : g_disabled_names) {
        if (d == name) return true;
    }
    return false;
}

/* Lista as fontes de mods (não carrega nada). */
static void scan_mods(void) {
    g_mods.clear();
    const fs::path mods_dir = fs::path(hd_pack_io_pack_dir()).parent_path();
    std::error_code ec;
    if (fs::is_directory(mods_dir, ec)) {
        std::vector<HdModSource> packs;
        for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (lower_ascii(entry.path().extension().string()) != ".o2r") continue;
            HdModSource s;
            s.name = entry.path().filename().string();
            s.path = entry.path();
            packs.push_back(s);
        }
        std::sort(packs.begin(), packs.end(), [](const HdModSource& a, const HdModSource& b) {
            return lower_ascii(a.name) < lower_ascii(b.name);
        });
        for (HdModSource& s : packs) g_mods.push_back(s);
    }
    if (fs::is_directory(fs::path(hd_pack_io_pack_dir()), ec)) {
        HdModSource s;
        s.name = "hd/";
        s.path = fs::path(hd_pack_io_pack_dir());
        s.is_folder = true;
        g_mods.push_back(s);
    }
    /* Passo 6d: pasta solta de trilhas, mods/music (por ultimo: sobrepoe). */
    if (fs::is_directory(mods_dir / "music", ec)) {
        HdModSource s;
        s.name = "music/";
        s.path = mods_dir / "music";
        s.is_folder = true;
        g_mods.push_back(s);
    }
    for (HdModSource& s : g_mods) {
        s.enabled = !name_disabled(s.name);
        s.info = s.enabled ? "" : "desligado";
    }
}

/* Carrega as fontes ligadas, na ordem da lista. */
static int load_enabled_mods(void) {
    ppu_native_hd_clear_tiles();
    g_mods_generation++; /* passo 6c */
    int loaded = 0, active = 0, no_ref = 0;
    for (HdModSource& s : g_mods) {
        if (!s.enabled) {
            s.tiles = 0;
            s.info = "desligado";
            continue;
        }
        active++;
        loaded += load_source(s, no_ref);
    }
    char buf[200];
    if (g_mods.empty()) {
        snprintf(buf, sizeof(buf), "nenhum mod em %s", hd_pack_io_mods_dir());
    } else {
        snprintf(buf, sizeof(buf), "%u tiles HD de %d mod(s) ligado(s)%s",
                 (unsigned)ppu_native_hd_tile_count(), active,
                 (loaded > (int)ppu_native_hd_tile_count()) ? " (alguns sobrepostos)" : "");
    }
    g_pack_status = buf;
    fprintf(stderr, "[HD] %s\n", buf);
    return (int)ppu_native_hd_tile_count();
}

int hd_pack_io_load_pack(void) {
    scan_mods();
    return load_enabled_mods();
}

int hd_pack_io_mod_count(void) {
    return (int)g_mods.size();
}

const char* hd_pack_io_mod_name(int index) {
    return (index >= 0 && index < (int)g_mods.size()) ? g_mods[(size_t)index].name.c_str() : "";
}

const char* hd_pack_io_mod_info(int index) {
    return (index >= 0 && index < (int)g_mods.size()) ? g_mods[(size_t)index].info.c_str() : "";
}

bool hd_pack_io_mod_is_folder(int index) {
    return index >= 0 && index < (int)g_mods.size() && g_mods[(size_t)index].is_folder;
}

bool hd_pack_io_mod_enabled(int index) {
    return index >= 0 && index < (int)g_mods.size() && g_mods[(size_t)index].enabled;
}

void hd_pack_io_set_mod_enabled(int index, bool enabled) {
    if (index < 0 || index >= (int)g_mods.size()) return;
    HdModSource& s = g_mods[(size_t)index];
    s.enabled = enabled;
    std::vector<std::string> kept;
    for (const std::string& d : g_disabled_names) {
        if (d != s.name) kept.push_back(d);
    }
    if (!enabled) kept.push_back(s.name);
    g_disabled_names.swap(kept);
    load_enabled_mods();   /* recarrega sem reler a pasta */
}

void hd_pack_io_clear_disabled_names(void) {
    g_disabled_names.clear();
}

void hd_pack_io_add_disabled_name(const char* name) {
    if (name && name[0] && !name_disabled(name)) {
        g_disabled_names.push_back(name);
    }
}

int hd_pack_io_disabled_count(void) {
    return (int)g_disabled_names.size();
}

const char* hd_pack_io_disabled_name(int index) {
    return (index >= 0 && index < (int)g_disabled_names.size())
        ? g_disabled_names[(size_t)index].c_str() : "";
}

/* ============================================================================
 * Passo 6c: arquivos avulsos dos mods (trilhas .ogg)
 *
 * Aqui o .o2r nao e lido inteiro: so o diretorio do ZIP e, na hora de tocar,
 * o pedaco do arquivo pedido (as trilhas podem somar centenas de MB).
 * ========================================================================== */
static bool zip_list_file(const fs::path& path, std::vector<ZipEntry>& out, std::string& err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { err = "nao consegui abrir"; return false; }
    const std::streamoff size = in.tellg();
    if (size < 22) { err = "arquivo pequeno demais para ser ZIP"; return false; }
    const std::streamoff tail_len = std::min<std::streamoff>(size, 22 + 65535);
    std::vector<uint8_t> tail((size_t)tail_len);
    in.seekg(size - tail_len);
    if (!in.read((char*)tail.data(), tail_len)) { err = "erro de leitura"; return false; }
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (rd32(&tail[i]) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { err = "nao e um ZIP (fim do diretorio nao encontrado)"; return false; }
    const uint16_t count = rd16(&tail[eocd + 10]);
    const uint32_t cd_size = rd32(&tail[eocd + 12]);
    const uint32_t cd_off = rd32(&tail[eocd + 16]);
    if (count == 0xFFFFu || cd_off == 0xFFFFFFFFu) { err = "ZIP64 nao suportado"; return false; }
    if ((uint64_t)cd_off + cd_size > (uint64_t)size) { err = "diretorio do ZIP corrompido"; return false; }
    std::vector<uint8_t> cd(cd_size);
    in.seekg((std::streamoff)cd_off);
    if (cd_size && !in.read((char*)cd.data(), cd_size)) { err = "erro de leitura"; return false; }
    size_t p = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50u) { err = "diretorio do ZIP corrompido"; return false; }
        ZipEntry e;
        e.flags = rd16(&cd[p + 8]);
        e.method = rd16(&cd[p + 10]);
        e.csize = rd32(&cd[p + 20]);
        e.usize = rd32(&cd[p + 24]);
        const uint16_t name_len = rd16(&cd[p + 28]);
        const uint16_t extra_len = rd16(&cd[p + 30]);
        const uint16_t comment_len = rd16(&cd[p + 32]);
        e.local_offset = rd32(&cd[p + 42]);
        if (p + 46 + name_len > cd.size()) { err = "diretorio do ZIP corrompido"; return false; }
        e.name.assign((const char*)&cd[p + 46], name_len);
        p += 46u + name_len + extra_len + comment_len;
        out.push_back(e);
    }
    return true;
}

static bool zip_read_entry_file(const fs::path& path, const ZipEntry& e, std::vector<uint8_t>& out) {
    if ((e.flags & 1u) != 0) return false;                       /* criptografado */
    if (e.csize == 0xFFFFFFFFu || e.usize == 0xFFFFFFFFu) return false;
    if (e.usize > (256u << 20)) return false;                    /* 256 MB por arquivo */
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint8_t lh[30];
    in.seekg((std::streamoff)e.local_offset);
    if (!in.read((char*)lh, 30) || rd32(lh) != 0x04034b50u) return false;
    const std::streamoff data = (std::streamoff)e.local_offset + 30 + rd16(&lh[26]) + rd16(&lh[28]);
    std::vector<uint8_t> comp(e.csize);
    in.seekg(data);
    if (e.csize && !in.read((char*)comp.data(), e.csize)) return false;
    if (e.method == 0) {
        if (e.csize != e.usize) return false;
        out.swap(comp);
        return true;
    }
    if (e.method == 8) {
        out.resize(e.usize);
        if (e.usize == 0) return true;
        const int got = stbi_zlib_decode_noheader_buffer(
            (char*)out.data(), (int)e.usize, (const char*)comp.data(), (int)e.csize);
        return got == (int)e.usize;
    }
    return false;
}

void hd_pack_io_list_files(hd_pack_io_file_visitor visit, void* user) {
    if (!visit) return;
    for (size_t i = 0; i < g_mods.size(); ++i) {
        const HdModSource& s = g_mods[i];
        if (!s.enabled) continue;
        std::error_code ec;
        if (s.is_folder) {
            for (auto it = fs::recursive_directory_iterator(s.path, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                /* Passo 6d: relativo a pasta mods (inclui "hd/" ou "music/"). */
                const std::string rel = fs::relative(it->path(), s.path.parent_path(), ec).generic_string();
                if (ec || rel.empty()) { ec.clear(); continue; }
                visit((int)i, s.name.c_str(), rel.c_str(), user);
            }
            continue;
        }
        std::vector<ZipEntry> entries;
        std::string err;
        if (!zip_list_file(s.path, entries, err)) continue;
        for (const ZipEntry& e : entries) {
            if (e.name.empty() || e.name.back() == '/') continue;
            visit((int)i, s.name.c_str(), e.name.c_str(), user);
        }
    }
}

uint8_t* hd_pack_io_read_file(int source_index, const char* path, size_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path || source_index < 0 || source_index >= (int)g_mods.size()) return NULL;
    const HdModSource& s = g_mods[(size_t)source_index];
    std::vector<uint8_t> bytes;
    if (s.is_folder) {
        if (!read_whole_file(s.path.parent_path() / fs::path(path), bytes, 256u << 20)) return NULL;
    } else {
        std::vector<ZipEntry> entries;
        std::string err;
        if (!zip_list_file(s.path, entries, err)) return NULL;
        bool ok = false;
        for (const ZipEntry& e : entries) {
            if (e.name == path) { ok = zip_read_entry_file(s.path, e, bytes); break; }
        }
        if (!ok) return NULL;
    }
    uint8_t* out = (uint8_t*)malloc(bytes.empty() ? 1 : bytes.size());
    if (!out) return NULL;
    if (!bytes.empty()) memcpy(out, bytes.data(), bytes.size());
    if (out_size) *out_size = bytes.size();
    return out;
}

uint32_t hd_pack_io_generation(void) {
    return g_mods_generation;
}

/* ============================================================================
 * Passo 7a: gravador de poses de sprites
 *
 * Enquanto grava, a plataforma entrega uma cena por frame. Em cada uma, os
 * pedaços de sprite que se encostam (caixas 8x8/8x16, 8 vizinhos) viram uma
 * pose. Cada pose nova (pelos tiles e pela posição de cada um; espelhada
 * conta como a mesma) é guardada uma vez. Ao parar, as poses são montadas
 * numa folha gravada no formato de uma cena (mods/scenes/sprites_NNN.json +
 * _obj.png + _obj_4x.png), para pintar e fatiar com o mesmo hd_slice.py.
 * ========================================================================== */
namespace {

struct RecPose {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> rgba;
    std::vector<uint16_t> tile;   /* índice em g_rec_tiles, ou NO_TILE */
    std::vector<uint8_t> uvf;
};

const size_t kRecMaxPoses = 2000;
const int kRecSheetWidth = 320;
const int kRecGap = 4;

bool g_rec_active = false;
uint32_t g_rec_frames = 0;
std::vector<RecPose> g_rec_poses;
std::unordered_set<uint64_t> g_rec_sigs;
std::vector<PPUNativeTileDump> g_rec_tiles;
std::unordered_map<uint64_t, uint16_t> g_rec_tile_map;   /* tile_hash -> índice */
bool g_rec_rect_on = false;
int g_rec_rx = 40, g_rec_ry = 24, g_rec_rw = 80, g_rec_rh = 96;
std::string g_rec_last;
std::vector<uint8_t> g_rec_seen;    /* rascunho da busca */
std::vector<int> g_rec_stack;

inline uint64_t fnv_step(uint64_t h, uint64_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (i * 16)) & 0xFFFFu;
        h *= 1099511628211ull;
    }
    return h;
}

/* uvf da mesma posição quando a pose inteira é espelhada na horizontal. */
inline uint8_t mirror_uvf(uint8_t uvf) {
    const uint8_t u = uvf & 7u;
    return (uint8_t)((uvf & 0xB8u) | (uint8_t)(7u - u) | ((uvf & 0x40u) ? 0u : 0x40u));
}

uint16_t rec_tile_index(const PPUNativeTileDump& t) {
    auto it = g_rec_tile_map.find(t.tile_hash);
    if (it != g_rec_tile_map.end()) return it->second;
    if (g_rec_tiles.size() >= PPU_NATIVE_SCENE_MAX_TILES) return PPU_NATIVE_SCENE_NO_TILE;
    const uint16_t id = (uint16_t)g_rec_tiles.size();
    g_rec_tiles.push_back(t);
    g_rec_tile_map.emplace(t.tile_hash, id);
    return id;
}

}  // namespace

void hd_pack_io_sprite_rec_start(void) {
    g_rec_active = true;
    g_rec_frames = 0;
    g_rec_poses.clear();
    g_rec_sigs.clear();
    g_rec_tiles.clear();
    g_rec_tile_map.clear();
}

bool hd_pack_io_sprite_rec_active(void) {
    return g_rec_active;
}

uint32_t hd_pack_io_sprite_rec_pose_count(void) {
    return (uint32_t)g_rec_poses.size();
}

uint32_t hd_pack_io_sprite_rec_frames(void) {
    return g_rec_frames;
}

const char* hd_pack_io_sprite_rec_last(void) {
    return g_rec_last.c_str();
}

void hd_pack_io_sprite_rec_set_rect(bool enabled, int x, int y, int w, int h) {
    g_rec_rect_on = enabled;
    g_rec_rx = x;
    g_rec_ry = y;
    g_rec_rw = w;
    g_rec_rh = h;
}

void hd_pack_io_sprite_rec_feed(const PPUNativeScene* scene) {
    if (!g_rec_active || !scene) return;
    g_rec_frames++;
    const int w = scene->width ? scene->width : GB_SCREEN_WIDTH;
    const int h = scene->height ? scene->height : GB_SCREEN_HEIGHT;
    const int off = (w > GB_SCREEN_WIDTH) ? (w - GB_SCREEN_WIDTH) / 2 : 0;   /* laterais */
    const size_t count = (size_t)w * (size_t)h;
    g_rec_seen.assign(count, 0);
    std::vector<int> pixels;
    for (size_t start = 0; start < count; ++start) {
        if (g_rec_seen[start] || scene->obj_tile[start] == PPU_NATIVE_SCENE_NO_TILE) continue;
        /* Junta os pixels de sprite encostados (8 vizinhos). */
        pixels.clear();
        g_rec_stack.clear();
        g_rec_stack.push_back((int)start);
        g_rec_seen[start] = 1;
        int minx = w, miny = h, maxx = -1, maxy = -1;
        bool visible = false;
        while (!g_rec_stack.empty()) {
            const int p = g_rec_stack.back();
            g_rec_stack.pop_back();
            pixels.push_back(p);
            const int px = p % w, py = p / w;
            minx = std::min(minx, px); maxx = std::max(maxx, px);
            miny = std::min(miny, py); maxy = std::max(maxy, py);
            if ((scene->obj_rgba[p] >> 24) != 0) visible = true;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t q = (size_t)ny * w + nx;
                    if (g_rec_seen[q] || scene->obj_tile[q] == PPU_NATIVE_SCENE_NO_TILE) continue;
                    g_rec_seen[q] = 1;
                    g_rec_stack.push_back((int)q);
                }
            }
        }
        if (!visible) continue;
        if (g_rec_rect_on) {
            const int cx = (minx + maxx) / 2 - off;
            const int cy = (miny + maxy) / 2;
            if (cx < g_rec_rx || cy < g_rec_ry ||
                cx >= g_rec_rx + g_rec_rw || cy >= g_rec_ry + g_rec_rh) continue;
        }
        RecPose pose;
        pose.w = maxx - minx + 1;
        pose.h = maxy - miny + 1;
        const size_t n = (size_t)pose.w * pose.h;
        pose.rgba.assign(n, 0u);
        pose.tile.assign(n, PPU_NATIVE_SCENE_NO_TILE);
        pose.uvf.assign(n, 0);
        std::vector<uint64_t> hashes(n, 0);
        for (int p : pixels) {
            const size_t k = (size_t)(p / w - miny) * pose.w + (size_t)(p % w - minx);
            const uint16_t sid = scene->obj_tile[p];
            if (sid >= scene->tile_count) continue;
            pose.rgba[k] = scene->obj_rgba[p];
            pose.uvf[k] = scene->obj_uvf[p];
            hashes[k] = scene->tiles[sid].tile_hash;
            pose.tile[k] = sid;   /* índice da cena por enquanto */
        }
        /* Passo 7b: assinatura pela lista de pedacos (tile, canto da caixa 8x8,
         * espelhos). Nao depende de qual pedaco ficou por cima nas
         * sobreposicoes (o jogo troca a ordem de desenho de um frame para
         * outro). A da pose espelhada vira cada caixa: x' = largura - 8 - x. */
        std::vector<std::array<uint64_t, 4>> pcs, pcm;
        for (int y = 0; y < pose.h; ++y) {
            for (int x = 0; x < pose.w; ++x) {
                const size_t k = (size_t)y * pose.w + x;
                if (pose.tile[k] == PPU_NATIVE_SCENE_NO_TILE) continue;
                const uint8_t f = pose.uvf[k];
                const int64_t bx = (int64_t)x - (int64_t)(f & 7u);
                const int64_t by = (int64_t)y - (int64_t)((f >> 3) & 7u);
                pcs.push_back({(uint64_t)bx, (uint64_t)by, hashes[k], (uint64_t)(f & 0xC0u)});
                pcm.push_back({(uint64_t)((int64_t)pose.w - 8 - bx), (uint64_t)by, hashes[k],
                               (uint64_t)(mirror_uvf(f) & 0xC0u)});
            }
        }
        std::sort(pcs.begin(), pcs.end());
        pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
        std::sort(pcm.begin(), pcm.end());
        pcm.erase(std::unique(pcm.begin(), pcm.end()), pcm.end());
        uint64_t sig = 1469598103934665603ull, mir = sig;
        sig = fnv_step(fnv_step(sig, (uint64_t)pose.w), (uint64_t)pose.h);
        mir = fnv_step(fnv_step(mir, (uint64_t)pose.w), (uint64_t)pose.h);
        for (const auto& p : pcs) for (uint64_t v : p) sig = fnv_step(sig, v);
        for (const auto& p : pcm) for (uint64_t v : p) mir = fnv_step(mir, v);
        if (g_rec_sigs.count(sig) || g_rec_sigs.count(mir)) continue;
        if (g_rec_poses.size() >= kRecMaxPoses) continue;
        g_rec_sigs.insert(sig);
        g_rec_sigs.insert(mir);
        for (size_t k = 0; k < n; ++k) {
            if (pose.tile[k] != PPU_NATIVE_SCENE_NO_TILE) {
                pose.tile[k] = rec_tile_index(scene->tiles[pose.tile[k]]);
            }
        }
        g_rec_poses.push_back(std::move(pose));
    }
}

int hd_pack_io_sprite_rec_stop(void) {
    if (!g_rec_active) return -1;
    g_rec_active = false;
    if (g_rec_poses.empty()) {
        fprintf(stderr, "[HD] Gravacao de sprites: nenhuma pose (nada gravado)\n");
        return -1;
    }
    /* Monta a folha: prateleiras, na ordem em que as poses apareceram. */
    int sheet_w = kRecSheetWidth;
    for (const RecPose& p : g_rec_poses) sheet_w = std::max(sheet_w, p.w + 2 * kRecGap);
    std::vector<int> pos_x(g_rec_poses.size()), pos_y(g_rec_poses.size());
    int x = kRecGap, y = kRecGap, row_h = 0;
    for (size_t i = 0; i < g_rec_poses.size(); ++i) {
        const RecPose& p = g_rec_poses[i];
        if (x + p.w + kRecGap > sheet_w) {
            x = kRecGap;
            y += row_h + kRecGap;
            row_h = 0;
        }
        pos_x[i] = x;
        pos_y[i] = y;
        x += p.w + kRecGap;
        row_h = std::max(row_h, p.h);
    }
    const int sheet_h = y + row_h + kRecGap;
    const size_t count = (size_t)sheet_w * sheet_h;
    std::vector<uint32_t> rgba(count, 0u);
    std::vector<uint16_t> tile(count, PPU_NATIVE_SCENE_NO_TILE);
    std::vector<uint8_t> uvf(count, 0);
    for (size_t i = 0; i < g_rec_poses.size(); ++i) {
        const RecPose& p = g_rec_poses[i];
        for (int py = 0; py < p.h; ++py) {
            for (int px = 0; px < p.w; ++px) {
                const size_t s = (size_t)py * p.w + px;
                const size_t d = (size_t)(pos_y[i] + py) * sheet_w + (size_t)(pos_x[i] + px);
                rgba[d] = p.rgba[s];
                tile[d] = p.tile[s];
                uvf[d] = p.uvf[s];
            }
        }
    }

    std::error_code ec;
    const fs::path dir(hd_pack_io_scene_dir());
    fs::create_directories(dir, ec);
    int number = 1;
    char base[32];
    for (;; ++number) {
        snprintf(base, sizeof(base), "sprites_%03d", number);
        if (!fs::exists(dir / (std::string(base) + ".json"), ec)) break;
    }
    bool ok = true;
    ok = write_rgba_png(dir / (std::string(base) + "_obj.png"), rgba.data(), sheet_w, sheet_h, 1) && ok;
    ok = write_rgba_png(dir / (std::string(base) + "_obj_4x.png"), rgba.data(), sheet_w, sheet_h, 4) && ok;
    const fs::path json_path = dir / (std::string(base) + ".json");
    FILE* f = fopen(json_path.string().c_str(), "w");
    if (!f) {
        fprintf(stderr, "[HD] Falha ao gravar %s\n", json_path.string().c_str());
        return -1;
    }
    fprintf(f, "{\n  \"version\": 1,\n  \"kind\": \"sprites\",\n  \"poses\": %u,\n  \"width\": %d,\n  \"height\": %d,\n  \"tiles\": [\n",
            (unsigned)g_rec_poses.size(), sheet_w, sheet_h);
    for (size_t t = 0; t < g_rec_tiles.size(); ++t) {
        const PPUNativeTileDump& d = g_rec_tiles[t];
        char index[65];
        for (int i = 0; i < 64; ++i) index[i] = (char)('0' + (d.index[i] & 3u));
        index[64] = '\0';
        fprintf(f, "    {\"name\": \"%c_%016llx\", \"kind\": \"%c\", \"palette\": [%u, %u, %u, %u], \"index\": \"%s\"}%s\n",
                d.kind, (unsigned long long)d.tile_hash, d.kind,
                (unsigned)d.rgba[0], (unsigned)d.rgba[1], (unsigned)d.rgba[2], (unsigned)d.rgba[3],
                index, (t + 1 < g_rec_tiles.size()) ? "," : "");
    }
    fprintf(f, "  ],\n");
    const std::vector<uint16_t> no_bg(count, PPU_NATIVE_SCENE_NO_TILE);
    const std::vector<uint8_t> zero(count, 0);
    write_int_array(f, "bg_tile", no_bg.data(), (int)count, false);
    write_byte_array(f, "bg_uvf", zero.data(), (int)count, false);
    write_int_array(f, "obj_tile", tile.data(), (int)count, false);
    write_byte_array(f, "obj_uvf", uvf.data(), (int)count, true);
    fprintf(f, "}\n");
    ok = (fclose(f) == 0) && ok;
    g_rec_last = (dir / base).string();
    fprintf(stderr, "[HD] Folha de sprites gravada: %s (%u poses, %u tiles, %dx%d, %u frames)%s\n",
            g_rec_last.c_str(), (unsigned)g_rec_poses.size(), (unsigned)g_rec_tiles.size(),
            sheet_w, sheet_h, g_rec_frames, ok ? "" : " COM ERROS");
    return number;
}

