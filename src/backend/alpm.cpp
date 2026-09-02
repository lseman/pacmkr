#include "pacmkr/alpm.h"
#include "pacmkr/error.h"

#include <alpm.h>
#include <alpm_list.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace pacmkr::alpm {

static alpm_handle_t* g_handle{nullptr};
static std::function<void(int, const char*)> g_log_cb;

// ─── Internal helpers ────────────────────────────────────────────────

static std::vector<std::string> list_to_strings(alpm_list_t* list) {
    std::vector<std::string> result;
    for (alpm_list_t* it = list; it; it = it->next) {
        if (it->data) {
            result.emplace_back(static_cast<char*>(it->data));
        }
    }
    return result;
}

/// Convert alpm_depend_t* list to display strings.
static std::vector<std::string> deps_to_strings(alpm_list_t* list) {
    std::vector<std::string> result;
    for (alpm_list_t* it = list; it; it = it->next) {
        auto* dep = static_cast<alpm_depend_t*>(it->data);
        if (dep) {
            char* str = alpm_dep_compute_string(dep);
            if (str) {
                result.emplace_back(str);
                free(str);  // alpm_dep_compute_string returns malloc'd memory
            }
        }
    }
    return result;
}

static std::string dep_to_string(const alpm_depend_t* dep) {
    if (!dep || !dep->name) return {};
    return dep->name;
}

static Package pkg_from_alpm(alpm_pkg_t* p, const std::string& origin_db = {}) {
    Package pkg{};
    pkg.name       = alpm_pkg_get_name(p)      ? alpm_pkg_get_name(p)      : "";
    pkg.version    = alpm_pkg_get_version(p)   ? alpm_pkg_get_version(p)   : "";
    pkg.desc       = alpm_pkg_get_desc(p)      ? alpm_pkg_get_desc(p)      : "";
    pkg.url        = alpm_pkg_get_url(p)       ? alpm_pkg_get_url(p)       : "";
    pkg.arch       = alpm_pkg_get_arch(p)      ? alpm_pkg_get_arch(p)      : "";
    pkg.packager   = alpm_pkg_get_packager(p)  ? alpm_pkg_get_packager(p)  : "";
    pkg.build_date = static_cast<unsigned long long>(alpm_pkg_get_builddate(p));
    pkg.install_date = static_cast<unsigned long long>(alpm_pkg_get_installdate(p));
    pkg.size       = alpm_pkg_get_size(p);
    pkg.isize      = alpm_pkg_get_isize(p);
    pkg.origin_db  = origin_db;

    auto reason = alpm_pkg_get_reason(p);
    if (reason == ALPM_PKG_REASON_DEPEND)      pkg.reason = Package::Reason::Dependency;
    else if (reason == ALPM_PKG_REASON_EXPLICIT) pkg.reason = Package::Reason::Explicit;
    else                                    pkg.reason = Package::Reason::Unknown;

    pkg.licenses       = list_to_strings(alpm_pkg_get_licenses(p));
    pkg.groups         = list_to_strings(alpm_pkg_get_groups(p));
    pkg.depends        = deps_to_strings(alpm_pkg_get_depends(p));
    pkg.makedepends    = deps_to_strings(alpm_pkg_get_makedepends(p));
    pkg.checkdepends   = deps_to_strings(alpm_pkg_get_checkdepends(p));
    pkg.optdepends     = deps_to_strings(alpm_pkg_get_optdepends(p));
    pkg.conflicts      = deps_to_strings(alpm_pkg_get_conflicts(p));
    pkg.provides       = deps_to_strings(alpm_pkg_get_provides(p));
    pkg.replaces       = deps_to_strings(alpm_pkg_get_replaces(p));

    return pkg;
}

static void log_callback(void* ctx, alpm_loglevel_t level, const char* fmt, va_list args) {
    (void)ctx; (void)level;
    if (g_log_cb) {
        g_log_cb(static_cast<int>(level), fmt);
    }
}

// ─── Core lookup functions (defined first so others can call them) ──

