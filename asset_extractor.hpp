#pragma once

// =============================================================================
// AssetExtractor — container de assets/mods "wario.o2r" (ZIP)
//
// O .o2r guarda a ROM JÁ PATCHEADA (DX), fatiada em arquivos contíguos que cobrem
// a ROM inteira. Depois da primeira execução, o jogo remonta a ROM só a partir do
// pacote: não precisa mais de rom.gb, do .ips nem do manifest JSON.
//
// Mods: substituir uma fatia (ex.: gfx/vram_tiles.bin) por outra do MESMO tamanho
// funciona — ela entra na ROM remontada. Entradas comprimidas (Deflate) também são
// lidas, então reempacotar no Ark/WinRAR/7-Zip não quebra o pacote.
//
// Requer stb_image.h com STBI_SUPPORT_ZLIB (a implementação fica no main.cpp).
// =============================================================================

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// stbi_zlib_decode_noheader_buffer (inflate de entradas Deflate). Se o main.cpp já
// incluiu o stb_image.h (com a implementação), não inclui de novo.
#ifndef STBI_INCLUDE_STB_IMAGE_H
#include "stb_image.h"
#endif

class AssetExtractor {
public:
    static constexpr int FORMAT_VERSION = 2;

    explicit AssetExtractor(std::filesystem::path o2r_path) : path_(std::move(o2r_path)) {}

    const std::filesystem::path& path() const { return path_; }

    // Passo 31a: arquivos extras no pacote (ex.: original_bw/*.png). O leitor da
    // ROM ignora o que não é fatia, então isso não muda a remontagem.
    struct ExtraFile {
        std::string name;
        std::vector<uint8_t> data;
    };

