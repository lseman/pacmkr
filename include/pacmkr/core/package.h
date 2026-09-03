#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace pacmkr::pkgbuild { struct Pkgbuild; }

namespace pacmkr::package {

/// Represents a built package ready for installation.
struct Package {
    std::string name, version, arch;
    std::filesystem::path dest;

    static Package make(const std::string& name, const std::string& version,
                        const std::string& arch, const std::filesystem::path& dest_dir);

    /// Write .PKGINFO metadata file.
    void write_pkginfo(const pkgbuild::Pkgbuild& pkgbuild,
                       const std::filesystem::path& pkgdir,
                       const std::string& packager,
                       uint64_t builddate) const;

    /// Create the final .pkg.tar.zst archive using bsdtar.
    void create_archive(const std::filesystem::path& pkgdir) const;
};

/// Calculate directory size in bytes.
uint64_t dir_size(const std::filesystem::path& path);

} // namespace pacmkr::package
