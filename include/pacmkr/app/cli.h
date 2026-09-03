#pragma once

#include <string>
#include <optional>
#include <vector>
#include <filesystem>

namespace pacmkr::cli {

enum class OptMode { Disabled, Enabled, Auto };
enum class Compiler { Gcc, Clang };

struct Cli {
    // ─── Pacman operation flags (D, F, Q, R, S, T, U) ─────────────
    enum class Op { None, Database, Files, Query, Remove, Sync, Deptest, Upgrade };
    Op operation{Op::None};

    // ─── Package arguments ─────────────────────────────────────────
    std::vector<std::string> packages;

    // ─── PKGBUILD options (makepkg-compatible) ─────────────────────
    std::filesystem::path packagefile{};  // -p
    bool log{false};                      // -l
    bool nodeps{false};                   // -N
    bool syncdeps{false};                 // -s (sync deps, not sync op)
    bool nobuild{false};                  // -o
    bool noextract{false};                // -e
    bool check{false};                    // --check
    bool nocheck{false};                  // --nocheck
    bool noprepare{false};                // --noprepare
    bool cleanbuild{false};               // -C
    bool clean{false};                    // -c
    bool force{false};                    // -f
    bool install{false};                  // -i
    bool noarchive{false};                // --noarchive
    bool geninteg{false};                 // -g
    bool skipchecksums{false};            // --skipchecksums
    bool skipinteg{false};                // --skipinteg
    bool skippgpcheck{false};             // --skippgpcheck
    bool verifysource{false};             // --verifysource
    bool allsource{false};                // -A (all source)
    bool sign{false};                     // --sign
    bool nosign{false};                   // --nosign
    std::optional<std::string> key;       // --key
    bool repackage{false};                // -R (repackage, not remove)
    bool nosave{false};                   // -Rns: no save configs
    bool rmdeps{false};                   // -r
    bool ignorearch{false};               // -A
    std::filesystem::path dir{};          // -D
    bool packagelist{false};              // --packagelist
    bool printsrcinfo{false};             // --printsrcinfo
    bool holdver{false};                  // --holdver

    // ─── Optimization flags (pacmkr unique features) ───────────────
    OptMode graphite{OptMode::Auto};      // --graphite [auto|yes|no]
    OptMode polly{OptMode::Auto};         // --polly [auto|yes|no]
    OptMode lto{OptMode::Auto};           // --lto [auto|yes|no]
    OptMode mold{OptMode::Auto};          // --mold [auto|yes|no]
    std::optional<Compiler> cc;           // --cc gcc|clang

    // ─── Config & extras ───────────────────────────────────────────
    bool add{false};                      // --add
    std::vector<std::string> extra_env;   // after --
    std::filesystem::path chdir{};
    std::filesystem::path config{};       // --config

    // ─── AUR integration (pacmkr extensions) ───────────────────────
    bool aur{false};                      // --aur: build from AUR
    bool aur_deps{false};                 // --aur-deps: auto-resolve deps
    std::optional<std::string> search;    // --search QUERY
    unsigned int limit{10};               // --limit N
    std::filesystem::path aur_dir{"aur"}; // --aur-dir DIR

    // ─── Upgrade workflow ──────────────────────────────────────────
    bool refresh{false};                  // -u: upgrade AUR packages
    bool no_deps_resolve{false};          // --no-deps-resolve

    // ─── Pacman Sync sub-flags (-S) ────────────────────────────────
    bool sync_search{false};              // -Ss
    bool sync_info{false};                // -Si
    bool sync_list{false};                // -Sl
    bool sync_groups{false};              // -Sg
    bool sync_download{false};            // -Sw
    bool sync_clean{false};               // -Scc / -Sc
    bool sysupgrade{false};               // -Su
    bool refresh_db{false};               // -Sy
    bool sync_force{false};               // -Sf (sync force)

    // Sync additional flags
    std::vector<std::string> ignore;      // --ignore=PKG
    std::vector<std::string> ignoregroup; // --ignoregroup=GRP
    std::vector<std::string> overwrite;   // --overwrite=PAT
    bool all_deps{false};                 // -S (all deps)
    bool nodownload{false};               // --nodownload
    bool needed{false};                   // --needed

