#include "pacmkr/cli.h"
#include "pacmkr/error.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace pacmkr::cli {

namespace {

OptMode parse_opt_mode(const std::optional<std::string>& s) {
    if (!s) return OptMode::Auto;
    auto v = *s;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "yes" || v == "y" || v == "1") return OptMode::Enabled;
    if (v == "no"  || v == "n" || v == "0") return OptMode::Disabled;
    return OptMode::Auto;
}

Compiler parse_compiler(const std::optional<std::string>& s) {
    if (!s) return Compiler::Gcc;
    auto v = *s;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "gcc" || v == "gnu") return Compiler::Gcc;
    if (v == "clang" || v == "llvm") return Compiler::Clang;
    throw opt_error("Unknown compiler: " + *s);
}

std::optional<std::string> peek_value(char** argv, int argc, size_t i) {
    if (i + 1 < static_cast<size_t>(argc)) {
        std::string next(argv[i + 1]);
        if (next == "--" || !detail::starts_with(next, "-")) {
            return next;
        }
    }
    return std::nullopt;
}

void set_bool(Cli& cli, const std::string& flag, bool val) {
    // ─── Pacman operation flags ────────────────────────────────────
    if (flag == "operation-d" || flag == "database")  { cli.operation = Cli::Op::Database; }
    else if (flag == "operation-f" || flag == "files") { cli.operation = Cli::Op::Files; }
    else if (flag == "operation-q" || flag == "query") { cli.operation = Cli::Op::Query; }
    else if (flag == "operation-r" || flag == "remove") { cli.operation = Cli::Op::Remove; }
    else if (flag == "operation-s" || flag == "sync" || flag == "sync-flag") { cli.operation = Cli::Op::Sync; }
    else if (flag == "operation-t" || flag == "deptest") { cli.operation = Cli::Op::Deptest; }
    else if (flag == "operation-u" || flag == "upgrade") { cli.operation = Cli::Op::Upgrade; }

    // Sync sub-flags
    else if (flag == "sync-search")      cli.sync_search = val;
    else if (flag == "sync-info")        cli.sync_info = val;
    else if (flag == "sync-list")        cli.sync_list = val;
    else if (flag == "sync-groups")      cli.sync_groups = val;
    else if (flag == "sync-download")    cli.sync_download = val;
    else if (flag == "sync-clean")       cli.sync_clean = val;
    else if (flag == "sysupgrade")       cli.sysupgrade = val;
    else if (flag == "refresh-db")       cli.refresh_db = val;
    else if (flag == "sync-force")       cli.sync_force = val;

    // Query sub-flags
    else if (flag == "query-info")        cli.query_info = val;
    else if (flag == "query-list-files")  cli.query_list_files = val;
    else if (flag == "query-mirrors")     cli.query_mirrors = val;
    else if (flag == "query-owned")       cli.query_owned = val;
    else if (flag == "query-dependents")  cli.query_dependents = val;
    else if (flag == "query-explicit")    cli.query_explicit = val;
    else if (flag == "query-upgrades")    cli.query_upgrades = val;
    else if (flag == "query-search")      cli.query_search = val;
    else if (flag == "query-check")       cli.query_check = val;
    else if (flag == "query-text")        cli.query_text = val;
    else if (flag == "query-tree")        cli.query_tree = val;
    else if (flag == "quiet")             cli.quiet = val;

    // Remove sub-flags
    else if (flag == "noconfirm")        cli.noconfirm = val;
    else if (flag == "remove-deps")      cli.remove_deps = val;
    else if (flag == "remove-configs")   cli.remove_configs = val;

    // Database sub-flags
    else if (flag == "database-sync")    cli.database_sync = val;
    else if (flag == "database-add")     cli.database_add = val;
    else if (flag == "database-remove")  cli.database_remove = val;

    // Files sub-flags
    else if (flag == "files-search")     cli.files_search = val;
    else if (flag == "files-info")       cli.files_info = val;
    else if (flag == "files-list")       cli.files_list = val;

    // PKGBUILD options ────────────────────────────────────────────
    else if (flag == "check")            cli.check = val;
    else if (flag == "nocheck")          cli.nocheck = val;
    else if (flag == "noprepare")        cli.noprepare = val;
    else if (flag == "cleanbuild")       cli.cleanbuild = val;
    else if (flag == "clean")            cli.clean = val;
    else if (flag == "force")            cli.force = val;
    else if (flag == "install")          cli.install = val;
    else if (flag == "noarchive")        cli.noarchive = val;
    else if (flag == "geninteg")         cli.geninteg = val;
    else if (flag == "skipchecksums")    cli.skipchecksums = val;
    else if (flag == "skipinteg")        cli.skipinteg = val;
    else if (flag == "skippgpcheck")     cli.skippgpcheck = val;
    else if (flag == "verifysource")     cli.verifysource = val;
    else if (flag == "allsource")        cli.allsource = val;
    else if (flag == "sign")             cli.sign = val;
    else if (flag == "nosign")           cli.nosign = val;
    else if (flag == "repackage")        cli.repackage = val;
    else if (flag == "rmdeps")           cli.rmdeps = val;
    else if (flag == "nosave")           cli.nosave = val;
    else if (flag == "ignorearch")       cli.ignorearch = val;
    else if (flag == "packagelist")      cli.packagelist = val;
    else if (flag == "printsrcinfo")     cli.printsrcinfo = val;
    else if (flag == "holdver")          cli.holdver = val;
    else if (flag == "log")              cli.log = val;
    else if (flag == "nodeps")           cli.nodeps = val;
    else if (flag == "syncdeps")         cli.syncdeps = val;
    else if (flag == "nobuild")          cli.nobuild = val;
    else if (flag == "noextract")        cli.noextract = val;
    else if (flag == "add")              cli.add = val;
    else if (flag == "aur")              cli.aur = val;
    else if (flag == "aur-deps")         cli.aur_deps = val;
    else if (flag == "refresh")          cli.refresh = val;
    else if (flag == "no-deps-resolve")  cli.no_deps_resolve = val;
    else if (flag == "nodownload")       cli.nodownload = val;
    else if (flag == "needed")           cli.needed = val;
    else if (flag == "debug")            cli.debug = val;
    else if (flag == "color")            cli.color_flag = val;
    else if (flag == "nocolor")          cli.nocolor_flag = val;
    else if (flag == "all-deps")         cli.all_deps = val;
    else if (flag == "getpkgbuild")      cli.getpkgbuild = val;
    else if (flag == "sync-force-flag")  cli.sync_force = val;
    else if (flag == "nodevel")          cli.nodevel = val;
    else if (flag == "devel")            cli.devel = val;
    else if (flag == "build" || flag == "build-local")  cli.build_local = val;
    else if (flag == "pgp-fetch")        cli.pgp_fetch = val;
}

