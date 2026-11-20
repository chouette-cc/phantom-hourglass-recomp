#include "mod_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

#include <miniz.h>

namespace ph {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMaxArchiveTotalBytes = 256ull * 1024ull * 1024ull;

std::string lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_archive_ext(const fs::path& p) {
    const std::string ext = lower_ascii(p.extension().string());
    return ext == ".phmod" || ext == ".zip";
}

bool is_safe_zip_relpath(const std::string& raw, fs::path& out_rel) {
    if (raw.empty() || raw.find('\0') != std::string::npos) return false;
    std::string norm = raw;
    for (char& c : norm) if (c == '\\') c = '/';
    if (!norm.empty() && norm[0] == '/') return false;
    if (norm.size() >= 2 && norm[1] == ':') return false;
    fs::path rel(norm);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    out_rel = rel;
    return true;
}

std::string make_archive_stamp(const fs::path& p) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec) return {};
    const auto wt = fs::last_write_time(p, ec);
    if (ec) return {};
    return std::to_string(sz) + ":" + std::to_string(wt.time_since_epoch().count());
}

std::string read_small(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), {});
    return s;
}

bool find_mod_root(const fs::path& extracted_dir, fs::path& out_root) {
    std::error_code ec;
    if (fs::is_regular_file(extracted_dir / "mod.json", ec)) {
        out_root = extracted_dir;
        return true;
    }
    for (const auto& e : fs::directory_iterator(extracted_dir, ec)) {
        if (ec) break;
        std::error_code sub_ec;
        if (e.is_directory(sub_ec) && fs::is_regular_file(e.path() / "mod.json", sub_ec)) {
            out_root = e.path();
            return true;
        }
    }
    return false;
}

}  // namespace

bool extract_mod_archive(const fs::path& archive_path, const fs::path& dest_dir, fs::path& out_mod_root,
                         std::string& error) {
    mz_zip_archive zip{};
    const std::string path_str = archive_path.string();
    if (!mz_zip_reader_init_file(&zip, path_str.c_str(), 0)) {
        error = "cannot open archive (invalid zip)";
        return false;
    }

    std::error_code ec;
    fs::remove_all(dest_dir, ec);
    fs::create_directories(dest_dir, ec);
    if (ec) {
        mz_zip_reader_end(&zip);
        error = "cannot create extraction directory";
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    uint64_t total_bytes = 0;
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            error = "corrupted zip entry";
            return false;
        }
        fs::path rel;
        if (!is_safe_zip_relpath(st.m_filename, rel)) {
            mz_zip_reader_end(&zip);
            error = std::string("unsafe path in archive: ") + st.m_filename;
            return false;
        }
        const fs::path dst = dest_dir / rel;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(dst, ec);
            continue;
        }
        total_bytes += st.m_uncomp_size;
        if (total_bytes > kMaxArchiveTotalBytes) {
            mz_zip_reader_end(&zip);
            error = "archive exceeds 256 MiB limit";
            return false;
        }
        fs::create_directories(dst.parent_path(), ec);
        const std::string dst_str = dst.string();
        if (!mz_zip_reader_extract_to_file(&zip, i, dst_str.c_str(), 0)) {
            mz_zip_reader_end(&zip);
            error = std::string("failed to extract ") + st.m_filename;
            return false;
        }
    }
    mz_zip_reader_end(&zip);

    if (!find_mod_root(dest_dir, out_mod_root)) {
        error = "no mod.json found in archive";
        return false;
    }
    return true;
}

std::vector<DiscoveredBundle> discover_mod_bundles(const fs::path& mods_dir, const fs::path& cache_dir) {
    std::vector<DiscoveredBundle> out;
    std::error_code ec;
    if (mods_dir.empty() || !fs::is_directory(mods_dir, ec)) return out;

    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
        if (ec) break;
        const std::string fname = entry.path().filename().string();
        if (fname.empty() || fname[0] == '.') continue;  // skip .cache, .data, hidden folders

        std::error_code sub_ec;
        if (entry.is_directory(sub_ec)) {
            if (fs::is_regular_file(entry.path() / "mod.json", sub_ec)) {
                out.push_back({entry.path(), entry.path(), false, {}});
            }
        } else if (entry.is_regular_file(sub_ec) && is_archive_ext(entry.path())) {
            DiscoveredBundle b;
            b.source_path = entry.path();
            b.is_archive = true;
            const std::string stem = entry.path().stem().string();
            const fs::path extract_dir = cache_dir / stem;
            const fs::path stamp_file = cache_dir / (stem + ".stamp");
            const std::string want_stamp = make_archive_stamp(entry.path());
            fs::path root;
            if (!want_stamp.empty() && read_small(stamp_file) == want_stamp && find_mod_root(extract_dir, root)) {
                b.content_dir = root;
            } else {
                std::string err;
                if (extract_mod_archive(entry.path(), extract_dir, root, err)) {
                    b.content_dir = root;
                    fs::create_directories(cache_dir, sub_ec);
                    std::ofstream sf(stamp_file, std::ios::binary | std::ios::trunc);
                    sf << want_stamp;
                } else {
                    b.extract_error = err;
                }
            }
            out.push_back(std::move(b));
        }
    }
    return out;
}

bool stage_native_library(const fs::path& src_lib, const fs::path& staging_dir, const std::string& mod_id,
                          uint64_t generation, fs::path& out_staged, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(src_lib, ec)) {
        error = "native library not found: " + src_lib.string();
        return false;
    }
    const fs::path mod_stage = staging_dir / (mod_id + "_" + std::to_string(generation));
    fs::create_directories(mod_stage, ec);
    if (ec) {
        error = "cannot create native staging directory";
        return false;
    }
    // Copy the entire sibling lib/<platform>/ directory so runtime dependency DLLs/.so resolve beside the mod!
    const fs::path src_dir = src_lib.parent_path();
    for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
        if (ec) break;
        std::error_code sub_ec;
        if (!entry.is_regular_file(sub_ec)) continue;
        fs::copy_file(entry.path(), mod_stage / entry.path().filename(), fs::copy_options::overwrite_existing, sub_ec);
    }
    out_staged = mod_stage / src_lib.filename();
    if (!fs::is_regular_file(out_staged, ec)) {
        error = "failed to stage native library";
        return false;
    }
    return true;
}

}  // namespace ph