    // Valor de uma linha "chave=valor" do bigodao/o2r_info.txt ("" se não houver).
    std::string info_value(const std::string& key) const {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return "";
        std::vector<uint8_t> z((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::map<std::string, std::vector<uint8_t>> files;
        std::string err;
        if (!read_zip(z, files, err)) return "";
        auto it = files.find(INFO_NAME);
        if (it == files.end()) return "";
        std::istringstream ss(std::string(it->second.begin(), it->second.end()));
        std::string line;
        while (std::getline(ss, line)) {
            if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=') {
                return line.substr(key.size() + 1);
            }
        }
        return "";
    }

    bool exists() const {
        std::error_code ec;
        return std::filesystem::is_regular_file(path_, ec);
    }

    // Gera o .o2r a partir da ROM patcheada. Escreve num arquivo temporário, confere
    // que dá para remontar a ROM byte a byte e só então substitui o pacote antigo.
    // extra: arquivos a mais no pacote; extra_info: linhas a mais no o2r_info.txt.
    bool build(const std::vector<uint8_t>& rom, std::string& err,
               const std::vector<ExtraFile>& extra = {}, const std::string& extra_info = "") const {
        namespace fs = std::filesystem;
        if (rom.size() <= MIN_ROM_SIZE) {
            err = "ROM too small (" + std::to_string(rom.size()) + " bytes)";
            return false;
        }

        const fs::path tmp = fs::path(path_).concat(".tmp");
        {
            std::ofstream zf(tmp, std::ios::binary | std::ios::trunc);
            if (!zf) { err = "cannot create " + tmp.string(); return false; }

            std::vector<CentralDir> cd;
            std::vector<std::string> names;

            // Informativo (para ver no Ark): o cabeçalho do cartucho.
            add_file(zf, "rom_header.bin", &rom[0x0100], 0x50, cd, names);

            // Fatias contíguas da ROM (são elas que remontam a ROM).
            for (const Segment& s : SEGMENTS) {
                const size_t end = s.end ? s.end : rom.size();
                add_file(zf, s.name, &rom[s.begin], end - s.begin, cd, names);
            }

            // Passo 31a: arquivos extras (texturas originais etc.)
            for (const ExtraFile& f : extra) {
                add_file(zf, f.name, f.data.data(), f.data.size(), cd, names);
            }

            const std::string info = "format=" + std::to_string(FORMAT_VERSION) + "\n"
                                     "rom_size=" + std::to_string(rom.size()) + "\n"
                                     "content=Wario Land: Super Mario Land 3 + DX v1.2 (patched)\n" + extra_info;
            add_file(zf, INFO_NAME, reinterpret_cast<const uint8_t*>(info.data()), info.size(), cd, names);

            const uint32_t cd_offset = static_cast<uint32_t>(zf.tellp());
            for (size_t i = 0; i < cd.size(); ++i) {
                zf.write(reinterpret_cast<const char*>(&cd[i]), sizeof(CentralDir));
                zf.write(names[i].data(), static_cast<std::streamsize>(names[i].size()));
            }
            EndOfCentralDir eocd;
            if (cd.size() > 0xFFFF) { err = "too many files for the package"; return false; }
            eocd.diskEntries = eocd.totalEntries = static_cast<uint16_t>(cd.size());
            eocd.centralDirSize = static_cast<uint32_t>(zf.tellp()) - cd_offset;
            eocd.centralDirOffset = cd_offset;
            zf.write(reinterpret_cast<const char*>(&eocd), sizeof(eocd));
            if (!zf) { err = "write error on " + tmp.string(); return false; }
        }

        // Verificação de ida e volta antes de trocar o arquivo.
        std::vector<uint8_t> check;
        std::string check_err;
        if (!load_from(tmp, check, check_err) || check != rom) {
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "verification failed" + (check_err.empty() ? std::string() : ": " + check_err);
            return false;
        }

        std::error_code ec;
        fs::remove(path_, ec);            // MinGW não sobrescreve no rename
        fs::rename(tmp, path_, ec);
        if (ec) { err = "cannot replace " + path_.string() + ": " + ec.message(); return false; }

        std::cout << "[VFS] " << path_.filename().string() << " created ("
                  << rom.size() << " bytes of patched ROM). ROM, patch and manifest are no longer needed.\n";
        return true;
    }

    // Remonta a ROM patcheada a partir do .o2r.
    bool load(std::vector<uint8_t>& rom, std::string& err) const {
        return load_from(path_, rom, err);
    }

private:
    struct Segment { const char* name; size_t begin; size_t end; };   // end == 0 → até o fim

    // Ordem = ordem na ROM. Os nomes antigos foram mantidos; o banco 0 é novo
    // (o formato antigo não o guardava, por isso não dava para remontar a ROM).
    static constexpr Segment SEGMENTS[] = {
        { "data/bank0.bin",        0x00000, 0x04000 },
        { "gfx/vram_tiles.bin",    0x04000, 0x0C000 },
        { "gfx/sprite_tiles.bin",  0x0C000, 0x14000 },
        { "audio/wave_tables.bin", 0x14000, 0x18000 },
        { "data/logic_banks.bin",  0x18000, 0       },
    };
    static constexpr size_t      MIN_ROM_SIZE = 0x18000;
    static constexpr const char* INFO_NAME    = "bigodao/o2r_info.txt";

    // Estruturas do formato ZIP (PKWARE), little-endian
    #pragma pack(push, 1)
    struct LocalHeader {
        uint32_t signature = 0x04034b50;
        uint16_t versionNeeded = 20;
        uint16_t flags = 0;
        uint16_t compression = 0;        // Store
        uint16_t lastModTime = 0x3A00;
        uint16_t lastModDate = 0x5401;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t fileNameLength = 0;
        uint16_t extraFieldLength = 0;
    };
    struct CentralDir {
        uint32_t signature = 0x02014b50;
        uint16_t versionMade = 20;
        uint16_t versionNeeded = 20;
        uint16_t flags = 0;
        uint16_t compression = 0;
        uint16_t lastModTime = 0x3A00;
        uint16_t lastModDate = 0x5401;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t fileNameLength = 0;
        uint16_t extraFieldLength = 0;
        uint16_t fileCommentLength = 0;
        uint16_t diskNumberStart = 0;
        uint16_t internalAttr = 0;
        uint32_t externalAttr = 0x20;
        uint32_t localHeaderOffset = 0;
    };
    struct EndOfCentralDir {
        uint32_t signature = 0x06054b50;
        uint16_t diskNumber = 0;
        uint16_t diskWithCentralDir = 0;
        uint16_t diskEntries = 0;
        uint16_t totalEntries = 0;
        uint32_t centralDirSize = 0;
        uint32_t centralDirOffset = 0;
        uint16_t commentLength = 0;
    };
    #pragma pack(pop)

    std::filesystem::path path_;

    static uint32_t crc32(const uint8_t* data, size_t len) {
        static uint32_t table[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
                table[i] = c;
            }
            init = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    static void add_file(std::ofstream& zf, const std::string& name, const uint8_t* data, size_t size,
                         std::vector<CentralDir>& cd_list, std::vector<std::string>& cd_names) {
        const uint32_t offset = static_cast<uint32_t>(zf.tellp());
        const uint32_t crc = crc32(data, size);

        LocalHeader lh;
        lh.crc32 = crc;
        lh.compressedSize = lh.uncompressedSize = static_cast<uint32_t>(size);
        lh.fileNameLength = static_cast<uint16_t>(name.size());
        zf.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
        zf.write(name.data(), static_cast<std::streamsize>(name.size()));
        if (size) zf.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

        CentralDir cd;
        cd.crc32 = crc;
        cd.compressedSize = cd.uncompressedSize = static_cast<uint32_t>(size);
        cd.fileNameLength = static_cast<uint16_t>(name.size());
        cd.localHeaderOffset = offset;
        cd_list.push_back(cd);
        cd_names.push_back(name);
    }

    template <typename T>
    static bool read_struct(const std::vector<uint8_t>& buf, size_t off, T& out) {
        if (off > buf.size() || buf.size() - off < sizeof(T)) return false;
        std::memcpy(&out, buf.data() + off, sizeof(T));
        return true;
    }

    // Lê todas as entradas do ZIP (Store ou Deflate), conferindo o CRC.
    static bool read_zip(const std::vector<uint8_t>& z, std::map<std::string, std::vector<uint8_t>>& files,
                         std::string& err) {
        if (z.size() < sizeof(EndOfCentralDir)) { err = "not a zip file"; return false; }

        // EOCD fica no fim (pode ter comentário de até 64 KB depois dele)
        size_t eocd_pos = std::string::npos;
        const size_t lowest = z.size() > 0xFFFF + sizeof(EndOfCentralDir) ? z.size() - 0xFFFF - sizeof(EndOfCentralDir) : 0;
        for (size_t p = z.size() - sizeof(EndOfCentralDir) + 1; p-- > lowest;) {
            if (z[p] == 0x50 && z[p + 1] == 0x4b && z[p + 2] == 0x05 && z[p + 3] == 0x06) { eocd_pos = p; break; }
        }
        EndOfCentralDir eocd;
        if (eocd_pos == std::string::npos || !read_struct(z, eocd_pos, eocd)) { err = "zip directory not found"; return false; }

        size_t p = eocd.centralDirOffset;
        for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
            CentralDir cd;
            if (!read_struct(z, p, cd) || cd.signature != 0x02014b50) { err = "corrupt zip directory"; return false; }
            p += sizeof(CentralDir);
            if (z.size() - p < cd.fileNameLength) { err = "corrupt zip directory"; return false; }
            std::string name(reinterpret_cast<const char*>(z.data() + p), cd.fileNameLength);
            p += size_t(cd.fileNameLength) + cd.extraFieldLength + cd.fileCommentLength;
            if (name.empty() || name.back() == '/') continue;   // pasta

            LocalHeader lh;
            if (!read_struct(z, cd.localHeaderOffset, lh) || lh.signature != 0x04034b50) { err = "corrupt entry: " + name; return false; }
            const size_t data_pos = size_t(cd.localHeaderOffset) + sizeof(LocalHeader) + lh.fileNameLength + lh.extraFieldLength;
            if (data_pos > z.size() || z.size() - data_pos < cd.compressedSize) { err = "truncated entry: " + name; return false; }

            std::vector<uint8_t> out(cd.uncompressedSize);
            if (cd.compression == 0) {
                if (cd.compressedSize != cd.uncompressedSize) { err = "corrupt entry: " + name; return false; }
                if (!out.empty()) std::memcpy(out.data(), z.data() + data_pos, out.size());
            } else if (cd.compression == 8) {
                const int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()),
                                                               reinterpret_cast<const char*>(z.data() + data_pos),
                                                               static_cast<int>(cd.compressedSize));
                if (n != static_cast<int>(out.size())) { err = "cannot decompress: " + name; return false; }
            } else {
                // Formato que não sabemos ler (LZMA, etc.) — só é erro se for uma entrada que usamos.
                files[name] = {};
                continue;
            }
            if (crc32(out.data(), out.size()) != cd.crc32) { err = "CRC mismatch: " + name; return false; }
            files[name] = std::move(out);
        }
        return true;
    }

    static bool load_from(const std::filesystem::path& file, std::vector<uint8_t>& rom, std::string& err) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { err = "cannot open file"; return false; }
        std::vector<uint8_t> z((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        std::map<std::string, std::vector<uint8_t>> files;
        if (!read_zip(z, files, err)) return false;

        // Versão do pacote (o formato antigo não tinha o banco 0 nem o patch aplicado)
        auto info_it = files.find(INFO_NAME);
        if (info_it == files.end()) { err = "created by an older version"; return false; }
        int format = 0;
        size_t rom_size = 0;
        {
            std::istringstream ss(std::string(info_it->second.begin(), info_it->second.end()));
            std::string line;
            while (std::getline(ss, line)) {
                if (line.rfind("format=", 0) == 0)   format = std::atoi(line.c_str() + 7);
                if (line.rfind("rom_size=", 0) == 0) rom_size = static_cast<size_t>(std::strtoull(line.c_str() + 9, nullptr, 10));
            }
        }
        if (format != FORMAT_VERSION) { err = "unsupported format version " + std::to_string(format); return false; }

        rom.clear();
        rom.reserve(rom_size);
        for (const Segment& s : SEGMENTS) {
            auto it = files.find(s.name);
            if (it == files.end()) { err = std::string("missing ") + s.name; return false; }
            if (s.end && it->second.size() != s.end - s.begin) {
                err = std::string(s.name) + " has the wrong size (" + std::to_string(it->second.size()) +
                      " bytes, expected " + std::to_string(s.end - s.begin) + ")";
                return false;
            }
            if (it->second.empty()) { err = std::string("cannot read ") + s.name + " (unsupported compression)"; return false; }
            rom.insert(rom.end(), it->second.begin(), it->second.end());
        }
        if (rom.size() != rom_size) {
            err = "ROM size mismatch (" + std::to_string(rom.size()) + " vs " + std::to_string(rom_size) + ")";
            return false;
        }
        return true;
    }
};