// Online & local Mod Catalog client implementation (pure SHA-256, WinHTTP / file:// / curl, safe .phmod install).
#include "mod_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "mod_bundle.hpp"
#include "mod_json.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace ph {

namespace fs = std::filesystem;

namespace {

std::string sanitize_id(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out.push_back(char(std::tolower(c)));
        else if (c == ' ') out.push_back('-');
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    s.erase(0, i);
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string read_small_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Parse `file://` URL to a local filesystem path (for offline testing and local catalogs).
bool parse_file_url(const std::string& url, fs::path& out_path) {
    if (url.rfind("file://", 0) != 0) return false;
    std::string p = url.substr(7);
    // Strip leading localhost if present
    if (p.rfind("localhost/", 0) == 0) p.erase(0, 9);
#ifdef _WIN32
    // `file:///C:/foo` -> `C:/foo`
    if (p.size() >= 3 && p[0] == '/' && std::isalpha(static_cast<unsigned char>(p[1])) && p[2] == ':') p.erase(0, 1);
#endif
    // Decode basic %20 escapes
    std::string dec;
    dec.reserve(p.size());
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size() && std::isxdigit(static_cast<unsigned char>(p[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(p[i + 2]))) {
            char hex[3] = {p[i + 1], p[i + 2], '\0'};
            dec.push_back(char(std::strtoul(hex, nullptr, 16)));
            i += 2;
        } else {
            dec.push_back(p[i]);
        }
    }
    out_path = fs::path(dec);
    return !dec.empty();
}

// Reads the `id` field from a mod.json inside a folder or `.phmod`/`.zip` archive.
std::string bundle_mod_id(const fs::path& bundle_path, const fs::path& temp_dir) {
    std::error_code ec;
    fs::path manifest_path;
    if (fs::is_directory(bundle_path, ec)) {
        manifest_path = bundle_path / "mod.json";
    } else if (fs::is_regular_file(bundle_path, ec)) {
        std::string ext = to_lower(bundle_path.extension().string());
        if (ext != ".phmod" && ext != ".zip") return {};
        fs::path mod_root;
        std::string err;
        fs::remove_all(temp_dir, ec);
        if (!extract_mod_archive(bundle_path, temp_dir, mod_root, err)) {
            fs::remove_all(temp_dir, ec);
            return sanitize_id(bundle_path.stem().string());
        }
        manifest_path = mod_root / "mod.json";
    } else {
        return {};
    }
    const std::string text = read_small_text(manifest_path);
    json::Value root;
    std::string err;
    std::string id;
    if (json::parse(text, root, err) && root.is_object()) id = sanitize_id(root.string_or("id"));
    if (!fs::is_directory(bundle_path, ec)) fs::remove_all(temp_dir, ec);
    return id.empty() ? sanitize_id(bundle_path.stem().string()) : id;
}

}  // namespace

// ---- FIPS 180-4 SHA-256 ----------------------------------------------------------------------------------------

std::string sha256_hex(const uint8_t* data, size_t len) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    auto rotr = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); };
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    auto process_block = [&](const uint8_t blk[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(blk[i * 4]) << 24) | (uint32_t(blk[i * 4 + 1]) << 16) |
                   (uint32_t(blk[i * 4 + 2]) << 8) | uint32_t(blk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };

    size_t pos = 0;
    while (pos + 64 <= len) {
        process_block(data + pos);
        pos += 64;
    }
    uint8_t tail[128]{};
    const size_t rem = len - pos;
    if (rem > 0 && data) std::memcpy(tail, data + pos, rem);
    tail[rem] = 0x80u;
    const size_t pad_blocks = (rem + 9 <= 64) ? 1 : 2;
    const uint64_t bit_len = uint64_t(len) * 8u;
    for (int i = 0; i < 8; i++) tail[pad_blocks * 64 - 1 - size_t(i)] = uint8_t((bit_len >> (i * 8)) & 0xFFu);
    process_block(tail);
    if (pad_blocks == 2) process_block(tail + 64);

    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

std::string sha256_hex(const std::string& data) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ---- Version comparison & JSON catalog parsing ----------------------------------------------------------------

int compare_mod_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> parts;
        size_t i = 0;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) i = 1;
        while (i < s.size()) {
            int val = 0;
            bool any = false;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                val = val * 10 + (s[i] - '0');
                any = true;
                i++;
            }
            if (any) parts.push_back(val);
            while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) i++;
        }
        return parts;
    };
    const auto pa = split(a), pb = split(b);
    const size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; i++) {
        const int va = i < pa.size() ? pa[i] : 0;
        const int vb = i < pb.size() ? pb[i] : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

bool parse_mod_catalog(const std::string& json_text, std::string& out_catalog_name,
                       std::vector<CatalogModEntry>& out_entries, std::string& error) {
    out_entries.clear();
    out_catalog_name = "Community Mods";
    json::Value root;
    if (!json::parse(json_text, root, error) || !root.is_object()) {
        if (error.empty()) error = "catalog root must be a JSON object";
        return false;
    }
    out_catalog_name = root.string_or("name", out_catalog_name);
    const json::Value* mods = root.get("mods");
    if (!mods || !mods->is_array()) {
        error = "catalog.json is missing a 'mods' array";
        return false;
    }
    for (const json::Value& item : mods->arr) {
        if (!item.is_object()) continue;
        CatalogModEntry e;
        e.id = sanitize_id(item.string_or("id"));
        e.name = item.string_or("name", e.id);
        e.version = item.string_or("version", "1.0.0");
        e.kind = to_lower(item.string_or("kind", "lua"));
        if (e.kind != "asset" && e.kind != "lua" && e.kind != "native") e.kind = "lua";
        e.author = item.string_or("author");
        e.description = item.string_or("description");
        e.download_url = trim(item.string_or("download_url"));
        e.sha256 = to_lower(trim(item.string_or("sha256")));
        e.homepage = trim(item.string_or("homepage"));
        if (const json::Value* tags = item.get("tags"); tags && tags->is_array()) {
            for (const auto& t : tags->arr)
                if (t.is_string() && !t.str.empty()) e.tags.push_back(t.str);
        }
        if (const json::Value* regs = item.get("regions"); regs && regs->is_array()) {
            for (const auto& r : regs->arr)
                if (r.is_string() && !r.str.empty()) e.regions.push_back(r.str);
        }
        if (!e.id.empty() && !e.download_url.empty()) out_entries.push_back(std::move(e));
    }
    return true;
}

std::string resolve_mod_catalog_url(const fs::path& mods_dir, const std::string& cli_override) {
    if (!trim(cli_override).empty()) return trim(cli_override);
    const fs::path url_file = mods_dir / "catalog.url";
    std::error_code ec;
    if (fs::is_regular_file(url_file, ec)) {
        std::ifstream f(url_file);
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (!line.empty() && line[0] != '#') return line;
        }
    }
    return kDefaultModCatalogUrl;
}

