#pragma once

#include <string>
#include <filesystem>

namespace pacmkr::pkgbuild { struct Pkgbuild; }

namespace pacmkr::source {

/// Handler for downloading and verifying PKGBUILD sources.
struct SourceHandler {
    std::filesystem::path srcdest;
    bool skip_checksums{false};
    bool hold_version{false};

    SourceHandler(const std::filesystem::path& srcdest, bool skip_checksums = false,
                  bool hold_version = false);

    /// Download all sources and verify checksums.
    void download_and_verify(const pkgbuild::Pkgbuild& pkgbuild,
                             const std::filesystem::path& srcdir) const;

private:
    void download_source(const std::string& source_spec,
                         const std::filesystem::path& srcdir) const;
    void verify_checksums(const pkgbuild::Pkgbuild& pkgbuild,
                          const std::filesystem::path& srcdir) const;
    void extract_sources(const pkgbuild::Pkgbuild& pkgbuild,
                         const std::filesystem::path& srcdir) const;
    std::string compute_hash(const std::filesystem::path& file,
                             const std::string& algo) const;
};

} // namespace pacmkr::source
