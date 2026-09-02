#pragma once

#include <string>
#include <vector>

namespace pacmkr::operations {

/// True only for a system-upgrade invocation (-Su/-Syu or long equivalents).
/// A database-only refresh (-Sy) is deliberately not an upgrade.
bool is_sync_upgrade(const std::vector<std::string>& args);

/// Build the single, atomic pacman transaction used for repository upgrades.
std::vector<std::string> repo_upgrade_args(bool refresh_databases,
                                           bool no_confirm);

} // namespace pacmkr::operations
