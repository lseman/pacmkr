#include "pacmkr/app/operations.h"

namespace pacmkr::operations {

bool is_sync_upgrade(const std::vector<std::string>& args) {
    bool sync = false;
    bool sysupgrade = false;

    for (const auto& arg : args) {
        if (arg == "--sync" || arg == "-S") {
            sync = true;
            continue;
        }
        if (arg == "--sysupgrade") {
            sysupgrade = true;
            continue;
        }
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'S') {
            sync = true;
            for (std::size_t i = 2; i < arg.size(); ++i) {
                if (arg[i] == 'u') sysupgrade = true;
            }
        }
    }

    return sync && sysupgrade;
}

std::vector<std::string> repo_upgrade_args(bool refresh_databases,
                                           bool no_confirm) {
    std::vector<std::string> args;
    args.emplace_back(refresh_databases ? "-Syu" : "-Su");
    if (no_confirm) args.emplace_back("--noconfirm");
    return args;
}

bool is_foreign_package_query(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "--list-foreign") return true;
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'Q' &&
            arg.find('m', 2) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_quiet_query(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "--quiet") return true;
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'Q' &&
            arg.find('q', 2) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool requires_root(const std::vector<std::string>& args) {
    bool sync = false;
    bool sync_read_only = false;
    bool sync_mutating = false;

    for (const auto& arg : args) {
        if (arg == "repo" || arg == "--native-system-upgrade" ||
            arg == "--remove" || arg == "--upgrade" || arg == "--database" ||
            arg == "--clean" || arg == "--sysupgrade" || arg == "--downloadonly" ||
            arg == "--refresh" || arg == "--install" || arg == "-u")
            return true;
        if (arg == "--sync") { sync = true; continue; }
        if (arg == "--search" || arg == "--info" || arg == "--list" || arg == "--groups") {
            sync_read_only = true;
            continue;
        }
        if (arg.size() >= 2 && arg.front() == '-' && arg[1] != '-') {
            switch (arg[1]) {
                case 'R': case 'U': case 'D': return true;
                case 'S':
                    sync = true;
                    for (std::size_t i = 2; i < arg.size(); ++i) {
                        const char flag = arg[i];
                        if (flag == 's' || flag == 'i' || flag == 'l' || flag == 'g')
                            sync_read_only = true;
                        if (flag == 'c' || flag == 'u' || flag == 'y' || flag == 'w')
                            sync_mutating = true;
                    }
                    continue;
                default: continue;
            }
        }
    }

    return sync && (sync_mutating || !sync_read_only);
}

} // namespace pacmkr::operations