// ---- HTTPS / file:// fetcher ----------------------------------------------------------------------------------

bool fetch_url_bytes(const std::string& url, std::vector<uint8_t>& out, std::string& error, size_t max_bytes) {
    out.clear();
    if (url.rfind("file://", 0) == 0) {
        fs::path p;
        if (!parse_file_url(url, p)) return error = "invalid file:// URL", false;
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return error = "cannot open " + p.string(), false;
        const auto sz = f.tellg();
        if (sz < 0 || size_t(sz) > max_bytes) return error = "file too large or unreadable", false;
        out.resize(size_t(sz));
        f.seekg(0);
        if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
        return bool(f);
    }

    if (url.rfind("https://", 0) != 0) {
        return error = "only https:// (or local file://) URLs are allowed", false;
    }

#ifdef _WIN32
    auto to_wide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    };

    const std::wstring wurl = to_wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), DWORD(wurl.size()), 0, &uc)) {
        return error = "invalid URL format", false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        return error = "only HTTPS URLs are permitted", false;
    }

    HINTERNET hSession = WinHttpOpen(L"PhantomHourglassRecomp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return error = "WinHttpOpen failed", false;
    WinHttpSetTimeouts(hSession, 8000, 8000, 12000, 20000);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return error = "could not connect to host", false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return error = "could not open HTTPS request", false;
    }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
        if (status >= 200 && status < 300) {
            ok = true;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                if (out.size() + avail > max_bytes) {
                    error = "download exceeds maximum allowed size";
                    ok = false;
                    break;
                }
                const size_t old_sz = out.size();
                out.resize(old_sz + avail);
                DWORD read_n = 0;
                if (!WinHttpReadData(hRequest, out.data() + old_sz, avail, &read_n)) {
                    error = "error while reading response";
                    ok = false;
                    break;
                }
                out.resize(old_sz + read_n);
            }
        } else {
            error = "HTTP " + std::to_string(status);
        }
    } else {
        const DWORD err_code = GetLastError();
        error = "network request failed (err " + std::to_string(err_code) + ")";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
#elif defined(__ANDROID__)
    (void)max_bytes;
    return error = "online catalog runs through the Android launcher", false;
#else
    // POSIX desktop fallback via curl (no extra library dependency).
    std::string safe_url;
    for (char c : url) {
        if (c == '\'' || c == '\n' || c == '\r' || c == '\0') return error = "invalid characters in URL", false;
        safe_url.push_back(c);
    }
    const std::string cmd = "curl -fsSL --max-time 25 --max-filesize " + std::to_string(max_bytes) + " '" + safe_url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return error = "could not invoke curl", false;
    uint8_t buf[16384];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        if (out.size() + n > max_bytes) {
            pclose(pipe);
            out.clear();
            return error = "download exceeds maximum allowed size", false;
        }
        out.insert(out.end(), buf, buf + n);
    }
    const int rc = pclose(pipe);
    if (rc != 0) {
        out.clear();
        return error = "curl failed (exit " + std::to_string(rc) + ")", false;
    }
    return true;
