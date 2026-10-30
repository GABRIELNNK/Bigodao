#pragma once

// =============================================================================
// Texturas P&B originais (passo 31a)
//
// No launcher, antes de recriar o wario.o2r, segue a receita gerada por
// tools/gerar_receita_original_bw.py (original_bw_receita.h): tira cada tile da
// ROM ORIGINAL, pinta com os tons do Game Boy (registrador BGP/OBP0 da tela) e
// grava um PNG 8x8 com o nome do tile do DX (b_<hash>[_fXX|_gXX].png). Os PNGs
// vão para a pasta original_bw/ dentro do wario.o2r; o jogo os carrega como
// uma fonte de mods embutida quando a opção "Cores" está em Original.
//
// Sem o original_bw_receita.h no projeto, available() devolve false e nada muda.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gbrt_hash.h"

#if defined(__has_include)
#  if __has_include("original_bw_receita.h")
#    include "original_bw_receita.h"
#    define ORIGINAL_BW_HAS_RECIPE 1
#  endif
#endif

namespace original_bw {

static constexpr const char* FOLDER = "original_bw/";

struct File {
    std::string name;
    std::vector<uint8_t> data;
};

#ifdef ORIGINAL_BW_HAS_RECIPE
inline bool available() { return sizeof(kOriginalBwTextures) > 0; }
inline uint32_t recipe_version() { return ORIGINAL_BW_RECIPE_VERSION; }
#else
inline bool available() { return false; }
inline uint32_t recipe_version() { return 0; }
#endif

namespace detail {

// Descompactador RLC do Wario Land (igual a gfx_decomp_rlc do gerar_mod_original_bw.py):
// grava nas posições pares e depois nas ímpares de um buffer de 0x1800 bytes.
inline std::vector<uint8_t> decomp_rlc(const uint8_t* data, size_t max_len, size_t buf_size = 0x1800) {
    std::vector<uint8_t> out(buf_size, 0);
    size_t i = 0, p = 0;
    while (i < max_len) {
        const uint8_t b = data[i++];
        if (b == 0) return out;
        const unsigned count = b & 0x7Fu;
        const bool literal = (b & 0x80u) != 0;
        uint8_t source = 0;
        if (!literal) {
            if (i >= max_len) return out;
            source = data[i++];
        }
        for (unsigned k = 0; k < (literal ? count : b); ++k) {
            if (literal && i >= max_len) break;
            if (p >= buf_size) {
                if (p == buf_size + 1) return out;
                p = 1;
            }
            out[p] = literal ? data[i++] : source;
            p += 2;
        }
    }
    return out;
}

inline uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

inline void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

inline void chunk(std::vector<uint8_t>& png, const char* tag, const std::vector<uint8_t>& data) {
    put_be32(png, (uint32_t)data.size());
    std::vector<uint8_t> body(tag, tag + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put_be32(png, crc32(body.data(), body.size()));
}

// PNG 8x8 indexado (8 bits por ponto), zlib sem compressão (bloco "stored").
// rgb: 4 cores; transparent0: cor 0 transparente (sprites).
inline std::vector<uint8_t> png_8x8(const uint8_t idx[64], const uint8_t rgb[4][3], bool transparent0) {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, 8); put_be32(ihdr, 8);
    ihdr.insert(ihdr.end(), {8, 3, 0, 0, 0});           // 8 bits, paleta
    chunk(png, "IHDR", ihdr);
    std::vector<uint8_t> plte;
    for (int c = 0; c < 4; ++c) plte.insert(plte.end(), {rgb[c][0], rgb[c][1], rgb[c][2]});
    chunk(png, "PLTE", plte);
    if (transparent0) chunk(png, "tRNS", {0, 255, 255, 255});
    std::vector<uint8_t> raw;
    for (int y = 0; y < 8; ++y) {
        raw.push_back(0);                                 // filtro 0
        raw.insert(raw.end(), idx + y * 8, idx + y * 8 + 8);
    }
    std::vector<uint8_t> z = {0x78, 0x01, 0x01,
                              (uint8_t)(raw.size() & 0xFF), (uint8_t)(raw.size() >> 8),
                              (uint8_t)(~raw.size() & 0xFF), (uint8_t)((~raw.size() >> 8) & 0xFF)};
    z.insert(z.end(), raw.begin(), raw.end());
    uint32_t a = 1, b = 0;                               // adler32
    for (uint8_t x : raw) { a = (a + x) % 65521u; b = (b + a) % 65521u; }
    put_be32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

}  // namespace detail

// Cria os PNGs (nome = "original_bw/<tile>.png"). orig: ROM original;
// dx: ROM do DX já com o patch. As duas precisam ser as da receita.
inline bool build(const std::vector<uint8_t>& orig, const std::vector<uint8_t>& dx,
                  std::vector<File>& out, std::string& err) {
#ifndef ORIGINAL_BW_HAS_RECIPE
    (void)orig; (void)dx; (void)out;
    err = "recipe not compiled in";
    return false;
#else
    if (!gbrt_sha256_matches_hex(orig.data(), orig.size(), kOriginalBwOrigSha256)) {
        err = "original ROM is not the one the recipe was made for";
        return false;
    }
    if (!gbrt_sha256_matches_hex(dx.data(), dx.size(), kOriginalBwDxSha256)) {
        err = "patched ROM is not the one the recipe was made for (different IPS?)";
        return false;
    }
    std::vector<std::vector<uint8_t>> bufs;
    for (const OriginalBwAsset& a : kOriginalBwAssets) {
        if ((size_t)a.offset + a.length > orig.size()) { err = "recipe asset out of range"; return false; }
        bufs.push_back(detail::decomp_rlc(orig.data() + a.offset, a.length));
    }
    static const uint8_t kShades[4][3] = {{255, 255, 255}, {170, 170, 170}, {85, 85, 85}, {0, 0, 0}};
    out.clear();
    out.reserve(sizeof(kOriginalBwTextures) / sizeof(kOriginalBwTextures[0]));
    for (const OriginalBwTexture& t : kOriginalBwTextures) {
        const OriginalBwSource& s = kOriginalBwSources[t.source];
        uint8_t tile[16] = {0};
        if (s.type == 0) {
            if ((size_t)s.offset + 16 > orig.size()) { err = "recipe offset out of range"; return false; }
            std::memcpy(tile, orig.data() + s.offset, 16);
        } else if (s.type == 1) {
            if (s.asset < 0 || (size_t)s.asset >= bufs.size() || (size_t)s.tile * 16 + 16 > bufs[s.asset].size()) {
                err = "recipe tile out of range"; return false;
            }
            std::memcpy(tile, bufs[s.asset].data() + s.tile * 16, 16);
        } else {
            for (int y = 0; y < 8; ++y) {
                const size_t lo = (size_t)s.offset + (size_t)y * s.frames + s.frame;
                if (lo >= orig.size()) { err = "recipe offset out of range"; return false; }
                tile[y * 2] = orig[lo];
                if (s.asset >= 0) {
                    if ((size_t)s.asset >= bufs.size() || (size_t)s.tile * 16 + 16 > bufs[s.asset].size()) {
                        err = "recipe tile out of range"; return false;
                    }
                    tile[y * 2 + 1] = bufs[s.asset][s.tile * 16 + y * 2 + 1];
                }
            }
        }
        uint8_t idx[64];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int bit = 7 - x;
                idx[y * 8 + x] = (uint8_t)(((tile[y * 2] >> bit) & 1) | (((tile[y * 2 + 1] >> bit) & 1) << 1));
            }
        }
        uint8_t rgb[4][3];
        for (int i = 0; i < 4; ++i) std::memcpy(rgb[i], kShades[(t.reg >> (i * 2)) & 3], 3);
        char name[64];
        std::snprintf(name, sizeof(name), "%s%c_%016llx%s%s.png", FOLDER, t.kind,
                      (unsigned long long)t.hash, t.ctx[0] ? "_" : "", t.ctx);
        out.push_back({name, detail::png_8x8(idx, rgb, t.kind == 'o')});
    }
    return true;
#endif
}

}  // namespace original_bw
