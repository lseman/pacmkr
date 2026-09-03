#include "pacmkr/app/cli.h"
#include "pacmkr/core/config.h"
#include "pacmkr/backend/aur.h"
#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/backend/deps.h"
#include "pacmkr/backend/hybrid_search.h"
#include "pacmkr/core/optimize.h"
#include "pacmkr/app/operations.h"
#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/build/build.h"
#include "pacmkr/core/error.h"
#include "pacmkr/build/pgp.h"
#include "pacmkr/backend/remove.h"
#include "pacmkr/app/terminal.h"
#include "pacmkr/app/json_output.h"

#include <iostream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <sstream>
#include <fstream>
#include <thread>
#include <httplib.h>
#include <limits.h>
#include <unistd.h>

// JSON support — must come after forward-declaring headers.
#include "json.hpp"
using json = nlohmann::json;

namespace {

using namespace pacmkr;

// Forward declarations — functions defined later in this file
int run_upgrade(const cli::Cli& cli);
int run_upgrade_dry_run(const cli::Cli& cli);
int run_local_build(const cli::Cli& cli, const config::Config& config);
int run_aur_build(cli::Cli cli);
int run_sync_install(const cli::Cli& cli);
cli::Cli apply_user_config(cli::Cli cli, const config::UserConfig& cfg);

int reexec_with_sudo(int argc, char* argv[]) {
    char executable[PATH_MAX + 1]{};
    const auto length = readlink("/proc/self/exe", executable, PATH_MAX);
    const std::string program = length > 0
        ? std::string(executable, static_cast<std::size_t>(length))
        : std::string(argv[0]);

    std::vector<std::string> storage{"sudo", "--", program};
    for (int i = 1; i < argc; ++i) storage.emplace_back(argv[i]);
    std::vector<char*> arguments;
    arguments.reserve(storage.size() + 1);
    for (auto& value : storage) arguments.push_back(value.data());
    arguments.push_back(nullptr);

    terminal::info("Administrator privileges required; requesting authorization");
    execvp("sudo", arguments.data());
    std::cerr << "error: unable to start sudo: " << std::strerror(errno) << "\n";
    return 1;
}

class WorkingDirectoryGuard {
public:
    WorkingDirectoryGuard() : original_(std::filesystem::current_path()) {}
    ~WorkingDirectoryGuard() {
        std::error_code error;
        std::filesystem::current_path(original_, error);
    }
    WorkingDirectoryGuard(const WorkingDirectoryGuard&) = delete;
    WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&) = delete;
private:
    std::filesystem::path original_;
};

/// Human-readable byte count for package info output.
std::string human_size(unsigned long long bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << " " << units[unit];
    return out.str();
}

/// Format a Unix timestamp the way pacman renders package dates.
std::string format_time(unsigned long long epoch) {
    if (!epoch) return "Unknown";
    const std::time_t when = static_cast<std::time_t>(epoch);
    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), "%a %d %b %Y %H:%M:%S %Z", std::localtime(&when)))
        return buffer;
    return std::to_string(epoch);
}

/// Print one "Label : value" info line with pacman-style column alignment.
void info_line(const char* label, const std::string& value) {
    std::cout << std::left << std::setw(17) << label << ": " << value << "\n";
}

void info_line(const char* label, const std::vector<std::string>& values, const char* empty = "None") {
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) { if (i) joined += "  "; joined += values[i]; }
    info_line(label, values.empty() ? empty : joined);
}

/// Render package details for -Qi/-Qii/-Qp and -Si/-Sii.
/// repository selects sync-database framing; extended adds the second-level (-ii) fields.
void print_pkg_info(const alpm::Package& pkg, bool repository, bool extended) {
    if (repository) info_line("Repository", pkg.origin_db.empty() ? "unknown" : pkg.origin_db);
    info_line("Name", pkg.name);
    info_line("Version", pkg.version);
    if (!pkg.desc.empty()) info_line("Description", pkg.desc);
    if (!pkg.arch.empty()) info_line("Architecture", pkg.arch);
    if (!pkg.url.empty())  info_line("URL", pkg.url);
    info_line("Licenses", pkg.licenses);
    info_line("Groups", pkg.groups);
    info_line("Provides", pkg.provides);
    info_line("Depends On", pkg.depends);
    info_line("Optional Deps", pkg.optdepends);
    if (!repository) {
        info_line("Required By", pkg.required_by);
        info_line("Optional For", pkg.optional_for);
    }
    info_line("Conflicts With", pkg.conflicts);
    info_line("Replaces", pkg.replaces);
    if (repository || pkg.origin_db == "file")
        info_line("Download Size", human_size(pkg.size));
    info_line("Installed Size", human_size(static_cast<unsigned long long>(pkg.isize)));
    if (!pkg.packager.empty()) info_line("Packager", pkg.packager);
    if (pkg.build_date) info_line("Build Date", format_time(pkg.build_date));
    if (!repository) {
        info_line("Install Date", format_time(pkg.install_date));
        const char* reason =
            pkg.reason == alpm::Package::Reason::Explicit   ? "Explicitly installed" :
            pkg.reason == alpm::Package::Reason::Dependency  ? "Installed as a dependency for another package" :
                                                              "Unknown";
        info_line("Install Reason", reason);
    }
    if (extended) {
        info_line("Make Deps", pkg.makedepends);
        info_line("Check Deps", pkg.checkdepends);
        if (!repository) info_line("Backup Files", pkg.backup);
    }
    std::cout << "\n";
}