#endif
}

// ---- Install & Uninstall --------------------------------------------------------------------------------------

bool install_catalog_mod(const CatalogModEntry& entry, const fs::path& mods_dir, std::string& error) {
    const std::string clean_id = sanitize_id(entry.id);
    if (clean_id.empty()) return error = "invalid mod id", false;
    if (entry.download_url.empty()) return error = "missing download_url", false;

    std::vector<uint8_t> bytes;
    if (!fetch_url_bytes(entry.download_url, bytes, error)) return false;
    if (bytes.size() < 22) return error = "downloaded file is too small to be a .phmod archive", false;

    if (!entry.sha256.empty()) {
        const std::string got = sha256_hex(bytes.data(), bytes.size());
        if (got != to_lower(entry.sha256)) {
            return error = "SHA-256 mismatch (expected " + entry.sha256.substr(0, 12) + "..., got " +
                           got.substr(0, 12) + "...)",
                   false;
        }
    }

    std::error_code ec;
    fs::create_directories(mods_dir, ec);
    const fs::path tmp_pkg = mods_dir / (".dl-" + clean_id + ".phmod.tmp");
    const fs::path verify_dir = mods_dir / ".cache" / (".verify-" + clean_id);
    {
        std::ofstream f(tmp_pkg, std::ios::binary | std::ios::trunc);
        if (!f) return error = "cannot write temporary download file", false;
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) {
            fs::remove(tmp_pkg, ec);
            return error = "failed writing downloaded archive", false;
        }
    }

    // Validate ZIP integrity and presence of valid mod.json before installing!
    fs::path mod_root;
    fs::remove_all(verify_dir, ec);
    const bool valid_zip = extract_mod_archive(tmp_pkg, verify_dir, mod_root, error);
    fs::remove_all(verify_dir, ec);
    if (!valid_zip) {
        fs::remove(tmp_pkg, ec);
        return false;
    }

    // Remove any existing installed copy of this mod ID (older .phmod / .zip / unpacked folder)
    std::string dummy_err;
    uninstall_mod_bundle(mods_dir, clean_id, dummy_err);

    const fs::path dest = mods_dir / (clean_id + ".phmod");
    fs::rename(tmp_pkg, dest, ec);
    if (ec) {
        fs::copy_file(tmp_pkg, dest, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_pkg, ec);
        if (ec) return error = "failed moving archive into mods folder: " + ec.message(), false;
    }
    return true;
}

