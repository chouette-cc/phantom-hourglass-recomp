// Online & local Mod Catalog client for Phantom Hourglass Recomp.
// Fetches a static JSON catalog (e.g. hosted on GitHub), verifies SHA-256 checksums and .phmod
// archive integrity, and installs/updates/removes mods atomically without blocking the game loop.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ph {

inline constexpr const char* kDefaultModCatalogUrl =
    "https://raw.githubusercontent.com/chouette-cc/ph-recomp-mods/main/catalog.json";

struct CatalogModEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string kind = "lua";  // "asset", "lua", "native"
    std::string author;
    std::string description;
    std::string download_url;
    std::string sha256;        // optional lowercase 64-hex SHA-256
    std::string homepage;
    std::vector<std::string> tags;
    std::vector<std::string> regions;
};

struct ModCatalogSnapshot {
    enum class Status { Idle, Fetching, Ready, Error };
    Status status = Status::Idle;
    std::string catalog_name;
    std::string catalog_url;
    std::string error;
    std::vector<CatalogModEntry> entries;
    bool installing = false;
    std::string installing_id;
    std::string last_message;
};

// Pure C++20 FIPS 180-4 SHA-256, returned as 64 lowercase hex characters.
std::string sha256_hex(const uint8_t* data, size_t len);
std::string sha256_hex(const std::string& data);

// Compares dotted numeric versions (e.g. "1.2.0" vs "1.10.0"). Returns <0, 0, or >0.
int compare_mod_versions(const std::string& a, const std::string& b);

// Parses a catalog.json document into `out_entries`. Returns false with `error` on malformed JSON.
bool parse_mod_catalog(const std::string& json_text, std::string& out_catalog_name,
                       std::vector<CatalogModEntry>& out_entries, std::string& error);

// Resolves the catalog URL: explicit CLI override > `<mods_dir>/catalog.url` > `kDefaultModCatalogUrl`.
std::string resolve_mod_catalog_url(const std::filesystem::path& mods_dir, const std::string& cli_override = {});

// Synchronous HTTP(S) or `file://` fetch (max_bytes safety cap). Supports WinHTTP on Windows.
bool fetch_url_bytes(const std::string& url, std::vector<uint8_t>& out, std::string& error,
                     size_t max_bytes = 32u * 1024u * 1024u);

// Installs a catalog mod synchronously (downloads, verifies SHA-256 & valid .phmod with mod.json,
// replaces any older version with the same id in `mods_dir`).
bool install_catalog_mod(const CatalogModEntry& entry, const std::filesystem::path& mods_dir, std::string& error);

// Removes an installed mod (.phmod, .zip, or unpacked folder matching `mod_id`) and its cache directory.
bool uninstall_mod_bundle(const std::filesystem::path& mods_dir, const std::string& mod_id, std::string& error);

// Non-blocking background client used by the PC menu so network I/O never stalls rendering.
class ModCatalogClient {
public:
    ModCatalogClient() = default;
    ~ModCatalogClient();

    ModCatalogClient(const ModCatalogClient&) = delete;
    ModCatalogClient& operator=(const ModCatalogClient&) = delete;

    void set_config(const std::filesystem::path& mods_dir, const std::string& catalog_url);
    void fetch_async();
    void install_async(const CatalogModEntry& entry);

    ModCatalogSnapshot snapshot() const;
    // Returns true once after an async install finishes (with `out_msg` and `out_ok`), signaling a mod reload.
    bool consume_install_finished(bool& out_ok, std::string& out_msg);

private:
    void join_worker();

    mutable std::mutex mutex_;
    ModCatalogSnapshot state_;
    std::filesystem::path mods_dir_;
    std::thread worker_;
    std::atomic<bool> worker_busy_{false};
    std::atomic<bool> install_done_{false};
    bool install_ok_ = false;
    std::string install_msg_;
};

}  // namespace ph
