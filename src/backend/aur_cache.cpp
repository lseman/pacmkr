#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/core/error.h"

#include <map>
#include <httplib.h>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cctype>

namespace pacmkr::aur_cache {

namespace {

using json = nlohmann::json;

/// Path to the cache file.
static std::filesystem::path get_cache_path() {
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base;
    if (xdg_cache && std::string(xdg_cache).size() > 0) {
        return std::filesystem::path{xdg_cache} / "pacmkr" / "aur-cache.json";
    } else {
        const char* home = std::getenv("HOME");
        if (!home) return std::filesystem::temp_directory_path() / "pacmkr" / "aur-cache.json";
        base = home;
    }
    return base / ".cache" / "pacmkr" / "aur-cache.json";
}

/// Fetch all AUR packages via the RPC API.
static json fetch_all_packages() {
    static httplib::Client cli("https://aur.archlinux.org");
    cli.set_connection_timeout(std::chrono::seconds(10));
    cli.set_read_timeout(std::chrono::seconds(60));

    auto res = cli.Get("/rpc?v=5&type=all");
    if (!res || res->status != 200) {
        throw source_error("Failed to fetch AUR package list (HTTP " +
                           std::to_string(res ? res->status : 0) + ")");
    }

    return json::parse(res->body);
}

/// Simple Arch Linux version comparison.
static int ver_cmp(const std::string& a, const std::string& b) {
    auto lower = [](std::string s) -> std::string {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    std::string la = lower(a), lb = lower(b);

    // Arch keyword ordering
    static const char* keywords[] = {"", "pre", "c", "rc", "a", "b", "ssl", "rel"};
    auto kw_pos = [&](const std::string& s) -> int {
        for (int i = 0; i < static_cast<int>(sizeof(keywords)/sizeof(keywords[0])); ++i) {
            if (keywords[i] == s) return i;
        }
        return -1;
    };

    int ai = kw_pos(la), bi = kw_pos(lb);
    if (ai >= 0 && bi >= 0) return (ai < bi ? -1 : (ai > bi ? 1 : 0));
    if (ai >= 0) return -1;
    if (bi >= 0) return 1;

    // Numeric comparison for alpha segments containing digits
    auto a_num = la.find_first_of("0123456789");
    auto b_num = lb.find_first_of("0123456789");
    if (a_num != std::string::npos && b_num != std::string::npos) {
        unsigned long long an = std::stoull(la.substr(a_num));
        unsigned long long bn = std::stoull(lb.substr(b_num));
        if (an != bn) return (an < bn ? -1 : 1);
    }

    return la.compare(lb);
}

} // anonymous

// How long an AUR check stays trustworthy for upgrade detection. The real
// build always pulls fresh PKGBUILDs, so this only bounds the staleness of the
// "is an update available" pre-check.
static constexpr auto kCheckTtl = std::chrono::hours(3);

/// Seconds since the Unix epoch.
static long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ─── Global state (file-local, accessible to all functions in this file) ──

static std::map<std::string, nlohmann::json> g_aur_data;  // name -> full JSON object
static std::map<std::string, long long> g_checked;        // name -> last-checked unix time
static bool g_initialized{false};
static bool g_loaded_from_disk{false};

// ─── Helper implementations (defined after global state) ──

static bool load_from_disk() {
    auto path = get_cache_path();
    if (!std::filesystem::exists(path)) return false;

    std::ifstream file(path);
    if (!file) return false;

    try {
        json data = json::parse(file);
        if (data.contains("checked") && data["checked"].is_object()) {
            for (auto& [name, ts] : data["checked"].items()) {
                if (ts.is_number()) g_checked[name] = ts.get<long long>();
            }
        }
        if (data.contains("packages") && data["packages"].is_array()) {
            for (auto& pkg : data["packages"]) {
                if (pkg.contains("Name")) {
                    std::string name = pkg["Name"].get<std::string>();
                    g_aur_data[name] = pkg;
                }
            }
            return true;
        }
    } catch (...) {}

    return false;
}

static void save_to_disk() {
    auto path = get_cache_path();
    std::filesystem::create_directories(path.parent_path());

    json data;
    data["packages"] = json::array();
    for (auto& [name, pkg] : g_aur_data) {
        data["packages"].push_back(pkg);
    }
    data["checked"] = json::object();
    for (auto& [name, ts] : g_checked) {
        data["checked"][name] = ts;
    }

    std::ofstream file(path);
    if (file) {
        file << data.dump(2);
    }
}

static bool cache_is_stale() {
    auto path = get_cache_path();
    if (!std::filesystem::exists(path)) return true;

    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return true;
    return std::filesystem::file_time_type::clock::now() - modified > kCheckTtl;
}

// ─── Public API ──────────────────────────────────────────────────────

void init(bool force_refresh) {
    if (g_initialized) return;

    // Load from disk first
    g_loaded_from_disk = load_from_disk();

    // Normal startup is disk-only. Upgrade checks refresh just the installed
    // foreign packages, avoiding a large global AUR request on the hot path.
    if (force_refresh) {
        try {
            json response = fetch_all_packages();
            if (response.contains("results") && response["results"].is_array()) {
                g_aur_data.clear();
                for (auto& pkg : response["results"]) {
                    if (pkg.contains("Name")) {
                        std::string name = pkg["Name"].get<std::string>();
                        g_aur_data[name] = pkg;
                    }
                }
                save_to_disk();
            }
        } catch (const source_error& e) {
            // Cache load failure is non-fatal — use what we have from disk
            std::cerr << "warning: failed to refresh AUR cache: " << e.what() << "\n";
            if (!g_loaded_from_disk) {
                std::cerr << "warning: no cached data available\n";
            }
        }
    }

    g_initialized = true;
}

void shutdown() {
    // Nothing to do — cache is in-memory only at shutdown
}

std::vector<std::string> get_all_names() {
    std::vector<std::string> names;
    names.reserve(g_aur_data.size());
    for (auto& [name, _] : g_aur_data) {
        names.push_back(name);
    }
    return names;
}

std::optional<nlohmann::json> get(const std::string& name) {
    auto it = g_aur_data.find(name);
    if (it != g_aur_data.end()) return it->second;
    return std::nullopt;
}

std::vector<SearchResult> search(const std::string& query, unsigned int limit, SortOrder order) {
    std::vector<SearchResult> results;
    std::string q_lower = query;
    std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);

