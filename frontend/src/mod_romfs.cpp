#include "mod_romfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include "ntr/rom.hpp"

namespace ph {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void write_u32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    if (off + 4 <= buf.size()) std::memcpy(buf.data() + off, &v, 4);
}

void write_u16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
    if (off + 2 <= buf.size()) std::memcpy(buf.data() + off, &v, 2);
}

uint16_t nds_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= uint16_t(data[i]);
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = uint16_t((crc >> 1) ^ 0xA001u);
            else crc >>= 1;
        }
    }
    return crc;
}

std::vector<uint8_t> read_binary(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = f.tellg();
    if (sz < 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    f.seekg(0);
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

}  // namespace

RomPatchStats apply_rom_mods(std::vector<uint8_t>& rom, const std::string& game_code,
                             const std::vector<RomModInput>& mods) {
    RomPatchStats stats;
    if (rom.size() < 0x200 || mods.empty()) return stats;

    ntr::Rom parsed;
    bool have_nitrofs = false;
    try {
        parsed = ntr::Rom::from_bytes(rom);
        have_nitrofs = true;
    } catch (...) {
        have_nitrofs = false;
    }

    std::unordered_map<std::string, ntr::NitroFile> exact_map;
    std::unordered_map<std::string, ntr::NitroFile> lower_map;
    if (have_nitrofs) {
        for (const ntr::NitroFile& f : parsed.files()) {
            exact_map[f.path] = f;
            lower_map[lower_ascii(f.path)] = f;
        }
    }

    const uint32_t fat_offset = have_nitrofs ? parsed.header().fat_offset : 0;
    const uint32_t fat_size = have_nitrofs ? parsed.header().fat_size : 0;
    bool header_dirty = false;

    for (const RomModInput& mod : mods) {
        // 1. Raw ROM byte patches from mod.json
        for (const RomPatchSpec& patch : mod.rom_patches) {
            if (!patch.region.empty() && patch.region != game_code) continue;
            if (patch.bytes.empty()) continue;
            if (uint64_t(patch.offset) + patch.bytes.size() > rom.size()) {
                stats.warnings.push_back("mod '" + mod.id + "': rom_patch out of bounds at 0x" +
                                         std::to_string(patch.offset));
                continue;
            }
            std::memcpy(rom.data() + patch.offset, patch.bytes.data(), patch.bytes.size());
            stats.byte_patches_applied++;
            if (patch.offset < 0x15E) header_dirty = true;
        }

        // 2. NitroFS overlay/ (Dusklight-style) and romfs/ overrides
        if (!have_nitrofs) continue;
        for (const char* sub : {"overlay", "romfs"}) {
            const fs::path romfs_dir = mod.dir / sub;
            std::error_code ec;
            if (!fs::is_directory(romfs_dir, ec)) continue;

            for (const auto& entry : fs::recursive_directory_iterator(romfs_dir, ec)) {
                if (ec) break;
                std::error_code sub_ec;
                if (!entry.is_regular_file(sub_ec)) continue;
                const fs::path rel = fs::relative(entry.path(), romfs_dir, sub_ec);
                if (sub_ec) continue;
                std::string nitro_path = rel.generic_string();
                while (!nitro_path.empty() && nitro_path[0] == '/') nitro_path.erase(0, 1);

                const ntr::NitroFile* target = nullptr;
                if (auto it = exact_map.find(nitro_path); it != exact_map.end()) target = &it->second;
                else if (auto it2 = lower_map.find(lower_ascii(nitro_path)); it2 != lower_map.end()) target = &it2->second;

                if (!target) {
                    stats.warnings.push_back("mod '" + mod.id + "': " + sub + " file not found in NitroFS: " + nitro_path);
                    continue;
                }
                const uint32_t fat_entry_off = fat_offset + uint32_t(target->id) * 8u;
                if (uint64_t(fat_entry_off) + 8u > uint64_t(fat_offset) + fat_size ||
                    uint64_t(fat_entry_off) + 8u > rom.size()) {
                    stats.warnings.push_back("mod '" + mod.id + "': FAT entry out of range for " + nitro_path);
                    continue;
                }

                const std::vector<uint8_t> data = read_binary(entry.path());
                if (data.size() > 64u * 1024u * 1024u) {
                    stats.warnings.push_back("mod '" + mod.id + "': " + sub + " file too large (>64 MB): " + nitro_path);
                    continue;
                }
                const uint32_t new_size = uint32_t(data.size());
                if (new_size <= target->size && uint64_t(target->offset) + new_size <= rom.size()) {
                    if (new_size > 0) std::memcpy(rom.data() + target->offset, data.data(), new_size);
                    write_u32(rom, fat_entry_off, target->offset);
                    write_u32(rom, fat_entry_off + 4u, target->offset + new_size);
                    stats.files_replaced++;
                } else {
                    // Append aligned to 512 bytes and repoint FAT entry (melonDS pads ROM to next power of 2).
                    const size_t aligned = (rom.size() + 0x1FFu) & ~size_t(0x1FFu);
                    if (aligned + new_size > 512u * 1024u * 1024u) {
                        stats.warnings.push_back("mod '" + mod.id + "': ROM exceeds 512 MB limit while adding " + nitro_path);
                        continue;
                    }
                    rom.resize(aligned + new_size, 0xFF);
                    if (new_size > 0) std::memcpy(rom.data() + aligned, data.data(), new_size);
                    const uint32_t start = uint32_t(aligned);
                    write_u32(rom, fat_entry_off, start);
                    write_u32(rom, fat_entry_off + 4u, start + new_size);
                    // Keep exact/lower maps updated in case a higher-priority mod also overrides the same file.
                    const std::string key_exact = target->path;
                    const std::string key_lower = lower_ascii(key_exact);
                    exact_map[key_exact].offset = start;
                    exact_map[key_exact].size = new_size;
                    lower_map[key_lower].offset = start;
                    lower_map[key_lower].size = new_size;
                    stats.files_replaced++;
                    stats.files_appended++;
                    header_dirty = true;
                }
            }
        }
    }

    if (header_dirty && rom.size() >= 0x200) {
        // Update ROM used size (0x80), device capacity (0x14 = log2(size / 128 KB)), and header CRC-16 (0x15E).
        write_u32(rom, 0x80, uint32_t(rom.size()));
        uint64_t cap_bytes = 128u * 1024u;
        uint8_t cap_code = 0;
        while (cap_bytes < rom.size() && cap_code < 15) {
            cap_bytes <<= 1;
            cap_code++;
        }
        if (cap_code > rom[0x14]) rom[0x14] = cap_code;
        write_u16(rom, 0x15E, nds_crc16(rom.data(), 0x15E));
    }

    return stats;
}

std::vector<NitroFileEntry> index_rom_nitrofs(const std::vector<uint8_t>& rom) {
    std::vector<NitroFileEntry> out;
    if (rom.size() < 0x200) return out;
    try {
        const ntr::Rom parsed = ntr::Rom::from_bytes(rom);
        out.reserve(parsed.files().size());
        for (const ntr::NitroFile& f : parsed.files()) out.push_back({f.path, f.offset, f.size});
    } catch (...) {
    }
    return out;
}

}  // namespace ph
