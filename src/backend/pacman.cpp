#include "pacmkr/pacman.h"
#include "pacmkr/optimize.h"
#include "pacmkr/error.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <system_error>

namespace pacmkr::pacman {

namespace {

/// Check if a short-flag cluster contains a write-indicating flag.
static bool has_write_flag(const std::string& arg) {
    // Must be a short flag cluster like -Syu, -Scc, -Rns, etc.
    if (arg.size() < 3 || arg[0] != '-' || arg[1] == '-') return false;

    char primary = arg[1];
    for (size_t i = 2; i < arg.size(); ++i) {
        char f = arg[i];
        switch (primary) {
            case 'S': // Sync: y, u, f, c, w are writes
                if (f == 'y' || f == 'u' || f == 'f' || f == 'c' || f == 'w') return true;
                break;
            case 'R': // Remove: always a write
                return true;
            case 'U': // Upgrade: always a write
                return true;
            case 'D': // Database: s (sync), a (add), r (remove) are writes
                if (f == 's' || f == 'a' || f == 'r') return true;
                break;
            default:
                break;
        }
    }
    return false;
}

/// Check if a long flag is a write operation.
static bool is_write_long_flag(const std::string& arg) {
    static const char* write_flags[] = {
        "--refresh",     // -y equivalent
        "--sysupgrade",  // -u equivalent
        "--force",       // -f (sync context)
        "--clean",       // -c
        "--downloadonly",// -w
        "--remove",      // -R
        "--upgrade",     // -U
    };
    for (auto* wf : write_flags) {
        if (arg == wf) return true;
    }
    return false;
}

/// Check if the first non-option argument is a package name (write op).
static bool looks_like_package(const std::string& arg) {
    if (arg.empty()) return false;
    // Package names don't start with '-' and typically contain alphanumerics, hyphens, underscores
    for (char c : arg) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

/// Detect if these args represent a write operation needing pacman subprocess.
/// Returns the full arg list if write, std::nullopt if read-only (handled by alpm).
static std::optional<std::vector<std::string>> detect_write(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        // Long primary operation flags
        if (arg == "--sync" || arg == "--remove" || arg == "--upgrade" || arg == "--database") {
            return args; // Always write context for these
        }

        // Short flag clusters
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            // R and U are always writes
            if (primary == 'R' || primary == 'U') return args;
            // S with write flags, or bare -S (sync install)
            if (primary == 'S') {
                if (arg.size() == 2) return args; // bare -S is write
                if (has_write_flag(arg)) return args;
            }
            // D with write flags
            if (primary == 'D' && has_write_flag(arg)) return args;
        }

        // Long read-only flags — skip these (not writes by themselves)
        static const char* read_flags[] = {
            "--search", "--info", "--list", "--groups",
            "--downloadonly", "--clean",
        };
        bool is_read_only = false;
        if (arg.size() >= 2 && arg[0] == '-') {
            for (auto* rf : read_flags) {
                if (arg == rf) { is_read_only = true; break; }
            }
        }
        if (is_read_only) continue;

        // Package names after -S without read sub-flags are writes.
        // Long form: --sync <pkg> is caught by "--sync" match above.
    }

    return std::nullopt;
}

bool needs_privilege(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        if (arg == "--remove" || arg == "--upgrade") return true;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
            char primary = arg[1];
            if (primary == 'R' || primary == 'U') return true;
            if (primary == 'S') {
                // Check for write flags that need privilege
                for (size_t i = 2; i < arg.size(); ++i) {
                    char f = arg[i];
                    if (f == 'y' || f == 'u' || f == 'f' || f == 'c') return true;
                }
            }
        }
    }
    return false;
}

bool is_root() {
    std::ifstream in("/proc/self/status");
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 4 && line.substr(0, 4) == "Uid:") {
            auto parts = line.substr(4);
            auto start = parts.find_first_not_of(" \t");
            if (start != std::string::npos) {
                parts = parts.substr(start);
                auto third = parts.find(' ');
                if (third != std::string::npos) {
                    parts = parts.substr(third + 1);
                    third = parts.find(' ');
                    if (third != std::string::npos) {
                        parts = parts.substr(third + 1);
                        auto end = parts.find(' ');
                        if (end != std::string::npos) parts = parts.substr(0, end);
                        try { return std::stoul(parts) == 0; } catch (...) {}
                    }
                }
            }
        }
    }
    return false;
}

std::string escape_arg(const std::string& arg) {
    std::string result = " \"";
    for (char c : arg) {
        if (c == '"') result += "\\\"";
        else if (c == '\'') result += "'\\''";
        else if (c == '\\') result += "\\\\";
        else if (c == '$') result += "\\$";
        else result += c;
    }
    result += "\"";
    return result;
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

std::optional<std::vector<std::string>> detect_write_op(const std::vector<std::string>& args) {
    auto maybe = detect_write(args);
    if (maybe && needs_privilege(*maybe)) {
        return maybe;
    }
    // Even non-privileged write ops need pacman subprocess (alpm can't do them)
    // But read-only -S/-Q operations should NOT be forwarded
    // Only forward if it's actually a write operation
    if (maybe) {
        // Verify it's really a write, not just a read op with no flags
        bool is_write = false;
        for (auto& arg : *maybe) {
            if (is_write_long_flag(arg)) { is_write = true; break; }
            if (arg.size() >= 2 && arg[0] == '-' && arg[1] != '-') {
                char primary = arg[1];
                if (primary == 'R' || primary == 'U') { is_write = true; break; }
                if (primary == 'S') {
                    for (size_t i = 2; i < arg.size(); ++i) {
                        if (arg[i] == 'y' || arg[i] == 'u' || arg[i] == 'f' || arg[i] == 'c') {
                            is_write = true; break;
                        }
                    }
                }
            }
        }
        if (!is_write) return std::nullopt; // Read-only, let alpm handle it
    }
    return maybe;
}

int run_write_op(const std::vector<std::string>& args) {
    auto pacman_path = optimize::find_tool("pacman");
    if (!pacman_path) throw missing_tool_error("pacman");

    std::string cmd = "exec ";
    if (needs_privilege(args) && !is_root()) {
        auto sudo_path = optimize::find_tool("sudo");
        if (!sudo_path) throw missing_tool_error("sudo");
        cmd += sudo_path.value() + " ";
    }
    cmd += pacman_path.value();

    for (auto& arg : args) {
        cmd += escape_arg(arg);
    }

    std::cout.flush();
    int rc = std::system(cmd.c_str());

    if (rc == -1) return 1;
    return WEXITSTATUS(rc);
}

} // namespace pacmkr::pacman
