#pragma once

#include <string>
#include <vector>
#include <optional>

namespace pacmkr::pacman {

/// Detect if these args represent a write operation that must be forwarded to pacman.
/// Read operations (-Q, -Ss, -Si, etc.) are handled by libalpm directly.
/// Only install/remove/upgrade/sync-db ops need subprocess forwarding.
std::optional<std::vector<std::string>> detect_write_op(const std::vector<std::string>& args);

/// Run a write operation via pacman (with sudo if needed).
int run_write_op(const std::vector<std::string>& args);

} // namespace pacmkr::pacman