    // ─── Pacman Query sub-flags (-Q) ───────────────────────────────
    bool query_info{false};               // -Qi
    bool query_list_files{false};         // -Ql
    bool query_mirrors{false};            // -Qm (foreign packages)
    bool query_owned{false};              // -Qo FILE
    bool query_dependents{false};         // -Qd
    bool query_explicit{false};           // -Qe (explicitly installed)
    bool query_orphans{false};            // -Qm or --orphans (foreign packages not required)
    bool query_upgrades{false};           // -Qu
    bool query_search{false};             // -Qs
    bool query_check{false};              // -Qk (check integrity)
    bool query_text{false};               // -Qx regex
    bool query_tree{false};               // -Qt (tree view)
    bool query_recursive{false};          // -Qdt (orphan cleanup candidates)
    bool query_file{false};               // -Qp (read metadata from a package file)
    bool query_changelog{false};          // -Qc (show changelog)
    bool quiet{false};                    // -Qq, -Rq

    // ─── Pacman Remove sub-flags (-R) ──────────────────────────────
    bool noconfirm{false};                // --noconfirm / -n
    bool remove_deps{false};              // -Rs (remove newly-unneeded dependencies)
    bool remove_configs{false};           // -Rc (cascade to dependent packages)
    bool recursive_remove{false};         // -Rr (include explicitly installed unneeded deps)

    // ─── Pacman Database sub-flags (-D) ────────────────────────────
    bool database_sync{false};            // -Ds
    bool database_add{false};             // -Da
    bool database_remove{false};          // -Dr

    // ─── Pacman Files sub-flags (-F) ───────────────────────────────
    bool files_search{false};             // -Fs
    bool files_info{false};               // -Fi
    bool files_list{false};               // -Fl

    // ─── Pacman Upgrade sub-flags (-U) ─────────────────────────────
    std::filesystem::path upgrade_file{}; // -U FILE...

    // ─── Getpkgbuild / source download ─────────────────────────────
    bool getpkgbuild{false};              // -G (get PKGBUILD)

    // ─── Misc pacman-compatible flags ──────────────────────────────
    bool debug{false};                    // --debug
    bool color_flag{false};               // --color
    bool nocolor_flag{false};             // --nocolor

    // ─── Build flags (passed to makepkg) ───────────────────────────
    std::vector<std::string> mflags;      // --mflags FLAGS

    // ─── Upgrade filters ───────────────────────────────────────────
    bool nodevel{false};                  // --nodevel (skip dev packages)
    bool devel{false};                    // --devel (include dev packages)

    // ─── Non-interactive defaults ──────────────────────────────────
    std::string answer_clean;             // --answerclean: default for clean prompt (y/n)
    std::string answer_upgrade;           // --answerupgrade: default for upgrade prompt (y/n)

    // ─── Local PKGBUILD build (pacmkr-specific) ────────────────────
    bool build_local{false};              // --build: build PKGBUILD in current dir

    // ─── Build workflow options ────────────────────────────────────
    bool pgp_fetch{false};                // --pgpfetch: auto-fetch missing PGP keys
    bool keep_src{false};                 // --keepsrc: preserve source files after build
    bool clean_after{false};              // --cleanafter: clean build dir after each package

    // ─── Search options ────────────────────────────────────────────
    enum class SortBy { Votes, Updated, Popular };
    SortBy sort_by{SortBy::Votes};        // --sortby

    // ─── Output format ───────────────────────────────────────────
    bool json_output{false};              // --json: machine-readable JSON output
    bool dry_run{false};                  // --dry-run: preview without executing

    // ─── Maintenance ──────────────────────────────────────────────
    bool cleanup{false};                  // --cleanup: remove old AUR sources and build logs
    unsigned int cleanup_age_days{30};    // --cleanup-age N: days threshold (default 30)

    // ─── Diagnostics ──────────────────────────────────────────────
    bool show_flags{false};               // --flags: print the flags used to build packages
};

/// Parse command-line arguments. Throws on failure.
Cli parse(int argc, char* argv[]);

/// Print help message.
void print_help();

} // namespace pacmkr::cli
