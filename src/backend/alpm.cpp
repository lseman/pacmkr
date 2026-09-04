#include "pacmkr/backend/alpm.h"
#include "pacmkr/core/error.h"
#include "pacmkr/core/pacman_config.h"
#include "pacmkr/app/terminal.h"

#include <alpm.h>
#include <alpm_list.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <utility>

namespace pacmkr::alpm {

static alpm_handle_t* g_handle{nullptr};
static std::function<void(int, const char*)> g_log_cb;
static bool g_no_confirm{false};
static std::vector<std::string> g_hold_packages;
static std::mutex g_warning_mutex;
static std::unordered_set<std::string> g_emitted_warnings;

static void clear_emitted_warnings() {
    std::lock_guard<std::mutex> lock(g_warning_mutex);
    g_emitted_warnings.clear();
}

static void emit_warning_once(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_warning_mutex);
    if (g_emitted_warnings.insert(message).second) terminal::warning(message);
}

static void require_root() {
    const char* configured_root = g_handle ? alpm_option_get_root(g_handle) : nullptr;
    const bool isolated_root = configured_root && std::string(configured_root) != "/";
    if (geteuid() != 0 && !isolated_root) {
        throw alpm_error("this operation modifies the system; run pacmkr as root");
    }
}

static std::string last_error(const std::string& action) {
    const auto error = alpm_errno(g_handle);
    std::string message = action + ": " + alpm_strerror(error);
    if (error == ALPM_ERR_HANDLE_LOCK) {
        const char* lockfile = alpm_option_get_lockfile(g_handle);
        message += ". Another package transaction may be running";
        if (lockfile) message += "; lock file: " + std::string(lockfile);
        message += ". If no package manager is running, remove the stale lock file manually";
    }
    return message;
}

static void question_callback(void*, alpm_question_t* question) {
    if (!question) return;
    if (question->type == ALPM_QUESTION_SELECT_PROVIDER) {
        question->select_provider.use_index = 0;
        return;
    }
    if (g_no_confirm) { question->any.answer = 1; return; }
    const char* prompt = "Accept the requested transaction change?";
    switch (question->type) {
        case ALPM_QUESTION_INSTALL_IGNOREPKG: prompt = "Install an ignored package?"; break;
        case ALPM_QUESTION_REPLACE_PKG: prompt = "Replace the installed package?"; break;
        case ALPM_QUESTION_CONFLICT_PKG: prompt = "Remove the conflicting package?"; break;
        case ALPM_QUESTION_CORRUPTED_PKG: prompt = "Delete the corrupted package file?"; break;
        case ALPM_QUESTION_REMOVE_PKGS: prompt = "Skip packages with unresolved dependencies?"; break;
        case ALPM_QUESTION_IMPORT_KEY: prompt = "Import the required signing key?"; break;
        default: break;
    }
    if (!isatty(STDIN_FILENO)) { question->any.answer = 0; return; }
    std::cout << prompt << " [Y/n] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    question->any.answer = answer.empty() || (answer[0] != 'n' && answer[0] != 'N');
}

static void progress_callback(void*, alpm_progress_t event, const char* pkg,
                              int percent, size_t total, size_t current) {
    // libalpm drives each transaction phase (keyring/integrity/load/conflict/
    // diskspace checks, then the per-package add/upgrade) from 0 to 100 and calls
    // this callback on every tick — including several times at 100%.  Treating
    // "percent == 100" as the line terminator therefore stacks a fresh progress
    // bar on every trailing tick.  Instead, key the bar on (phase, package):
    // redraw it in place while that identity holds, emit the closing newline the
    // first time it reaches 100%, and drop the redundant 100% ticks that follow.
    static alpm_progress_t current_event{};
    static std::string current_label;
    static bool current_done{false};

    const char* phase = nullptr;
    switch (event) {
        case ALPM_PROGRESS_KEYRING_START:   phase = "Checking keyring"; break;
        case ALPM_PROGRESS_INTEGRITY_START: phase = "Checking integrity"; break;
        case ALPM_PROGRESS_LOAD_START:      phase = "Loading packages"; break;
        case ALPM_PROGRESS_CONFLICTS_START: phase = "Checking conflicts"; break;
        case ALPM_PROGRESS_DISKSPACE_START: phase = "Checking disk space"; break;
        default: break;
    }
    std::string label = phase ? phase : (pkg && *pkg ? pkg : "Transaction");

    const bool same_bar = (event == current_event && label == current_label);
    if (same_bar && current_done) return;  // ignore trailing 100% ticks
    if (!same_bar) {
        current_event = event;
        current_label = label;
        current_done = false;
    }

    const bool done = percent >= 100;
    current_done = done;
    terminal::progress(label, percent, current, total, done);
}

/// Base name for grouping: strip .sig suffix so "foo.db.sig" → "foo.db".
static std::string group_key(const std::string& filename) {
    auto key = filename;
    if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".sig") == 0)
        key.resize(key.size() - 4);
    return key;
}

/// Track a group of downloads that should share one progress bar.
struct DownloadGroup {
    std::string display_name;       // e.g. "core" or "cachyos-extra-v3"
    size_t total = 0;               // combined total bytes for all files in group
    size_t downloaded = 0;          // combined downloaded bytes
    int completed_files = 0;        // how many files have finished
    int started_files = 0;          // how many files have been seen (ALPM_DOWNLOAD_INIT)
    std::string status;             // final status text: "up to date", "downloaded", etc.
};

