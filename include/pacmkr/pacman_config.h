#pragma once

#include <istream>
#include <string>
#include <vector>

namespace pacmkr::pacman_config {

/// Parse repository section names from pacman.conf content.
/// The special [options] section is not a repository.
std::vector<std::string> repository_names(std::istream& input);

} // namespace pacmkr::pacman_config