struct OptDef {
    const char* long_flag{nullptr};
    const char* short_flag{nullptr};
    bool has_value{false};
};

constexpr OptDef opts[] = {
    // PKGBUILD options
    {"--packagefile", "-p", true},
    {"--log", nullptr, false},
    {"--nodeps", "-N", false},
    {"--syncdeps", nullptr, false},
    {"--nobuild", nullptr, false},
    {"--noextract", nullptr, false},
    {"--check", nullptr, false},
    {"--nocheck", nullptr, false},
    {"--noprepare", nullptr, false},
    {"--cleanbuild", "-C", false},
    {"--clean", nullptr, false},
    {"--force", nullptr, false},
    {"--install", nullptr, false},
    {"--noarchive", nullptr, false},
    {"--geninteg", nullptr, false},
    {"--skipchecksums", nullptr, false},
    {"--skipinteg", nullptr, false},
    {"--skippgpcheck", nullptr, false},
    {"--verifysource", nullptr, false},
    {"--allsource", nullptr, false},
    {"--sign", nullptr, false},
    {"--nosign", nullptr, false},
    {"--key", nullptr, true},
    {"--repackage", nullptr, false},
    {"--rmdeps", nullptr, false},
    {"--ignorearch", nullptr, false},
    {"--dir", "-D", true},
    {"--packagelist", nullptr, false},
    {"--printsrcinfo", nullptr, false},
    {"--holdver", nullptr, false},

    // Optimization flags
    {"--graphite", nullptr, true},
    {"--polly", nullptr, true},
    {"--lto", nullptr, true},
    {"--mold", nullptr, true},
    {"--cc", nullptr, true},

    // Config & extras
    {"--config", nullptr, true},
    {"--add", nullptr, false},
    {"--build", "-B", false},

    // AUR integration
    {"--aur", nullptr, false},
    {"--aur-deps", nullptr, false},
    {"--search", nullptr, true},
    {"--limit", nullptr, true},
    {"--aur-dir", nullptr, true},
    {"--refresh", "-u", false},
    {"--no-deps-resolve", nullptr, false},

    // Sync operation flag (standalone -S)
    {"--sync", "-S", false},

    // Pacman compatibility flags
    {"--ignore", nullptr, true},
    {"--ignoregroup", nullptr, true},
    {"--overwrite", nullptr, true},
    {"--needed", nullptr, false},
    {"--noconfirm", "-n", false},
    {"--nodownload", nullptr, false},
    {"--debug", nullptr, false},
    {"--color", nullptr, false},
    {"--nocolor", nullptr, false},
    {"--all-deps", nullptr, false},

    // Getpkgbuild
    {"--getpkgbuild", "-G", false},

    // Installed AUR/local PKGBUILD packages
    {"--list-foreign", nullptr, false},

    // Build flags
    {"--mflags", nullptr, true},

    // Upgrade filters
    {"--nodevel", nullptr, false},
    {"--devel", nullptr, false},

    // Build workflow options
    {"--answerclean", nullptr, true},
    {"--answerupgrade", nullptr, true},
    {"--pgpfetch", nullptr, false},
    {"--keepsrc", nullptr, false},
    {"--cleanafter", nullptr, false},

    // Search options
    {"--sortby", nullptr, true},

    // Help & version
    {"--help", "-h", false},
    {"--version", "-V", false},
};