/// Groups keyed by base filename. Lives across callback invocations.
static std::unordered_map<std::string, DownloadGroup> download_groups;
static std::string current_group_key;  // the group currently being shown

static void download_callback(void*, const char* filename,
                              alpm_download_event_type_t event, void* data) {
    const std::string raw = filename ? filename : "database";
    const bool is_signature = raw.size() >= 4 &&
        raw.compare(raw.size() - 4, 4, ".sig") == 0;
    const auto key = group_key(raw);

    // For database files, show a short display name (strip .db/.db.sig)
    std::string display = raw;
    if (!is_signature && raw.size() >= 4 && raw.compare(raw.size() - 4, 4, ".db") == 0) {
        display = raw.substr(0, raw.size() - 4);
    } else if (is_signature && raw.size() >= 8 &&
               raw.compare(raw.size() - 8, 8, ".db.sig") == 0) {
        display = raw.substr(0, raw.size() - 8);
    }

    switch (event) {
        case ALPM_DOWNLOAD_INIT: {
            auto& grp = download_groups[key];
            if (grp.started_files == 0)
                grp.display_name = display;
            grp.started_files++;
            current_group_key = key;
            break;
        }
        case ALPM_DOWNLOAD_PROGRESS: {
            const auto* progress = static_cast<alpm_download_event_progress_t*>(data);
            if (progress && progress->total > 0) {
                auto it = download_groups.find(key);
                if (it != download_groups.end()) {
                    // Accumulate total and downloaded across all files in group
                    it->second.total += static_cast<size_t>(progress->total);
                    it->second.downloaded += static_cast<size_t>(progress->downloaded);
                }
                const auto& grp = download_groups[key];
                if (grp.total > 0) {
                    const auto percent = static_cast<int>(100 * grp.downloaded / grp.total);
                    terminal::progress(grp.display_name, percent);
                } else {
                    // Single-file fallback
                    const auto percent = static_cast<int>(100 * progress->downloaded / progress->total);
                    terminal::progress(display, percent);
                }
            }
            break;
        }
        case ALPM_DOWNLOAD_RETRY:
            terminal::warning(std::string("Retrying ") + display + " with another mirror");
            break;
        case ALPM_DOWNLOAD_COMPLETED: {
            const auto* completed = static_cast<alpm_download_event_completed_t*>(data);
            const bool current = completed && completed->result == 1;
            const bool failed = completed && completed->result < 0;

            auto& grp = download_groups[key];
            grp.completed_files++;

            if (failed && is_signature) {
                // Unsigned repo: mark this file done, but don't block the group.
                grp.status = "no signature";
            } else if (failed) {
                grp.status = "failed";
            } else if (current) {
                grp.status = "up to date";
            } else {
                grp.status = "downloaded";
            }

            // When all files in the group are done, show final status.
            if (grp.completed_files >= grp.started_files) {
                terminal::progress(grp.display_name, 100, 0, 0, true, grp.status);
                download_groups.erase(key);
                current_group_key.clear();
            }
            break;
        }
    }
}

struct Transaction {
    explicit Transaction(int flags, bool no_confirm) {
        require_root();
        g_no_confirm = no_confirm;
        if (alpm_trans_init(g_handle, flags) < 0) throw alpm_error(last_error("cannot start transaction"));
    }
    ~Transaction() { alpm_trans_release(g_handle); g_no_confirm = false; }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};

static int commit_transaction() {
    if (!alpm_trans_get_add(g_handle) && !alpm_trans_get_remove(g_handle)) {
        terminal::success("System packages are already current");
        return 0;
    }

    alpm_list_t* data = nullptr;
    if (alpm_trans_prepare(g_handle, &data) < 0)
        throw alpm_error(last_error("cannot prepare transaction"));
    if (!g_no_confirm) {
        std::cout << "\nPackages to install or upgrade:\n";
        for (auto* item = alpm_trans_get_add(g_handle); item; item = item->next) {
            auto* pkg = static_cast<alpm_pkg_t*>(item->data);
            std::cout << "  " << alpm_pkg_get_name(pkg) << " " << alpm_pkg_get_version(pkg) << "\n";
        }
        if (alpm_trans_get_remove(g_handle)) std::cout << "Packages to remove:\n";
        for (auto* item = alpm_trans_get_remove(g_handle); item; item = item->next) {
            auto* pkg = static_cast<alpm_pkg_t*>(item->data);
            std::cout << "  " << alpm_pkg_get_name(pkg) << " " << alpm_pkg_get_version(pkg) << "\n";
        }
        if (!isatty(STDIN_FILENO)) {
            alpm_list_free(data);
            throw alpm_error("confirmation requires a terminal; use --noconfirm for unattended operation");
        }
        std::cout << "\nProceed with transaction? [Y/n] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (!answer.empty() && (answer[0] == 'n' || answer[0] == 'N')) {
            alpm_list_free(data);
            throw alpm_error("transaction cancelled");
        }
    }
    if (alpm_trans_commit(g_handle, &data) < 0) {
        alpm_list_free(data);
        throw alpm_error(last_error("cannot commit transaction"));
    }
    alpm_list_free(data);
    return 0;
}

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

