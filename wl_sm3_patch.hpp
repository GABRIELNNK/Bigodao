#pragma once

#include "gbrt_hash.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wl_sm3_patch {

inline std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Nao foi possivel abrir: " + path.string());
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Nao foi possivel abrir: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), {});
}

inline std::string json_string(const std::string& json, const std::string& key) {
    const std::regex field("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, field)) {
        throw std::runtime_error("Campo ausente no manifesto: " + key);
    }
    return match[1].str();
}

inline size_t json_size(const std::string& json, const std::string& key) {
    const std::regex field("\\\"" + key + "\\\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (!std::regex_search(json, match, field)) {
        throw std::runtime_error("Campo numerico ausente no manifesto: " + key);
    }
    const unsigned long long value = std::stoull(match[1].str());
    if (value > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("Valor numerico fora do limite: " + key);
    }
    return static_cast<size_t>(value);
}

inline std::vector<uint8_t> apply_ips(
    std::vector<uint8_t> image,
    const std::vector<uint8_t>& ips,
    size_t expected_records) {
    if (ips.size() < 8 || !std::equal(ips.begin(), ips.begin() + 5, "PATCH")) {
        throw std::runtime_error("Assinatura IPS invalida");
    }

    size_t position = 5;
    size_t records = 0;
    while (true) {
        if (position + 3 > ips.size()) {
            throw std::runtime_error("IPS truncado antes do marcador EOF");
        }
        if (std::equal(ips.begin() + position, ips.begin() + position + 3, "EOF")) {
            position += 3;
            break;
        }
        if (position + 5 > ips.size()) {
            throw std::runtime_error("Cabecalho de registro IPS truncado");
        }

        const size_t offset = (static_cast<size_t>(ips[position]) << 16) |
            (static_cast<size_t>(ips[position + 1]) << 8) | ips[position + 2];
        size_t length = (static_cast<size_t>(ips[position + 3]) << 8) | ips[position + 4];
        position += 5;

        std::vector<uint8_t> data;
        if (length == 0) {
            if (position + 3 > ips.size()) {
                throw std::runtime_error("Registro RLE IPS truncado");
            }
            length = (static_cast<size_t>(ips[position]) << 8) | ips[position + 1];
            data.assign(length, ips[position + 2]);
            position += 3;
        } else {
            if (position + length > ips.size()) {
                throw std::runtime_error("Dados de registro IPS truncados");
            }
            data.assign(ips.begin() + position, ips.begin() + position + length);
            position += length;
        }

        if (length == 0 || offset > 32u * 1024u * 1024u ||
            length > 32u * 1024u * 1024u - offset) {
            throw std::runtime_error("Registro IPS vazio ou fora do limite permitido");
        }
        const size_t end = offset + length;
        if (end > image.size()) {
            image.resize(end, 0);
        }
        std::copy(data.begin(), data.end(), image.begin() + offset);
        ++records;
    }

    if (position != ips.size()) {
        const size_t trailing = ips.size() - position;
        if (trailing != 3) {
            throw std::runtime_error("Dados extras invalidos no final do IPS");
        }
        const size_t final_size = (static_cast<size_t>(ips[position]) << 16) |
            (static_cast<size_t>(ips[position + 1]) << 8) | ips[position + 2];
        image.resize(final_size, 0);
    }
    if (records != expected_records) {
        throw std::runtime_error("Quantidade de registros IPS diferente do manifesto");
    }
    return image;
}

inline std::vector<uint8_t> load_and_patch_rom(
    const std::filesystem::path& rom_path,
    const std::filesystem::path& manifest_path) {
    const std::string manifest = read_text_file(manifest_path);
    if (json_string(manifest, "schema") != "wl-sm3.ips-rom-patch" ||
        json_size(manifest, "version") != 1 ||
        json_string(manifest, "patch_format") != "IPS") {
        throw std::runtime_error("Schema ou formato de patch nao suportado");
    }

    const std::vector<uint8_t> original = read_binary_file(rom_path);
    const size_t input_size = json_size(manifest, "input_size");
    const std::string input_sha256 = json_string(manifest, "input_sha256");
    if (original.size() != input_size ||
        !gbrt_sha256_matches_hex(original.data(), original.size(), input_sha256.c_str())) {
        throw std::runtime_error("A ROM selecionada nao corresponde a ROM original esperada");
    }

    const std::filesystem::path ips_path =
        manifest_path.parent_path() / json_string(manifest, "patch_file");
    const std::vector<uint8_t> ips = read_binary_file(ips_path);
    const std::string patch_sha256 = json_string(manifest, "patch_sha256");
    if (!gbrt_sha256_matches_hex(ips.data(), ips.size(), patch_sha256.c_str())) {
        throw std::runtime_error("O arquivo IPS nao corresponde ao manifesto");
    }

    std::vector<uint8_t> patched = apply_ips(
        original,
        ips,
        json_size(manifest, "patch_record_count"));
    const size_t output_size = json_size(manifest, "output_size");
    const std::string output_sha256 = json_string(manifest, "output_sha256");
    if (patched.size() != output_size ||
        !gbrt_sha256_matches_hex(patched.data(), patched.size(), output_sha256.c_str())) {
        throw std::runtime_error("O resultado do IPS nao corresponde a ROM hack 1.2 esperada");
    }
    return patched;
}

}