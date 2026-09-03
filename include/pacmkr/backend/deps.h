#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <future>
#include <filesystem>

#include "pacmkr/backend/aur.h"

namespace pacmkr::deps {

/// What kind of dependency this is.
enum class DepKind { Runtime, Make, Check, Opt };

/// Where a dependency is satisfied from.
enum class DepSource { Installed, Official, Aur };

/// A single resolved dependency edge.
struct ResolvedDep {
    std::string name;
    DepKind kind;
    DepSource source;
    std::optional<std::vector<std::string>> satisfied_by_provides;
};

/// Metadata for a package node in the build graph.
struct BuildNode {
    std::string name;
    std::vector<std::string> aur_deps;          // AUR deps to build
    std::vector<ResolvedDep> non_aur_deps;      // Official/installed deps
    aur::AurPackageInfo info;                   // Full AUR metadata
};

/// Parallel build groups: packages that can be built simultaneously.
struct BuildGroup {
    std::vector<std::string> packages;  // Packages in this group
    int level;                          // Depth in dependency graph (0 = no deps)
};

/// The complete resolved dependency graph.
struct DepGraph {
    std::map<std::string, BuildNode> nodes;     // All nodes by name
    std::vector<std::string> build_order;       // Topologically sorted
    std::vector<ResolvedDep> satisfied_deps;    // Non-AUR deps (satisfied)
    struct Conflict { std::string conflicting, by, reason; };
    std::vector<Conflict> conflicts;

    // Parallel build support
    std::vector<BuildGroup> parallel_groups;    // Groups of packages that can build in parallel

    int aur_count() const { return static_cast<int>(build_order.size()); }
    void print_plan(const std::vector<std::string>& roots) const;
};

/// Compare two Arch Linux package versions.
/// Returns: -1 if a < b, 0 if a == b, 1 if a > b
int compare_versions(const std::string& a, const std::string& b);

/// Resolve all dependencies for root packages.
DepGraph resolve(const std::vector<std::string>& roots,
                 bool include_makedeps = true,
                 bool include_checkdeps = true,
                 bool include_optdeps = false);

/// Detect installed AUR packages that have updates available.
struct OutOfDatePkg {
    std::string name;
    std::string installed_version;
    std::string aur_version;
    std::string desc;
};

std::vector<OutOfDatePkg> detect_out_of_date();

/// Begin an out-of-date detection: the libalpm reads run synchronously (so the
/// caller may safely start a transaction afterwards), while the AUR network
/// query and version comparison run on the returned future. This lets an
/// upgrade overlap the AUR check with the repository transaction.
std::future<std::vector<OutOfDatePkg>> detect_out_of_date_async();

/// Check if a package name looks like a dev/VCS package (-git, -dev, etc.)
bool is_dev_package(const std::string& pkgname);

/// Print the upgrade plan.
void print_upgrade_plan(const std::vector<OutOfDatePkg>& ootd);

} // namespace pacmkr::deps