static std::vector<std::string> owned_list_to_strings(alpm_list_t* list) {
    auto result = list_to_strings(list);
    alpm_list_free_inner(list, free);
    alpm_list_free(list);
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

static std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static int signature_level(const std::vector<std::string>& values, int inherited = 0) {
    int level = inherited;
    constexpr int package_bits = ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL |
        ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK;
    constexpr int database_bits = ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL |
        ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK;
    for (const auto& value : values) {
        if (value == "Never") level &= ~(package_bits | database_bits);
        else if (value == "Optional") level |= ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL |
                                                  ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL;
        else if (value == "Required") {
            level |= ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE;
            level &= ~(ALPM_SIG_PACKAGE_OPTIONAL | ALPM_SIG_DATABASE_OPTIONAL);
        }
        else if (value == "TrustedOnly") level &= ~(ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK |
                                                       ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK);
        else if (value == "TrustAll") level |= ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK |
                                                   ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK;
        else if (value == "PackageNever") level &= ~package_bits;
        else if (value == "PackageOptional") level |= ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL;
        else if (value == "PackageRequired") { level |= ALPM_SIG_PACKAGE; level &= ~ALPM_SIG_PACKAGE_OPTIONAL; }
        else if (value == "PackageTrustedOnly") level &= ~(ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK);
        else if (value == "PackageTrustAll") level |= ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK;
        else if (value == "DatabaseNever") level &= ~database_bits;
        else if (value == "DatabaseOptional") level |= ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL;
        else if (value == "DatabaseRequired") { level |= ALPM_SIG_DATABASE; level &= ~ALPM_SIG_DATABASE_OPTIONAL; }
        else if (value == "DatabaseTrustedOnly") level &= ~(ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK);
        else if (value == "DatabaseTrustAll") level |= ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK;
    }
    return level;
}

static int repository_usage(const std::vector<std::string>& values) {
    int usage = 0;
    for (const auto& value : values) {
        if (value == "All") usage |= ALPM_DB_USAGE_ALL;
        else if (value == "Sync") usage |= ALPM_DB_USAGE_SYNC;
        else if (value == "Search") usage |= ALPM_DB_USAGE_SEARCH;
        else if (value == "Install") usage |= ALPM_DB_USAGE_INSTALL;
        else if (value == "Upgrade") usage |= ALPM_DB_USAGE_UPGRADE;
    }
    return usage;
}

static void add_servers_from_file(alpm_db_t* db, const std::string& path,
                                  const std::string& repository,
                                  const std::string& architecture,
                                  bool sectioned) {
    std::ifstream input(path);
    std::string line, section;
    while (std::getline(input, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        if (sectioned && section != repository) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = trim(line.substr(0, eq));
        auto value = trim(line.substr(eq + 1));
        if (key == "Include") {
            add_servers_from_file(db, value, repository, architecture, false);
        } else if (key == "Server") {
            for (auto at = value.find("$repo"); at != std::string::npos; at = value.find("$repo"))
                value.replace(at, 5, repository);
            for (auto at = value.find("$arch"); at != std::string::npos; at = value.find("$arch"))
                value.replace(at, 5, architecture);
            if (alpm_db_add_server(db, value.c_str()) < 0)
                throw alpm_error(last_error("cannot add repository server"));
        }
    }
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
    pkg.required_by    = owned_list_to_strings(alpm_pkg_compute_requiredby(p));
    pkg.optional_for   = owned_list_to_strings(alpm_pkg_compute_optionalfor(p));

    for (alpm_list_t* it = alpm_pkg_get_backup(p); it; it = it->next) {
        auto* entry = static_cast<alpm_backup_t*>(it->data);
        if (entry && entry->name) pkg.backup.emplace_back(entry->name);
    }

    return pkg;
}

/// Ask a yes/no question on the terminal unless confirmations are suppressed.
static bool confirm(const std::string& question, bool no_confirm) {
    if (no_confirm) return true;
    if (!isatty(STDIN_FILENO))
        throw alpm_error(question + " (requires a terminal; pass --noconfirm)");
    std::cout << question << " [Y/n] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return answer.empty() || (answer[0] != 'n' && answer[0] != 'N');
}

static void log_callback(void* ctx, alpm_loglevel_t level, const char* fmt, va_list args) {
    (void)ctx;
    if (!fmt) return;

    va_list length_args;
    va_copy(length_args, args);
    const int length = std::vsnprintf(nullptr, 0, fmt, length_args);
    va_end(length_args);
    if (length < 0) return;

    std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
    va_list format_args;
    va_copy(format_args, args);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, format_args);
    va_end(format_args);

    std::string message(buffer.data(), static_cast<std::size_t>(length));
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();

    if (g_log_cb) {
        g_log_cb(static_cast<int>(level), message.c_str());
    } else if ((level & ALPM_LOG_WARNING) && !message.empty()) {
        emit_warning_once(message);
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

bool uses_system_root() {
    const char* root = g_handle ? alpm_option_get_root(g_handle) : nullptr;
    return root && std::string(root) == "/";
}

/// Detect which x86_64 microarchitectures the CPU supports by reading
/// /proc/cpuinfo and checking for the instruction-set flags that define
/// each level (v2, v3, v4).  Returns them in **descending** priority order
/// so libalpm tries the most specific architecture first — avoiding
/// redundant retries when a repo only serves a microarch variant.
static std::vector<std::string> detect_cpu_microarchitectures() {
    std::vector<std::string> result;
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) return result;

    std::string line, flags_str;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("flags", 0) == 0) {
            auto eq = line.find(':');
            if (eq != std::string::npos)
                flags_str += ' ' + trim(line.substr(eq + 1));
        }
    }

    auto has_flag = [&](const std::string& flag) {
        return flags_str.find(' ' + flag + ' ') != std::string::npos;
    };

    // x86_64_v2: sse2, cx16, rdtscp
    bool v2 = has_flag("sse2") && has_flag("cx16") && has_flag("rdtscp");
    // x86_64_v3: bmi1/bmi2, lzcnt, movbe, f16c, fma, adx
    bool v3 = v2 && has_flag("bmi1") && has_flag("bmi2") &&
              has_flag("lzcnt") && has_flag("movbe") &&
              has_flag("f16c") && has_flag("fma") && has_flag("adx");
    // x86_64_v4: avx512f + subset of v3
    bool v4 = v3 && has_flag("avx512f") && has_flag("avx512bw") &&
              has_flag("avx512cd") && has_flag("avx512dq") &&
              has_flag("avx512vl");

    // Return descending so the most specific is tried first.
    if (v4) result.push_back("x86_64_v4");
    if (v3) result.push_back("x86_64_v3");
    if (v2) result.push_back("x86_64_v2");
    return result;
}