    for (auto& [name, pkg] : g_aur_data) {
        bool matched = false;

        // Check name
        if (!name.empty()) {
            std::string n_lower = name;
            std::transform(n_lower.begin(), n_lower.end(), n_lower.begin(), ::tolower);
            if (n_lower.find(q_lower) != std::string::npos) {
                matched = true;
            }
        }

        // Check description
        if (!matched && pkg.contains("Description")) {
            std::string desc = pkg["Description"].get<std::string>();
            std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
            if (desc.find(q_lower) != std::string::npos) {
                matched = true;
            }
        }

        if (matched) {
            SearchResult sr{};
            sr.name = name;
            sr.pkgname = pkg.value("PackageBase", name);
            sr.desc = pkg.value("Description", "");
            sr.numvotes = pkg.value("NumVotes", 0u);
            sr.outofdate_ts = pkg.value("OutOfDate", 0ULL);
            results.push_back(std::move(sr));

            if (results.size() >= limit) break;
        }
    }

    // Sort based on requested order
    switch (order) {
        case SortOrder::Votes:
            std::sort(results.begin(), results.end(),
                      [](const SearchResult& a, const SearchResult& b) {
                          return a.numvotes > b.numvotes;
                      });
            break;
        case SortOrder::Updated:
            // Sort by last update timestamp (descending), outofdate_ts = 0 means never updated
            std::sort(results.begin(), results.end(),
                      [](const SearchResult& a, const SearchResult& b) {
                          if (a.outofdate_ts == 0 && b.outofdate_ts == 0) return a.name < b.name;
                          if (a.outofdate_ts == 0) return false;
                          if (b.outofdate_ts == 0) return true;
                          return a.outofdate_ts > b.outofdate_ts;
                      });
            break;
        case SortOrder::Popular:
            // Same as votes for now (could use Flairs/Popularity field if available)
            std::sort(results.begin(), results.end(),
                      [](const SearchResult& a, const SearchResult& b) {
                          return a.numvotes > b.numvotes;
                      });
            break;
    }

    return results;
}

bool has(const std::string& name) {
    return g_aur_data.find(name) != g_aur_data.end();
}

bool is_fresh() {
    return g_loaded_from_disk && !cache_is_stale();
}

void merge_results(const nlohmann::json& results) {
    if (!results.is_array()) return;
    for (const auto& pkg : results) {
        const auto name = pkg.value("Name", std::string{});
        if (!name.empty()) g_aur_data[name] = pkg;
    }
    save_to_disk();
    g_loaded_from_disk = true;
}

void note_checked(const std::vector<std::string>& names) {
    const long long now = now_unix();
    for (const auto& name : names) g_checked[name] = now;
    save_to_disk();
}

bool checked_recently(const std::string& name) {
    auto it = g_checked.find(name);
    if (it == g_checked.end()) return false;
    const auto age = std::chrono::seconds(now_unix() - it->second);
    return age >= std::chrono::seconds(0) && age < kCheckTtl;
}

OutOfDateInfo check_ootd(const std::string& pkgname, const std::string& local_version) {
    OutOfDateInfo info{};
    info.is_out_of_date = false;
    info.outofdate_ts = 0;

    auto it = g_aur_data.find(pkgname);
    if (it == g_aur_data.end()) return info;  // Not in AUR

    const auto& pkg = it->second;
    std::string aur_version = pkg.value("Version", "");
    info.aur_version = aur_version;
    info.outofdate_ts = pkg.value("OutOfDate", 0ULL);

    // Extract just the version part (without -rel)
    auto dash_pos = aur_version.find('-');
    if (dash_pos != std::string::npos) {
        aur_version = aur_version.substr(0, dash_pos);
    }

    // Compare versions (Arch Linux style)
    int cmp = ver_cmp(local_version, aur_version);
    info.is_out_of_date = (cmp < 0);  // local < aur means out of date

    return info;
}

std::filesystem::path cache_path() {
    return get_cache_path();
}

void refresh() {
    g_aur_data.clear();
    g_loaded_from_disk = false;
    g_initialized = false;
    init(true);
}

} // namespace pacmkr::aur_cache
