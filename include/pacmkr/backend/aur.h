#pragma once

#include <string>
#include <optional>
#include <vector>
#include <filesystem>

// Forward declarations
// Note: nlohmann::json is pulled in via httplib.h or json.hpp in .cpp files.

namespace pacmkr::aur {

/// AUR package info from search API.
struct AurPackage {
    std::string name;
    std::string pkgname;       // package base
    std::optional<std::string> desc;
    std::optional<std::string> url;
    unsigned int numvotes{0};
    double popularity{0.0};     // AUR v5 API Popularity score (0-∞)
    double relevance_score{0.0}; // Client-side fuzzy match score
    std::optional<unsigned long long> outofdate;
    std::optional<unsigned long long> firstsubmitted;
    unsigned long long lastmodified{};
};

/// Full AUR package info (includes dependency lists, sources, checksums).
struct AurPackageInfo {
    std::string name;
    std::string pkgname;
    std::string version;
    std::optional<std::string> desc;
    std::optional<std::string> url;
    std::vector<std::string> license;
    std::vector<std::string> depends;
    std::vector<std::string> makedepends;
    std::vector<std::string> checkdepends;
    std::vector<std::string> optdepends;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
    std::vector<std::string> backup;
    std::vector<std::string> options;
    std::vector<std::string> source;
    std::vector<std::string> sha256sums;
    std::vector<std::string> md5sums;
    std::vector<std::string> sha1sums;
    std::vector<std::string> sha384sums;
    std::vector<std::string> sha512sums;

    // Split package support (AUR RPC v5)
    std::vector<std::string> subpackages;  // Names of sub-packages in this base
};

/// Search AUR for packages matching a query.
std::vector<AurPackage> search(const std::string& query);

/// Fetch full info for a single AUR package.
AurPackageInfo fetch(const std::string& pkgname);

/// Fetch full info for multiple AUR packages in one RPC call.
std::vector<AurPackageInfo> fetch_batch(const std::vector<std::string>& pkgnames);

/// Download PKGBUILD from AUR git repo into dest directory.
/// Returns the path to the PKGBUILD file.
std::filesystem::path download_pkgbuild(const std::string& pkgname,
                                         const std::filesystem::path& dest);

/// Check if a package exists in official repos.
bool is_official_package(const std::string& pkgname);

/// Check if a package exists in AUR.
bool is_aur_package(const std::string& pkgname);

/// Display search results (interactive or list mode).
void search_aur(const std::string& query, unsigned int limit, bool json_mode = false);

/// Hybrid sync search: query both official repos and AUR.
void hybrid_sync_search(const std::string& query, unsigned int limit, bool json_mode = false);

/// Hybrid sync info: query both official repos and AUR for a package.
void hybrid_sync_info(const std::string& pkgname, bool json_mode = false);

} // namespace pacmkr::aur