void init(const std::string& root, const std::string& db_path, const std::string& requested_config) {
    if (g_handle) return;
    const std::string config_path = requested_config.empty()
        ? (root == "/" ? "/etc/pacman.conf" : root + "/etc/pacman.conf")
        : requested_config;
    std::ifstream config_input(config_path);
    const auto config = pacman_config::parse(config_input);
    const auto& options = config.options;
    g_hold_packages = options.hold_packages;
    const std::string effective_root = root == "/" ? options.root_dir : root;
    const std::string effective_db_path = db_path == "/var/lib/pacman/" ? options.db_path : db_path;
    g_handle = alpm_initialize(effective_root.c_str(), effective_db_path.c_str(), nullptr);
    if (!g_handle) throw alpm_error("Failed to initialize libalpm");
    alpm_option_set_logcb(g_handle, log_callback, nullptr);
    alpm_option_set_questioncb(g_handle, question_callback, nullptr);
    alpm_option_set_progresscb(g_handle, progress_callback, nullptr);
    alpm_option_set_dlcb(g_handle, download_callback, nullptr);
    for (const auto& path : options.cache_dirs) alpm_option_add_cachedir(g_handle, path.c_str());
    for (const auto& path : options.hook_dirs) alpm_option_add_hookdir(g_handle, path.c_str());
    alpm_option_set_gpgdir(g_handle, options.gpg_dir.c_str());
    alpm_option_set_logfile(g_handle, options.log_file.c_str());
    if (!options.download_user.empty()) alpm_option_set_sandboxuser(g_handle, options.download_user.c_str());
    alpm_option_set_parallel_downloads(g_handle, options.parallel_downloads);
    alpm_option_set_checkspace(g_handle, options.check_space ? 1 : 0);
    for (const auto& value : options.ignore_packages) alpm_option_add_ignorepkg(g_handle, value.c_str());
    for (const auto& value : options.ignore_groups) alpm_option_add_ignoregroup(g_handle, value.c_str());
    for (const auto& value : options.no_upgrade) alpm_option_add_noupgrade(g_handle, value.c_str());
    for (const auto& value : options.no_extract) alpm_option_add_noextract(g_handle, value.c_str());
    struct utsname system_info{};
    const std::string detected_arch = uname(&system_info) == 0 ? system_info.machine : "x86_64";

    // Parse Architecture and keep all supported targets (for example, x86_64
    // plus x86_64_v2/x86_64_v3 on a capable host).
    std::string arch_value = options.architecture;
    // Strip leading/trailing whitespace (already done in parser, but be safe)
    auto first = arch_value.find_first_not_of(" \t");
    if (first != std::string::npos) arch_value = arch_value.substr(first);
    auto last = arch_value.find_last_not_of(" \t");
    if (last != std::string::npos) arch_value = arch_value.substr(0, last + 1);

    // Strip brackets: [x86_64] -> x86_64 or [auto, x86_64] -> auto, x86_64
    if (arch_value.size() >= 2 && arch_value.front() == '[' && arch_value.back() == ']') {
        arch_value = arch_value.substr(1, arch_value.size() - 2);
    }

    auto trim_check = [](std::string s) -> std::string {
        auto f = s.find_first_not_of(" \t,");
        if (f != std::string::npos) s = s.substr(f);
        auto l = s.find_last_not_of(" \t,");
        if (l != std::string::npos) s = s.substr(0, l + 1);
        return s;
    };

    std::vector<std::string> architectures;
    const auto normalized_arch = trim_check(arch_value);
    if (normalized_arch.empty() || normalized_arch == "auto" ||
        normalized_arch.find("auto") != std::string::npos) {
        for (auto* item = alpm_option_get_physical_architectures(g_handle);
             item; item = item->next) {
            if (item->data) architectures.emplace_back(static_cast<char*>(item->data));
        }
        if (architectures.empty()) architectures.push_back(detected_arch);
    } else {
        std::istringstream values(normalized_arch);
        for (std::string architecture; values >> architecture;) {
            if (!architecture.empty() && architecture.back() == ',') architecture.pop_back();
            if (!architecture.empty()) architectures.push_back(std::move(architecture));
        }
    }

    // The $arch variable in mirror URLs is the base machine architecture, never
    // a microarchitecture level.  pacman substitutes it with the first configured
    // architecture (uname's machine when Architecture = auto); CachyOS mirrorlists
    // append the level as literal text ("$arch_v3") and Arch mirrors use
    // "$repo/os/$arch", so substituting "x86_64_v3" here yields "x86_64_v3_v3" and
    // ".../os/x86_64_v3" respectively — both 404.  Capture it before the
    // microarchitecture entries are prepended below.
    const std::string url_architecture =
        architectures.empty() ? detected_arch : architectures.front();

    // When the configured architecture is x86_64 (or auto resolved to it),
    // also register any supported microarchitectures so libalpm can match
    // packages built for CachyOS v3/v4, Archcraft, etc.
    // Prepend in reverse order so the final list is descending-priority:
    // [v4, v3, v2, x86_64].  libalpm tries architectures sequentially per
    // package lookup — highest first avoids redundant retries on repos that
    // only serve a microarch variant.
    if (!architectures.empty() && architectures.front() == "x86_64") {
        auto detected = detect_cpu_microarchitectures();
        for (auto it = detected.rbegin(); it != detected.rend(); ++it) {
            if (std::find(architectures.begin(), architectures.end(), *it)
                == architectures.end())
                architectures.insert(architectures.begin(), *it);
        }
    }
    for (const auto& architecture : architectures) {
        if (alpm_option_add_architecture(g_handle, architecture.c_str()) < 0)
            throw alpm_error(last_error("cannot set package architecture"));
    }
    const std::string& architecture = url_architecture;
    const int default_siglevel = signature_level(options.sig_level);
    alpm_option_set_default_siglevel(g_handle, default_siglevel);
    alpm_option_set_local_file_siglevel(g_handle, options.local_file_sig_level.empty()
        ? default_siglevel : signature_level(options.local_file_sig_level));
    alpm_option_set_remote_file_siglevel(g_handle, options.remote_file_sig_level.empty()
        ? default_siglevel : signature_level(options.remote_file_sig_level));
    auto repositories = config.repositories;
    if (repositories.empty()) {
        repositories = {
            {"core", {}, {"All"}},
            {"extra", {}, {"All"}},
            {"multilib", {}, {"All"}},
        };
    }
    for (const auto& repository : repositories) {
        const int siglevel = repository.sig_level.empty()
            ? ALPM_SIG_USE_DEFAULT : signature_level(repository.sig_level, default_siglevel);
        auto* db = alpm_register_syncdb(g_handle, repository.name.c_str(), siglevel);
        if (!db) throw alpm_error(last_error("cannot register repository " + repository.name));
        if (alpm_db_set_usage(db, repository_usage(repository.usage)) < 0)
            throw alpm_error(last_error("cannot set repository usage for " + repository.name));
        add_servers_from_file(db, config_path, repository.name, architecture, true);
    }
}

