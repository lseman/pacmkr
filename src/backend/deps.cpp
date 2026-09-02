#include "pacmkr/deps.h"
#include "pacmkr/aur.h"
#include "pacmkr/aur_cache.h"
#include "pacmkr/alpm.h"
#include "pacmkr/status.h"
#include "pacmkr/error.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <fstream>
#include <httplib.h>

namespace pacmkr::deps {

namespace {

// ─── pkgvercmp (Arch Linux algorithm) ────────────────────────────────

enum class TokenKind { Null, Numeric, Alpha };
struct Token {
    TokenKind kind;
    unsigned long long num_val{};
    std::string str_val;
    bool tilde{false};
};

static int strverscmp_impl(const std::string& a, const std::string& b) {
    auto lower = [](std::string s) -> std::string {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    std::string la = lower(a), lb = lower(b);

    // Special keyword ordering (from Arch's pkgver.c)
    static const char* keywords[] = {"", "pre", "c", "rc", "a", "b", "ssl", "rel"};

    auto kw_pos = [&](const std::string& s) -> int {
        for (int i = 0; i < static_cast<int>(sizeof(keywords)/sizeof(keywords[0])); ++i) {
            if (keywords[i] == s) return i;
        }
        return -1;
    };

    int ai = kw_pos(la), bi = kw_pos(lb);
    if (ai >= 0 && bi >= 0) return (ai < bi ? -1 : (ai > bi ? 1 : 0));
    if (ai >= 0) return -1;
    if (bi >= 0) return 1;

    // Numeric comparison for alpha segments containing digits
    auto a_num = la.find_first_of("0123456789");
    auto b_num = lb.find_first_of("0123456789");
    if (a_num != std::string::npos && b_num != std::string::npos) {
        unsigned long long an = std::stoull(la.substr(a_num));
        unsigned long long bn = std::stoull(lb.substr(b_num));
        if (an != bn) return (an < bn ? -1 : 1);
    }

    return la.compare(lb);
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    size_t i = 0;

    while (i < s.size()) {
        Token tok{};

        if (s[i] == '~') {
            tok.tilde = true;
            ++i;
            // Read tilde-numeric or tilde-alpha
            if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                tok.kind = TokenKind::Numeric;
                size_t start = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                tok.num_val = std::stoull(s.substr(start, i - start));
            } else {
                tok.kind = TokenKind::Alpha;
                size_t start = i;
                while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                tok.str_val = s.substr(start, i - start);
            }
        } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            tok.kind = TokenKind::Numeric;
            size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            tok.num_val = std::stoull(s.substr(start, i - start));
        } else if (std::isalpha(static_cast<unsigned char>(s[i]))) {
            tok.kind = TokenKind::Alpha;
            size_t start = i;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            tok.str_val = s.substr(start, i - start);
        } else {
            ++i; // skip unknown characters
            continue;
        }

        tokens.push_back(tok);
    }

