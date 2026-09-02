#include "pacmkr/cli.h"
#include "pacmkr/config.h"
#include "pacmkr/aur.h"
#include "pacmkr/aur_cache.h"
#include "pacmkr/alpm.h"
#include "pacmkr/deps.h"
#include "pacmkr/optimize.h"
#include "pacmkr/operations.h"
#include "pacmkr/pacman.h"
#include "pacmkr/pkgbuild.h"
#include "pacmkr/build.h"
#include "pacmkr/progress.h"
#include "pacmkr/status.h"
#include "pacmkr/error.h"
#include "pacmkr/pgp.h"

#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <set>
#include <fstream>
#include <httplib.h>

namespace {

using namespace pacmkr;

// Forward declarations — functions defined later in this file
int run_upgrade(const cli::Cli& cli);
int run_local_build(const cli::Cli& cli, const config::Config& config);
int run_aur_build(cli::Cli cli);
int run_sync_install(const std::vector<std::string>& raw_args);
cli::Cli apply_user_config(cli::Cli cli, const config::UserConfig& cfg);

/// Check if args contain aur-specific flags that should NOT be forwarded to pacman.
bool has_aur_flags(const std::vector<std::string>& args) {
    for (auto& a : args) {
        if (a == "--aur" || a == "--search" || a == "--refresh" ||
            a == "-u" || a == "--aur-deps" || a == "--no-deps-resolve") {
            return true;
        }
    }
    return false;
}

/// Check if args represent a read-only sync operation (handled by alpm).
bool is_sync_read(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        if (arg == "--sync" || arg == "-S") continue; // skip primary op marker
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'S') {
                // -Ss, -Si, -Sl, -Sg are read-only
                for (size_t i = 2; i < arg.size(); ++i) {
                    if (arg[i] == 's' || arg[i] == 'i' || arg[i] == 'l' || arg[i] == 'g') {
                        return true;
                    }
                }
            }
        }
        // Long form: --sync --search, --sync --info, etc.
        if (arg == "--search" || arg == "--info" || arg == "--list" || arg == "--groups") {
            return true;
        }
    }
    return false;
}

/// Check if args represent a sync install operation: -S pkgname with no y/u/f/c flags.
/// This is the aur-aware install path: check repos first, fall back to AUR.
bool is_sync_install(const std::vector<std::string>& args) {
    bool has_s = false;
    bool has_y = false, has_u = false, has_f = false, has_c = false;
    for (auto& arg : args) {
        if (arg == "--sync" || arg == "-S") { has_s = true; continue; }
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'S') {
                for (size_t i = 2; i < arg.size(); ++i) {
                    if (arg[i] == 'y') has_y = true;
                    if (arg[i] == 'u') has_u = true;
                    if (arg[i] == 'f') has_f = true;
                    if (arg[i] == 'c') has_c = true;
                }
            }
        }
    }
    return has_s && !has_y && !has_u && !has_f && !has_c;
}

/// Extract package names from sync install args.
std::vector<std::string> extract_install_packages(const std::vector<std::string>& args) {
    std::vector<std::string> packages;
    bool found_s = false;
    for (auto& arg : args) {
        if (arg == "--sync" || arg == "-S") { found_s = true; continue; }
        if (found_s && arg.size() >= 2 && arg[0] == '-') {
            // Flags after -S are not packages
            break;
        }
        if (found_s) {
            packages.push_back(arg);
        }
    }
    return packages;
}

/// Check if args represent a read-only query operation (handled by alpm).
bool is_query_read(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        if (arg == "--query" || arg == "-Q") continue;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'Q') {
                // -Qi, -Ql, -Qm, -Qe, -Qu, -Qs, -Qd, -Qt, -Qk, -Qx are read-only
                for (size_t i = 2; i < arg.size(); ++i) {
                    char f = arg[i];
                    if (f == 'i' || f == 'l' || f == 'm' || f == 'e' ||
                        f == 'u' || f == 's' || f == 'd' || f == 't' ||
                        f == 'k' || f == 'x') {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/// Check if args represent an aur-extended query (--aur -Qm).
bool is_aur_query(const std::vector<std::string>& args) {
    bool has_aur = false, has_q = false;
    for (auto& arg : args) {
        if (arg == "--aur") has_aur = true;
        if (arg == "--query" || arg == "-Q") has_q = true;
    }
    return has_aur && has_q;
}

/// Extract search query from -Ss/-Si or package name from -Qi etc.
std::string extract_search_query(const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].size() >= 3 && args[i][0] == '-') {
            char primary = args[i][1];
            // Sync search: -Ss query, -Si pkg
            if (primary == 'S') {
                for (size_t j = 2; j < args[i].size(); ++j) {
                    if (args[i][j] == 's' || args[i][j] == 'i') {
                        if (i + 1 < args.size()) return args[i + 1];
                    }
                }
            }
            // Query info: -Qi pkg, -Ql pkg, -Qo path, etc.
            if (primary == 'Q') {
                for (size_t j = 2; j < args[i].size(); ++j) {
                    char f = args[i][j];
                    if (f == 'i' || f == 'l' || f == 'o' || f == 's' || f == 'x') {
                        if (i + 1 < args.size()) return args[i + 1];
                    }
                }
            }
        }
    }
    return {};
}

