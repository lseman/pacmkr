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
    std::vector<std::string> pkgname;
    std::optional<std::string> pkgbase_opt;
    std::string pkgver{"0"}, pkgrel{"1"};
    std::string desc, url;
    std::vector<std::string> arch, license;
    std::vector<std::string> depends, makedepends, checkdepends, optdepends;
    std::vector<std::string> options, backup;
    std::vector<std::string> conflicts, provides, replaces, groups;
    std::vector<std::string> source, validpgpkeys;
    std::vector<std::string> md5sums, sha256sums, sha512sums;
    std::vector<std::string> b2sums, sha384sums, sha224sums, sha1sums;
    std::vector<PackageSection> packages;

    /// Parse a PKGBUILD file from disk.
    static Pkgbuild parse(const std::filesystem::path& path);

    /// Parse PKGBUILD content from a string.
    static Pkgbuild parse_content(const std::string& content);

    /// Get the effective package base name.
    std::string pkgbase() const;

    /// Get the full version string (pkgbase-pkgver-pkgrel).
    std::string full_version() const;

    /// Check if a specific function exists in the PKGBUILD.
    bool has_function(const std::string& name) const;
};

} // namespace pacmkr::pkgbuild
