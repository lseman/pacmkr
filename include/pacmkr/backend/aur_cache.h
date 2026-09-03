#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <filesystem>
#include <json.hpp>

namespace pacmkr::aur_cache {

/// Sort order for AUR search results.
enum class SortOrder { Votes, Updated, Popular };

/// Initialize cache: load from disk or fetch fresh data.
void init(bool force_refresh = false);

/// Shutdown: nothing to do, cache is in-memory.
void shutdown();

/// Get all cached AUR package names (for iteration).
std::vector<std::string> get_all_names();

/// Get a single cached AUR package by name (fast lookup).
std::optional<nlohmann::json> get(const std::string& name);

/// Search cached AUR packages by query (name or description).
struct SearchResult {
    std::string name;
    std::string pkgname;
    std::string desc;
    unsigned int numvotes;
    unsigned long long outofdate_ts{};  // For --sortby=updated
};
std::vector<SearchResult> search(const std::string& query, unsigned int limit = 10, SortOrder order = SortOrder::Votes);

/// Check if a package exists in the AUR cache.
bool has(const std::string& name);

/// Whether cached metadata is recent enough for an upgrade check.
bool is_fresh();

/// Merge AUR RPC result objects into the memory and disk cache.
void merge_results(const nlohmann::json& results);

/// Record that `names` were just checked against the AUR. Names absent from the
/// merged results are thereby known to not exist in the AUR (rather than merely
/// missing from the cache), so a later upgrade check need not re-query them.
void note_checked(const std::vector<std::string>& names);

/// Whether `name` was checked against the AUR recently enough to trust the
/// cache (present -> use cached version; absent -> not an AUR package).
bool checked_recently(const std::string& name);

/// Get the out-of-date status for a local package (from cache).
struct OutOfDateInfo {
    bool is_out_of_date;
    std::string aur_version;  // from AUR JSON "pkgver" or git clone
    unsigned long long outofdate_ts;  // AUR outofdate timestamp (0 = up-to-date)
};
OutOfDateInfo check_ootd(const std::string& pkgname, const std::string& local_version);

/// Get cache file path.
std::filesystem::path cache_path();

/// Force refresh the cache from the AUR API.
void refresh();

} // namespace pacmkr::aur_cache
