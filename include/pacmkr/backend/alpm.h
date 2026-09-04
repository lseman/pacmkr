#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstddef>

namespace pacmkr::alpm {

/// Whether the initialized handle targets the real system root.
bool uses_system_root();

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
    std::vector<std::string> required_by;
    std::vector<std::string> optional_for;
    std::vector<std::string> backup;   // package-tracked config file paths
    std::vector<std::string> files;    // only populated for loaded package files

    // Which database this came from (empty = local, "repo" name = sync db)
    std::string origin_db;

    /// For local packages: why was it installed?
    enum class Reason { Unknown, Explicit, Dependency } reason{Reason::Unknown};
};

/// Initialize libalpm. Call once at startup.
void init(const std::string& root = "/", const std::string& db_path = "/var/lib/pacman/",
          const std::string& config_path = {});

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

/// Return the installed package owning a filesystem path, if any.
std::optional<std::string> find_file_owner(const std::string& path);

struct PackageGroup {
    std::string repository;
    std::string name;
    std::vector<std::string> packages;
};
std::vector<PackageGroup> list_sync_groups();

struct FileMatch {
    std::string repository;
    std::string package;
    std::string path;
};
std::vector<FileMatch> search_sync_files(const std::string& query);

/// Return dependency expressions not satisfied by the installed database.
std::vector<std::string> missing_dependencies(const std::vector<std::string>& dependencies);

/// Compare two version strings with libalpm's canonical algorithm
/// (handles epoch, pkgrel, tilde, and keyword ordering). Result is
/// normalized to -1 (a < b), 0 (a == b), or 1 (a > b).
int vercmp(const std::string& a, const std::string& b);

/// Name of an installed package satisfying `dep_spec` (a dependency string
/// that may carry a version constraint, e.g. "foo>=1.2"), or nullopt.
/// Honors `provides` and version constraints exactly as pacman does.
std::optional<std::string> local_satisfier(const std::string& dep_spec);

/// Name of a sync-repo package satisfying `dep_spec`, honoring `provides`
/// and version constraints, or nullopt.
std::optional<std::string> sync_satisfier(const std::string& dep_spec);

/// Change the install reason stored in the local package database.
int set_install_reason(const std::vector<std::string>& packages, bool explicit_reason);

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

/// Refresh configured sync databases. Mutating operations require root.
int refresh_databases(bool force = false);

/// Install repository packages, resolving dependencies in one transaction.
int install(const std::vector<std::string>& packages, bool needed = false,
            bool no_confirm = false,
            const std::vector<std::string>& ignore = {},
            const std::vector<std::string>& ignore_groups = {},
            const std::vector<std::string>& overwrite = {});

/// Upgrade every installed package with an available repository version.
int system_upgrade(bool allow_downgrade = false, bool no_confirm = false,
                   const std::vector<std::string>& ignore = {},
                   const std::vector<std::string>& ignore_groups = {},
                   const std::vector<std::string>& overwrite = {});

/// Remove installed packages. recurse removes newly-unneeded dependencies.
int remove(const std::vector<std::string>& packages, bool recurse = false,
           bool cascade = false, bool no_save = false, bool no_confirm = false,
           bool recurse_all = false, bool nodeps = false);

/// Download repository packages (and their dependencies) into the package cache
/// without installing them (for -Sw). With sysupgrade the pending upgrade set is
/// added as well (-Suw).
int download(const std::vector<std::string>& packages, bool sysupgrade = false,
             bool no_confirm = false,
             const std::vector<std::string>& ignore = {},
             const std::vector<std::string>& ignore_groups = {});

/// Install package archives produced locally (for example AUR builds).
/// Entries containing "://" are downloaded to the package cache first (for -U <url>).
int install_files(const std::vector<std::string>& paths, bool no_confirm = false);

/// Install missing build dependencies (makedepends/checkdepends) from sync repos.
/// Filters out already-installed packages and installs only what's needed.
int install_sync_packages(const std::vector<std::string>& makedeps,
                          const std::vector<std::string>& checkdeps,
                          bool no_confirm = false);

/// Read package metadata straight from a .pkg.tar archive (for -Qp).
/// Throws if the file cannot be loaded. Populates Package::files.
Package load_package_file(const std::string& path);

/// Return a local package's changelog text, or empty if it ships none (for -Qc).
std::string get_changelog(const std::string& name);

/// Remove cached package files. With all=false (-Sc) only versions that are no
/// longer installed are dropped; with all=true (-Scc) every cache file and the
/// downloaded sync databases are removed.
struct CacheCleanResult {
    std::size_t files_removed{};
    unsigned long long bytes_freed{};
};
CacheCleanResult clean_cache(bool all, bool no_confirm = false);

} // namespace pacmkr::alpm