/// Run a read-only sync operation via alpm.
int run_sync_read(const std::vector<std::string>& args) {
    // Detect sub-operation
    bool do_search = false, do_info = false, do_list = false, do_groups = false;

    for (auto& arg : args) {
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'S') {
                for (size_t i = 2; i < arg.size(); ++i) {
                    if (arg[i] == 's') do_search = true;
                    if (arg[i] == 'i') do_info = true;
                    if (arg[i] == 'l') do_list = true;
                    if (arg[i] == 'g') do_groups = true;
                }
            }
        }
        if (arg == "--search") do_search = true;
        if (arg == "--info") do_info = true;
        if (arg == "--list") do_list = true;
        if (arg == "--groups") do_groups = true;
    }

    try {
        if (do_search) {
            std::string query = extract_search_query(args);
            if (!query.empty()) {
                unsigned int limit = 10;
                for (auto& arg : args) {
                    if (arg.rfind("--limit=", 0) == 0 || arg.rfind("-n", 0) == 0) {
                        try { limit = std::stoul(arg.substr(arg.find('=') + 1)); } catch (...) {}
                    }
                }

                // Parse --sortby from args
                aur_cache::SortOrder sort_order = aur_cache::SortOrder::Votes;
                for (auto& arg : args) {
                    if (arg.rfind("--sortby=", 0) == 0) {
                        auto val = arg.substr(9);
                        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                        if (val == "updated" || val == "date") sort_order = aur_cache::SortOrder::Updated;
                        else if (val == "popular") sort_order = aur_cache::SortOrder::Popular;
                        else sort_order = aur_cache::SortOrder::Votes;
                    }
                }

                // Search repos via alpm
                auto repo_results = alpm::search_sync(query);

                // Search AUR cache with sort order
                auto aur_results = aur_cache::search(query, limit, sort_order);

                // Print repo results first
                unsigned int printed = 0;
                for (auto& r : repo_results) {
                    if (printed >= limit) break;
                    std::cout << r.origin_db << "/" << r.name << "  " << r.version;
                    if (!r.desc.empty()) std::cout << "    " << r.desc;
                    std::cout << "\n";
                    ++printed;
                }

                // Print AUR results (skip repos with same name)
                std::set<std::string> repo_names;
                for (auto& r : repo_results) repo_names.insert(r.name);

                for (auto& a : aur_results) {
                    if (printed >= limit) break;
                    if (repo_names.find(a.name) != repo_names.end()) continue; // Skip duplicates
                    std::cout << "aur/" << a.name << "  " << a.pkgname;
                    if (!a.desc.empty()) std::cout << "    " << a.desc;
                    std::cout << "\n";
                    ++printed;
                }

                // Interactive picker in TTY mode
                bool is_tty = isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
                if (is_tty && !repo_results.empty()) {
                    std::cout << "\n==> Type a number to select, or Enter for first result, Esc to cancel\n";

                    // Collect all results into a single list for selection
                    struct SearchResultItem {
                        std::string origin;  // "core/pkg", "extra/pkg", "aur/pkg"
                        std::string name;
                        std::string version;
                        std::string desc;
                    };

                    std::vector<SearchResultItem> all_results;
                    for (auto& r : repo_results) {
                        all_results.push_back({r.origin_db + "/" + r.name, r.name, r.version, r.desc});
                    }
                    for (auto& a : aur_results) {
                        if (repo_names.find(a.name) == repo_names.end()) {
                            all_results.push_back({"aur/" + a.name, a.name, "", a.desc});
                        }
                    }

                    if (!all_results.empty()) {
                        // Simple number-based selection
                        std::cout << "\nSelect package [1-" << all_results.size() << "]: ";
                        std::string input;
                        std::getline(std::cin, input);

                        if (!input.empty() && input != "\x1b") {  // Not Esc
                            try {
                                unsigned int idx = std::stoul(input) - 1;
                                if (idx < all_results.size()) {
                                    std::cout << "\n==> Selected: " << all_results[idx].name << "\n";
                                    std::cout << "     To install: pacmkr -S " << all_results[idx].name << "\n";
                                } else {
                                    std::cout << "Invalid selection.\n";
                                }
                            } catch (...) {
                                // Empty or invalid input - select first by default
                                if (input.empty()) {
                                    std::cout << "\n==> Selected: " << all_results[0].name << " (default)\n";
                                } else {
                                    std::cout << "Invalid selection.\n";
                                }
                            }
                        }
                    }
                }
            } else {
                // -Ss with no query: print usage hint
                std::cerr << "error: missing search query\n";
                return 1;
            }
        }

        if (do_info) {
            std::string pkgname = extract_search_query(args);
            if (!pkgname.empty()) {
                auto pkg = alpm::get_sync_package(pkgname);
                if (pkg) {
                    std::cout << "Repository      : " << pkg->origin_db << "\n";
                    std::cout << "Name            : " << pkg->name << "\n";
                    std::cout << "Version         : " << pkg->version << "\n";
                    if (!pkg->desc.empty())   std::cout << "Description     : " << pkg->desc << "\n";
                    if (!pkg->url.empty())    std::cout << "URL             : " << pkg->url << "\n";
                    if (!pkg->arch.empty())   std::cout << "Architecture    : " << pkg->arch << "\n";
                    if (!pkg->packager.empty()) std::cout << "Packager        : " << pkg->packager << "\n";
                    if (!pkg->licenses.empty()) {
                        std::cout << "License         : ";
                        for (size_t i = 0; i < pkg->licenses.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->licenses[i];
                        }
                        std::cout << "\n";
                    }
                    if (!pkg->depends.empty()) {
                        std::cout << "Dependencies    : ";
                        for (size_t i = 0; i < pkg->depends.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->depends[i];
                        }
                        std::cout << "\n";
                    }
                } else {
                    std::cerr << "error: package '" << pkgname << "' not found in any repository\n";
                    return 1;
                }
            }
        }

        if (do_list) {
            auto pkgs = alpm::list_sync_packages();
            for (auto& p : pkgs) {
                std::cout << p.db << "/" << p.name << "  " << p.version << "\n";
            }
        }

        if (do_groups) {
            // Groups: iterate sync dbs and collect group members
            auto db_names = alpm::get_sync_db_names();
            for (auto& dbname : db_names) {
                // We'd need alpm_db_get_groupcache() for this — simplified for now
                std::cout << "[group info from " << dbname << "]\n";
            }
        }

    } catch (const alpm_error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

/// Run a read-only query operation via alpm.
int run_query_read(const std::vector<std::string>& args) {
    bool do_info = false, do_list_files = false, do_mirrors = false,
         do_explicit = false, do_upgrades = false, do_search = false,
         do_dependents = false, do_tree = false, do_orphans = false,
         do_quiet = false;

    for (auto& arg : args) {
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'Q') {
                for (size_t i = 2; i < arg.size(); ++i) {
                    char f = arg[i];
                    if (f == 'i') do_info = true;
                    if (f == 'l') do_list_files = true;
                    if (f == 'm') do_mirrors = true;
                    if (f == 'e') do_explicit = true;
                    if (f == 'u') do_upgrades = true;
                    if (f == 's') do_search = true;
                    if (f == 'd') do_dependents = true;
                    if (f == 't') do_tree = true;
                    if (f == 'q') do_quiet = true;
                }
            }
        }
    }

    // -Qdt: orphans (top-level deps that are no longer needed)
    bool has_dt = false;
    for (auto& arg : args) {
        if (arg.size() >= 3 && arg[0] == '-' && arg[1] == 'Q' &&
            ((arg[2] == 'd' && arg[3] == 't') || (arg[2] == 't' && arg[3] == 'd'))) {
            has_dt = true;
        }
    }

    try {
        if (do_info) {
            std::string pkgname = extract_search_query(args);
            if (!pkgname.empty()) {
                auto pkg = alpm::get_local_package(pkgname);
                if (pkg) {
                    std::cout << "Name            : " << pkg->name << "\n";
                    std::cout << "Version         : " << pkg->version << "\n";
                    if (!pkg->desc.empty())   std::cout << "Description     : " << pkg->desc << "\n";
                    if (!pkg->url.empty())    std::cout << "URL             : " << pkg->url << "\n";
                    if (!pkg->arch.empty())   std::cout << "Architecture    : " << pkg->arch << "\n";
                    if (!pkg->packager.empty()) std::cout << "Packager        : " << pkg->packager << "\n";
                    std::cout << "Installed Size  : " << pkg->isize << " bytes\n";
                    std::cout << "Install Date    : ";
                    if (pkg->install_date) {
                        std::cout << pkg->install_date;
                    } else {
                        std::cout << "never";
                    }
                    std::cout << "\n";

                    switch (pkg->reason) {
                        case alpm::Package::Reason::Explicit:
                            std::cout << "Install Reason  : Explicit\n"; break;
                        case alpm::Package::Reason::Dependency:
                            std::cout << "Install Reason  : As Dependency\n"; break;
                        default:
                            std::cout << "Install Reason  : Unknown\n"; break;
                    }

                    if (!pkg->licenses.empty()) {
                        std::cout << "License         : ";
                        for (size_t i = 0; i < pkg->licenses.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->licenses[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->depends.empty()) {
                        std::cout << "Required By     : (none — would need reverse dep scan)\n";
                        std::cout << "Optional For    : (none — would need reverse dep scan)\n";
                        std::cout << "Depends On      : ";
                        for (size_t i = 0; i < pkg->depends.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->depends[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->conflicts.empty()) {
                        std::cout << "Conflicts With  : ";
                        for (size_t i = 0; i < pkg->conflicts.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->conflicts[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->provides.empty()) {
                        std::cout << "Provides With   : ";
                        for (size_t i = 0; i < pkg->provides.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->provides[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->replaces.empty()) {
                        std::cout << "Replaces        : ";
                        for (size_t i = 0; i < pkg->replaces.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->replaces[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->groups.empty()) {
                        std::cout << "Installed In    : ";
                        for (size_t i = 0; i < pkg->groups.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->groups[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->optdepends.empty()) {
                        std::cout << "Optional Deps   : ";
                        for (size_t i = 0; i < pkg->optdepends.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->optdepends[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->makedepends.empty()) {
                        std::cout << "Make Deps       : ";
                        for (size_t i = 0; i < pkg->makedepends.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->makedepends[i];
                        }
                        std::cout << "\n";
                    }

                    if (!pkg->checkdepends.empty()) {
                        std::cout << "Check Deps      : ";
                        for (size_t i = 0; i < pkg->checkdepends.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << pkg->checkdepends[i];
                        }
                        std::cout << "\n";
                    }
                } else {
                    std::cerr << "error: package '" << pkgname << "' not found in local database\n";
                    return 1;
                }
            }
        }

        if (do_list_files) {
            std::string pkgname = extract_search_query(args);
            if (!pkgname.empty()) {
                auto files = alpm::list_package_files(pkgname);
                for (auto& f : files) {
                    std::cout << pkgname << " " << f << "\n";
                }
            }
        }

        if (do_mirrors) {
            // -Qm: list explicitly installed packages not in repos (orphans/AUR)
            auto local = alpm::get_local_packages();
            for (auto& p : local) {
                bool in_repo = false;
                try {
                    auto repo_pkg = alpm::get_sync_package(p.name);
                    if (repo_pkg) in_repo = true;
                } catch (...) {}
                if (!in_repo && p.reason == alpm::Package::Reason::Explicit) {
                    std::cout << p.name << " " << p.version << "\n";
                }
            }
        }

        if (do_explicit) {
            auto pkgs = alpm::get_explicit_packages();
            for (auto& p : pkgs) {
                std::cout << p.name << " " << p.version << "\n";
            }
        }

        if (do_upgrades) {
            auto upgrades = alpm::get_upgrades();
            for (auto& u : upgrades) {
                std::cout << u.name << " " << u.installed_version << " -> "
                          << u.repo_version << "\n";
            }
        }

        if (do_search) {
            std::string query = extract_search_query(args);
            if (!query.empty()) {
                auto local = alpm::get_local_packages();
                std::string q_lower = query;
                std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);

                for (auto& p : local) {
                    std::string n_lower = p.name;
                    std::transform(n_lower.begin(), n_lower.end(), n_lower.begin(), ::tolower);
                    if (n_lower.find(q_lower) != std::string::npos ||
                        (p.desc.find(query) != std::string::npos)) {
                        std::cout << p.name << " " << p.version << "\n";
                    }
                }
            }
        }

        if (has_dt || do_tree) {
            // -Qdt: list orphans (deps no longer needed)
            // -Qt: list top-level explicitly installed packages
            auto pkgs = alpm::get_local_packages();

            if (has_dt) {
                // Find orphans: deps not required by any explicit package
                std::vector<std::string> orphan_names;
                for (auto& pkg : pkgs) {
                    if (pkg.reason == alpm::Package::Reason::Dependency) {
                        bool still_needed = false;
                        for (auto& other : pkgs) {
                            if (other.reason == alpm::Package::Reason::Explicit) {
                                for (auto& dep : other.depends) {
                                    // Simple name match (strip version constraints)
                                    std::string dep_name = dep;
                                    for (char sep : {'>', '<', '=', '!'}) {
                                        auto pos = dep_name.find(sep);
                                        if (pos != std::string::npos) {
                                            dep_name = dep_name.substr(0, pos);
                                            break;
                                        }
                                    }
                                    if (dep_name == pkg.name) {
                                        still_needed = true;
                                        break;
                                    }
                                }
                            }
                            if (still_needed) break;
                        }
                        if (!still_needed) {
                            orphan_names.push_back(pkg.name);
                        }
                    }
                }

                // Sort for consistent output
                std::sort(orphan_names.begin(), orphan_names.end());

                for (auto& name : orphan_names) {
                    std::cout << name << "\n";
                }
            } else if (do_tree) {
                // -Qt: top-level explicitly installed packages
                auto top_level = alpm::get_top_level_packages();
                for (auto& p : top_level) {
                    if (do_quiet) {
                        std::cout << p.name << "\n";
                    } else {
                        std::cout << p.name << " " << p.version << "\n";
                    }
                }
            }
        }

    } catch (const alpm_error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

/// Run the aur-aware upgrade flow.
int run_upgrade(const cli::Cli& cli) {
    status::print_upgrade_banner();

    // Refresh and upgrade in one transaction. Splitting -Sy and -Su can create
    // a partial-upgrade window and was also skipping refresh because -Syu sets
    // refresh_db (not the AUR-only refresh flag).
    status::print_section(cli.refresh_db
        ? "Syncing databases & upgrading repositories"
        : "Upgrading repository packages");
    auto repo_args = operations::repo_upgrade_args(cli.refresh_db, cli.noconfirm);
    auto inv = pacman::detect_write_op(repo_args);
    if (!inv) {
        std::cerr << "error: failed to construct repository upgrade invocation\n";
        return 1;
    }

    int rc = pacman::run_write_op(*inv);
    if (rc != 0) {
        std::cerr << "error: repository upgrade failed with exit code " << rc << "\n";
        return rc;
    }

    status::print_success("Official repository packages upgraded");

    // Step 3: Detect out-of-date AUR packages using aur_cache (fast, no network)
    auto ootd = deps::detect_out_of_date();

    // Filter dev packages if --nodevel is set
    if (cli.nodevel) {
        ootd.erase(std::remove_if(ootd.begin(), ootd.end(),
            [](const deps::OutOfDatePkg& p) { return deps::is_dev_package(p.name); }),
            ootd.end());
    }

    deps::print_upgrade_plan(ootd);

    if (ootd.empty()) {
        status::print_summary(0, 0, true);
        return 0;
    }

    // Resolve dependencies for all out-of-date AUR packages
    std::vector<std::string> names;
    for (auto& p : ootd) names.push_back(p.name);

    int aur_count = static_cast<int>(ootd.size());
    auto aur_dir = cli.aur_dir.empty() ? std::filesystem::path{"aur"} : cli.aur_dir;

    try {
        auto graph = deps::resolve(names, true, true, false);
        graph.print_plan(names);

        if (graph.aur_count() == 0) {
            status::print_status("No AUR packages to build");
            return 0;
        }

        std::filesystem::create_directories(aur_dir);

        status::print_section("Building " + std::to_string(graph.aur_count())
                      + " AUR package(s) in dependency order");

        // Collect package directories (download PKGBUILDs on-demand)
        std::vector<std::string> pkg_dirs;
        progress::MultiProgress mp("Build Queue");
        std::vector<int> pkg_indices;
        for (size_t i = 0; i < graph.build_order.size(); ++i) {
            pkg_indices.push_back(mp.add_package(graph.build_order[i]));
        }

        // Collect PGP keys from PKGBUILDs before building
        std::set<std::string> all_pgp_keys;
        for (size_t i = 0; i < graph.build_order.size(); ++i) {
            auto& build_pkg = graph.build_order[i];
            mp.set_active(static_cast<int>(i));

            auto pkg_dir = aur_dir / build_pkg;
            if (!std::filesystem::exists(pkg_dir / "PKGBUILD")) {
                mp.set_status("Downloading PKGBUILD...");
                aur::download_pkgbuild(build_pkg, aur_dir);
                mp.update(pkg_indices[i], 20);
            }

            // Parse PKGBUILD to collect validpgpkeys
            auto pg = pkgbuild::Pkgbuild::parse(pkg_dir / "PKGBUILD");
            for (auto& k : pg.validpgpkeys) {
                all_pgp_keys.insert(k);
            }

            pkg_dirs.push_back(pkg_dir.string());
        }

        // Auto-fetch PGP keys if --pgpfetch is set
        if (cli.pgp_fetch && !all_pgp_keys.empty()) {
            status::print_status("Checking for missing PGP keys...");
            std::vector<std::string> keys(all_pgp_keys.begin(), all_pgp_keys.end());
            int rc = pgp::fetch_keys(keys);
            if (rc != 0) {
                status::print_warning("Some PGP keys could not be fetched (continuing build)");
            }
        }

        // Use parallel builds if multiple packages and -j flag set
        int max_jobs = 4;
        for (auto& f : cli.mflags) {
            if (f.rfind("-j", 0) == 0 || f == "-j") {
                try { max_jobs = std::stoi(f.substr(2)); } catch (...) {}
            }
        }
        max_jobs = std::min(max_jobs, static_cast<int>(graph.build_order.size()));
        if (max_jobs < 1) max_jobs = 1;

        bool use_parallel = graph.parallel_groups.size() > 1 && max_jobs > 1;

        if (use_parallel) {
            // Parallel build across dependency groups
            for (auto& group : graph.parallel_groups) {
                status::print_starting("Building stage: ");
                for (size_t i = 0; i < group.packages.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << group.packages[i];
                }
                std::cout << "\n";

                std::vector<std::string> group_dirs;
                for (auto& pkg_name : group.packages) {
                    auto it = std::find(graph.build_order.begin(), graph.build_order.end(), pkg_name);
                    if (it != graph.build_order.end()) {
                        size_t idx = std::distance(graph.build_order.begin(), it);
                        if (idx < pkg_dirs.size()) {
                            group_dirs.push_back(pkg_dirs[idx]);
                        }
                    }
                }

                if (!group_dirs.empty()) {
                    int rc = build::run_parallel_builds(group_dirs, cli,
                                                         config::Config::load(), mp, max_jobs);
                    if (rc != 0) {
                        std::cerr << "error: build failed for stage\n";
                        return 1;
                    }
                }
            }
        } else {
            // Sequential build in dependency order
            for (size_t i = 0; i < graph.build_order.size(); ++i) {
                auto& build_pkg = graph.build_order[i];
                mp.set_active(static_cast<int>(i));

                std::filesystem::current_path(pkg_dirs[i]);

                cli::Cli build_cli = cli;
                build_cli.install = (i == graph.build_order.size() - 1) || cli.install;
                build_cli.nocheck = true;

                mp.set_status("Building...");
                int rc = run_local_build(build_cli, config::Config::load());
                if (rc != 0) {
                    std::cerr << "error: build failed for " << build_pkg << "\n";
                    return rc;
                }

                // Clean up after build (--cleanafter / --keepsrc)
                if (!cli.keep_src && cli.clean_after) {
                    auto pkg_dir = aur_dir / build_pkg;
                    std::filesystem::remove_all(pkg_dir);
                }

                mp.complete(pkg_indices[i]);
            }
        }
        mp.finish();
        std::cout << "\n";

        if (cli.rmdeps) {
            status::print_status("Cleaning up built AUR dependencies...");
            for (auto& entry : std::filesystem::directory_iterator(aur_dir)) {
                if (entry.is_directory()) {
                    status::print_status("Removing " + entry.path().string());
                    std::filesystem::remove_all(entry.path());
                }
            }
        }
    } catch (const source_error& e) {
        std::cerr << "warning: dependency resolution failed: " << e.what() << "\n";
        for (auto& name : names) {
            auto pkg_dir = aur_dir / name;
            if (!std::filesystem::exists(pkg_dir / "PKGBUILD")) {
                aur::download_pkgbuild(name, aur_dir);
            }
            std::filesystem::current_path(pkg_dir);
            cli::Cli build_cli = cli;
            build_cli.install = true;
            run_local_build(build_cli, config::Config::load());
        }
    }

    status::print_summary(0, aur_count, false);
    return 0;
}

int run_local_build(const cli::Cli& cli, const config::Config& config) {
    auto pkgbuild_path = cli.packagefile.empty() ? std::filesystem::path{"PKGBUILD"} : cli.packagefile;

    if (!std::filesystem::exists(pkgbuild_path)) {
        std::cerr << "error: " << pkgbuild_path.string() << " does not exist\n";
        return 1;
    }

    auto pkgbuild = pkgbuild::Pkgbuild::parse(pkgbuild_path);
    build::BuildOrchestrator orchestrator{cli, config, std::move(pkgbuild)};
    return orchestrator.run();
}

int run_aur_build(cli::Cli cli) {
    std::string pkgname;
    if (!cli.packages.empty()) {
        pkgname = cli.packages[0];
    } else if (!cli.extra_env.empty()) {
        for (auto& arg : cli.extra_env) {
            if (arg.find('=') == std::string::npos) {
                pkgname = arg;
                break;
            }
        }
    }

    if (pkgname.empty()) {
        std::cerr << "error: No package name specified. Usage: pacmkr --aur <package-name>\n";
        return 1;
    }

    auto aur_dir = cli.aur_dir.empty() ? std::filesystem::path{"aur"} : cli.aur_dir;
    std::filesystem::create_directories(aur_dir);

    // Beautiful progress bar for single package build
    progress::ProgressBar pb(pkgname);
    pb.set_phase(progress::Phase::Downloading);
    pb.set_status("Fetching from AUR...");

    auto pkgdir = aur::download_pkgbuild(pkgname, aur_dir);
    pb.update(30);
    pb.set_status("PKGBUILD downloaded");

    if (cli.aur_deps) {
        try {
            auto graph = deps::resolve({pkgname}, true, true, false);
            graph.print_plan({pkgname});

            if (graph.aur_count() > 0) {
                status::print_section("Building " + std::to_string(graph.aur_count())
                              + " package(s) in dependency order");

                for (size_t i = 0; i < graph.build_order.size(); ++i) {
                    auto& build_pkg = graph.build_order[i];
                    bool is_root = (build_pkg == pkgname);
                    std::cout << "    " << (i + 1) << ". "
                              << (is_root ? "\xe2\x98\x85" : "\xe2\x86\xb4")
                              << " " << build_pkg << "\n";

                    auto step_dir = aur_dir / build_pkg;
                    if (!std::filesystem::exists(step_dir / "PKGBUILD")) {
                        aur::download_pkgbuild(build_pkg, aur_dir);
                    }

                    std::filesystem::current_path(step_dir);

                    if (!is_root) {
                        cli::Cli dep_cli = cli;
                        dep_cli.install = false;
                        dep_cli.nocheck = true;
                        run_local_build(dep_cli, config::Config::load());
                        std::cout << "\n";
                    }
                }

                auto root_pkgdir = aur_dir / pkgname;
                if (std::filesystem::exists(root_pkgdir)) {
                    std::filesystem::current_path(root_pkgdir);
                }
            }
        } catch (const source_error& e) {
            std::cerr << "warning: dependency resolution failed: " << e.what() << "\n";
            std::cerr << "==> Building without dependency resolution.\n\n";
        }
    }

    if (cli.rmdeps) {
        auto deps_dir = aur_dir / "aur-deps";
        if (std::filesystem::exists(deps_dir)) {
            status::print_status("Cleaning up built AUR dependencies...");
            std::filesystem::remove_all(deps_dir);
        }
    }

    return run_local_build(cli, config::Config::load());
}

/// Aur-aware sync install: check repos first, fall back to AUR for missing packages.
int run_sync_install(const std::vector<std::string>& raw_args) {
    auto packages = extract_install_packages(raw_args);
    if (packages.empty()) {
        std::cerr << "error: no package names specified\n";
        return 1;
    }

    // Separate packages into repo vs AUR
    std::vector<std::string> repo_pkgs, aur_pkgs;
    for (auto& pkg : packages) {
        auto sync_pkg = alpm::get_sync_package(pkg);
        if (sync_pkg) {
            repo_pkgs.push_back(pkg);
        } else {
            // Check if it exists in local db (already installed)
            auto local_pkg = alpm::get_local_package(pkg);
            if (local_pkg) {
                std::cout << "==> " << pkg << " is already installed (version: "
                          << local_pkg->version << ")\n";
                continue;  // skip, already installed
            }
            aur_pkgs.push_back(pkg);
        }
    }

    // Install repo packages via pacman
    if (!repo_pkgs.empty()) {
        status::print_section("Installing " + std::to_string(repo_pkgs.size())
                              + " package(s) from repositories");
        std::vector<std::string> pacman_args = {"-S", "--noconfirm"};
        for (auto& pkg : repo_pkgs) {
            pacman_args.push_back(pkg);
        }
        int rc = pacman::run_write_op(pacman_args);
        if (rc != 0) {
            std::cerr << "error: failed to install repository packages\n";
            return rc;
        }
    }

    // Build AUR packages
    if (!aur_pkgs.empty()) {
        status::print_section("Building " + std::to_string(aur_pkgs.size())
                              + " package(s) from AUR");

        auto aur_dir = std::filesystem::path{"aur"};
        std::filesystem::create_directories(aur_dir);

        for (auto& pkgname : aur_pkgs) {
            status::print_starting("Fetching " + pkgname + " from AUR...");

            auto pkgdir = aur::download_pkgbuild(pkgname, aur_dir);
            status::print_success("PKGBUILD downloaded");

            std::filesystem::current_path(pkgdir);

            cli::Cli build_cli;
            build_cli.install = true;
            build_cli.nocheck = true;

            auto sys_config = config::Config::load();
            int rc = run_local_build(build_cli, sys_config);
            if (rc != 0) {
                std::cerr << "error: failed to build " << pkgname << "\n";
                return rc;
            }
            std::cout << "\n";
        }
    }

    status::print_summary(static_cast<int>(repo_pkgs.size()),
                          static_cast<int>(aur_pkgs.size()), false);
    return 0;
}

/// Apply user config defaults to CLI args.
cli::Cli apply_user_config(cli::Cli cli, const config::UserConfig& cfg) {
    if (!cli.aur_dir.empty() && cfg.aur_dir.has_value()) {
        cli.aur_dir = *cfg.aur_dir;
    }

    if (cli.limit == 10 && cfg.search_limit != 10) {
        cli.limit = cfg.search_limit;
    }

    if (!cli.syncdeps && cfg.sync_deps)   cli.syncdeps = true;
    if (!cli.install && cfg.install)      cli.install = true;
    if (!cli.cleanbuild && cfg.clean_build) cli.cleanbuild = true;
    if (!cli.clean && cfg.clean)          cli.clean = true;

    if (cli.lto == cli::OptMode::Auto && cfg.lto)       cli.lto = cli::OptMode::Enabled;
    if (cli.mold == cli::OptMode::Auto && cfg.mold)     cli.mold = cli::OptMode::Enabled;
    if (cli.graphite == cli::OptMode::Auto && cfg.graphite) cli.graphite = cli::OptMode::Enabled;
    if (cli.polly == cli::OptMode::Auto && cfg.polly)   cli.polly = cli::OptMode::Enabled;

    if (!cli.cc.has_value() && cfg.cc.has_value()) {
        cli.cc = (*cfg.cc == "clang" || *cfg.cc == "llvm") ? cli::Compiler::Clang : cli::Compiler::Gcc;
    }

    return cli;
}

} // anonymous

int main(int argc, char* argv[]) {
    // Initialize libalpm for read operations
    try {
        alpm::init();
    } catch (...) {
        // libalpm init failure is non-fatal — fall back to subprocess mode
    }

    // Initialize AUR cache (loads from disk, fetches fresh if stale)
    aur_cache::init();

    // Detect pacman invocation before CLI parsing
    std::vector<std::string> raw_args;
    for (int i = 1; i < argc; ++i) {
        raw_args.push_back(argv[i]);
    }

    // Aur-specific flags: parse as pacmkr CLI, not forward to pacman
    if (has_aur_flags(raw_args)) {
        // fall through to CLI parsing
    } else if (operations::is_sync_upgrade(raw_args)) {
        // Sync+refresh (-Syu etc.): aur-extended upgrade flow
        cli::Cli cli;
        try {
            cli = cli::parse(argc, argv);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }

        config::UserConfig user_cfg;
        try {
            user_cfg = config::UserConfig::load();
        } catch (...) {}

        cli = apply_user_config(cli, user_cfg);

        if (!cli.chdir.empty()) {
            try {
                std::filesystem::current_path(cli.chdir);
            } catch (const std::exception& e) {
                std::cerr << "error: cannot change to directory '"
                          << cli.chdir.string() << "': " << e.what() << "\n";
                return 1;
            }
        }

        int rc = run_upgrade(cli);
        alpm::shutdown();
        return rc;
    } else if (is_sync_read(raw_args)) {
        // Read-only sync: -Ss, -Si, -Sl, -Sg — use alpm directly
        try {
            int rc = run_sync_read(raw_args);
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    } else if (is_query_read(raw_args)) {
        // Read-only query: -Qi, -Ql, -Qm, etc. — use alpm directly
        try {
            int rc = run_query_read(raw_args);
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    } else if (is_aur_query(raw_args)) {
        // Aur-extended query (--aur -Qm): local + AUR out-of-date
        try {
            auto upgrades = alpm::get_upgrades();

            // Show local out-of-date non-repo packages
            for (auto& u : upgrades) {
                if (!aur::is_official_package(u.name)) {
                    std::cout << u.name << " " << u.installed_version << " -> "
                              << u.repo_version << "\n";
                }
            }

            alpm::shutdown();
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    } else if (is_sync_install(raw_args)) {
        // Sync install: -S pkgname — check repos first, fall back to AUR
        try {
            int rc = run_sync_install(raw_args);
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    } else if (auto write_args = pacman::detect_write_op(raw_args)) {
        // Write operation: forward to pacman subprocess
        int rc = pacman::run_write_op(*write_args);
        alpm::shutdown();
        return rc;
    }

    // Parse CLI args for pacmkr-specific operations
    cli::Cli cli;
    try {
        cli = cli::parse(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        alpm::shutdown();
        return 1;
    }

    config::UserConfig user_cfg;
    try {
        user_cfg = config::UserConfig::load();
    } catch (...) {}

    cli = apply_user_config(cli, user_cfg);

    if (!cli.chdir.empty()) {
        try {
            std::filesystem::current_path(cli.chdir);
        } catch (const std::exception& e) {
            std::cerr << "error: cannot change to directory '"
                      << cli.chdir.string() << "': " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    }

    // Dispatch
    try {
        if (cli.search.has_value()) {
            aur::search_aur(*cli.search, cli.limit);
        } else if (cli.refresh) {
            // Refresh AUR cache before upgrade
            aur_cache::refresh();
            alpm::shutdown();
            return run_upgrade(cli);
        } else if (cli.aur) {
            alpm::shutdown();
            return run_aur_build(cli);
        } else if (cli.getpkgbuild && !cli.packages.empty()) {
            // -G: Download PKGBUILD from AUR
            for (auto& pkgname : cli.packages) {
                status::print_starting("Downloading PKGBUILD for " + pkgname);
                auto info = aur::fetch(pkgname);
                if (!info.pkgname.empty()) {
                    std::string aur_dir = cli.aur_dir.empty() ? "aur" : cli.aur_dir.string();
                    std::string base_dir = aur_dir + "/" + info.pkgname;

                    // Create directory
                    std::filesystem::create_directories(base_dir);

                    // Download PKGBUILD using cpp-httplib
                    httplib::Client cli_aur("aur.archlinux.org");
                    cli_aur.set_max_timeout(30000);

                    auto res = cli_aur.Get("/cgit/aur.git/plain/PKGBUILD?h=" + info.pkgname);
                    if (res && res->status == 200) {
                        std::string pkgbuild_path = base_dir + "/PKGBUILD";
                        std::ofstream out(pkgbuild_path, std::ios::binary);
                        out << res->body;
                        out.close();
                        status::print_success("PKGBUILD downloaded to " + pkgbuild_path);
                    } else {
                        std::cerr << "error: failed to download PKGBUILD for " << pkgname << "\n";
                        return 1;
                    }
                } else {
                    std::cerr << "error: package '" << pkgname << "' not found in AUR\n";
                    return 1;
                }
            }
            alpm::shutdown();
            return 0;
        } else if (cli.build_local) {
            // Explicit local PKGBUILD build: --build / -B
            auto sys_config = config::Config::load(cli.config.empty() ? nullptr : &cli.config);
            int rc = run_local_build(cli, sys_config);
            alpm::shutdown();
            return rc;
        } else {
            // No meaningful operation — show help
            cli::print_help();
            alpm::shutdown();
            aur_cache::shutdown();
            return 0;
        }
    } catch (const error& e) {
        std::cerr << "error: " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
    }

    alpm::shutdown();
    aur_cache::shutdown();
    return 0;
}
