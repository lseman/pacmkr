#include "pacmkr/operations.h"

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

} // namespace pacmkr::operations
