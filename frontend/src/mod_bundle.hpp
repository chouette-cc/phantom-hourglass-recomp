// Bundle loader for .phmod / .zip archives and unpacked mod folders.
// Extracts .phmod/.zip archives into `<mods_dir>/.cache/<stem>/` (with mtime+size stamp caching
// and strict zip-slip validation) so mods can be distributed as single files like Dusklight's .dusk bundles.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ph {

struct DiscoveredBundle {
    std::filesystem::path source_path;   // original .phmod/.zip file or unpacked folder
    std::filesystem::path content_dir;   // directory containing mod.json (unpacked or extracted cache)
    bool is_archive = false;
    std::string extract_error;
};

// Scans `mods_dir` for both unpacked mod directories (containing mod.json) and `.phmod`/`.zip` archives.
// Extracts or refreshes cached archives into `cache_dir`.
std::vector<DiscoveredBundle> discover_mod_bundles(const std::filesystem::path& mods_dir,
                                                   const std::filesystem::path& cache_dir);

// Extracts a single zip/.phmod file to `dest_dir` with zip-slip protection.
// Sets `out_mod_root` to the directory containing `mod.json` (root or single top-level folder).
bool extract_mod_archive(const std::filesystem::path& archive_path, const std::filesystem::path& dest_dir,
                         std::filesystem::path& out_mod_root, std::string& error);

// Copies a native shared library into a unique session file so the original DLL/.so is never locked (hot-reload).
bool stage_native_library(const std::filesystem::path& src_lib, const std::filesystem::path& staging_dir,
                          const std::string& mod_id, uint64_t generation, std::filesystem::path& out_staged,
                          std::string& error);

}  // namespace ph
