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

/// True for pacman's -Qm form or pacmkr's --list-foreign alias.
bool is_foreign_package_query(const std::vector<std::string>& args);

/// True when a query requests names-only output (-q/--quiet).
bool is_quiet_query(const std::vector<std::string>& args);

/// True when the invocation can modify the system package database or cache.
bool requires_root(const std::vector<std::string>& args);

} // namespace pacmkr::operations
