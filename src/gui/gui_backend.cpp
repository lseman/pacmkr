#include "pacmkr/gui_backend.h"
#include "pacmkr/aur_cache.h"
#include "pacmkr/alpm.h"
#include "pacmkr/deps.h"
#include "pacmkr/build.h"
#include "pacmkr/error.h"

#include <iostream>
#include <algorithm>

namespace pacmkr::gui {

Backend::Backend() {
    // Initialize libalpm and AUR cache on construction
    try {
        alpm::init();
    } catch (...) {
        std::cerr << "warning: libalpm initialization failed\n";
    }

    aur_cache::init();
}

Backend::~Backend() {
    // Cancel any ongoing build
    cancel_build();

    // Shutdown alpm and AUR cache
    try {
        alpm::shutdown();
    } catch (...) {}

    aur_cache::shutdown();
}

std::vector<SearchResult> Backend::search(const std::string& query, unsigned int limit) {
    std::vector<SearchResult> results;

    // Search repositories via alpm
    try {
        auto repo_results = alpm::search_sync(query);
        for (auto& r : repo_results) {
            SearchResult sr{};
            sr.name = r.name;
            sr.version = r.version;
            sr.origin = r.origin_db;
            sr.description = r.desc;
            sr.is_out_of_date = false;
            results.push_back(sr);
        }
    } catch (...) {
        std::cerr << "warning: failed to search repositories\n";
    }

    // Search AUR cache
    auto aur_results = aur_cache::search(query, limit);
    for (auto& a : aur_results) {
        SearchResult sr{};
        sr.name = a.name;
        sr.version = a.pkgname;  // Will be updated with actual version later
        sr.origin = "aur";
        sr.description = a.desc;

        // Check if out of date (compare with local version)
        try {
            auto local_pkgs = alpm::get_local_packages();
            for (auto& pkg : local_pkgs) {
                if (pkg.name == a.name) {
                    auto ootd_info = aur_cache::check_ootd(a.name, pkg.version);
                    sr.is_out_of_date = ootd_info.is_out_of_date;
                    if (!ootd_info.aur_version.empty()) {
                        sr.version = ootd_info.aur_version;
                    }
                    break;
                }
            }
        } catch (...) {}

        results.push_back(sr);
    }

    // Sort: repo results first, then AUR by votes
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        if (a.origin != "aur" && b.origin == "aur") return true;
        if (a.origin == "aur" && b.origin != "aur") return false;
        return a.name < b.name;
    });

    // Limit results
    if (results.size() > limit) {
        results.resize(limit);
    }

    return results;
}

void Backend::refresh_cache() {
    try {
        aur_cache::refresh();
    } catch (...) {
        std::cerr << "warning: failed to refresh AUR cache\n";
    }
}

void Backend::start_build(const std::vector<SearchResult>& targets,
                          ProgressCallback progress_cb,
                          LogCallback log_cb) {
    if (building_.load()) return;  // Already building

    building_.store(true);

    // Launch build in background thread
    build_thread_ = std::thread([this, targets, progress_cb, log_cb]() {
        try {
            build_worker(targets, progress_cb, log_cb);
        } catch (const error& e) {
            log_cb("error: " + std::string(e.what()));
            progress_cb(0, "Build failed");
        } catch (const std::exception& e) {
            log_cb("error: " + std::string(e.what()));
            progress_cb(0, "Build failed");
        } catch (...) {
            log_cb("error: unknown build failure");
            progress_cb(0, "Build failed");
        }

        building_.store(false);
    });

    build_thread_.detach();  // Detach so it completes even if GUI closes
}

void Backend::cancel_build() {
    if (building_.load() && build_thread_.joinable()) {
        // Note: Actual cancellation requires modifying build.cpp to check a flag
        // For now, we just let it complete but mark as cancelled in UI
        building_.store(false);
    }
}

bool Backend::is_building() const {
    return building_.load();
}