bool uninstall_mod_bundle(const fs::path& mods_dir, const std::string& mod_id, std::string& error) {
    const std::string want = sanitize_id(mod_id);
    if (want.empty()) return error = "invalid mod id", false;
    std::error_code ec;
    if (!fs::is_directory(mods_dir, ec)) return error = "mods directory does not exist", false;

    const fs::path probe_tmp = mods_dir / ".cache" / ".probe_uninstall";
    bool removed_any = false;
    std::vector<fs::path> targets;
    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
        if (ec) break;
        const std::string fname = entry.path().filename().string();
        if (fname.empty() || fname[0] == '.') continue;
        const std::string id = bundle_mod_id(entry.path(), probe_tmp);
        if (id == want) targets.push_back(entry.path());
    }
    fs::remove_all(probe_tmp, ec);

    for (const fs::path& t : targets) {
        const std::string stem = sanitize_id(t.stem().string());
        fs::remove_all(t, ec);
        if (!ec) {
            removed_any = true;
            if (!stem.empty()) fs::remove_all(mods_dir / ".cache" / stem, ec);
        }
    }
    if (!removed_any) {
        error = "mod '" + mod_id + "' not found on disk";
        return false;
    }
    return true;
}

// ---- ModCatalogClient (async worker) --------------------------------------------------------------------------

ModCatalogClient::~ModCatalogClient() { join_worker(); }

void ModCatalogClient::join_worker() {
    if (worker_.joinable()) worker_.join();
}

void ModCatalogClient::set_config(const fs::path& mods_dir, const std::string& catalog_url) {
    std::lock_guard<std::mutex> lock(mutex_);
    mods_dir_ = mods_dir;
    state_.catalog_url = catalog_url;
}

void ModCatalogClient::fetch_async() {
    if (worker_busy_.exchange(true)) return;
    join_worker();
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.status = ModCatalogSnapshot::Status::Fetching;
        state_.error.clear();
        url = state_.catalog_url;
    }
    worker_ = std::thread([this, url]() {
        std::vector<uint8_t> raw;
        std::string err;
        std::string name;
        std::vector<CatalogModEntry> entries;
        bool ok = fetch_url_bytes(url, raw, err, 2u * 1024u * 1024u);
        if (ok) {
            const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
            ok = parse_mod_catalog(text, name, entries, err);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) {
                state_.status = ModCatalogSnapshot::Status::Ready;
                state_.catalog_name = name;
                state_.entries = std::move(entries);
                state_.error.clear();
            } else {
                state_.status = ModCatalogSnapshot::Status::Error;
                state_.error = err.empty() ? "failed to load catalog" : err;
            }
        }
        worker_busy_.store(false);
    });
}

void ModCatalogClient::install_async(const CatalogModEntry& entry) {
    if (worker_busy_.exchange(true)) return;
    join_worker();
    fs::path dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = mods_dir_;
        state_.installing = true;
        state_.installing_id = entry.id;
        state_.last_message = "Downloading " + entry.name + "...";
    }
    worker_ = std::thread([this, entry, dir]() {
        std::string err;
        const bool ok = install_catalog_mod(entry, dir, err);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.installing = false;
            state_.installing_id.clear();
            install_ok_ = ok;
            install_msg_ = ok ? ("Installed " + entry.name + " v" + entry.version)
                              : ("Install failed: " + err);
            state_.last_message = install_msg_;
        }
        install_done_.store(true);
        worker_busy_.store(false);
    });
}

ModCatalogSnapshot ModCatalogClient::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool ModCatalogClient::consume_install_finished(bool& out_ok, std::string& out_msg) {
    if (!install_done_.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    out_ok = install_ok_;
    out_msg = install_msg_;
    return true;
}

}  // namespace ph
