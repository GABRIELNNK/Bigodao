#pragma once

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

class AssetExtractor {
private:
    const size_t EXPECTED_ROM_SIZE = 524288; // 524KB (ROM Original)

    // Estruturas de bits oficiais do formato ZIP (PKWARE) para o Ark ler sem erros
    #pragma pack(push, 1)
    struct ZipLocalHeader {
        uint32_t signature = 0x04034b50; // "PK\x03\x04"
        uint16_t versionNeeded = 20;     // Versão 2.0 para suporte a pastas
        uint16_t flags = 0;
        uint16_t compression = 0;        // Modo Store (Sem compressão, ultra rápido para mods)
        uint16_t lastModTime = 0x3A00;   // Hora padrão limpa
        uint16_t lastModDate = 0x5401;   // Data padrão limpa
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t fileNameLength = 0;
        uint16_t extraFieldLength = 0;
    };

    struct ZipCentralDirectory {
        uint32_t signature = 0x02014b50; // "PK\x01\x02"
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
        uint32_t externalAttr = 0x20;    // Arquivo normal
        uint32_t localHeaderOffset = 0;
    };

    struct ZipEndOfCentralDirectory {
        uint32_t signature = 0x06054b50; // "PK\x05\x06"
        uint16_t diskNumber = 0;
        uint16_t diskWithCentralDir = 0;
        uint16_t diskEntries = 0;
        uint16_t totalEntries = 0;
        uint32_t centralDirSize = 0;
        uint32_t centralDirOffset = 0;
        uint16_t commentLength = 0;
    };
    #pragma pack(pop)

    uint32_t calculate_crc32(const uint8_t* data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            }
        }
        return ~crc;
    }

    // Adiciona um arquivo virtual para dentro do fluxo do arquivo ZIP
    void add_file_to_zip(std::ofstream& zf, const std::string& filename, const uint8_t* data, size_t size, 
                         std::vector<ZipCentralDirectory>& cd_list, std::vector<std::string>& cd_names) {
        
        uint32_t offset = zf.tellp();
        uint32_t crc = calculate_crc32(data, size);

        ZipLocalHeader lh;
        lh.crc32 = crc;
        lh.compressedSize = size;
        lh.uncompressedSize = size;
        lh.fileNameLength = filename.size();

        zf.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
        zf.write(filename.c_str(), filename.size());
        if (size > 0) {
            zf.write(reinterpret_cast<const char*>(data), size);
        }

        ZipCentralDirectory cd;
        cd.crc32 = crc;
        cd.compressedSize = size;
        cd.uncompressedSize = size;
        cd.fileNameLength = filename.size();
        cd.localHeaderOffset = offset;

        cd_list.push_back(cd);
        cd_names.push_back(filename);
    }

public:
    bool check_and_prepare_assets(const fs::path& rom_path) {
        fs::path o2r_path = "wario.o2r";
        
        if (fs::exists(o2r_path)) {
            std::cout << "[VFS] Pacote de assets wario.o2r localizado. Pronto para carregar mods...\n";
            return true;
        }

        std::cout << "[VFS] Criando container de mods wario.o2r estilo PaperBoat...\n";
        
        if (!fs::exists(rom_path)) {
            std::cerr << "ROM nao encontrada: " << rom_path << '\n';
            return false;
        }

        std::ifstream rom_file(rom_path, std::ios::binary);
        std::vector<uint8_t> rom_data((std::istreambuf_iterator<char>(rom_file)), std::istreambuf_iterator<char>());
        rom_file.close();

        if (rom_data.size() != EXPECTED_ROM_SIZE) {
            std::cerr << "❌ Erro: Tamanho de ROM invalido. Esperado exatamente 524KB.\n";
            return false;
        }

        std::ofstream zf(o2r_path, std::ios::binary);
        if (!zf) return false;

        std::vector<ZipCentralDirectory> cd_list;
        std::vector<std::string> cd_names;

        // EXTRAÇÃO CIRÚRGICA DOS GRÁFICOS E ÁUDIOS ORIGINAIS DA ROM
        // No Game Boy, dividimos os bancos lógicos para virarem arquivos acessíveis dentro do ZIP
        add_file_to_zip(zf, "rom_header.bin", &rom_data[0x0100], 0x50, cd_list, cd_names);
        add_file_to_zip(zf, "gfx/vram_tiles.bin", &rom_data[0x4000], 0x8000, cd_list, cd_names);
        add_file_to_zip(zf, "gfx/sprite_tiles.bin", &rom_data[0xC000], 0x8000, cd_list, cd_names);
        add_file_to_zip(zf, "audio/wave_tables.bin", &rom_data[0x14000], 0x4000, cd_list, cd_names);
        // Salva o restante da ROM para compatibilidade de dados lógicos do jogo
        add_file_to_zip(zf, "data/logic_banks.bin", &rom_data[0x18000], rom_data.size() - 0x18000, cd_list, cd_names);

        // ESCREVE O ÍNDICE CENTRAL DO ZIP (Permite que o Ark, WinRAR e o jogo leiam o sumário)
        uint32_t cd_offset = zf.tellp();
        for (size_t i = 0; i < cd_list.size(); ++i) {
            zf.write(reinterpret_cast<const char*>(&cd_list[i]), sizeof(ZipCentralDirectory));
            zf.write(cd_names[i].c_str(), cd_names[i].size());
        }

        uint32_t cd_end = zf.tellp();
        ZipEndOfCentralDirectory eocd;
        eocd.diskEntries = cd_list.size();
        eocd.totalEntries = cd_list.size();
        eocd.centralDirSize = cd_end - cd_offset;
        eocd.centralDirOffset = cd_offset;

        zf.write(reinterpret_cast<const char*>(&eocd), sizeof(eocd));
        zf.close();

        std::cout << "============= CONTAINER DE MODS PRONTO =============\n";
        std::cout << "✅ Arquivo 'wario.o2r' gerado com sucesso!\n";
        std::cout << "Abra o 'wario.o2r' no Ark para inspecionar os arquivos de textura.\n";
        std::cout << "====================================================\n";

        return true;
    }
};