// Map short chars to their set_bool flag names (for -Q* and -S* clusters)
struct ShortFlagMap {
    char ch;
    const char* flag_name;
};

constexpr ShortFlagMap query_flags[] = {
    {'i', "query-info"},
    {'l', "query-list-files"},
    {'m', "query-mirrors"},
    {'o', "query-owned"},
    {'d', "query-dependents"},
    {'e', "query-explicit"},
    {'u', "query-upgrades"},
    {'s', "query-search"},
    {'k', "query-check"},
    {'x', "query-text"},
    {'t', "query-tree"},
    {'q', "quiet"},
};

constexpr ShortFlagMap sync_flags[] = {
    {'y', "refresh-db"},
    {'u', "sysupgrade"},
    {'s', "sync-search"},
    {'i', "sync-info"},
    {'l', "sync-list"},
    {'g', "sync-groups"},
    {'w', "sync-download"},
    {'f', "sync-force"},
};

constexpr ShortFlagMap files_flags[] = {
    {'s', "files-search"},
    {'i', "files-info"},
    {'l', "files-list"},
};

constexpr ShortFlagMap database_flags[] = {
    {'s', "database-sync"},
    {'a', "database-add"},
    {'r', "database-remove"},
};

bool apply_short_flag(Cli& cli, char ch, const ShortFlagMap* table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i].ch == ch) {
            set_bool(cli, table[i].flag_name, true);
            return true;
        }
    }
    return false;
}

} // anonymous