void shutdown() {
    if (g_handle) { alpm_release(g_handle); g_handle = nullptr; }
    g_hold_packages.clear();
}

void set_log_callback(std::function<void(int, const char*)> cb) {
    g_log_cb = std::move(cb);
}

int refresh_databases(bool force) {
    require_root();
    auto* databases = alpm_get_syncdbs(g_handle);
    if (!databases) return 0;

    for (auto* item = databases; item; item = item->next) {
        auto* db = static_cast<alpm_db_t*>(item->data);
        const char* name = db ? alpm_db_get_name(db) : nullptr;
        terminal::info(std::string("Synchronizing ") + (name ? name : "repository"));
    }

    // Submit every sync database in one operation. libalpm schedules their
    // transfers concurrently up to the pacman.conf ParallelDownloads limit.
    // Calling alpm_db_update once per database serializes those transfers.
    if (alpm_db_update(g_handle, databases, force ? 1 : 0) < 0) {
        throw alpm_error(last_error("failed to synchronize package databases"));
    }
    return 0;
}

static void apply_transaction_options(const std::vector<std::string>& ignore,
                                      const std::vector<std::string>& ignore_groups,
                                      const std::vector<std::string>& overwrite) {
    for (const auto& value : ignore) alpm_option_add_ignorepkg(g_handle, value.c_str());
    for (const auto& value : ignore_groups) alpm_option_add_ignoregroup(g_handle, value.c_str());
    for (const auto& value : overwrite) alpm_option_add_overwrite_file(g_handle, value.c_str());
}

static void warn_about_newer_local_packages() {
    auto* localdb = alpm_get_localdb(g_handle);
    if (!localdb) return;

    for (auto* local_item = alpm_db_get_pkgcache(localdb);
         local_item; local_item = local_item->next) {
        auto* local_pkg = static_cast<alpm_pkg_t*>(local_item->data);
        const char* name = local_pkg ? alpm_pkg_get_name(local_pkg) : nullptr;
        const char* local_version = local_pkg ? alpm_pkg_get_version(local_pkg) : nullptr;
        if (!name || !local_version) continue;

        for (auto* db_item = alpm_get_syncdbs(g_handle); db_item; db_item = db_item->next) {
            auto* db = static_cast<alpm_db_t*>(db_item->data);
            int usage = 0;
            if (!db || alpm_db_get_usage(db, &usage) < 0 ||
                !(usage & ALPM_DB_USAGE_UPGRADE)) continue;

            auto* sync_pkg = alpm_db_get_pkg(db, name);
            if (!sync_pkg) continue;
            const char* sync_version = alpm_pkg_get_version(sync_pkg);
            const char* repository = alpm_db_get_name(db);
            if (sync_version && repository &&
                alpm_pkg_vercmp(local_version, sync_version) > 0) {
                emit_warning_once(std::string(name) + ": local (" + local_version +
                    ") is newer than " + repository + " (" + sync_version + ")");
            }
            break; // Repository priority selects the first upgrade-enabled match.
        }
    }
}

