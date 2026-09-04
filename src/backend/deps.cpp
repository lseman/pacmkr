#include "pacmkr/backend/deps.h"
#include "pacmkr/backend/aur.h"
#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/backend/dep_graph.h"
#include "pacmkr/core/error.h"
#include "pacmkr/app/terminal.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <future>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <fstream>
#include <httplib.h>

namespace pacmkr::deps {

std::string parse_dep_spec(const std::string& spec) {
    for (char sep : {'>', '<', '='}) {
        auto pos = spec.find(sep);
        if (pos != std::string::npos) return spec.substr(0, pos);
    }
    return spec;
}

namespace {

// These accept a full dependency string (name plus an optional version
// constraint, e.g. "foo>=1.2"). libalpm's satisfier lookup honors both the
// constraint and any package that merely `provides` the name, so an
// out-of-date installed dependency no longer counts as satisfied and virtual
// dependencies resolve to their real providers.
bool check_installed(const std::string& dep) {
    return alpm::local_satisfier(dep).has_value();
}

bool check_official(const std::string& dep) {
    return alpm::sync_satisfier(dep).has_value();
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

bool is_dev_package(const std::string& pkgname) {
    static const char* suffixes[] = {"-git", "-svn", "-hg", "-bzr", "-dev", "-latest", "-bin"};
    for (auto* suffix : suffixes) {
        if (pkgname.size() > strlen(suffix) &&
            pkgname.substr(pkgname.size() - strlen(suffix)) == suffix) {
            return true;
        }
    }
    return false;
}

int compare_versions(const std::string& a, const std::string& b) {
    // Delegate to libalpm's canonical comparison — the same routine pacman
    // uses to decide upgrades — so pacmkr never disagrees with pacman about
    // which version is newer.
    return alpm::vercmp(a, b);
}

DepGraph resolve(const std::vector<std::string>& roots,
                 bool include_makedeps,
                 bool include_checkdeps,
                 bool include_optdeps) {
    DepGraph graph;

    // Discover + fetch AUR packages in batches (parallel RPC calls).
    // Each round: collect unknown names → batch-fetch → discover new deps.
    std::set<std::string> to_fetch(roots.begin(), roots.end());
    std::map<std::string, aur::AurPackageInfo> resolved;
    // A dependency named virtually (satisfied only by another package's
    // `provides`) maps here to the AUR pkgbase that actually supplies it.
    std::map<std::string, std::string> provider_map;
    std::set<std::string> unresolved;

    while (!to_fetch.empty()) {
        // Collect all names to fetch this round
        std::vector<std::string> batch(to_fetch.begin(), to_fetch.end());
        to_fetch.clear();

        // Batch-fetch all packages (fetch_batch chunks large request sets).
        std::vector<aur::AurPackageInfo> infos;
        try {
            infos = aur::fetch_batch(batch);
        } catch (const source_error&) {
            std::cerr << "warning: AUR batch fetch failed\n";
            for (auto& pkg : batch) {
                std::cerr << "warning: '" << pkg << "' not found in AUR\n";
            }
            continue;
        }

        // Process results and discover new dependencies
        auto process_info = [&](const aur::AurPackageInfo& info) {
            auto add_new_deps = [&](const std::vector<std::string>& dependencies) {
                for (const auto& raw : dependencies) {
                    const auto name = parse_dep_spec(raw);
                    if (name.empty()) continue;
                    // Pass the full spec so a version constraint or a
                    // provides-only match is evaluated correctly.
                    if (check_installed(raw) || check_official(raw)) continue;
                    if (resolved.count(name) || to_fetch.count(name)
                        || provider_map.count(name)) {
                        continue;
                    }
                    to_fetch.insert(name);
                }
            };
            add_new_deps(info.depends);
            if (include_makedeps) add_new_deps(info.makedepends);
            if (include_checkdeps) add_new_deps(info.checkdepends);
            if (include_optdeps) add_new_deps(info.optdepends);
            resolved[info.name] = info;
        };

        // Track which requested names an RPC result actually accounted for,
        // whether by exact name, by pkgbase, or by a `provides` entry.
        std::set<std::string> accounted;
        for (auto& info : infos) {
            try {
                process_info(info);
                accounted.insert(info.name);
                if (!info.pkgname.empty()) accounted.insert(info.pkgname);
                for (const auto& prov : info.provides) accounted.insert(parse_dep_spec(prov));
            } catch (const source_error&) {
                std::cerr << "warning: '" << info.name << "' not found in AUR\n";
            }
        }

        // Names with no direct AUR package: try to resolve them through a
        // provider (RPC `by=provides`) before giving up.
        for (const auto& name : batch) {
            if (accounted.count(name) || resolved.count(name)) continue;
            const auto candidates = aur::providers(name);
            if (candidates.empty()) {
                unresolved.insert(name);
                continue;
            }
            const std::string& provider = candidates.front();
            provider_map[name] = provider;
            if (!resolved.count(provider)) to_fetch.insert(provider);
        }
    }

    if (!unresolved.empty()) {
        std::ostringstream message;
        message << "no package or provider found for ";
        size_t index = 0;
        for (const auto& name : unresolved) {
            if (index++ != 0) message << ", ";
            message << "'" << name << "'";
        }
        throw dependency_error(message.str());
    }

    // Build dependency edges
    for (auto& [name, info] : resolved) {
        BuildNode node{name, {}, {}, {}};
        node.info = info;

        // Collect deps
        auto add_dep = [&](const std::vector<std::string>& dep_list, DepKind kind) {
            for (auto& raw : dep_list) {
                std::string dep_name = parse_dep_spec(raw);

                // Check installed (full spec: honors version constraint + provides)
                if (check_installed(raw)) {
                    node.non_aur_deps.push_back({dep_name, kind, DepSource::Installed, std::nullopt});
                    continue;
                }

                // Check official repo
                if (check_official(raw)) {
                    node.non_aur_deps.push_back({dep_name, kind, DepSource::Official, std::nullopt});
                    continue;
                }

                // Must be AUR — resolve a virtual name to its real provider.
                if (auto it = provider_map.find(dep_name); it != provider_map.end()) {
                    dep_name = it->second;
                }
                node.aur_deps.push_back(dep_name);
            }
        };

        add_dep(info.depends, DepKind::Runtime);
        if (include_makedeps)  add_dep(info.makedepends, DepKind::Make);
        if (include_checkdeps) add_dep(info.checkdepends, DepKind::Check);
        if (include_optdeps)   add_dep(info.optdepends, DepKind::Opt);

        graph.nodes[name] = std::move(node);
    }

    std::map<std::string, std::vector<std::string>> dependency_edges;
    for (const auto& [name, node] : graph.nodes) dependency_edges[name] = node.aur_deps;
    const auto schedule = dep_graph::schedule(dependency_edges);
    graph.build_order = schedule.order;

    // Collect satisfied deps
    for (auto& [_, node] : graph.nodes) {
        for (auto& dep : node.non_aur_deps) {
            if (!std::any_of(graph.satisfied_deps.begin(), graph.satisfied_deps.end(),
                             [&](const ResolvedDep& d) { return d.name == dep.name; })) {
                graph.satisfied_deps.push_back(dep);
            }
        }
    }

    for (const auto& stage : schedule.stages)
        graph.parallel_groups.push_back({stage.packages, stage.level});

    return graph;
}

namespace {

// Collect installed foreign packages (name + version). Must run on the thread
// that owns the libalpm handle, and before any transaction is started.
bool collect_foreign(std::vector<std::string>& names,
                     std::map<std::string, std::string>& versions) {
    std::vector<alpm::Package> foreign_packages;
    try {
        foreign_packages = alpm::get_foreign_packages();
    } catch (...) {
        return false; // alpm not available
    }
    for (const auto& pkg : foreign_packages) {
        names.push_back(pkg.name);
        versions[pkg.name] = pkg.version;
    }
    return true;
}

// Given the pre-collected foreign set, work out which packages have an AUR
// update. Uses the on-disk cache when every name was checked recently;
// otherwise issues one batch RPC query and records the outcome for next time.
// This function touches no libalpm state, so it is safe to run on a worker
// thread while a transaction is in progress.
std::vector<OutOfDatePkg> resolve_out_of_date(
    const std::vector<std::string>& foreign_names,
    const std::map<std::string, std::string>& installed_versions,
    bool quiet) {
    std::vector<OutOfDatePkg> result;
    if (foreign_names.empty()) return result;

    auto description_of = [](const nlohmann::json& obj) -> std::string {
        return obj.contains("Description") && obj.at("Description").is_string()
                   ? obj.at("Description").get<std::string>() : std::string{};
    };

    // Fast path: every foreign package was checked against the AUR recently, so
    // trust the cache. A name checked recently but absent from the cache is
    // known to not be an AUR package (so, no update) rather than a cache miss.
    bool all_known = true;
    std::vector<OutOfDatePkg> cached;
    for (const auto& name : foreign_names) {
        if (!aur_cache::checked_recently(name)) all_known = false;
        const auto entry = aur_cache::get(name);
        if (!entry) continue;
        const auto version = entry->value("Version", std::string{});
        if (version.empty()) continue;
        if (compare_versions(version, installed_versions.at(name)) > 0) {
            cached.push_back({name, installed_versions.at(name), version,
                              description_of(*entry)});
        }
    }
    if (all_known) {
        if (!quiet) {
            std::cout << "==> Checked " << foreign_names.size()
                      << " foreign package(s) using cached AUR metadata\n";
        }
        return cached;
    }

    // Batch-fetch AUR info for all foreign packages. The AUR rejects
    // over-long URLs, so split the request into fixed-size chunks.
    constexpr size_t kRpcBatchSize = 150;
    if (!quiet) {
        std::cout << "==> Checking " << foreign_names.size()
                  << " foreign package(s) against the AUR..." << std::flush;
    }

    httplib::Client cli("https://aur.archlinux.org");
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(15));
    cli.set_write_timeout(std::chrono::seconds(5));

    std::map<std::string, std::string> aur_versions;
    std::map<std::string, std::string> aur_descs;
    bool had_failure = false;

    for (size_t offset = 0; offset < foreign_names.size(); offset += kRpcBatchSize) {
        const size_t end = std::min(offset + kRpcBatchSize, foreign_names.size());
        std::string path = "/rpc?v=5&type=info";
        for (size_t i = offset; i < end; ++i) {
            path += "&arg[]=" + detail::url_encode(foreign_names[i]);
        }

        try {
            auto res = cli.Get(path);
            if (!res || res->status != 200) {
                had_failure = true;
                continue;
            }
            auto data = nlohmann::json::parse(res->body);
            if (!data.contains("results")) continue;
            aur_cache::merge_results(data["results"]);
            for (auto& item : data["results"]) {
                const std::string name = item.value("Name", "");
                const std::string version = item.value("Version", "");
                if (name.empty() || version.empty()) continue;
                aur_versions[name] = version;
                if (item.contains("Description") && item.at("Description").is_string()) {
                    aur_descs[name] = item.at("Description").get<std::string>();
                }
            }
        } catch (...) {
            had_failure = true;
        }
    }
    if (!quiet) std::cout << " done\n";

    if (had_failure) {
        std::cerr << "warning: one or more AUR queries failed; "
                     "results for the affected packages fall back to cached metadata\n";
    } else {
        aur_cache::note_checked(foreign_names);
    }

    for (const auto& name : foreign_names) {
        const auto& installed_ver = installed_versions.at(name);
        if (installed_ver.empty()) continue;

        auto it = aur_versions.find(name);
        if (it != aur_versions.end() && !it->second.empty()) {
            if (compare_versions(it->second, installed_ver) > 0) {
                std::string desc;
                if (auto dit = aur_descs.find(name); dit != aur_descs.end()) desc = dit->second;
                result.push_back({name, installed_ver, it->second, desc});
            }
            continue;
        }

        // No fresh answer for this package — use the cached comparison if we have one.
        if (had_failure) {
            if (const auto entry = aur_cache::get(name)) {
                const auto version = entry->value("Version", std::string{});
                if (!version.empty() && compare_versions(version, installed_ver) > 0) {
                    const std::string desc = entry->value("Description", std::string{});
                    result.push_back({name, installed_ver, version, desc});
                }
            }
        }
    }

    return result;
}

} // namespace

std::future<std::vector<OutOfDatePkg>> detect_out_of_date_async() {
    std::vector<std::string> foreign_names;
    std::map<std::string, std::string> installed_versions;
    const bool ok = collect_foreign(foreign_names, installed_versions);

    if (!ok || foreign_names.empty()) {
        std::promise<std::vector<OutOfDatePkg>> done;
        done.set_value({});
        return done.get_future();
    }

    return std::async(std::launch::async,
        [names = std::move(foreign_names),
         versions = std::move(installed_versions)]() {
            return resolve_out_of_date(names, versions, /*quiet=*/true);
        });
}

std::vector<OutOfDatePkg> detect_out_of_date() {
    std::vector<std::string> foreign_names;
    std::map<std::string, std::string> installed_versions;
    if (!collect_foreign(foreign_names, installed_versions)) return {};
    return resolve_out_of_date(foreign_names, installed_versions, /*quiet=*/false);
}

void print_upgrade_plan(const std::vector<OutOfDatePkg>& ootd) {
    if (ootd.empty()) {
        return;
    }

    terminal::info(std::to_string(ootd.size()) + " package update(s) available");
    for (auto& pkg : ootd) {
        std::cout << "  " << pkg.name << " " << pkg.installed_version
                  << " → " << pkg.aur_version << "\n";
        if (!pkg.desc.empty()) {
            auto desc = pkg.desc;
            if (desc.size() > 60) desc = desc.substr(0, 57) + "...";
            std::cout << "      " << desc << "\n";
        }
    }
    std::cout << "\n";
}

void DepGraph::print_plan(const std::vector<std::string>& roots) const {
    terminal::success("Dependencies resolved");
    std::cout << "\n";

    if (!build_order.empty()) {
        // Show parallel build groups
        if (!parallel_groups.empty()) {
            std::cout << "Build plan (" << build_order.size() << " package(s), "
                      << parallel_groups.size() << " stage(s)):\n";
            for (size_t i = 0; i < parallel_groups.size(); ++i) {
                auto& group = parallel_groups[i];
                std::cout << "  Stage " << (i + 1) << ": ";
                for (size_t j = 0; j < group.packages.size(); ++j) {
                    if (j > 0) std::cout << ", ";
                    auto& name = group.packages[j];
                    bool is_root = std::find(roots.begin(), roots.end(), name) != roots.end();
                    std::cout << name;
                    if (is_root) std::cout << " ★";
                }
                std::cout << "\n";
            }
        } else {
            std::cout << "Packages to build (" << build_order.size() << " total):\n";
            for (size_t i = 0; i < build_order.size(); ++i) {
                auto& name = build_order[i];
                bool is_root = std::find(roots.begin(), roots.end(), name) != roots.end();
                std::cout << "  " << (i + 1) << ". " << name;
                if (is_root) std::cout << " ★";
                if (auto it = nodes.find(name); it != nodes.end()) {
                    auto desc = it->second.info.desc.value_or("");
                    if (!desc.empty()) {
                        if (desc.size() > 60) std::cout << " — " << desc.substr(0, 57) + "...";
                        else std::cout << " — " << desc;
                    }
                }
                std::cout << "\n";
            }
        }
    } else {
        terminal::success("No AUR packages to build");
    }

    if (!satisfied_deps.empty()) {
        std::cout << "\nSatisfied dependencies:\n";
        for (auto& dep : satisfied_deps) {
            std::string src;
            switch (dep.source) {
                case DepSource::Installed: src = "installed"; break;
                case DepSource::Official:  src = "official repo"; break;
                case DepSource::Aur:       src = "aur"; break;
            }
            std::cout << "  " << dep.name << " → " << src << "\n";
        }
    }

    std::cout << "\n";
}

} // namespace pacmkr::deps