Cli parse(int argc, char* argv[]) {
    Cli cli{};
    std::vector<std::string> packages;

    auto is_opt = [](const std::string& s) -> bool {
        return detail::starts_with(s, "--") || (s.size() >= 2 && s[0] == '-');
    };

    size_t i = 1;
    while (i < static_cast<size_t>(argc)) {
        auto arg = argv[i];
        std::string sarg(arg);

        // Handle -- separator
        if (sarg == "--") {
            for (size_t j = i + 1; j < static_cast<size_t>(argc); ++j) {
                cli.extra_env.push_back(argv[j]);
            }
            break;
        }

        // Handle clustered short flags like -Syu, -Rns, -Qdt
        if (detail::starts_with(sarg, "-") && !detail::starts_with(sarg, "--") && sarg.size() > 2) {
            char primary = sarg[1];

            // Special handling for -R* clusters (remove operations)
            if (primary == 'R') {
                cli.operation = Cli::Op::Remove;
                for (size_t k = 2; k < sarg.size(); ++k) {
                    char c = sarg[k];
                    if (c == 'n') cli.nosave = true;
                    else if (c == 's') cli.remove_deps = true;
                    else if (c == 'r') cli.recursive_remove = true;
                    else if (c == 'c') cli.remove_configs = true;
                    else if (c == 'd') cli.remove_deps = true;
                }
                ++i;
                continue;
            }

            // Special handling for -Q* clusters (query operations)
            if (primary == 'Q') {
                cli.operation = Cli::Op::Query;
                bool has_action = false;
                for (size_t k = 2; k < sarg.size(); ++k) {
                    char c = sarg[k];
                    if (apply_short_flag(cli, c, query_flags, std::size(query_flags))) {
                        has_action = true;
                    } else {
                        // Unknown char in -Q cluster — treat as package name
                        packages.push_back(std::string(1, c));
                    }
                }
                if (!has_action) cli.query_explicit = true; // default -Qe
                ++i;
                continue;
            }

            // Special handling for -S* clusters (sync operations)
            if (primary == 'S') {
                cli.operation = Cli::Op::Sync;
                bool has_action = false;
                for (size_t k = 2; k < sarg.size(); ++k) {
                    char c = sarg[k];
                    if (apply_short_flag(cli, c, sync_flags, std::size(sync_flags))) {
                        has_action = true;
                    } else if (c == 'c') {
                        // -Sc or -Scc
                        cli.sync_clean = true;
                        has_action = true;
                        if (k + 1 < sarg.size() && sarg[k + 1] == 'c') ++k;
                    } else {
                        // Unknown char in -S cluster — treat as package name
                        packages.push_back(std::string(1, c));
                    }
                }
                ++i;
                continue;
            }

            // Special handling for -F* clusters (files operations)
            if (primary == 'F') {
                cli.operation = Cli::Op::Files;
                bool has_action = false;
                for (size_t k = 2; k < sarg.size(); ++k) {
                    char c = sarg[k];
                    if (apply_short_flag(cli, c, files_flags, std::size(files_flags))) {
                        has_action = true;
                    } else {
                        packages.push_back(std::string(1, c));
                    }
                }
                ++i;
                continue;
            }

            // Special handling for -D* clusters (database operations)
            if (primary == 'D') {
                cli.operation = Cli::Op::Database;
                bool has_action = false;
                for (size_t k = 2; k < sarg.size(); ++k) {
                    char c = sarg[k];
                    if (apply_short_flag(cli, c, database_flags, std::size(database_flags))) {
                        has_action = true;
                    } else {
                        packages.push_back(std::string(1, c));
                    }
                }
                ++i;
                continue;
            }

            // Special handling for -T* (deptest)
            if (primary == 'T') {
                cli.operation = Cli::Op::Deptest;
                ++i;
                continue;
            }

            // Single short flag with attached value: -D/usr/local, -Spkg
            if (primary == 'D' && sarg.size() > 2) {
                cli.dir = sarg.substr(2);
                ++i;
                continue;
            }

            // Generic single short flag (-n, -C, etc.)
            char sf = sarg[1];
            bool found = false;
            for (auto& def : opts) {
                if (def.short_flag && def.short_flag[0] == sf) {
                    if (def.has_value) {
                        if (sarg.size() > 2) {
                            // Attached value: -D/usr/local
                            cli.extra_env.push_back(std::string(def.long_flag, 2) + "=" + sarg.substr(2));
                        } else {
                            auto val = peek_value(argv, argc, i);
                            if (val) {
                                cli.extra_env.push_back(std::string(def.long_flag, 2) + "=" + *val);
                                ++i;
                            }
                        }
                    } else {
                        set_bool(cli, std::string(def.long_flag, 2), true);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Unknown single short flag — treat as package name
                packages.push_back(sarg);
            }
            ++i;
            continue;
        }

        // Long flag or value
        bool consumed = false;
        for (auto& def : opts) {
            std::string check_arg = arg;
            auto eq_pos = check_arg.find('=');
            if (eq_pos != std::string::npos) {
                check_arg = check_arg.substr(0, eq_pos);
            }

            if (check_arg == def.long_flag || (def.short_flag && sarg == def.short_flag)) {
                if (def.has_value) {
                    std::string value;
                    auto orig_eq = sarg.find('=');
                    if (orig_eq != std::string::npos) {
                        value = sarg.substr(orig_eq + 1);
                    } else {
                        auto val = peek_value(argv, argc, i);
                        if (val) {
                            value = *val;
                            ++i;
                        }
                    }

                    std::string lf(def.long_flag + 2);
                    if (lf == "packagefile") cli.packagefile = value;
                    else if (lf == "key")    cli.key = value;
                    else if (lf == "dir")    cli.dir = value;
                    else if (lf == "graphite") cli.graphite = parse_opt_mode(value);
                    else if (lf == "polly")  cli.polly = parse_opt_mode(value);
                    else if (lf == "lto")    cli.lto = parse_opt_mode(value);
                    else if (lf == "mold")   cli.mold = parse_opt_mode(value);
                    else if (lf == "cc")     cli.cc = parse_compiler(value);
                    else if (lf == "config") cli.config = value;
                    else if (lf == "search") cli.search = value;
                    else if (lf == "limit")  { try { cli.limit = std::stoul(value); } catch (...) {} }
                    else if (lf == "aur-dir") cli.aur_dir = value;
                    else if (lf == "mflags") {
                        // Split mflags by whitespace
                        std::istringstream iss(value);
                        std::string flag;
                        while (iss >> flag) {
                            cli.mflags.push_back(flag);
                        }
                    }
                    else if (lf == "sortby") {
                        auto sv = value;
                        std::transform(sv.begin(), sv.end(), sv.begin(), ::tolower);
                        if (sv == "votes") cli.sort_by = Cli::SortBy::Votes;
                        else if (sv == "updated" || sv == "date") cli.sort_by = Cli::SortBy::Updated;
                        else if (sv == "popular") cli.sort_by = Cli::SortBy::Popular;
                        else cli.sort_by = Cli::SortBy::Votes;
                    }

                    // Build workflow options
                    else if (lf == "answerclean")    cli.answer_clean = value;
                    else if (lf == "answerupgrade")  cli.answer_upgrade = value;
                } else {
                    std::string lf(def.long_flag + 2);
                    if (lf == "help")   { print_help(); exit(0); }
                    else if (lf == "version") { std::cout << "pacmkr 0.1.0\n"; exit(0); }
                    else if (lf == "list-foreign") {
                        cli.operation = Cli::Op::Query;
                        cli.query_mirrors = true;
                    } else set_bool(cli, lf, true);
                }
                consumed = true;
                break;
            }
        }

        if (!consumed) {
            packages.push_back(sarg);
        }

        ++i;
    }

    cli.packages = std::move(packages);
    return cli;
}

void print_help() {
    const char* help_text =
        "Usage: pacmkr [OPTIONS] [-- <EXTRA_ENV>...]\n"
        "       pacmkr --build\n"
        "\n"
        "pacmkr is a pacman drop-in replacement with AUR integration.\n"
        "Standard pacman operations (D, F, Q, R, S, T, U) are fully supported.\n"
        "Running pacmkr with no arguments displays this help message.\n"
        "\n"
        "Pacman Operations:\n"
        "  -D, --database    Database operation (-Da, -Dr, -Ds)\n"
        "  -F, --files       File database search (-Fs, -Fi, -Fl)\n"
        "  -Q, --query       Query local package DB (-Qi, -Ql, -Qe, -Qu, -Qs, etc.)\n"
        "  -R, --remove      Remove packages (-Rs, -Rc, -Rd, -Rns)\n"
        "  -S, --sync        Sync/install packages (-Sy, -Su, -Ss, -Si, -Sl, etc.)\n"
        "  -T, --deptest     Test dependency resolution\n"
        "  -U, --upgrade     Upgrade from local file (-U package.pkg.tar.zst)\n"
        "      --list-foreign List installed AUR/local PKGBUILD packages (-Qm)\n"
        "\n"
        "PKGBUILD Options (makepkg-compatible):\n"
        "  -p, --packagefile FILE    Use alternate build script\n"
        "  -N, --nodeps              Skip dependency checks\n"
        "  -s, --syncdeps            Install missing dependencies\n"
        "  -o, --nobuild             Download/extract only (no build)\n"
        "  -C, --cleanbuild          Remove existing $srcdir/\n"
        "  -c, --clean               Clean up work files after build\n"
        "  -f, --force               Overwrite existing package\n"
        "  -i, --install             Install after successful build\n"
        "  -g, --geninteg            Generate checksums for sources\n"
        "  -e, --noextract           Do not extract source files\n"
        "      --check               Run check() function\n"
        "      --nocheck             Skip check() function\n"
        "      --noprepare           Skip prepare() function\n"
        "      --verifysource        Download and verify sources\n"
        "      --skipchecksums       Skip checksum verification\n"
        "      --sign                Sign package with GPG\n"
        "  -r, --rmdeps              Remove installed dependencies after build\n"
        "      --printsrcinfo        Print generated SRCINFO\n"
        "      --holdver             Do not update VCS sources\n"
        "\n"
        "Optimization Flags (pacmkr unique):\n"
        "      --graphite [MODE]     Graphite loop optimizer (gcc only)\n"
        "      --polly [MODE]        Polly compiler optimization framework\n"
        "      --lto [MODE]          Link-time optimization (-flto)\n"
        "      --mold [MODE]         Mold linker (faster linking)\n"
        "      --cc [COMPILER]       Override compiler: gcc or clang\n"
        "\n"
        "AUR Integration:\n"
        "      --aur                 Build package from AUR\n"
        "      --aur-deps            Auto-resolve and build AUR dependencies\n"
        "      --search QUERY        Search AUR packages\n"
        "      --limit N             Limit search results (default: 10)\n"
        "      --aur-dir DIR         AUR clone directory (default: ./aur)\n"
        "  -u, --refresh             Upgrade out-of-date AUR packages\n"
        "\n"
        "Local Build (pacmkr-specific):\n"
        "  -B, --build               Build PKGBUILD in current directory\n"
        "\n"
        "Build Options:\n"
        "      --mflags FLAGS        Pass custom flags to makepkg (e.g., --mflags=\"-j8\")\n"
        "      --nodevel             Skip dev/-git packages in upgrades\n"
        "      --devel               Include dev/-git packages in upgrades\n"
        "      --answerclean TEXT    Answer for clean build directory conflicts\n"
        "      --answerupgrade TEXT  Answer for upgrade conflicts\n"
        "      --pgpfetch            Auto-fetch missing PGP keys from keyserver\n"
        "      --keepsrc             Preserve source files after build\n"
        "      --cleanafter          Clean build directory after each package\n"
        "\n"
        "Config:\n"
        "      --config FILE         Alternate config file\n"
        "  -n, --noconfirm           Skip all confirmation prompts\n"
        "      --color               Force color output\n"
        "      --nocolor             Disable color output\n"
        "  -- <KEY=VALUE...>         Extra environment variables for makepkg\n"
        "\n"
        "Examples:\n"
        "  pacmkr repo help               Manage a custom/local repository\n"
        "  pacmkr -Syu                    Full system upgrade with AUR support\n"
        "  pacmkr -Ss query               Search repos + AUR\n"
        "  pacmkr -Qi package             Show package info\n"
        "  pacmkr -Qe                     List explicitly installed packages\n"
        "  pacmkr -Qu                     List upgradable packages\n"
        "  pacmkr -Rns package            Remove package and unused deps\n"
        "  pacmkr --aur -Ss neovim        Search AUR for neovim\n"
        "  pacmkr -S --aur neovim         Install from AUR\n"
        "  pacmkr -S --mold --lto auto pkg  Build with mold + LTO\n"
        "  pacmkr --build              Build PKGBUILD in current directory\n"
        "\n"
        "  -h, --help                Print help\n"
        "  -V, --version             Print version\n";
    std::cout << help_text;
}

} // namespace pacmkr::cli