int install(const std::vector<std::string>& packages, bool needed, bool no_confirm,
            const std::vector<std::string>& ignore,
            const std::vector<std::string>& ignore_groups,
            const std::vector<std::string>& overwrite) {
    apply_transaction_options(ignore, ignore_groups, overwrite);
    Transaction tx(needed ? ALPM_TRANS_FLAG_NEEDED : 0, no_confirm);
    for (const auto& name : packages) {
        alpm_pkg_t* found = nullptr;
        for (auto* it = alpm_get_syncdbs(g_handle); it && !found; it = it->next)
            found = alpm_db_get_pkg(static_cast<alpm_db_t*>(it->data), name.c_str());
        if (!found) throw alpm_error("repository package not found: " + name);
        if (alpm_add_pkg(g_handle, found) < 0) throw alpm_error(last_error("cannot add " + name));
    }
    return commit_transaction();
}

int system_upgrade(bool allow_downgrade, bool no_confirm,
                   const std::vector<std::string>& ignore,
                   const std::vector<std::string>& ignore_groups,
                   const std::vector<std::string>& overwrite) {
    apply_transaction_options(ignore, ignore_groups, overwrite);
    Transaction tx(0, no_confirm);
    clear_emitted_warnings();
    warn_about_newer_local_packages();
    if (alpm_sync_sysupgrade(g_handle, allow_downgrade ? 1 : 0) < 0)
        throw alpm_error(last_error("cannot calculate system upgrade"));
    return commit_transaction();
}

int remove(const std::vector<std::string>& packages, bool recurse, bool cascade,
           bool no_save, bool no_confirm, bool recurse_all, bool nodeps) {
    int flags = (recurse ? ALPM_TRANS_FLAG_RECURSE : 0) |
                (cascade ? ALPM_TRANS_FLAG_CASCADE : 0) |
                (no_save ? ALPM_TRANS_FLAG_NOSAVE : 0) |
                (recurse_all ? ALPM_TRANS_FLAG_RECURSEALL : 0) |
                (nodeps ? ALPM_TRANS_FLAG_NODEPS : 0);
    Transaction tx(flags, no_confirm);
    auto* local = alpm_get_localdb(g_handle);
    for (const auto& name : packages) {
        if (std::find(g_hold_packages.begin(), g_hold_packages.end(), name) != g_hold_packages.end())
            throw alpm_error("refusing to remove HoldPkg package: " + name);
        auto* pkg = alpm_db_get_pkg(local, name.c_str());
        if (!pkg) throw alpm_error("installed package not found: " + name);
        if (alpm_remove_pkg(g_handle, pkg) < 0) throw alpm_error(last_error("cannot remove " + name));
    }
    return commit_transaction();
}

int download(const std::vector<std::string>& packages, bool sysupgrade, bool no_confirm,
             const std::vector<std::string>& ignore,
             const std::vector<std::string>& ignore_groups) {
    apply_transaction_options(ignore, ignore_groups, {});
    Transaction tx(ALPM_TRANS_FLAG_DOWNLOADONLY, no_confirm);
    if (sysupgrade && alpm_sync_sysupgrade(g_handle, 0) < 0)
        throw alpm_error(last_error("cannot calculate system upgrade"));
    for (const auto& name : packages) {
        alpm_pkg_t* found = nullptr;
        for (auto* it = alpm_get_syncdbs(g_handle); it && !found; it = it->next)
            found = alpm_db_get_pkg(static_cast<alpm_db_t*>(it->data), name.c_str());
        if (!found) throw alpm_error("repository package not found: " + name);
        if (alpm_add_pkg(g_handle, found) < 0) throw alpm_error(last_error("cannot add " + name));
    }
    return commit_transaction();
}

int install_files(const std::vector<std::string>& paths, bool no_confirm) {
    Transaction tx(0, no_confirm);

    // Split remote URLs from local paths and download the former into the cache.
    std::vector<std::string> local_paths;
    alpm_list_t* urls = nullptr;
    for (const auto& path : paths) {
        if (path.find("://") != std::string::npos)
            urls = alpm_list_add(urls, strdup(path.c_str()));
        else
            local_paths.push_back(path);
    }
    if (urls) {
        alpm_list_t* fetched = nullptr;
        const int rc = alpm_fetch_pkgurl(g_handle, urls, &fetched);
        alpm_list_free_inner(urls, free);
        alpm_list_free(urls);
        if (rc != 0) {
            alpm_list_free(fetched);
            throw alpm_error(last_error("cannot download package URL"));
        }
        for (alpm_list_t* it = fetched; it; it = it->next)
            if (it->data) local_paths.emplace_back(static_cast<char*>(it->data));
        alpm_list_free_inner(fetched, free);
        alpm_list_free(fetched);
    }

    for (const auto& path : local_paths) {
        alpm_pkg_t* pkg = nullptr;
        if (alpm_pkg_load(g_handle, path.c_str(), 1, alpm_option_get_local_file_siglevel(g_handle), &pkg) < 0)
            throw alpm_error(last_error("cannot load package " + path));
        if (alpm_add_pkg(g_handle, pkg) < 0) throw alpm_error(last_error("cannot add package " + path));
    }
    return commit_transaction();
}

