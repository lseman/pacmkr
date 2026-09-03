#pragma once

// Minimal JSON output helpers — wraps nlohmann::json for common pacmkr types.
// When --json is set, operations serialize their results via these functions
// instead of printing human-readable text.

#include <string>
#include <vector>
#include <optional>
#include <map>
#include "json.hpp"

using json = nlohmann::json;

namespace pacmkr {
namespace alpm { struct Package; }
namespace aur  { struct AurPackage; struct AurPackageInfo; }
namespace deps { struct OutOfDatePkg; }
}

namespace pacmkr::json_output {

/// Serialize a json object to a compact string (no trailing newline).
std::string serialize(const json& j);

/// ─── AUR helpers ────────────────────────────────────────────────────

/// Serialize an AurPackage search result.
json aur_package_to_json(const struct aur::AurPackage& pkg);

/// Serialize a vector of AurPackage results.
json aur_search_results_to_json(const std::vector<struct aur::AurPackage>& results);

/// Serialize an AurPackageInfo (full metadata).
json aur_package_info_to_json(const struct aur::AurPackageInfo& info);

/// ─── Repository helpers ─────────────────────────────────────────────

/// Serialize a Package from alpm.
json alpm_package_to_json(const struct alpm::Package& pkg);

/// Serialize a vector of Package results.
json alpm_packages_to_json(const std::vector<struct alpm::Package>& pkgs);

/// ─── Hybrid search result ───────────────────────────────────────────

/// Combined repo + AUR search with labeled sections.
json hybrid_search_to_json(
    const std::vector<struct alpm::Package>& repo_results,
    const std::vector<struct aur::AurPackage>& aur_results);

/// ─── Out-of-date / upgrade helpers ──────────────────────────────────

/// Serialize an OutOfDatePkg entry.
json ootd_package_to_json(const struct deps::OutOfDatePkg& pkg);

/// Serialize a vector of OutOfDatePkg entries (upgrade plan).
json ootd_plan_to_json(const std::vector<struct deps::OutOfDatePkg>& pkgs);

} // namespace pacmkr::json_output
