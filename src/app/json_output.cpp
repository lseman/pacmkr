#include "pacmkr/app/json_output.h"
#include "pacmkr/backend/aur.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/backend/deps.h"
#include "pacmkr/core/package.h"

// JSON must be fully defined before use — include after forward-declaring headers.
#include "json.hpp"
using json = nlohmann::json;

namespace pacmkr::json_output {

std::string serialize(const json& j) {
    return j.dump(2);
}

// ─── AUR helpers ──────────────────────────────────────────────────────

json aur_package_to_json(const struct aur::AurPackage& pkg) {
    json j;
    j["name"] = pkg.name;
    j["pkgname"] = pkg.pkgname;
    if (pkg.desc) j["description"] = *pkg.desc;
    if (pkg.url) j["url"] = *pkg.url;
    j["num_votes"] = pkg.numvotes;
    j["popularity"] = pkg.popularity;
    j["relevance_score"] = pkg.relevance_score;
    if (pkg.outofdate) j["outofdate_ts"] = *pkg.outofdate;
    if (pkg.firstsubmitted) j["first_submitted_ts"] = *pkg.firstsubmitted;
    j["last_modified"] = pkg.lastmodified;
    return j;
}

json aur_search_results_to_json(const std::vector<struct aur::AurPackage>& results) {
    json arr = json::array();
    for (auto& pkg : results) arr.push_back(aur_package_to_json(pkg));
    return arr;
}

json aur_package_info_to_json(const struct aur::AurPackageInfo& info) {
    json j;
    j["name"] = info.name;
    j["pkgname"] = info.pkgname;
    j["version"] = info.version;
    if (info.desc) j["description"] = *info.desc;
    if (info.url) j["url"] = *info.url;
    j["license"] = info.license;
    j["depends"] = info.depends;
    j["makedepends"] = info.makedepends;
    j["checkdepends"] = info.checkdepends;
    j["optdepends"] = info.optdepends;
    j["provides"] = info.provides;
    j["conflicts"] = info.conflicts;
    j["replaces"] = info.replaces;
    j["backup"] = info.backup;
    j["options"] = info.options;
    j["source"] = info.source;
    j["subpackages"] = info.subpackages;
    // Hashes
    if (!info.sha256sums.empty()) j["sha256sums"] = info.sha256sums;
    if (!info.md5sums.empty()) j["md5sums"] = info.md5sums;
    return j;
}

// ─── Repository helpers ───────────────────────────────────────────────

json alpm_package_to_json(const struct alpm::Package& pkg) {
    json j;
    j["name"] = pkg.name;
    j["version"] = pkg.version;
    if (!pkg.desc.empty()) j["description"] = pkg.desc;
    if (!pkg.url.empty()) j["url"] = pkg.url;
    j["architecture"] = pkg.arch;
    if (!pkg.packager.empty()) j["packager"] = pkg.packager;
    j["build_date"] = pkg.build_date;
    j["install_date"] = pkg.install_date;
    j["size"] = pkg.size;
    j["isize"] = pkg.isize;
    j["origin_db"] = pkg.origin_db;

    // Reason: 0=unknown, 1=dependency, 2=explicit
    switch (pkg.reason) {
        case alpm::Package::Reason::Dependency:  j["reason"] = "dependency"; break;
        case alpm::Package::Reason::Explicit:    j["reason"] = "explicit"; break;
        default:                           j["reason"] = "unknown"; break;
    }

    j["licenses"] = pkg.licenses;
    j["groups"] = pkg.groups;
    j["depends"] = pkg.depends;
    j["makedepends"] = pkg.makedepends;
    j["checkdepends"] = pkg.checkdepends;
    j["optdepends"] = pkg.optdepends;
    j["conflicts"] = pkg.conflicts;
    j["provides"] = pkg.provides;
    j["replaces"] = pkg.replaces;
    j["required_by"] = pkg.required_by;
    j["optional_for"] = pkg.optional_for;
    if (!pkg.backup.empty()) j["backup"] = pkg.backup;
    if (!pkg.files.empty()) j["files"] = pkg.files;
    return j;
}

json alpm_packages_to_json(const std::vector<struct alpm::Package>& pkgs) {
    json arr = json::array();
    for (auto& pkg : pkgs) arr.push_back(alpm_package_to_json(pkg));
    return arr;
}

// ─── Hybrid search result ─────────────────────────────────────────────

json hybrid_search_to_json(
    const std::vector<struct alpm::Package>& repo_results,
    const std::vector<struct aur::AurPackage>& aur_results) {
    json j;
    j["repository"] = alpm_packages_to_json(repo_results);
    j["aur"] = aur_search_results_to_json(aur_results);
    return j;
}

// ─── Out-of-date / upgrade helpers ─────────────────────────────────────

json ootd_package_to_json(const struct deps::OutOfDatePkg& pkg) {
    json j;
    j["name"] = pkg.name;
    j["installed_version"] = pkg.installed_version;
    j["aur_version"] = pkg.aur_version;
    if (!pkg.desc.empty()) j["description"] = pkg.desc;
    return j;
}

json ootd_plan_to_json(const std::vector<struct deps::OutOfDatePkg>& pkgs) {
    json arr = json::array();
    for (auto& pkg : pkgs) arr.push_back(ootd_package_to_json(pkg));
    return arr;
}

} // namespace pacmkr::json_output