/// Check whether arguments select pacmkr's AUR-specific workflow.
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

/// Check if args select download-only sync (-Sw / -Swy / -Suw / --downloadonly).
bool is_sync_download(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        if (arg == "--downloadonly") return true;
        if (arg.size() >= 3 && arg[0] == '-' && arg[1] != '-' && arg[1] == 'S' &&
            arg.find('w', 2) != std::string::npos) {
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

/// Print the compiler and flags pacmkr would use to build packages, resolved
/// the same way BuildOrchestrator::setup_environment() does: makepkg.conf
/// baselines (or the matching environment variable), plus the optimization
/// options (--lto/--mold/--graphite/--polly/--cc) and --mflags.
void print_build_flags(const cli::Cli& cli) {
    const auto config = config::Config::load(
        cli.config.empty() ? nullptr : &cli.config);
    const auto opt = optimize::OptConfig::from_cli(cli);

    const auto baseline = [](const char* name, const std::string& configured) {
        if (const char* value = std::getenv(name)) return std::string(value);
        return configured;
    };
    auto [cflags, cxxflags, ldflags] = opt.apply_flags_to(
        baseline("CFLAGS", config.cflags),
        baseline("CXXFLAGS", config.cxxflags),
        baseline("LDFLAGS", config.ldflags));

    std::string makeflags;
    if (!cli.mflags.empty()) {
        for (std::size_t i = 0; i < cli.mflags.size(); ++i) {
            if (i) makeflags += ' ';
            makeflags += cli.mflags[i];
        }
    } else if (const char* env = std::getenv("MAKEFLAGS")) {
        makeflags = env;
    } else {
        makeflags = config.makeflags;
        const auto marker = makeflags.find("$(nproc)");
        if (marker != std::string::npos) {
            makeflags.replace(marker, 8, std::to_string(
                std::max(1u, std::thread::hardware_concurrency())));
        }
    }

    const bool gcc = opt.compiler == cli::Compiler::Gcc;
    const auto show = [](const std::string& s) {
        return s.empty() ? std::string("(none)") : s;
    };
    std::cout << "CC: "            << (gcc ? "gcc" : "clang")  << "\n"
              << "CXX: "           << (gcc ? "g++" : "clang++") << "\n"
              << "Optimizations: " << opt.summary()            << "\n"
              << "CFLAGS: "        << show(cflags)              << "\n"
              << "CXXFLAGS: "      << show(cxxflags)            << "\n"
              << "LDFLAGS: "       << show(ldflags)             << "\n"
              << "MAKEFLAGS: "     << show(makeflags)           << "\n";
}

/// Check if args represent a read-only query operation (handled by alpm).
bool is_query_read(const std::vector<std::string>& args) {
    if (std::find(args.begin(), args.end(), "-Q") != args.end() ||
        std::find(args.begin(), args.end(), "--query") != args.end()) return true;
    if (operations::is_foreign_package_query(args)) return true;
    for (auto& arg : args) {
        if (arg == "--query" || arg == "-Q") continue;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'Q') {
                // -Qi, -Ql, -Qm, -Qe, -Qu, -Qs, -Qd, -Qt, -Qk, -Qx, -Qp, -Qc are read-only
                for (size_t i = 2; i < arg.size(); ++i) {
                    char f = arg[i];
                    if (f == 'i' || f == 'l' || f == 'm' || f == 'e' || f == 'o' ||
                        f == 'u' || f == 's' || f == 'd' || f == 't' ||
                        f == 'k' || f == 'x' || f == 'p' || f == 'c') {
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
    // Check for --json flag
    bool json_mode = std::find(args.begin(), args.end(), "--json") != args.end();

    // Detect sub-operation
    bool do_search = false, do_info = false, do_list = false, do_groups = false;
    int info_level = 0;

    for (auto& arg : args) {
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'S') {
                for (size_t i = 2; i < arg.size(); ++i) {
                    if (arg[i] == 's') do_search = true;
                    if (arg[i] == 'i') { do_info = true; ++info_level; }
                    if (arg[i] == 'l') do_list = true;
                    if (arg[i] == 'g') do_groups = true;
                }
            }
        }
        if (arg == "--search") do_search = true;
        if (arg == "--info") { do_info = true; ++info_level; }
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

                // Unified hybrid search: rank repo and AUR results together by relevance
                auto results = hybrid_search::search(query, limit);

                // JSON output
                if (json_mode) {
                    json j;
                    j["query"] = query;
                    j["results"] = json::array();
                    for (auto& r : results) {
                        json pkg;
                        pkg["name"] = r.name;
                        pkg["version"] = r.version;
                        if (r.desc) pkg["description"] = *r.desc;
                        pkg["source"] = (r.source == hybrid_search::Source::Repository) ? "repository" : "aur";
                        if (r.source == hybrid_search::Source::Repository) {
                            pkg["origin_db"] = r.origin_db;
                        } else {
                            pkg["num_votes"] = r.numvotes;
                            pkg["popularity"] = r.popularity;
                        }
                        pkg["relevance_score"] = r.relevance_score;
                        j["results"].push_back(pkg);
                    }
                    std::cout << json_output::serialize(j) << "\n";
                } else {
                    // Print unified results
                    for (size_t i = 0; i < results.size(); ++i) {
                        auto& r = results[i];
                        if (r.source == hybrid_search::Source::Repository) {
                            std::cout << r.origin_db << "/" << r.name << "  " << r.version;
                        } else {
                            std::cout << "aur/" << r.name << "  " << r.version;
                        }
                        if (r.desc.has_value() && !r.desc->empty()) {
                            std::cout << "    " << *r.desc;
                        }
                        std::cout << "\n";
                    }
                }

                // Interactive picker in TTY mode (text only)
                bool is_tty = isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
                if (is_tty && !results.empty()) {
                    std::cout << "\n==> Type a number to select, or Enter for first result, Esc to cancel\n";

                    // Simple number-based selection on unified results
                    std::cout << "\nSelect package [1-" << results.size() << "]: ";
                    std::string input;
                    std::getline(std::cin, input);

                    if (!input.empty() && input != "\x1b") {  // Not Esc
                        try {
                            unsigned int idx = std::stoul(input) - 1;
                            if (idx < results.size()) {
                                auto& r = results[idx];
                                std::cout << "\n==> Selected: " << r.name << "\n";
                                std::cout << "     Source: " << (r.source == hybrid_search::Source::Repository ? r.origin_db : "aur") << "\n";
                                std::cout << "     To install: pacmkr -S " << r.name << "\n";
                            } else {
                                std::cout << "Invalid selection.\n";
                            }
                        } catch (...) {
                            // Empty or invalid input - select first by default
                            if (input.empty()) {
                                std::cout << "\n==> Selected: " << results[0].name << " (default)\n";
                                std::cout << "     To install: pacmkr -S " << results[0].name << "\n";
                            } else {
                                std::cout << "Invalid selection.\n";
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
                    if (json_mode) {
                        json j;
                        j["name"] = pkgname;
                        j["source"] = "repository";
                        j["package"] = json_output::alpm_package_to_json(*pkg);
                        std::cout << json_output::serialize(j) << "\n";
                    } else {
                        print_pkg_info(*pkg, /*repository=*/true, info_level >= 2);
                    }
                } else {
                    // Try AUR
                    try {
                        auto info = aur::fetch(pkgname);
                        if (json_mode) {
                            json j;
                            j["name"] = pkgname;
                            j["source"] = "aur";
                            j["package"] = json_output::aur_package_info_to_json(info);
                            std::cout << json_output::serialize(j) << "\n";
                        } else {
                            std::cout << "     Name          : " << info.name << "\n"
                                      << "     Description   : " << (info.desc.value_or("N/A")) << "\n"
                                      << "     URL           : " << (info.url.value_or("N/A")) << "\n"
                                      << "     Version       : " << info.version << "\n";
                        }
                    } catch (const source_error&) {
                        std::cerr << "error: package '" << pkgname << "' not found in repos or AUR\n";
                        return 1;
                    }
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
            for (const auto& group : alpm::list_sync_groups()) {
                for (const auto& package : group.packages)
                    std::cout << group.repository << " " << group.name << " " << package << "\n";
            }
        }

    } catch (const alpm_error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

/// Run a read-only query operation via alpm.
int run_query_read(const std::vector<std::string>& args,
                   const std::vector<std::string>& parsed_targets = {}) {
    bool do_info = false, do_list_files = false, do_mirrors = false, do_orphans = false, do_owned = false,
         do_explicit = false, do_upgrades = false, do_search = false,
         do_dependents = false, do_tree = false, do_check = false,
         do_file = false, do_changelog = false,
         do_quiet = operations::is_quiet_query(args);
    int info_level = 0;

    for (auto& arg : args) {
        if (arg == "--list-foreign") do_mirrors = true;
        if (arg == "--orphans")        do_orphans = true;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'Q') {
                for (size_t i = 2; i < arg.size(); ++i) {
                    char f = arg[i];
                    if (f == 'i') { do_info = true; ++info_level; }
                    if (f == 'l') do_list_files = true;
                    if (f == 'm') do_mirrors = true;
                    if (f == 'o') do_owned = true;
                    if (f == 'e') do_explicit = true;
                    if (f == 'u') do_upgrades = true;
                    if (f == 's') do_search = true;
                    if (f == 'd') do_dependents = true;
                    if (f == 't') do_tree = true;
                    if (f == 'k') do_check = true;
                    if (f == 'p') do_file = true;
                    if (f == 'c') do_changelog = true;
                    if (f == 'q') do_quiet = true;
                }
            }
        }
    }

    // Package-file and changelog queries fully replace the local-database read.
    if (do_file || do_changelog) {
        std::vector<std::string> targets = parsed_targets;
        if (targets.empty()) {
            const auto single = extract_search_query(args);
            if (!single.empty()) targets.push_back(single);
        }
        if (targets.empty()) {
            std::cerr << "error: " << (do_file ? "-Qp" : "-Qc") << " requires a target\n";
            return 1;
        }
        int status = 0;
        try {
            for (const auto& target : targets) {
                if (do_changelog) {
                    const auto pkg = alpm::get_local_package(target);
                    if (!pkg) { std::cerr << "error: package '" << target << "' was not found\n"; return 1; }
                    const auto text = alpm::get_changelog(target);
                    if (text.empty()) {
                        std::cerr << "error: no changelog available for '" << target << "'\n";
                        status = 1;
                    } else {
                        std::cout << text;
                    }
                    continue;
                }
                const auto pkg = alpm::load_package_file(target);
                if (do_list_files) {
                    for (const auto& file : pkg.files) std::cout << pkg.name << " /" << file << "\n";
                } else if (do_info) {
                    print_pkg_info(pkg, /*repository=*/false, info_level >= 2);
                } else {
                    std::cout << pkg.name << " " << pkg.version << "\n";
                }
            }
        } catch (const alpm_error& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
        return status;
    }

    // -Qdt: orphans (top-level deps that are no longer needed)
    const bool has_dt = do_dependents && do_tree;

    try {
        const bool has_filter = do_info || do_list_files || do_mirrors || do_orphans || do_owned || do_explicit ||
                                do_upgrades || do_search || do_dependents || do_tree || do_check;
        if (!has_filter) {
            const auto& targets = parsed_targets;
            if (targets.empty()) {
                for (const auto& pkg : alpm::get_local_packages())
                    std::cout << pkg.name << " " << pkg.version << "\n";
            } else {
                for (const auto& target : targets) {
                    const auto pkg = alpm::get_local_package(target);
                    if (!pkg) { std::cerr << "error: package '" << target << "' was not found\n"; return 1; }
                    std::cout << pkg->name << " " << pkg->version << "\n";
                }
            }
        }
        if (do_info) {
            std::string pkgname = extract_search_query(args);
            if (!pkgname.empty()) {
                auto pkg = alpm::get_local_package(pkgname);
                if (pkg) {
                    print_pkg_info(*pkg, /*repository=*/false, info_level >= 2);
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

        if (do_owned) {
            const auto path = extract_search_query(args);
            if (auto owner = alpm::find_file_owner(path)) std::cout << path << " is owned by " << *owner << "\n";
            else { std::cerr << "error: no package owns " << path << "\n"; return 1; }
        }

        if (do_mirrors) {
            // Foreign means absent from all sync databases. Installation
            // reason is irrelevant: AUR dependencies are foreign too.
            for (const auto& pkg : alpm::get_foreign_packages()) {
                std::cout << pkg.name;
                if (!do_quiet) std::cout << " " << pkg.version;
                std::cout << "\n";
            }
        }

        if (do_orphans) {
            // Orphans: foreign packages installed as dependencies that no longer
            // satisfy any explicit package's requirements.
            auto orphans = alpm::get_orphan_packages();
            if (orphans.empty()) {
                std::cout << "No orphan packages found.\n";
            } else {
                std::cout << "Orphan packages (" << orphans.size() << "):\n";
                for (const auto& pkg : orphans) {
                    std::cout << pkg.name;
                    if (!do_quiet) std::cout << " " << pkg.version;
                    // Show what originally pulled it in, if known
                    if (!pkg.required_by.empty() && !do_quiet)
                        std::cout << "  (was required by: " << pkg.required_by.front() << ")";
                    std::cout << "\n";
                }
            }
        }

        if (do_explicit && !do_tree) {
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

        if (do_check) {
            std::string package = extract_search_query(args);
            if (package.empty()) {
                for (const auto& arg : args) if (!arg.empty() && arg.front() != '-') { package = arg; break; }
            }
            if (package.empty()) { std::cerr << "error: -Qk requires a package name\n"; return 1; }
            size_t missing = 0;
            const auto files = alpm::list_package_files(package);
            for (const auto& file : files) {
                const auto path = std::filesystem::path{"/"} / file;
                if (!std::filesystem::exists(path)) { ++missing; std::cout << "warning: " << path << " is missing\n"; }
            }
            std::cout << package << ": " << files.size() << " total files, " << missing << " missing files\n";
            if (missing) return 1;
        }

        if (has_dt || do_tree || do_dependents) {
            // -Qdt: list orphans (deps no longer needed)
            // -Qt: list top-level explicitly installed packages
            auto pkgs = alpm::get_local_packages();

            if (has_dt) {
                for (const auto& pkg : alpm::get_orphan_packages())
                    std::cout << pkg.name << "\n";
            } else if (do_tree) {
                // -Qt: installed packages not required by another package.
                for (auto& p : pkgs) {
                    if (!p.required_by.empty()) continue;
                    if (do_dependents && p.reason != alpm::Package::Reason::Dependency) continue;
                    if (do_explicit && p.reason != alpm::Package::Reason::Explicit) continue;
                    if (do_quiet) {
                        std::cout << p.name << "\n";
                    } else {
                        std::cout << p.name << " " << p.version << "\n";
                    }
                }
            } else if (do_dependents) {
                for (const auto& p : pkgs)
                    if (p.reason == alpm::Package::Reason::Dependency)
                        std::cout << p.name << (do_quiet ? "" : " " + p.version) << "\n";
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
    WorkingDirectoryGuard cwd_guard;
    terminal::title("System upgrade");

    terminal::section(cli.refresh_db
        ? "Syncing databases & upgrading repositories"
        : "Upgrading repository packages");

    // Kick off the AUR out-of-date check now: its libalpm reads run
    // synchronously here, then the network query runs on a worker thread while
    // the repository transaction below proceeds, hiding the round-trip.
    auto ootd_future = deps::detect_out_of_date_async();

    // Refresh and upgrade in one transaction. Splitting -Sy and -Su can create
    // a partial-upgrade window and was also skipping refresh because -Syu sets
    // refresh_db (not the AUR-only refresh flag).
    if (cli.refresh_db) alpm::refresh_databases();
    int rc = alpm::system_upgrade(false, cli.noconfirm,
                                  cli.ignore, cli.ignoregroup, cli.overwrite);
    if (rc != 0) {
        std::cerr << "error: repository upgrade failed with exit code " << rc << "\n";
        return rc;
    }

    terminal::success("Repository phase complete");

    // Step 3: Collect the AUR out-of-date results started before the transaction.
    std::vector<deps::OutOfDatePkg> ootd;
    try {
        ootd = ootd_future.get();
    } catch (const std::exception& e) {
        std::cerr << "warning: AUR update check failed: " << e.what() << "\n";
    }

    // Filter dev packages if --nodevel is set
    if (cli.nodevel) {
        ootd.erase(std::remove_if(ootd.begin(), ootd.end(),
            [](const deps::OutOfDatePkg& p) { return deps::is_dev_package(p.name); }),
            ootd.end());
    }

    if (ootd.empty()) {
        terminal::success("AUR packages are already current");
        std::cout << "\n";
        terminal::success("System upgrade complete");
        return 0;
    }

    terminal::section("AUR updates");
    deps::print_upgrade_plan(ootd);

    // Resolve dependencies for all out-of-date AUR packages
    std::vector<std::string> names;
    for (auto& p : ootd) names.push_back(p.name);

    int aur_count = static_cast<int>(ootd.size());
    const auto aur_dir = std::filesystem::absolute(
        cli.aur_dir.empty() ? std::filesystem::path{"aur"} : cli.aur_dir);

    try {
        auto graph = deps::resolve(names, true, true, false);
        graph.print_plan(names);

        if (graph.aur_count() == 0) {
            return 0;
        }

        std::filesystem::create_directories(aur_dir);

        std::cout << "==> Building " << graph.aur_count()
                  << " AUR package(s) in dependency order\n";

        // Collect package directories (download PKGBUILDs on-demand)
        std::vector<std::string> pkg_dirs;
        // Collect PGP keys from PKGBUILDs before building
        std::set<std::string> all_pgp_keys;
        for (size_t i = 0; i < graph.build_order.size(); ++i) {
            auto& build_pkg = graph.build_order[i];
            terminal::info("Preparing " + build_pkg);
            const auto pkgbuild_path = aur::download_pkgbuild(build_pkg, aur_dir);
            const auto pkg_dir = pkgbuild_path.parent_path();

            // Parse PKGBUILD to collect validpgpkeys
            auto pg = pkgbuild::Pkgbuild::parse(pkg_dir / "PKGBUILD");
            for (auto& k : pg.validpgpkeys) {
                all_pgp_keys.insert(k);
            }

            pkg_dirs.push_back(pkg_dir.string());
        }

        // Auto-fetch PGP keys if --pgpfetch is set
        if (cli.pgp_fetch && !all_pgp_keys.empty()) {
            std::cout << "==> Checking for missing PGP keys...\n";
            std::vector<std::string> keys(all_pgp_keys.begin(), all_pgp_keys.end());
            int rc = pgp::fetch_keys(keys);
            if (rc != 0) {
                std::cerr << "warning: some PGP keys could not be fetched (continuing build)\n";
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

        // Building currently changes the process working directory. Keep AUR
        // builds sequential until each build has a fully isolated workspace.
        bool use_parallel = false;

        if (use_parallel) {
            // Parallel build across dependency groups
            for (auto& group : graph.parallel_groups) {
                std::cout << "==> Building stage: ";
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
                                                         config::Config::load(), max_jobs);
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
                std::filesystem::current_path(pkg_dirs[i]);

                cli::Cli build_cli = cli;
                // AUR dependencies must be installed before their dependents.
                build_cli.install = true;
                build_cli.nocheck = true;

                std::cout << "==> Building " << build_pkg << "\n";
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

            }
        }
        std::cout << "\n";

        if (cli.rmdeps) {
            std::cout << "==> Cleaning up built AUR dependencies...\n";
            for (auto& entry : std::filesystem::directory_iterator(aur_dir)) {
                if (entry.is_directory()) {
                    std::cout << "==> Removing " << entry.path().string() << "\n";
                    std::filesystem::remove_all(entry.path());
                }
            }
        }
    } catch (const source_error& e) {
        std::cerr << "error: AUR build failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "==> Upgrade complete. Repos: 0, AUR: " << aur_count << "\n";
    return 0;
}

/// Preview what a system upgrade would do without executing anything.
int run_upgrade_dry_run(const cli::Cli& cli) {
    terminal::title("Upgrade preview");

    // Repository upgrades
    auto repo_upgrades = alpm::get_upgrades();
    if (!repo_upgrades.empty()) {
        terminal::section("Repository upgrades (" + std::to_string(repo_upgrades.size()) + ")");
        for (auto& u : repo_upgrades) {
            std::cout << "  " << u.name << ": " << u.installed_version << " -> " << u.repo_version << "\n";
        }
    } else {
        terminal::success("Repository packages are already current");
    }

    // AUR out-of-date check
    auto ootd_future = deps::detect_out_of_date_async();
    
    // Wait for AUR check to complete (it runs on a background thread)
    std::vector<deps::OutOfDatePkg> ootd;
    try {
        ootd = ootd_future.get();
    } catch (const std::exception& e) {
        std::cerr << "warning: AUR update check failed: " << e.what() << "\n";
    }

    // Filter dev packages if --nodevel is set
    if (cli.nodevel) {
        ootd.erase(std::remove_if(ootd.begin(), ootd.end(),
            [](const deps::OutOfDatePkg& p) { return deps::is_dev_package(p.name); }),
            ootd.end());
    }

    if (!ootd.empty()) {
        terminal::section("AUR updates (" + std::to_string(ootd.size()) + ")");
        for (auto& p : ootd) {
            std::cout << "  aur/" << p.name << ": " << p.installed_version << " -> " << p.aur_version;
            if (!p.desc.empty()) std::cout << "  -- " << p.desc;
            std::cout << "\n";
        }

        // Show dependency resolution plan
        std::vector<std::string> names;
        for (auto& p : ootd) names.push_back(p.name);
        
        try {
            auto graph = deps::resolve(names, true, true, false);
            if (!graph.conflicts.empty()) {
                terminal::warning("Dependency conflicts detected:");
                for (auto& c : graph.conflicts) {
                    std::cout << "  CONFLICT: " << c.conflicting << " (required by " << c.by << ": " << c.reason << ")\n";
                }
            }
            if (!graph.build_order.empty()) {
                std::cout << "\n==> Would build " << graph.aur_count() << " AUR package(s):\n";
                for (size_t i = 0; i < graph.build_order.size(); ++i) {
                    std::cout << "  " << (i + 1) << ". " << graph.build_order[i];
                    auto it = graph.nodes.find(graph.build_order[i]);
                    if (it != graph.nodes.end() && !it->second.aur_deps.empty()) {
                        std::cout << " [deps: ";
                        for (size_t j = 0; j < it->second.aur_deps.size(); ++j) {
                            if (j > 0) std::cout << ", ";
                            std::cout << it->second.aur_deps[j];
                        }
                        std::cout << "]";
                    }
                    std::cout << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "warning: dependency resolution failed: " << e.what() << "\n";
        }
    } else {
        terminal::success("AUR packages are already current");
    }

    // Summary
    std::cout << "\n==> Summary: Repos: " << repo_upgrades.size()
              << ", AUR: " << ootd.size() << "\n";
    std::cout << "==> No changes were made.\n";
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
    WorkingDirectoryGuard cwd_guard;
    terminal::title("AUR build");
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

    const auto aur_dir = std::filesystem::absolute(
        cli.aur_dir.empty() ? std::filesystem::path{"aur"} : cli.aur_dir);
    std::filesystem::create_directories(aur_dir);

    terminal::info("Preparing " + pkgname);
    const auto pkgbuild_path = aur::download_pkgbuild(pkgname, aur_dir);
    const auto pkgdir = pkgbuild_path.parent_path();
    terminal::success("PKGBUILD ready");

    if (cli.aur_deps) {
        try {
            auto graph = deps::resolve({pkgname}, true, true, false);
            graph.print_plan({pkgname});

            if (graph.aur_count() > 0) {
                std::cout << "==> Building " << graph.aur_count()
                          << " package(s) in dependency order\n";

                for (size_t i = 0; i < graph.build_order.size(); ++i) {
                    auto& build_pkg = graph.build_order[i];
                    bool is_root = (build_pkg == pkgname);
                    std::cout << "    " << (i + 1) << ". "
                              << (is_root ? "\xe2\x98\x85" : "\xe2\x86\xb4")
                              << " " << build_pkg << "\n";

                    const auto step_pkgbuild = aur::download_pkgbuild(build_pkg, aur_dir);
                    const auto step_dir = step_pkgbuild.parent_path();

                    std::filesystem::current_path(step_dir);

                    if (!is_root) {
                        cli::Cli dep_cli = cli;
                        dep_cli.install = true;
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
            std::cout << "==> Cleaning up built AUR dependencies...\n";
            std::filesystem::remove_all(deps_dir);
        }
    }

    return run_local_build(cli, config::Config::load());
}

/// Aur-aware sync install: check repos first, fall back to AUR for missing packages.
int run_sync_install(const cli::Cli& cli) {
    WorkingDirectoryGuard cwd_guard;
    terminal::title("Package installation");
    const auto& packages = cli.packages;
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
            auto local_pkg = alpm::get_local_package(pkg);
            if (local_pkg) {
                const auto aur_info = aur::fetch(pkg);
                if (aur_info.version.empty() ||
                    deps::compare_versions(aur_info.version, local_pkg->version) <= 0) {
                    terminal::success(pkg + " is already current (" + local_pkg->version + ")");
                    continue;
                }
                terminal::info("Upgrade " + pkg + " " + local_pkg->version
                               + " -> " + aur_info.version + " (AUR)");
            }
            aur_pkgs.push_back(pkg);
        }
    }

    // Install repository packages through libalpm.
    if (!repo_pkgs.empty()) {
        terminal::section("Repository packages");
        terminal::info("Installing " + std::to_string(repo_pkgs.size()) + " package(s)");
        int rc = alpm::install(repo_pkgs, cli.needed, cli.noconfirm,
                               cli.ignore, cli.ignoregroup, cli.overwrite);
        if (rc != 0) {
            std::cerr << "error: failed to install repository packages\n";
            return rc;
        }
    }

    // Build AUR packages
    if (!aur_pkgs.empty()) {
        terminal::section("AUR packages");
        terminal::info("Building " + std::to_string(aur_pkgs.size()) + " package(s)");

        const auto aur_dir = std::filesystem::absolute(
            cli.aur_dir.empty() ? std::filesystem::path{"aur"} : cli.aur_dir);
        std::filesystem::create_directories(aur_dir);

        for (auto& pkgname : aur_pkgs) {
            terminal::info("Preparing " + pkgname);

            const auto pkgbuild_path = aur::download_pkgbuild(pkgname, aur_dir);
            const auto pkgdir = pkgbuild_path.parent_path();
            terminal::success("PKGBUILD ready");

            std::filesystem::current_path(pkgdir);

            cli::Cli build_cli = cli;
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

    terminal::success("Install complete (repositories: " + std::to_string(repo_pkgs.size())
                      + ", AUR: " + std::to_string(aur_pkgs.size()) + ")");
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
    // Initialize the native libalpm backend used by all package operations.
    try {
        std::string pacman_config_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) pacman_config_path = argv[++i];
            else if (arg.rfind("--config=", 0) == 0) pacman_config_path = arg.substr(9);
        }
        alpm::init("/", "/var/lib/pacman/", pacman_config_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    // Preserve pacman-compatible argument forms before CLI parsing.
    std::vector<std::string> raw_args;
    for (int i = 1; i < argc; ++i) {
        raw_args.push_back(argv[i]);
    }

    // --flags: report the compiler and flags used to build packages, then exit.
    // Handled here so it works regardless of the operation and never needs root.
    if (std::find(raw_args.begin(), raw_args.end(), "--flags") != raw_args.end()) {
        try {
            cli::Cli flags_cli = cli::parse(argc, argv);
            config::UserConfig user_cfg;
            try { user_cfg = config::UserConfig::load(); } catch (...) {}
            print_build_flags(apply_user_config(std::move(flags_cli), user_cfg));
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
        alpm::shutdown();
        return 0;
    }

    if (geteuid() != 0 && alpm::uses_system_root() && operations::requires_root(raw_args)) {
        alpm::shutdown();
        return reexec_with_sudo(argc, argv);
    }

    // Initialize AUR cache only after any required privilege re-exec, so the
    // authorization prompt is immediate and initialization is not duplicated.
    aur_cache::init();

    if (raw_args == std::vector<std::string>{"--native-system-upgrade"}) {
        try {
            alpm::refresh_databases();
            int rc = alpm::system_upgrade(false, true);
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    }

    // AUR-specific flags use pacmkr's extended CLI workflow.
    if (has_aur_flags(raw_args)) {
        // fall through to CLI parsing
    } else if (is_sync_download(raw_args)) {
        // Download-only sync (-Sw / -Swy / -Suw): fetch to the cache, install nothing.
        try {
            auto cli = cli::parse(argc, argv);
            if (cli.packages.empty() && !cli.sysupgrade)
                throw std::runtime_error("no targets specified (use -Suw to fetch all pending upgrades)");
            if (cli.refresh_db) alpm::refresh_databases();
            int rc = alpm::download(cli.packages, cli.sysupgrade, cli.noconfirm,
                                    cli.ignore, cli.ignoregroup);
            alpm::shutdown();
            aur_cache::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            aur_cache::shutdown();
            return 1;
        }
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

        try {
            if (cli.dry_run) {
                int rc = run_upgrade_dry_run(cli);
                alpm::shutdown();
                aur_cache::shutdown();
                return rc;
            }
            int rc = run_upgrade(cli);
            alpm::shutdown();
            aur_cache::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            aur_cache::shutdown();
            return 1;
        }
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
            int rc = run_query_read(raw_args, cli::parse(argc, argv).packages);
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
            auto sync_cli = apply_user_config(cli::parse(argc, argv), config::UserConfig::load());
            int rc = run_sync_install(sync_cli);
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
    } else if (!raw_args.empty() && (raw_args.front() == "-Sy" || raw_args.front() == "--refresh")) {
        try {
            int rc = alpm::refresh_databases();
            alpm::shutdown();
            return rc;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            alpm::shutdown();
            return 1;
        }
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
        if (cli.operation == cli::Cli::Op::Remove) {
            if (cli.packages.empty()) throw std::runtime_error("no packages specified for removal");
            return alpm::remove(cli.packages, cli.remove_deps || cli.recursive_remove,
                                cli.remove_configs, cli.nosave, cli.noconfirm,
                                cli.recursive_remove, false);
        } else if (cli.operation == cli::Cli::Op::Database) {
            if (cli.packages.empty()) throw std::runtime_error("no packages specified for database operation");
            if (cli.database_add == cli.database_remove)
                throw std::runtime_error("use exactly one of --asexplicit (-Da) or --asdeps (-Dr)");
            return alpm::set_install_reason(cli.packages, cli.database_add);
        } else if (cli.operation == cli::Cli::Op::Deptest) {
            const auto missing = alpm::missing_dependencies(cli.packages);
            for (const auto& dependency : missing) std::cout << dependency << "\n";
            return missing.empty() ? 0 : 127;
        } else if (cli.operation == cli::Cli::Op::Files) {
            if (!cli.files_search && !cli.files_list && !cli.files_info)
                throw std::runtime_error("a file operation such as -Fs is required");
            if (cli.packages.empty()) throw std::runtime_error("no file search term specified");
            bool found = false;
            for (const auto& query : cli.packages) {
                for (const auto& match : alpm::search_sync_files(query)) {
                    found = true;
                    std::cout << match.repository << "/" << match.package << " " << match.path << "\n";
                }
            }
            return found ? 0 : 1;
        } else if (cli.operation == cli::Cli::Op::Upgrade) {
            std::vector<std::string> files;
            for (const auto& arg : raw_args) if (!arg.empty() && arg.front() != '-') files.push_back(arg);
            return alpm::install_files(files, cli.noconfirm);
        } else if (cli.operation == cli::Cli::Op::Sync && cli.sync_clean) {
            // -Sc drops cached versions that are no longer installed; -Scc wipes
            // every cache file and the downloaded sync databases.
            int clean_count = 0;
            for (const auto& arg : raw_args) {
                if (arg == "--clean") ++clean_count;
                else if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'S' && arg[2] != '-')
                    clean_count += static_cast<int>(std::count(arg.begin() + 2, arg.end(), 'c'));
            }
            auto result = alpm::clean_cache(clean_count >= 2, cli.noconfirm);
            std::cout << "==> Removed " << result.files_removed << " file(s), "
                      << human_size(result.bytes_freed) << " reclaimed\n";
            return 0;
        } else if (cli.search.has_value()) {
            aur::search_aur(*cli.search, cli.limit, cli.json_output);
        } else if (cli.refresh) {
            // Refresh AUR cache before upgrade
            aur_cache::refresh();
            return run_upgrade(cli);
        } else if (cli.aur) {
            alpm::shutdown();
            return run_aur_build(cli);
        } else if (cli.getpkgbuild && !cli.packages.empty()) {
            // -G: Download PKGBUILD from AUR
            for (auto& pkgname : cli.packages) {
                std::cout << "==> Downloading PKGBUILD for " << pkgname << "\n";
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
                        std::cout << "==> PKGBUILD downloaded to " << pkgbuild_path << "\n";
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
        alpm::shutdown();
        aur_cache::shutdown();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        alpm::shutdown();
        aur_cache::shutdown();
        return 1;
    }

    alpm::shutdown();
    aur_cache::shutdown();
    return 0;
}