    tokens.push_back({TokenKind::Null});
    return tokens;
}

int pkgvercmp(const std::string& a, const std::string& b) {
    auto ta = tokenize(a), tb = tokenize(b);
    size_t ai = 0, bi = 0;

    while (ai < ta.size() && bi < tb.size()) {
        auto& tok_a = ta[ai], tok_b = tb[bi];

        if (tok_a.kind == TokenKind::Null && tok_b.kind == TokenKind::Null) return 0;
        if (tok_a.kind == TokenKind::Null) return -1;
        if (tok_b.kind == TokenKind::Null) return 1;

        // Same kind comparison
        if (tok_a.kind == TokenKind::Numeric && tok_b.kind == TokenKind::Numeric) {
            // Tilde < normal for same numeric value
            if (tok_a.tilde != tok_b.tilde) return tok_a.tilde ? -1 : 1;
            if (tok_a.num_val != tok_b.num_val) return (tok_a.num_val < tok_b.num_val ? -1 : 1);
        } else if (tok_a.kind == TokenKind::Alpha && tok_b.kind == TokenKind::Alpha) {
            if (tok_a.tilde != tok_b.tilde) return tok_a.tilde ? -1 : 1;
            int c = strverscmp_impl(tok_a.str_val, tok_b.str_val);
            if (c != 0) return c;
        } else {
            // Mixed: numeric vs alpha — compare numeric-as-string to alpha
            std::string num_str = std::to_string(tok_a.kind == TokenKind::Numeric ? tok_a.num_val : tok_b.num_val);
            const std::string& alpha_str = tok_a.kind == TokenKind::Alpha ? tok_a.str_val : tok_b.str_val;
            int c = strverscmp_impl(num_str, alpha_str);
            if (c != 0) return c;
        }

        ++ai; ++bi;
    }

    // Remaining tokens
    if (ai < ta.size()) {
        // Check if remaining are Null
        bool all_null = true;
        for (; ai < ta.size(); ++ai) if (ta[ai].kind != TokenKind::Null) { all_null = false; break; }
        return all_null ? 0 : 1;
    }
    if (bi < tb.size()) {
        bool all_null = true;
        for (; bi < tb.size(); ++bi) if (tb[bi].kind != TokenKind::Null) { all_null = false; break; }
        return all_null ? 0 : -1;
    }

    return 0;
}

/// Compare two Arch Linux version segments (split on '-').
/// Handles '~': tilde sorts before any character or end-of-string.
/// Dots are skipped (not significant). Uses keyword ordering for alpha.
/// Empty segment means "no release" (final), which is greater than any
/// pre-release or numbered release (non-empty < empty).
static int compare_segment(const std::string& a, const std::string& b) {
    // Empty segment = no release (final) > any non-empty segment
    if (a.empty() && !b.empty()) return 1;
    if (!a.empty() && b.empty()) return -1;
    if (a.empty() && b.empty()) return 0;

    std::string la = a, lb = b;
    std::transform(la.begin(), la.end(), la.begin(), ::tolower);
    std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);

    size_t ai = 0, bi = 0;
    while (ai < la.size() || bi < lb.size()) {
        // Skip dots (not significant in Arch versioning)
        if (ai < la.size() && la[ai] == '.') { ++ai; continue; }
        if (bi < lb.size() && lb[bi] == '.') { ++bi; continue; }

        bool a_end = (ai >= la.size()), b_end = (bi >= lb.size());

        // Tilde sorts before everything else, including end-of-string.
        // Check tilde BEFORE end-of-string so "1~rc1" < "1".
        if (!a_end && !b_end) {
            if (la[ai] == '~' && lb[bi] != '~') return -1;
            if (lb[bi] == '~' && la[ai] != '~') return 1;
        } else if (!a_end && b_end) {
            // a has chars left, b exhausted — tilde < nothing
            if (la[ai] == '~') return -1;
            return 1;
        } else if (a_end && !b_end) {
            // b has chars left, a exhausted — nothing > tilde
            if (lb[bi] == '~') return 1;
            return -1;
        } else {
            return 0;
        }

        bool a_digit = std::isdigit(static_cast<unsigned char>(la[ai]));
        bool b_digit = std::isdigit(static_cast<unsigned char>(lb[bi]));

        if (a_digit && b_digit) {
            // Both numeric: compare as numbers
            size_t a_start = ai, b_start = bi;
            while (ai < la.size() && std::isdigit(static_cast<unsigned char>(la[ai]))) ++ai;
            while (bi < lb.size() && std::isdigit(static_cast<unsigned char>(lb[bi]))) ++bi;
            std::string anum = la.substr(a_start, ai - a_start);
            std::string bnum = lb.substr(b_start, bi - b_start);
            if (anum.size() != bnum.size()) return (anum.size() < bnum.size() ? -1 : 1);
            if (anum != bnum) return (anum < bnum ? -1 : 1);
        } else if (a_digit || b_digit) {
            // Mixed: digit sorts before letter
            return a_digit ? -1 : 1;
        } else {
            // Both alpha: compare lexicographically with keyword ordering
            size_t a_start = ai, b_start = bi;
            while (ai < la.size() && std::isalpha(static_cast<unsigned char>(la[ai]))) ++ai;
            while (bi < lb.size() && std::isalpha(static_cast<unsigned char>(lb[bi]))) ++bi;
            std::string aalpha = la.substr(a_start, ai - a_start);
            std::string balpha = lb.substr(b_start, bi - b_start);

            // Special keyword ordering (from Arch's pkgver.c)
            static const char* keywords[] = {"pre", "c", "rc", "a", "b", "ssl", "rel"};
            auto kw_pos = [&](const std::string& s) -> int {
                for (int i = 0; i < static_cast<int>(sizeof(keywords)/sizeof(keywords[0])); ++i) {
                    if (keywords[i] == s) return i;
                }
                return -1;
            };

            int ka = kw_pos(aalpha), kb = kw_pos(balpha);
            if (ka >= 0 && kb >= 0) {
                if (ka != kb) return (ka < kb ? -1 : 1);
            } else if (ka >= 0) {
                return -1;
            } else if (kb >= 0) {
                return 1;
            } else {
                if (aalpha != balpha) return (aalpha < balpha ? -1 : 1);
            }
        }
    }

