#pragma once

#include <string>
#include <vector>

namespace pacmkr::local_repo {

/// Execute a `pacmkr-repo ...` command. Returns a process exit code.
int run(const std::vector<std::string>& args);

void print_help();

} // namespace pacmkr::local_repo
