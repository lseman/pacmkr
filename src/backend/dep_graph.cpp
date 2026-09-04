#include "pacmkr/backend/dep_graph.h"

#include "pacmkr/core/error.h"

#include <algorithm>
#include <sstream>

namespace pacmkr::dep_graph {

Schedule schedule(const std::map<std::string, std::vector<std::string>>& dependencies) {
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> dependents;
    std::map<std::string, int> depth;

    for (const auto& [name, _] : dependencies) {
        in_degree[name] = 0;
        depth[name] = 0;
    }
    for (const auto& [name, package_dependencies] : dependencies) {
        for (const auto& dependency : package_dependencies) {
            if (!dependencies.contains(dependency)) continue;
            ++in_degree[name];
            dependents[dependency].push_back(name);
        }
    }

    std::vector<std::string> ready;
    for (const auto& [name, degree] : in_degree) {
        if (degree == 0) ready.push_back(name);
    }

    Schedule result;
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end());
        auto current = std::move(ready.front());
        ready.erase(ready.begin());
        result.order.push_back(current);

        for (const auto& dependent : dependents[current]) {
            depth[dependent] = std::max(depth[dependent], depth[current] + 1);
            if (--in_degree[dependent] == 0) ready.push_back(dependent);
        }
    }

    if (result.order.size() != dependencies.size()) {
        std::ostringstream message;
        message << "circular dependency among: ";
        bool first = true;
        for (const auto& [name, degree] : in_degree) {
            if (degree == 0) continue;
            if (!first) message << ", ";
            message << name;
            first = false;
        }
        throw dependency_error(message.str());
    }

    std::map<int, std::vector<std::string>> by_depth;
    for (const auto& name : result.order) by_depth[depth[name]].push_back(name);
    for (auto& [level, packages] : by_depth) {
        result.stages.push_back({level, std::move(packages)});
    }
    return result;
}

} // namespace pacmkr::dep_graph