    return 0;
}

/// Arch Linux pkgvercmp: split on '-', compare segments.
static int arch_pkgvercmp(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '-') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return parts;
    };

    auto a_parts = split(a), b_parts = split(b);
    size_t max_len = std::max(a_parts.size(), b_parts.size());
    for (size_t i = 0; i < max_len; ++i) {
        const auto& pa = (i < a_parts.size()) ? a_parts[i] : "";
        const auto& pb = (i < b_parts.size()) ? b_parts[i] : "";
        int c = compare_segment(pa, pb);
        if (c != 0) return c;
    }
    return 0;
}

int compare_epoch_upstream_release(const std::string& a, const std::string& b) {
    auto parse_ver = [](const std::string& v) -> std::tuple<unsigned long long, std::string> {
        auto colon = v.find(':');
        unsigned long long epoch = 0;
        std::string rest = v;

        if (colon != std::string::npos) {
            try { epoch = std::stoull(v.substr(0, colon)); } catch (...) {}
            rest = v.substr(colon + 1);
        }

        return {epoch, rest};
    };

    auto [a_epoch, a_rest] = parse_ver(a);
    auto [b_epoch, b_rest] = parse_ver(b);

    if (a_epoch != b_epoch) return (a_epoch < b_epoch ? -1 : 1);
    // Full version comparison handles all edge cases: tildes, pre-releases,
    // numeric segments, keyword ordering, etc.
    return arch_pkgvercmp(a_rest, b_rest);
}

// ─── Dependency resolution helpers ───────────────────────────────────

std::string parse_dep_spec(const std::string& spec) {
    for (char sep : {'>', '<', '='}) {
        auto pos = spec.find(sep);
        if (pos != std::string::npos) return spec.substr(0, pos);
    }
    return spec;
}

bool check_installed(const std::string& dep) {
    std::ostringstream cmd;
    cmd << "pacman -Qi " << dep << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

bool check_official(const std::string& dep) {
    std::ostringstream cmd;
    cmd << "pacman -Si " << dep << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

std::string parse_pacman_version(const std::string& output) {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (detail::starts_with(line, "Version ")) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto ver = line.substr(colon + 1);
                // Trim whitespace
                auto start = ver.find_first_not_of(" \t");
                if (start != std::string::npos) return ver.substr(start);
            }
        }
    }
    return "0";
}

std::string extract_pkgver_from_git(const std::string& pkgname,
                                     const std::filesystem::path& tmpdir) {
    auto pkgdir = tmpdir / pkgname;
    std::ostringstream cmd;
    cmd << "git clone --depth 1 \"https://aur.archlinux.org/" << pkgname << ".git\" \""
        << pkgdir.string() << "\" 2>/dev/null";

    if (std::system(cmd.str().c_str()) != 0) return "0";

    auto pkgbuild_path = pkgdir / "PKGBUILD";
    std::ifstream in(pkgbuild_path);
    if (!in) return "0";

    std::string line;
    while (std::getline(in, line)) {
        auto t = line;
        // Trim leading whitespace
        auto start = t.find_first_not_of(" \t");
        if (start != std::string::npos) t = t.substr(start);

        if (detail::starts_with(line, "pkgver=")) {
            auto val = t.substr(7);
            // Strip quotes and trailing comments
            auto end = val.find_first_of(" #\"'");
            if (end != std::string::npos) val = val.substr(0, end);
            // Trim
            auto e2 = val.find_last_not_of(" \t\r\n");
            if (e2 != std::string::npos) val = val.substr(0, e2 + 1);
            return val;
        }
    }
    return "0";
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
    return compare_epoch_upstream_release(a, b);
}

