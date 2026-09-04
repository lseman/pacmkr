#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>

namespace pacmkr::pkgbuild {

struct PackageSection {
    std::string name, body;
};

/// Parsed representation of a PKGBUILD file.
struct Pkgbuild {
    std::filesystem::path source_path;
    std::string raw_content;
    std::map<std::string, std::string> variables;
    std::vector<std::string> pkgname;
    std::optional<std::string> pkgbase_opt;
    std::optional<std::string> epoch;
    std::string pkgver{"0"}, pkgrel{"1"};
    std::string desc, url, install, changelog;
    std::vector<std::string> arch, license;
    std::vector<std::string> depends, makedepends, checkdepends, optdepends;
    std::vector<std::string> options, backup;
    std::vector<std::string> conflicts, provides, replaces, groups;
    std::vector<std::string> source, noextract, validpgpkeys;
    std::vector<std::string> md5sums, sha256sums, sha512sums;
    std::vector<std::string> b2sums, sha384sums, sha224sums, sha1sums;
    std::vector<PackageSection> packages;
    std::vector<std::string> functions;

    /// Parse and evaluate a PKGBUILD file with Bash-compatible semantics.
    /// Callers must perform any required trust review before invoking this.
    static Pkgbuild parse(const std::filesystem::path& path);

    /// Parse PKGBUILD content without executing it (for inspection and tests).
    static Pkgbuild parse_content(const std::string& content);

    /// Get the effective package base name.
    std::string pkgbase() const;

    /// Get the full version string (pkgbase-pkgver-pkgrel).
    std::string full_version() const;

    /// Get the package version (epoch:pkgver-pkgrel when an epoch is set).
    std::string version() const;

    /// Run pkgver() after sources are prepared and adopt its returned version.
    void update_version(const std::filesystem::path& srcdir);

    /// Merge metadata captured after a split package_*() function ran.
    void merge_function_metadata(const std::filesystem::path& path);

    /// Check if a specific function exists in the PKGBUILD.
    bool has_function(const std::string& name) const;
};

} // namespace pacmkr::pkgbuild
