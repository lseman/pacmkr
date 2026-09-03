#include "pacmkr/core/disk.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/statvfs.h>
#include <iostream>

namespace pacmkr::disk {

std::string format_bytes(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        ++unit_idx;
    }
    
    std::ostringstream oss;
    if (unit_idx == 0) {
        oss << bytes << " " << units[unit_idx];
    } else {
        oss.precision(1);
        oss << size << " " << units[unit_idx];
    }
    return oss.str();
}

std::optional<std::uint64_t> get_free_space(const std::filesystem::path& path) {
    struct statvfs st{};
    if (statvfs(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    
    // f_bavail = blocks available to unprivileged user
    // f_frsize = fragment size (use f_blocks if f_frsize is weird)
    std::uint64_t free_bytes = static_cast<std::uint64_t>(st.f_bavail) * 
                               static_cast<std::uint64_t>(st.f_frsize);
    return free_bytes;
}

bool check_space(const std::filesystem::path& path, std::uint64_t min_bytes) {
    auto free_bytes = get_free_space(path);
    if (!free_bytes.has_value()) {
        // Cannot determine free space — warn but don't block
        std::cerr << "warning: could not determine free disk space\n";
        return true;
    }
    
    if (*free_bytes >= min_bytes) {
        return true;
    }
    
    // Not enough space
    std::cerr << "error: insufficient disk space for build\n";
    std::cerr << "  Required: " << format_bytes(min_bytes) << "\n";
    std::cerr << "  Available: " << format_bytes(*free_bytes) << "\n";
    return false;
}

void report_disk_status(const std::filesystem::path& build_path,
                        const std::filesystem::path& pkgdest) {
    auto free_build = get_free_space(build_path);
    auto free_pkg = get_free_space(pkgdest);
    
    std::cout << "  Build dir: " << build_path.string() 
              << " — " << (free_build ? format_bytes(*free_build) : "unknown") << " free\n";
    if (pkgdest != build_path) {
        std::cout << "  Pkg dest:  " << pkgdest.string() 
                  << " — " << (free_pkg ? format_bytes(*free_pkg) : "unknown") << " free\n";
    }
}

} // namespace pacmkr::disk
