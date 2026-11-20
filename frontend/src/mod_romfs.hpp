// In-memory NitroFS file override and ROM patcher for mods.
// Applies mod `romfs/` replacement files and `rom_patches` to the in-memory ROM vector before Runtime::create(),
// without modifying the user's ROM file on disk and without requiring any melonDS core patches.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ph {

struct RomPatchSpec {
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
    std::string region;  // empty = all regions
};

struct RomModInput {
    std::string id;
    std::filesystem::path dir;
    std::vector<RomPatchSpec> rom_patches;
};

struct RomPatchStats {
    size_t files_replaced = 0;
    size_t files_appended = 0;
    size_t byte_patches_applied = 0;
    std::vector<std::string> warnings;
};

// Patches `rom` in place for all enabled mods in `mods` (in load order; later mods win on conflicts).
// Updates FAT entries (supporting larger replacement files by appending to the ROM), header used-size,
// device capacity, and header CRC-16.
RomPatchStats apply_rom_mods(std::vector<uint8_t>& rom, const std::string& game_code,
                             const std::vector<RomModInput>& mods);

// NitroFS file index built from the active (possibly modded) ROM for Lua's `ph.rom` API.
struct NitroFileEntry {
    std::string path;
    uint32_t offset = 0;
    uint32_t size = 0;
};

std::vector<NitroFileEntry> index_rom_nitrofs(const std::vector<uint8_t>& rom);

}  // namespace ph