std::optional<Package> get_local_package(const std::string& name) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    alpm_db_t* localdb = alpm_get_localdb(g_handle);
    if (!localdb) return std::nullopt;
    auto* pkg = alpm_db_get_pkg(localdb, name.c_str());
    if (!pkg) return std::nullopt;
    return pkg_from_alpm(pkg, "local");
}

std::optional<Package> get_sync_package(const std::string& name) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    for (alpm_list_t* it = alpm_get_syncdbs(g_handle); it; it = it->next) {
        auto* db = static_cast<alpm_db_t*>(it->data);
        if (!db) continue;
        auto* pkg = alpm_db_get_pkg(db, name.c_str());
        if (pkg) return pkg_from_alpm(pkg, alpm_db_get_name(db));
    }
    return std::nullopt;
}

// ─── Public API ──────────────────────────────────────────────────────

void init(const std::string& root, const std::string& db_path) {
    if (g_handle) return;
    g_handle = alpm_initialize(root.c_str(), db_path.c_str(), nullptr);
    if (!g_handle) throw alpm_error("Failed to initialize libalpm");
    alpm_option_set_logcb(g_handle, log_callback, nullptr);
    alpm_register_syncdb(g_handle, "core",  ALPM_DB_USAGE_ALL);
    alpm_register_syncdb(g_handle, "extra", ALPM_DB_USAGE_ALL);
    alpm_register_syncdb(g_handle, "multilib", ALPM_DB_USAGE_ALL);
}

void shutdown() {
    if (g_handle) { alpm_release(g_handle); g_handle = nullptr; }
}

void set_log_callback(std::function<void(int, const char*)> cb) {
    g_log_cb = std::move(cb);
}

std::vector<Package> get_local_packages() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    alpm_db_t* localdb = alpm_get_localdb(g_handle);
    if (!localdb) throw alpm_error("Failed to get local database");
    std::vector<Package> result;
    for (alpm_list_t* it = alpm_db_get_pkgcache(localdb); it; it = it->next) {
        auto* pkg = static_cast<alpm_pkg_t*>(it->data);
        if (pkg) result.push_back(pkg_from_alpm(pkg, "local"));
    }
    return result;
}

std::vector<std::string> get_sync_db_names() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<std::string> result;
    for (alpm_list_t* it = alpm_get_syncdbs(g_handle); it; it = it->next) {
        auto* db = static_cast<alpm_db_t*>(it->data);
        if (db && alpm_db_get_name(db)) result.emplace_back(alpm_db_get_name(db));
    }
    return result;
}

std::vector<Package> search_sync(const std::string& query) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<Package> result;
    std::string q_lower = query;
    std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);

    for (alpm_list_t* it = alpm_get_syncdbs(g_handle); it; it = it->next) {
        auto* db = static_cast<alpm_db_t*>(it->data);
        if (!db) continue;
        for (alpm_list_t* pit = alpm_db_get_pkgcache(db); pit; pit = pit->next) {
            auto* pkg = static_cast<alpm_pkg_t*>(pit->data);
            if (!pkg) continue;
            bool matched = false;
            const char* pname = alpm_pkg_get_name(pkg);
            const char* pdesc = alpm_pkg_get_desc(pkg);
            if (pname) {
                std::string n_lower = pname;
                std::transform(n_lower.begin(), n_lower.end(), n_lower.begin(), ::tolower);
                if (n_lower.find(q_lower) != std::string::npos) matched = true;
            }
            if (!matched && pdesc && !q_lower.empty()) {
                std::string d_lower = pdesc;
                std::transform(d_lower.begin(), d_lower.end(), d_lower.begin(), ::tolower);
                if (d_lower.find(q_lower) != std::string::npos) matched = true;
            }
            if (matched) result.push_back(pkg_from_alpm(pkg, alpm_db_get_name(db)));
        }
    }
    return result;
}

