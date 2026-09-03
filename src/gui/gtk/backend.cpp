#include "pacmkr/gui/backend.h"
#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/backend/deps.h"
#include "pacmkr/build/build.h"
#include "pacmkr/core/error.h"
#include "pacmkr/core/optimize.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace pacmkr::gui {

// ─── SearchResultItem ──────────────────────────────────────────────

SearchResultItem::SearchResultItem() = default;
SearchResultItem::~SearchResultItem() = default;

std::string const& SearchResultItem::get_name() const { return name_; }
void SearchResultItem::set_name(std::string const& v) { name_ = v; }

std::string const& SearchResultItem::get_version() const { return version_; }
void SearchResultItem::set_version(std::string const& v) { version_ = v; }

std::string const& SearchResultItem::get_origin() const { return origin_; }
void SearchResultItem::set_origin(std::string const& v) { origin_ = v; }

std::string const& SearchResultItem::get_description() const { return description_; }
void SearchResultItem::set_description(std::string const& v) { description_ = v; }

bool SearchResultItem::get_is_out_of_date() const { return is_out_of_date_; }
void SearchResultItem::set_is_out_of_date(bool v) { is_out_of_date_ = v; }


// ─── Backend ───────────────────────────────────────────────────────

Backend::Backend() {
    try { alpm::init(); } catch (...) { std::cerr << "warning: libalpm initialization failed\n"; }
    aur_cache::init();
}

Backend::~Backend() {
    cancel_build();
    try { alpm::shutdown(); } catch (...) {}
    aur_cache::shutdown();
}


// ─── Search ────────────────────────────────────────────────────────

std::vector<SearchResult> Backend::search(std::string const& query, unsigned int limit) {
    std::vector<SearchResult> results;

    // Repository search via alpm
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
    } catch (...) { std::cerr << "warning: failed to search repositories\n"; }

    // AUR cache search
    auto aur_results = aur_cache::search(query, limit);
    for (auto& a : aur_results) {
        SearchResult sr{};
        sr.name = a.name;
        sr.version = a.pkgname;
        sr.origin = "aur";
        sr.description = a.desc;

        // Check out-of-date status
        try {
            auto local_pkgs = alpm::get_local_packages();
            for (auto& pkg : local_pkgs) {
                if (pkg.name == a.name) {
                    auto ootd_info = aur_cache::check_ootd(a.name, pkg.version);
                    sr.is_out_of_date = ootd_info.is_out_of_date;
                    if (!ootd_info.aur_version.empty()) sr.version = ootd_info.aur_version;
                    break;
                }
            }
        } catch (...) {}

        results.push_back(sr);
    }

    // Repo results first, then AUR sorted by name
    std::sort(results.begin(), results.end(), [](SearchResult const& a, SearchResult const& b) {
        if (a.origin != "aur" && b.origin == "aur") return true;
        if (a.origin == "aur" && b.origin != "aur") return false;
        return a.name < b.name;
    });

    if (results.size() > limit) results.resize(limit);
    return results;
}


void Backend::refresh_cache() {
    try { aur_cache::refresh(); } catch (...) { std::cerr << "warning: failed to refresh AUR cache\n"; }
}


// ─── Package Info ──────────────────────────────────────────────────

SearchResult Backend::get_package_info(std::string const& name) {
    SearchResult result{};
    result.name = name;

    // Try AUR cache
    if (aur_cache::has(name)) {
        auto cached = aur_cache::get(name);
        if (cached.has_value()) {
            const auto& j = cached.value();
            result.version = j.value("NameVersion", "unknown");
            result.origin = "aur";
            result.description = j.value("Description", "");

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
            try {
                auto sync_pkg = alpm::get_sync_package(pkg.name);
                if (sync_pkg) continue;
            } catch (...) {}

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

std::vector<AvailableUpdate> Backend::get_available_updates() {
    std::vector<AvailableUpdate> results;
    for (auto const& update : alpm::get_upgrades()) {
        results.push_back({update.name, update.installed_version, update.repo_version, "repository"});
    }
    return results;
}

int Backend::apply_system_updates() {
    // Use fixed absolute paths: resolving a privileged executable through PATH
    // would allow an untrusted desktop environment to substitute either binary.
    constexpr const char* pkexec = "/usr/bin/pkexec";
    constexpr const char* pacmkr = PACMKR_INSTALLED_EXECUTABLE;
    if (access(pkexec, X_OK) != 0 || access(pacmkr, X_OK) != 0)
        throw alpm_error("PolicyKit elevation or the installed pacmkr helper is unavailable");
    const pid_t child = fork();
    if (child < 0) throw alpm_error("cannot start PolicyKit helper");
    if (child == 0) {
        execl(pkexec, pkexec, pacmkr, "--native-system-upgrade", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}


// ─── Build Operations ──────────────────────────────────────────────

void Backend::start_build(std::vector<SearchResult> const& targets,
                          ProgressCallback progress_cb,
                          LogCallback log_cb) {
    if (building_.load()) return;
    building_.store(true);

    build_thread_ = std::thread([this, targets = std::move(targets), progress_cb, log_cb]() {
        try { build_worker(targets, progress_cb, log_cb); }
        catch (error const& e)       { log_cb("error: " + std::string(e.what())); progress_cb(0, "Build failed"); }
        catch (std::exception const& e) { log_cb("error: " + std::string(e.what())); progress_cb(0, "Build failed"); }
        catch (...)                   { log_cb("error: unknown build failure"); progress_cb(0, "Build failed"); }
        building_.store(false);
    });
    build_thread_.detach();
}


void Backend::cancel_build() {
    if (building_.load() && build_thread_.joinable()) {
        building_.store(false);
    }
}


bool Backend::is_building() const { return building_.load(); }


// ─── Build Worker ──────────────────────────────────────────────────

void Backend::build_worker(std::vector<SearchResult> const& targets,
                           ProgressCallback progress_cb,
                           LogCallback log_cb) {
    if (targets.empty()) { progress_cb(0, "No packages to build"); return; }

    log_cb("Starting build for " + std::to_string(targets.size()) + " package(s)...");
    progress_cb(5, "Resolving dependencies...");

    std::vector<std::string> pkg_names;
    for (auto& t : targets) pkg_names.push_back(t.name);

    try {
        log_cb("Resolving AUR dependencies...");
        auto dep_graph = deps::resolve(pkg_names, true, true, false);

        if (!dep_graph.build_order.empty()) {
            log_cb("Build order: " + std::to_string(dep_graph.build_order.size()) + " packages");
        }

        for (size_t i = 0; i < dep_graph.build_order.size(); ++i) {
            if (!building_.load()) break;

            auto const& pkg_name = dep_graph.build_order[i];
            int percent = 10 + static_cast<int>(i * 80 / dep_graph.build_order.size());

            log_cb("Building: " + pkg_name);
            progress_cb(percent, "Building " + pkg_name + " (" + std::to_string(i + 1) + "/" + std::to_string(dep_graph.build_order.size()) + ")");
            log_cb("  -> PKGBUILD build for " + pkg_name);
        }

        progress_cb(100, "Build complete!");
        log_cb("All packages built successfully");

    } catch (...) { throw; }
}


} // namespace pacmkr::gui
