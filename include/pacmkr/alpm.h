#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstddef>

namespace pacmkr::alpm {

/// A package as returned by libalpm.
struct Package {
    std::string name;
    std::string version;
    std::string desc;
    std::string url;
    std::string arch;
    std::string packager;
    unsigned long long build_date{};
    unsigned long long install_date{};
    off_t size{};           // installed size on disk
    off_t isize{};          // package file size

    std::vector<std::string> licenses;
    std::vector<std::string> groups;
    std::vector<std::string> depends;
    std::vector<std::string> makedepends;
    std::vector<std::string> checkdepends;
    std::vector<std::string> optdepends;
    std::vector<std::string> conflicts;
    std::vector<std::string> provides;
    std::vector<std::string> replaces;

    // Which database this came from (empty = local, "repo" name = sync db)
    std::string origin_db;

    /// For local packages: why was it installed?
    enum class Reason { Unknown, Explicit, Dependency } reason{Reason::Unknown};
};

/// Initialize libalpm. Call once at startup.
void init(const std::string& root = "/", const std::string& db_path = "/var/lib/pacman/");

/// Shutdown libalpm. Call at exit.
void shutdown();

/// Get the local database package list.
std::vector<Package> get_local_packages();

/// Get a specific package from the local database.
std::optional<Package> get_local_package(const std::string& name);

/// Get all sync (repo) databases.
std::vector<std::string> get_sync_db_names();

/// Get a specific package from sync databases.
std::optional<Package> get_sync_package(const std::string& name);

/// Search sync databases for packages matching query in name or description.
std::vector<Package> search_sync(const std::string& query);

/// List packages in a sync database (for -Sl).
struct RepoPkg {
    std::string db;       // repo name (core, extra, etc.)
    std::string name;
    std::string version;
};
std::vector<RepoPkg> list_sync_packages();

/// List files installed by a local package.
std::vector<std::string> list_package_files(const std::string& name);

/// Check if a package exists in any database (local or sync).
bool package_exists(const std::string& name);

/// Get out-of-date packages: installed packages that have newer versions in repos.
struct OutOfDatePkg {
    std::string name;
    std::string installed_version;
    std::string repo_version;
};
std::vector<OutOfDatePkg> get_upgrades();

/// Check if a package is an orphan (installed as dependency but no longer needed).
bool is_orphan(const std::string& name);

/// Get all orphan packages: dependencies no longer required by any explicit package.
std::vector<Package> get_orphan_packages();

/// Get top-level explicitly installed packages.
std::vector<Package> get_top_level_packages();

/// Get packages installed explicitly by the user.
std::vector<Package> get_explicit_packages();

/// Get installed packages absent from every configured sync database.
/// This includes AUR packages and packages installed from local PKGBUILDs.
std::vector<Package> get_foreign_packages();

/// Set a callback for libalpm log messages.
void set_log_callback(std::function<void(int, const char*)> cb);

} // namespace pacmkr::alpm