DepGraph resolve(const std::vector<std::string>& roots,
                 bool include_makedeps,
                 bool include_checkdeps,
                 bool _include_optdeps) {
    DepGraph graph;

    // Fetch metadata for all root + discovered AUR packages
    std::set<std::string> to_fetch(roots.begin(), roots.end());
    std::map<std::string, aur::AurPackageInfo> resolved;

    while (!to_fetch.empty()) {
        auto it = to_fetch.begin();
        std::string pkg = *it;
        to_fetch.erase(it);

        if (resolved.find(pkg) != resolved.end()) continue;

        try {
            resolved[pkg] = aur::fetch(pkg);
        } catch (const source_error&) {
            // Package not in AUR — might be a virtual provide or official
            std::cerr << "warning: '" << pkg << "' not found in AUR\n";
        }
    }

    // Build dependency edges
    for (auto& [name, info] : resolved) {
        BuildNode node{name};

        // Collect deps
        auto add_dep = [&](const std::vector<std::string>& dep_list, DepKind kind) {
            for (auto& raw : dep_list) {
                std::string dep_name = parse_dep_spec(raw);

                // Check installed
                if (check_installed(dep_name)) {
                    node.non_aur_deps.push_back({dep_name, kind, DepSource::Installed});
                    continue;
                }

                // Check official repo
                if (check_official(dep_name)) {
                    node.non_aur_deps.push_back({dep_name, kind, DepSource::Official});
                    continue;
                }

                // Must be AUR
                node.aur_deps.push_back(dep_name);
                to_fetch.insert(dep_name);
            }
        };

        add_dep(info.depends, DepKind::Runtime);
        if (include_makedeps)  add_dep(info.makedepends, DepKind::Make);
        if (include_checkdeps) add_dep(info.checkdepends, DepKind::Check);

        graph.nodes[name] = std::move(node);
    }

    // Topological sort (Kahn's algorithm)
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> adj;

    for (auto& [name, node] : graph.nodes) {
        in_degree[name] = 0;
    }

    for (auto& [name, node] : graph.nodes) {
        for (auto& dep : node.aur_deps) {
            if (graph.nodes.find(dep) != graph.nodes.end()) {
                in_degree[name]++;
                adj[dep].push_back(name);
            }
        }
    }

    // Sorted queue for deterministic output
    std::vector<std::string> queue;
    for (auto& [name, deg] : in_degree) {
        if (deg == 0) queue.push_back(name);
    }
    std::sort(queue.begin(), queue.end());

    while (!queue.empty()) {
        std::sort(queue.begin(), queue.end());
        auto current = queue.front();
        queue.erase(queue.begin());
        graph.build_order.push_back(current);

        if (adj.find(current) != adj.end()) {
            for (auto& dependent : adj[current]) {
                in_degree[dependent]--;
                if (in_degree[dependent] == 0) {
                    queue.push_back(dependent);
                }
            }
        }
    }

    // Handle cycles
    if (static_cast<int>(graph.build_order.size()) < static_cast<int>(graph.nodes.size())) {
        std::vector<std::string> remaining;
        for (auto& [name, _] : graph.nodes) {
            if (!std::count(graph.build_order.begin(), graph.build_order.end(), name)) {
                remaining.push_back(name);
            }
        }
        std::cerr << "warning: circular dependency among: ";
        for (size_t i = 0; i < remaining.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << remaining[i];
        }
        std::cerr << "\n";
        for (auto& r : remaining) graph.build_order.push_back(r);
    }

    // Collect satisfied deps
    for (auto& [_, node] : graph.nodes) {
        for (auto& dep : node.non_aur_deps) {
            if (!std::any_of(graph.satisfied_deps.begin(), graph.satisfied_deps.end(),
                             [&](const ResolvedDep& d) { return d.name == dep.name; })) {
                graph.satisfied_deps.push_back(dep);
            }
        }
    }

    // Compute parallel build groups (BFS by dependency depth)
    std::map<std::string, int> depth;
    for (auto& name : graph.build_order) {
        depth[name] = 0;
    }

    for (auto& [name, node] : graph.nodes) {
        for (auto& dep : node.aur_deps) {
            if (depth.find(dep) != depth.end()) {
                depth[name] = std::max(depth[name], depth[dep] + 1);
            }
        }
    }

    // Group by depth level
    std::map<int, std::vector<std::string>> groups;
    for (auto& [name, d] : depth) {
        groups[d].push_back(name);
    }

    for (auto& [level, pkgs] : groups) {
        BuildGroup group{pkgs, level};
        graph.parallel_groups.push_back(std::move(group));
    }

    return graph;
}