std::vector<RepoPkg> list_sync_packages() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<RepoPkg> result;
    for (alpm_list_t* it = alpm_get_syncdbs(g_handle); it; it = it->next) {
        auto* db = static_cast<alpm_db_t*>(it->data);
        if (!db || !alpm_db_get_name(db)) continue;
        const char* dbname = alpm_db_get_name(db);
        for (alpm_list_t* pit = alpm_db_get_pkgcache(db); pit; pit = pit->next) {
            auto* pkg = static_cast<alpm_pkg_t*>(pit->data);
            if (!pkg) continue;
            RepoPkg rp{};
            rp.db     = dbname;
            rp.name   = alpm_pkg_get_name(pkg)  ? alpm_pkg_get_name(pkg)  : "";
            rp.version = alpm_pkg_get_version(pkg) ? alpm_pkg_get_version(pkg) : "";
            result.push_back(std::move(rp));
        }
    }
    return result;
}

std::vector<std::string> list_package_files(const std::string& name) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    auto pkg = get_local_package(name);
    if (!pkg) return {};
    return {}; // File iteration requires alpm_filelist_t — simplified
}

bool package_exists(const std::string& name) {
    try {
        return get_local_package(name).has_value() || get_sync_package(name).has_value();
    } catch (...) { return false; }
}

std::vector<OutOfDatePkg> get_upgrades() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<OutOfDatePkg> result;
    auto local_pkgs = get_local_packages();
    for (auto& lpkg : local_pkgs) {
        auto sync_pkg = get_sync_package(lpkg.name);
        if (sync_pkg && alpm_pkg_vercmp(lpkg.version.c_str(), sync_pkg->version.c_str()) < 0) {
            OutOfDatePkg ood{};
            ood.name         = lpkg.name;
            ood.installed_version = lpkg.version;
            ood.repo_version    = sync_pkg->version;
            result.push_back(std::move(ood));
        }
    }
    return result;
}

bool is_orphan(const std::string& name) {
    try {
        auto pkg = get_local_package(name);
        if (!pkg) return false;
        return pkg->reason == Package::Reason::Dependency;
    } catch (...) { return false; }
}

/// Extract package name from a dependency spec (e.g., "pkg>=1.0" -> "pkg").
static std::string dep_spec_name(const std::string& spec) {
    for (char sep : {'>', '<', '=', '!'}) {
        auto pos = spec.find(sep);
        if (pos != std::string::npos) return spec.substr(0, pos);
    }
    return spec;
}

/// Check if a package is still required by any explicitly installed or retained package.
static bool is_still_needed(const std::string& name, const std::vector<Package>& all_pkgs) {
    // Build reverse dependency map: for each package, collect what it depends on
    for (auto& pkg : all_pkgs) {
        if (pkg.reason == Package::Reason::Explicit) {
            for (auto& dep : pkg.depends) {
                auto dep_name = dep_spec_name(dep);
                if (dep_name == name) return true;
            }
        }
    }
    return false;
}

std::vector<Package> get_orphan_packages() {
    try {
        auto pkgs = get_local_packages();
        std::vector<Package> orphans;
        for (auto& pkg : pkgs) {
            if (pkg.reason == Package::Reason::Dependency) {
                // Check if still needed by any explicit package
                if (!is_still_needed(pkg.name, pkgs)) {
                    orphans.push_back(pkg);
                }
            }
        }
        return orphans;
    } catch (...) { return {}; }
}

std::vector<Package> get_top_level_packages() {
    try {
        auto pkgs = get_local_packages();
        std::vector<Package> top_level;
        for (auto& pkg : pkgs) {
            if (pkg.reason == Package::Reason::Explicit) {
                top_level.push_back(pkg);
            }
        }
        return top_level;
    } catch (...) { return {}; }
}

std::vector<Package> get_explicit_packages() {
    try {
        auto pkgs = get_local_packages();
        std::vector<Package> result;
        for (auto& p : pkgs) {
            if (p.reason == Package::Reason::Explicit) result.push_back(std::move(p));
        }
        return result;
    } catch (...) { return {}; }
}

} // namespace pacmkr::alpm
