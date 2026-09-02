#pragma once

#include <string>
#include <optional>
#include <vector>
#include <filesystem>

// Forward declarations — nlohmann/json will be included in the .cpp
struct json;

namespace pacmkr::aur {

/// AUR package info from search API.
struct AurPackage {
    std::string name;
    std::string pkgname;       // package base
    std::optional<std::string> desc;
    std::optional<std::string> url;
    unsigned int numvotes{0};
    std::optional<unsigned long long> outofdate;
    std::optional<unsigned long long> firstsubmitted;
    unsigned long long lastmodified{};
};

/// Full AUR package info (includes dependency lists, sources, checksums).
struct AurPackageInfo {
    std::string name;
    std::string pkgname;
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

/// Download PKGBUILD from AUR git repo into dest directory.
/// Returns the path to the PKGBUILD file.
std::filesystem::path download_pkgbuild(const std::string& pkgname,
                                         const std::filesystem::path& dest);

/// Check if a package exists in official repos.
bool is_official_package(const std::string& pkgname);

/// Check if a package exists in AUR.
bool is_aur_package(const std::string& pkgname);

/// Display search results (interactive or list mode).
void search_aur(const std::string& query, unsigned int limit);

/// Hybrid sync search: query both official repos and AUR.
void hybrid_sync_search(const std::string& query, unsigned int limit);

/// Hybrid sync info: query both official repos and AUR for a package.
void hybrid_sync_info(const std::string& pkgname);

} // namespace pacmkr::aur
