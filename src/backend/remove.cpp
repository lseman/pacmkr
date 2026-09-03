#include "pacmkr/backend/remove.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/core/error.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <sstream>
#include <set>

namespace pacmkr::remove {

namespace {

/// Parse dependency spec to extract package name (strip version constraints).
std::string parse_dep_name(const std::string& dep) {
    for (char sep : {'>', '<', '=', '!'}) {
        auto pos = dep.find(sep);
        if (pos != std::string::npos) return dep.substr(0, pos);
    }
    return dep;
}

/// Check if a package is explicitly installed.
bool is_explicit(const std::string& pkgname) {
    try {
        auto local = alpm::get_local_package(pkgname);
        if (local && local->reason == alpm::Package::Reason::Explicit) {
            return true;
        }
    } catch (...) {}
    return false;
}

/// Get all installed packages.
std::vector<std::string> get_installed_packages() {
    std::vector<std::string> pkgs;
    try {
        auto local = alpm::get_local_packages();
        for (auto& p : local) {
            pkgs.push_back(p.name);
        }
    } catch (...) {}
    return pkgs;
}

/// Get dependencies of a package.
std::vector<std::string> get_package_deps(const std::string& pkgname) {
    std::vector<std::string> deps;
    try {
        auto local = alpm::get_local_package(pkgname);
        if (local) {
            for (auto& dep : local->depends) {
                deps.push_back(parse_dep_name(dep));
            }
        }
    } catch (...) {}
    return deps;
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

std::vector<std::string> find_reverse_deps(const std::string& pkgname) {
    auto installed = get_installed_packages();
    std::vector<std::string> reverse_deps;

    for (auto& pkg : installed) {
        if (pkg == pkgname) continue; // Skip self

        auto deps = get_package_deps(pkg);
        if (std::find(deps.begin(), deps.end(), pkgname) != deps.end()) {
            reverse_deps.push_back(pkg);
        }
    }

    std::sort(reverse_deps.begin(), reverse_deps.end());
    return reverse_deps;
}

int cascade_remove(const std::vector<std::string>& packages, bool noconfirm) {
    // First, find all reverse dependencies recursively
    std::set<std::string> to_remove(packages.begin(), packages.end());
    std::vector<std::string> work_list(packages);
    size_t idx = 0;

    while (idx < work_list.size()) {
        auto current = work_list[idx++];

        auto rdeps = find_reverse_deps(current);
        for (auto& rd : rdeps) {
            if (to_remove.find(rd) == to_remove.end()) {
                // Check if it's explicitly installed
                if (!is_explicit(rd)) {
                    to_remove.insert(rd);
                    work_list.push_back(rd);
                }
            }
        }
    }

    if (to_remove.empty()) {
        std::cout << "==> Nothing to remove\n";
        return 0;
    }

    // Show what will be removed
    std::cout << "==> Packages to remove (" << to_remove.size() << " total):\n";
    for (auto& pkg : to_remove) {
        std::cout << "  - " << pkg << "\n";
    }

    // Ask for confirmation unless --noconfirm
    if (!noconfirm) {
        std::cout << "\n==> Proceed with removal? [Y/n] ";
        std::string input;
        std::getline(std::cin, input);

        if (!input.empty() && (input[0] == 'n' || input[0] == 'N')) {
            std::cout << "==> Aborted.\n";
            return 0;
        }
    }

    std::cout << "\n==> Removing packages...\n";
    int rc = alpm::remove({to_remove.begin(), to_remove.end()}, true, true, true, noconfirm);

    if (rc == 0) {
        std::cout << "==> Packages removed successfully.\n";
    } else {
        std::cerr << "error: package removal failed with exit code " << rc << "\n";
    }

    return rc;
}

int recursive_remove(const std::vector<std::string>& packages, bool noconfirm) {
    // Remove packages and their unused dependencies
    std::cout << "==> Removing packages and unused dependencies...\n";
    int rc = alpm::remove(packages, true, false, true, noconfirm);

    if (rc == 0) {
        std::cout << "==> Packages removed successfully.\n";
    } else {
        std::cerr << "error: package removal failed with exit code " << rc << "\n";
    }

    return rc;
}

int remove_unneeded(bool noconfirm) {
    // Find orphans (deps not required by any explicit package)
    std::vector<std::string> orphans;
    for (const auto& pkg : alpm::get_orphan_packages()) orphans.push_back(pkg.name);

    if (orphans.empty()) {
        std::cout << "==> No unneeded packages found.\n";
        return 0;
    }

    // Show orphans
    std::cout << "==> Unneeded packages (" << orphans.size() << " total):\n";
    for (auto& pkg : orphans) {
        std::cout << "  - " << pkg << "\n";
    }

    if (!noconfirm) {
        std::cout << "\n==> Proceed with removal? [Y/n] ";
        std::string input;
        std::getline(std::cin, input);

        if (!input.empty() && (input[0] == 'n' || input[0] == 'N')) {
            std::cout << "==> Aborted.\n";
            return 0;
        }
    }

    std::cout << "\n==> Removing unneeded packages...\n";
    int rc = alpm::remove(orphans, true, false, true, noconfirm);

    if (rc == 0) {
        std::cout << "==> Unneeded packages removed successfully.\n";
    } else {
        std::cerr << "error: orphan removal failed with exit code " << rc << "\n";
    }

    return rc;
}

} // namespace pacmkr::remove
