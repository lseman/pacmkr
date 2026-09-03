#pragma once

#include <filesystem>
#include <string>
#include <optional>
#include <cstdint>

namespace pacmkr::disk {

/// Minimum free space required (in bytes) for a typical AUR build.
constexpr std::uint64_t kDefaultMinSpace = 2ULL * 1024 * 1024 * 1024; // 2 GB

/// Human-readable byte size.
std::string format_bytes(std::uint64_t bytes);

/// Get free space on the filesystem containing `path`.
/// Returns nullopt if the query fails.
std::optional<std::uint64_t> get_free_space(const std::filesystem::path& path);

/// Check whether there is enough free space for a build.
/// `min_bytes` is the threshold; defaults to 2 GB.
/// Returns true if sufficient, false otherwise.
bool check_space(const std::filesystem::path& path,
                 std::uint64_t min_bytes = kDefaultMinSpace);

/// Report disk status: free space on build target and pkgdest.
void report_disk_status(const std::filesystem::path& build_path,
                        const std::filesystem::path& pkgdest);

} // namespace pacmkr::disk
