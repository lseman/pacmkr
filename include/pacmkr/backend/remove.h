#pragma once

#include <string>
#include <vector>

namespace pacmkr::remove {

/// Reverse dependencies: find packages that depend on the given package.
std::vector<std::string> find_reverse_deps(const std::string& pkgname);

/// Cascade remove: remove a package and all packages that depend on it.
/// Returns 0 on success, non-zero on failure.
int cascade_remove(const std::vector<std::string>& packages, bool noconfirm = false);

/// Recursive remove: remove a package and its unused dependencies.
int recursive_remove(const std::vector<std::string>& packages, bool noconfirm = false);

/// Remove unneeded packages (orphans).
int remove_unneeded(bool noconfirm = false);

} // namespace pacmkr::remove