Package load_package_file(const std::string& path) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    alpm_pkg_t* pkg = nullptr;
    if (alpm_pkg_load(g_handle, path.c_str(), 1,
                      alpm_option_get_local_file_siglevel(g_handle), &pkg) < 0 || !pkg)
        throw alpm_error(last_error("cannot load package file " + path));
    Package result = pkg_from_alpm(pkg, "file");
    if (auto* files = alpm_pkg_get_files(pkg)) {
        result.files.reserve(files->count);
        for (std::size_t i = 0; i < files->count; ++i)
            if (files->files[i].name) result.files.emplace_back(files->files[i].name);
    }
    alpm_pkg_free(pkg);
    return result;
}

std::string get_changelog(const std::string& name) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    auto* pkg = alpm_db_get_pkg(alpm_get_localdb(g_handle), name.c_str());
    if (!pkg) throw alpm_error("installed package not found: " + name);
    void* stream = alpm_pkg_changelog_open(pkg);
    if (!stream) return {};
    std::string out;
    char buffer[4096];
    for (size_t read; (read = alpm_pkg_changelog_read(buffer, sizeof(buffer), pkg, stream)) != 0;)
        out.append(buffer, read);
    alpm_pkg_changelog_close(pkg, stream);
    return out;
}

CacheCleanResult clean_cache(bool all, bool no_confirm) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    require_root();
    namespace fs = std::filesystem;
    auto* localdb = alpm_get_localdb(g_handle);
    CacheCleanResult result{};

    for (auto* it = alpm_option_get_cachedirs(g_handle); it; it = it->next) {
        const auto* raw = static_cast<const char*>(it->data);
        if (!raw) continue;
        const fs::path dir(raw);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        if (!confirm((all ? "Remove all files from " : "Remove unused packages from ")
                     + dir.string() + "?", no_confirm))
            continue;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const fs::path file = entry.path();

            bool drop = all;
            if (!all) {
                if (file.filename().string().find(".pkg.tar") == std::string::npos) continue;
                alpm_pkg_t* pkg = nullptr;
                if (alpm_pkg_load(g_handle, file.c_str(), 0, 0, &pkg) == 0 && pkg) {
                    auto* held = alpm_db_get_pkg(localdb, alpm_pkg_get_name(pkg));
                    drop = !held || alpm_pkg_vercmp(alpm_pkg_get_version(pkg),
                                                   alpm_pkg_get_version(held)) != 0;
                    alpm_pkg_free(pkg);
                } else {
                    drop = false;  // unreadable archive — leave it in place
                }
            }
            if (!drop) continue;

            const auto size = fs::file_size(file, ec);
            if (fs::remove(file, ec)) {
                ++result.files_removed;
                if (!ec) result.bytes_freed += static_cast<unsigned long long>(size);
            }
            std::error_code sig_ec;
            fs::remove(fs::path(file) += ".sig", sig_ec);
        }
    }

    if (all) {
        if (const char* dbpath = alpm_option_get_dbpath(g_handle)) {
            const fs::path sync = fs::path(dbpath) / "sync";
            std::error_code ec;
            if (fs::is_directory(sync, ec) &&
                confirm("Remove downloaded sync databases from " + sync.string() + "?", no_confirm)) {
                for (const auto& entry : fs::directory_iterator(sync, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file(ec)) continue;
                    const auto size = fs::file_size(entry.path(), ec);
                    if (fs::remove(entry.path(), ec)) {
                        ++result.files_removed;
                        if (!ec) result.bytes_freed += static_cast<unsigned long long>(size);
                    }
                }
            }
        }
    }

    return result;
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

std::vector<Package> get_foreign_packages() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    alpm_db_t* localdb = alpm_get_localdb(g_handle);
    if (!localdb) throw alpm_error("Failed to get local database");

    std::vector<Package> result;
    for (alpm_list_t* item = alpm_db_get_pkgcache(localdb); item; item = item->next) {
        auto* pkg = static_cast<alpm_pkg_t*>(item->data);
        const char* name = pkg ? alpm_pkg_get_name(pkg) : nullptr;
        if (!name) continue;

        bool found_in_sync = false;
        for (alpm_list_t* db_item = alpm_get_syncdbs(g_handle);
             db_item; db_item = db_item->next) {
            auto* db = static_cast<alpm_db_t*>(db_item->data);
            if (db && alpm_db_get_pkg(db, name)) {
                found_in_sync = true;
                break;
            }
        }
        if (!found_in_sync) result.push_back(pkg_from_alpm(pkg, "local"));
    }

    std::sort(result.begin(), result.end(),
              [](const Package& lhs, const Package& rhs) {
                  return lhs.name < rhs.name;
              });
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
    auto* pkg = alpm_db_get_pkg(alpm_get_localdb(g_handle), name.c_str());
    if (!pkg) return {};
    auto* files = alpm_pkg_get_files(pkg);
    std::vector<std::string> result;
    if (!files) return result;
    result.reserve(files->count);
    for (std::size_t i = 0; i < files->count; ++i)
        if (files->files[i].name) result.emplace_back(files->files[i].name);
    return result;
}

std::optional<std::string> find_file_owner(const std::string& path) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::string relative = path;
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    for (auto* item = alpm_db_get_pkgcache(alpm_get_localdb(g_handle)); item; item = item->next) {
        auto* pkg = static_cast<alpm_pkg_t*>(item->data);
        if (alpm_filelist_contains(alpm_pkg_get_files(pkg), relative.c_str()))
            return std::string(alpm_pkg_get_name(pkg));
    }
    return std::nullopt;
}