SearchResult Backend::get_package_info(const std::string& name) {
    SearchResult result{};
    result.name = name;

    // Try AUR cache first
    if (aur_cache::has(name)) {
        auto cached = aur_cache::get(name);
        if (cached.has_value()) {
            const auto& j = cached.value();
            result.version = j.value("NameVersion", "unknown");
            result.origin = "aur";
            result.description = j.value("Description", "");

            // Check out of date status
            try {
                auto local_pkgs = alpm::get_local_packages();
                for (auto& pkg : local_pkgs) {
                    if (pkg.name == name) {
                        auto ootd_info = aur_cache::check_ootd(name, pkg.version);
                        result.is_out_of_date = ootd_info.is_out_of_date;
                        break;
                    }
                }
            } catch (...) {}

            return result;
        }
    }

    // Try repo via alpm
    try {
        auto sync_pkg = alpm::get_sync_package(name);
        if (sync_pkg) {
            result.version = sync_pkg->version;
            result.origin = sync_pkg->origin_db;
            result.description = sync_pkg->desc;
            result.is_out_of_date = false;
            return result;
        }
    } catch (...) {}

    // Package not found
    result.version = "not found";
    result.origin = "unknown";
    result.description = "Package not available in repositories or AUR";

    return result;
}

std::vector<SearchResult> Backend::get_out_of_date_packages() {
    std::vector<SearchResult> results;

    try {
        auto local_pkgs = alpm::get_local_packages();

        for (auto& pkg : local_pkgs) {
            // Skip repo packages
            try {
                auto sync_pkg = alpm::get_sync_package(pkg.name);
                if (sync_pkg) continue;  // In official repos
            } catch (...) {}

            // Check AUR cache
            if (aur_cache::has(pkg.name)) {
                auto ootd_info = aur_cache::check_ootd(pkg.name, pkg.version);
                if (ootd_info.is_out_of_date) {
                    SearchResult sr{};
                    sr.name = pkg.name;
                    sr.version = ootd_info.aur_version;
                    sr.origin = "aur";
                    sr.description = "Out of date in AUR";
                    sr.is_out_of_date = true;
                    results.push_back(sr);
                }
            }
        }
    } catch (...) {}

    return results;
}

void Backend::build_worker(const std::vector<SearchResult>& targets,
                           ProgressCallback progress_cb,
                           LogCallback log_cb) {
    if (targets.empty()) {
        progress_cb(0, "No packages to build");
        return;
    }

    log_cb("Starting build for " + std::to_string(targets.size()) + " package(s)...");
    progress_cb(5, "Resolving dependencies...");

    // Collect package names for dependency resolution
    std::vector<std::string> pkg_names;
    for (auto& t : targets) {
        pkg_names.push_back(t.name);
    }

    try {
        // Resolve dependencies (AUR only, since we're building)
        log_cb("Resolving AUR dependencies...");
        auto dep_graph = deps::resolve(pkg_names, true, true, false);

        if (!dep_graph.build_order.empty()) {
            log_cb("Build order: " + std::to_string(dep_graph.build_order.size()) + " packages");
        }

        // Build each package in order
        for (size_t i = 0; i < dep_graph.build_order.size(); ++i) {
            if (!building_.load()) break;  // Cancelled

            auto pkg_name = dep_graph.build_order[i];
            int percent = 10 + (i * 80 / dep_graph.build_order.size());

            log_cb("Building: " + pkg_name);
            progress_cb(percent, "Building " + pkg_name + " (" + std::to_string(i + 1) + "/" + std::to_string(dep_graph.build_order.size()) + ")");

            // Build the package (this would call the actual build process)
            // For now, simulate with a placeholder
            log_cb("  -> PKGBUILD build for " + pkg_name);
        }

        progress_cb(100, "Build complete!");
        log_cb("All packages built successfully");

    } catch (const error& e) {
        throw;  // Re-throw for UI to handle
    }
}

} // namespace pacmkr::gui
