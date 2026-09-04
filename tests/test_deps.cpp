#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <algorithm>

#include <alpm.h>
#include "pacmkr/backend/dep_graph.h"
#include "pacmkr/core/error.h"

namespace {

// deps::compare_versions delegates straight to libalpm; mirror that here so
// the test exercises the exact routine pacman uses to decide upgrades.
int compare_versions(const std::string& a, const std::string& b) {
    const int result = alpm_pkg_vercmp(a.c_str(), b.c_str());
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

// Topological sort (Kahn's algorithm) — same as deps.cpp
struct BuildNode {
    std::string name;
    std::vector<std::string> aur_deps;
};

std::vector<std::string> topological_sort(const std::map<std::string, BuildNode>& nodes) {
    std::map<std::string, std::vector<std::string>> dependencies;
    for (const auto& [name, node] : nodes) dependencies[name] = node.aur_deps;
    return pacmkr::dep_graph::schedule(dependencies).order;
}

} // anonymous

// ─── Tests ───────────────────────────────────────────────────────────

void test_compare_versions() {
    // Basic numeric
    assert(compare_versions("1.0", "2.0") < 0);
    assert(compare_versions("2.0", "1.0") > 0);
    assert(compare_versions("1.0", "1.0") == 0);

    // Numeric vs numeric (not string)
    assert(compare_versions("1.10", "1.9") > 0);   // 10 > 9 numerically
    assert(compare_versions("1.2", "1.10") < 0);

    // Release comparison
    assert(compare_versions("1.0-1", "1.0-2") < 0);
    assert(compare_versions("1.0-2", "1.0-1") > 0);

    // A missing pkgrel makes the two equal (pacman ignores pkgrel then)
    assert(compare_versions("1.0-1", "1.0") == 0);

    // Pre-release suffix in pkgver: 1.0rc1 < 1.0 < 1.0.1 (vercmp(8) ordering)
    assert(compare_versions("1.0rc1", "1.0") < 0);
    assert(compare_versions("1.0beta", "1.0") < 0);
    assert(compare_versions("1.0", "1.0.1") < 0);
    assert(compare_versions("1.0a", "1.0b") < 0);

    // Epoch always wins over the rest of the version
    assert(compare_versions("1:1.0", "2:0.9") < 0);
    assert(compare_versions("2:1.0", "1:9.9") > 0);
    assert(compare_versions("1.0", "1:0.1") < 0);

    // libalpm treats '~' as a plain segment separator (like '.'), with no
    // "sorts before end-of-string" rule. The old hand-rolled comparator
    // invented that rule and disagreed with pacman; these lock in reality.
    assert(compare_versions("1.0~a", "1.0.a") == 0);
    assert(compare_versions("1~rc1", "1") > 0);
    assert(compare_versions("1.0~beta", "1.0") > 0);

    std::cout << "  PASSED: compare_versions\n";
}

void test_topological_sort_linear() {
    std::map<std::string, BuildNode> nodes;
    nodes["c"] = {"c", {}};
    nodes["b"] = {"b", {"c"}};
    nodes["a"] = {"a", {"b"}};

    auto order = topological_sort(nodes);
    auto c_idx = std::find(order.begin(), order.end(), "c") - order.begin();
    auto b_idx = std::find(order.begin(), order.end(), "b") - order.begin();
    auto a_idx = std::find(order.begin(), order.end(), "a") - order.begin();

    assert(c_idx < b_idx && "c must come before b");
    assert(b_idx < a_idx && "b must come before a");

    std::cout << "  PASSED: topological_sort (linear)\n";
}

void test_topological_sort_diamond() {
    std::map<std::string, BuildNode> nodes;
    for (auto& n : {"a", "b", "c", "d"}) {
        nodes[n] = {n, {}};
    }
    nodes["a"].aur_deps = {"b", "c"};
    nodes["b"].aur_deps = {"d"};
    nodes["c"].aur_deps = {"d"};

    auto order = topological_sort(nodes);
    auto d_idx = std::find(order.begin(), order.end(), "d") - order.begin();
    auto b_idx = std::find(order.begin(), order.end(), "b") - order.begin();
    auto c_idx = std::find(order.begin(), order.end(), "c") - order.begin();
    auto a_idx = std::find(order.begin(), order.end(), "a") - order.begin();

    assert(d_idx < b_idx && "d before b");
    assert(d_idx < c_idx && "d before c");
    assert(b_idx < a_idx && "b before a");
    assert(c_idx < a_idx && "c before a");

    std::cout << "  PASSED: topological_sort (diamond)\n";
}

void test_topological_sort_cycle_fails() {
    std::map<std::string, BuildNode> nodes;
    nodes["a"] = {"a", {"b"}};
    nodes["b"] = {"b", {"a"}};

    bool failed = false;
    try {
        (void)topological_sort(nodes);
    } catch (const pacmkr::dependency_error&) {
        failed = true;
    }
    assert(failed && "dependency cycles must abort instead of producing a build order");
}

void test_parallel_stages_follow_dependencies() {
    const std::map<std::string, std::vector<std::string>> dependencies{
        {"app", {"left", "right"}},
        {"left", {"base"}},
        {"right", {"base"}},
        {"base", {}},
    };
    const auto schedule = pacmkr::dep_graph::schedule(dependencies);
    assert(schedule.stages.size() == 3);
    assert(schedule.stages[0].packages == std::vector<std::string>{"base"});
    assert((schedule.stages[1].packages == std::vector<std::string>{"left", "right"}));
    assert(schedule.stages[2].packages == std::vector<std::string>{"app"});
}

int main() {
    std::cout << "Running deps tests...\n";
    test_compare_versions();
    test_topological_sort_linear();
    test_topological_sort_diamond();
    test_topological_sort_cycle_fails();
    test_parallel_stages_follow_dependencies();
    std::cout << "All deps tests passed.\n";
    return 0;
}