std::vector<OutOfDatePkg> detect_out_of_date() {
    std::vector<OutOfDatePkg> result;

    // Get installed packages via alpm
    std::vector<alpm::Package> local_pkgs;
    try {
        local_pkgs = alpm::get_local_packages();
    } catch (...) {
        return result; // alpm not available
    }

    // Collect foreign package names (not in official repos)
    std::vector<std::string> foreign_names;
    for (auto& pkg : local_pkgs) {
        try {
            auto sync_pkg = alpm::get_sync_package(pkg.name);
            if (sync_pkg) continue; // In official repos, skip
        } catch (...) {}
        foreign_names.push_back(pkg.name);
    }

    if (foreign_names.empty()) return result;

    // Batch-fetch AUR info for all foreign packages in one API call
    // This is MUCH faster than per-package HTTP requests
    try {
        httplib::Client cli("aur.archlinux.org");
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(15));

        std::string args;
        for (size_t i = 0; i < foreign_names.size(); ++i) {
            if (i > 0) args += ",";
            args += foreign_names[i];
        }

        auto res = cli.Get("/rpc?v=5&type=info&arg=" + args);
        if (res && res->status == 200) {
            auto data = nlohmann::json::parse(res->body);
            if (data.contains("results")) {
                // Build a map of name -> AUR version
                std::map<std::string, std::string> aur_versions;
                std::map<std::string, std::string> aur_descs;

                for (auto& item : data["results"]) {
                    std::string nv = item.value("NameVersion", "");
                    auto dash_pos = nv.find('-');
                    std::string ver = (dash_pos != std::string::npos) ? nv.substr(0, dash_pos) : nv;
                    aur_versions[item.at("PackageBase").get<std::string>()] = ver;
                    if (item.contains("Description") && item.at("Description").is_string()) {
                        aur_descs[item.at("PackageBase").get<std::string>()] = item.at("Description").get<std::string>();
                    }
                }

                // Compare versions for each foreign package
                for (auto& name : foreign_names) {
                    auto it = aur_versions.find(name);
                    if (it != aur_versions.end()) {
                        std::string installed_ver;
                        for (auto& pkg : local_pkgs) {
                            if (pkg.name == name) {
                                installed_ver = pkg.version;
                                break;
                            }
                        }

                        if (!installed_ver.empty() && !it->second.empty()) {
                            if (compare_versions(it->second, installed_ver) > 0) {
                                std::string desc;
                                auto dit = aur_descs.find(name);
                                if (dit != aur_descs.end()) desc = dit->second;

                                result.push_back({name, installed_ver, it->second, desc});
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Batch fetch failed — fall back to cache-only (no network)
        std::cerr << "warning: AUR batch query failed, using cache only\n";
        for (auto& name : foreign_names) {
            if (aur_cache::has(name)) {
                auto ootd_info = aur_cache::check_ootd(name, ""); // version unknown from alpm
                if (ootd_info.is_out_of_date) {
                    std::string desc;
                    auto cached = aur_cache::get(name);
                    if (cached.has_value()) {
                        desc = cached.value().value("Description", "");
                    }
                    result.push_back({name, "", ootd_info.aur_version, desc});
                }
            }
        }
    }

    return result;
}

void print_upgrade_plan(const std::vector<OutOfDatePkg>& ootd) {
    if (ootd.empty()) {
        status::print_success("AUR packages are up to date");
        return;
    }

    status::print_section(std::to_string(ootd.size()) + " AUR package(s) have updates available");
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
    std::cout << "\n==> Dependency resolution complete.\n\n";

    if (!build_order.empty()) {
        // Show parallel build groups
        if (!parallel_groups.empty()) {
            std::cout << "Packages to build (" << build_order.size() << " total, "
                      << parallel_groups.size() << " parallel stages):\n";
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
        status::print_status("No AUR packages to build");
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