std::vector<PackageGroup> list_sync_groups() {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<PackageGroup> result;
    for (auto* db_item = alpm_get_syncdbs(g_handle); db_item; db_item = db_item->next) {
        auto* db = static_cast<alpm_db_t*>(db_item->data);
        for (auto* item = alpm_db_get_groupcache(db); item; item = item->next) {
            auto* group = static_cast<alpm_group_t*>(item->data);
            PackageGroup out{alpm_db_get_name(db), group->name, {}};
            for (auto* pkg_item = group->packages; pkg_item; pkg_item = pkg_item->next)
                out.packages.emplace_back(alpm_pkg_get_name(static_cast<alpm_pkg_t*>(pkg_item->data)));
            result.push_back(std::move(out));
        }
    }
    return result;
}

std::vector<FileMatch> search_sync_files(const std::string& query) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<FileMatch> result;
    for (auto* db_item = alpm_get_syncdbs(g_handle); db_item; db_item = db_item->next) {
        auto* db = static_cast<alpm_db_t*>(db_item->data);
        if (!db) continue;
        for (auto* pkg_item = alpm_db_get_pkgcache(db); pkg_item; pkg_item = pkg_item->next) {
            auto* pkg = static_cast<alpm_pkg_t*>(pkg_item->data);
            const auto* files = pkg ? alpm_pkg_get_files(pkg) : nullptr;
            if (!files) continue;
            for (size_t i = 0; i < files->count; ++i) {
                const char* name = files->files[i].name;
                if (name && std::string(name).find(query) != std::string::npos)
                    result.push_back({alpm_db_get_name(db), alpm_pkg_get_name(pkg), name});
            }
        }
    }
    return result;
}

std::vector<std::string> missing_dependencies(const std::vector<std::string>& dependencies) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    std::vector<std::string> missing;
    auto* packages = alpm_db_get_pkgcache(alpm_get_localdb(g_handle));
    for (const auto& dependency : dependencies)
        if (!alpm_find_satisfier(packages, dependency.c_str())) missing.push_back(dependency);
    return missing;
}

int vercmp(const std::string& a, const std::string& b) {
    const int result = alpm_pkg_vercmp(a.c_str(), b.c_str());
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

std::optional<std::string> local_satisfier(const std::string& dep_spec) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    auto* packages = alpm_db_get_pkgcache(alpm_get_localdb(g_handle));
    auto* satisfier = alpm_find_satisfier(packages, dep_spec.c_str());
    if (!satisfier) return std::nullopt;
    return std::string(alpm_pkg_get_name(satisfier));
}

std::optional<std::string> sync_satisfier(const std::string& dep_spec) {
    if (!g_handle) throw alpm_error("libalpm not initialized");
    for (auto* db_item = alpm_get_syncdbs(g_handle); db_item; db_item = db_item->next) {
        auto* db = static_cast<alpm_db_t*>(db_item->data);
        if (!db) continue;
        auto* satisfier = alpm_find_satisfier(alpm_db_get_pkgcache(db), dep_spec.c_str());
        if (satisfier) return std::string(alpm_pkg_get_name(satisfier));
    }
    return std::nullopt;
}

int set_install_reason(const std::vector<std::string>& packages, bool explicit_reason) {
    require_root();
    auto* local = alpm_get_localdb(g_handle);
    for (const auto& name : packages) {
        auto* pkg = alpm_db_get_pkg(local, name.c_str());
        if (!pkg) throw alpm_error("installed package not found: " + name);
        const auto reason = explicit_reason ? ALPM_PKG_REASON_EXPLICIT : ALPM_PKG_REASON_DEPEND;
        if (alpm_pkg_set_reason(pkg, reason) < 0)
            throw alpm_error(last_error("cannot update install reason for " + name));
    }
    return 0;
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
        return pkg->reason == Package::Reason::Dependency && pkg->required_by.empty();
    } catch (...) { return false; }
}

std::vector<Package> get_orphan_packages() {
    try {
        auto pkgs = get_local_packages();
        std::vector<Package> orphans;
        for (auto& pkg : pkgs) {
            if (pkg.reason == Package::Reason::Dependency && pkg.required_by.empty())
                orphans.push_back(pkg);
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

/// Extract the package name from a dependency spec (strips version constraints).
static std::string parse_dep_spec_alpm(const std::string& spec) {
    for (char sep : {'>', '<', '='}) {
        auto pos = spec.find(sep);
        if (pos != std::string::npos) return spec.substr(0, pos);
    }
    return spec;
}

int install_sync_packages(const std::vector<std::string>& makedeps,
                          const std::vector<std::string>& checkdeps,
                          bool no_confirm) {
    // Collect all build dependencies.
    std::set<std::string> all_deps;
    for (const auto& dep : makedeps) all_deps.insert(dep);
    for (const auto& dep : checkdeps) all_deps.insert(dep);

    if (all_deps.empty()) return 0;  // Nothing to install.

    // Filter out already-installed packages and packages from official repos.
    std::vector<std::string> to_install;
    for (const auto& dep : all_deps) {
        const auto name = parse_dep_spec_alpm(dep);
        if (name.empty()) continue;
        
        // Check if already installed (honors version constraints via full spec).
        if (local_satisfier(dep).has_value()) continue;
        
        to_install.push_back(name);
    }

    if (to_install.empty()) return 0;  // All deps satisfied.

    terminal::info("Installing " + std::to_string(to_install.size()) +
                   " build dependency(ies)...");
    for (const auto& pkg : to_install) {
        std::cout << "  - " << pkg << "\n";
    }

    return install(to_install, /*needed=*/false, no_confirm);
}

} // namespace pacmkr::alpm
