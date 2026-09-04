#pragma once

#include <map>
#include <string>
#include <vector>

namespace pacmkr::dep_graph {

struct Stage {
    int level{};
    std::vector<std::string> packages;
};

struct Schedule {
    std::vector<std::string> order;
    std::vector<Stage> stages;
};

/// Produce a deterministic dependency-first schedule. Dependencies absent
/// from `dependencies` are already satisfied outside the AUR build graph and
/// do not create graph edges. Throws dependency_error when a cycle exists.
Schedule schedule(const std::map<std::string, std::vector<std::string>>& dependencies);

} // namespace pacmkr::dep_graph
