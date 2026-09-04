#include "pacmkr/backend/aur.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/core/error.h"
#include "pacmkr/core/fuzzy_search.h"
#include "pacmkr/app/terminal.h"
#include "pacmkr/app/json_output.h"

// json.hpp is included via json_output.h; just alias it here.
using json = nlohmann::json;

#include <httplib.h>
#include <json.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace pacmkr::aur {


namespace {

const char* AUR_API_ORIGIN = "https://aur.archlinux.org";

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) return 127;
    const pid_t child = fork();
    if (child < 0) return 127;
    if (child == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/// Read a JSON value with a safe fallback.
template<typename T>
T get_or(const nlohmann::json& v, const T& default_val) {
    if (v.is_null()) return default_val;
    try { return v.get<T>(); } catch (...) { return default_val; }
}

/// Specialization for std::optional — unwrap only if not null.
template<typename T>
std::optional<T> get_optional(const nlohmann::json& v) {
    if (v.is_null()) return std::nullopt;
    try { return v.get<T>(); } catch (...) { return std::nullopt; }
}

AurPackageInfo info_from_json(const nlohmann::json& obj) {
    AurPackageInfo info{};

    auto get_str = [&obj](const std::string& key) -> std::optional<std::string> {
        if (obj.is_object() && obj.find(key) != obj.end() && obj[key].is_string()) return obj[key].get<std::string>();
        return {};
    };

    auto get_vec = [&obj](const std::string& key) -> std::vector<std::string> {
        if (!obj.is_object()) return {};
        const auto value = obj.find(key);
        if (value == obj.end() || !value->is_array()) return {};
        std::vector<std::string> result;
        for (const auto& item : *value) {
            if (item.is_string()) result.push_back(item.get<std::string>());
        }
        return result;
    };

    info.name          = get_or(obj["Name"], std::string{});
    info.pkgname       = get_or(obj["PackageBase"], std::string{});
    info.version       = get_or(obj["Version"], std::string{});
    info.desc          = get_str("Description");
    info.url           = get_str("URL");
    info.license       = get_vec("License");
    info.depends       = get_vec("Depends");
    info.makedepends   = get_vec("MakeDepends");
    info.checkdepends  = get_vec("CheckDepends");
    info.optdepends    = get_vec("OptDepends");
    info.provides      = get_vec("Provides");
    info.conflicts     = get_vec("Conflicts");
    info.replaces      = get_vec("Replaces");
    info.backup        = get_vec("Backup");
    info.options       = get_vec("Options");
    info.source        = get_vec("Source");
    info.sha256sums    = get_vec("Sha256sums");
    info.md5sums       = get_vec("Md5sums");
    info.sha1sums      = get_vec("Sha1sums");
    info.sha384sums    = get_vec("Sha384sums");
    info.sha512sums    = get_vec("Sha512sums");
    info.subpackages   = get_vec("SubPackages");

    return info;
}

AurPackage pkg_from_json(const nlohmann::json& obj) {
    AurPackage pkg{};
    pkg.name         = get_or(obj["Name"], std::string{});
    pkg.pkgname      = get_or(obj["PackageBase"], std::string{});
    pkg.desc         = get_optional<std::string>(obj["Description"]);
    pkg.url          = get_optional<std::string>(obj["URL"]);
    pkg.numvotes     = get_or(obj["NumVotes"], 0u);
    pkg.popularity   = obj.contains("Popularity") && obj["Popularity"].is_number() ? obj["Popularity"].get<double>() : 0.0;
    pkg.outofdate    = get_optional<unsigned long long>(obj["OutOfDate"]);
    pkg.firstsubmitted = get_optional<unsigned long long>(obj["FirstSubmitted"]);
    pkg.lastmodified = get_or(obj["LastModified"], 0ULL);
    return pkg;
}

/// Make an HTTP GET request to the AUR API with retries.
nlohmann::json aur_get(const std::string& path) {
    static httplib::Client cli(AUR_API_ORIGIN);
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(15));
    cli.set_write_timeout(std::chrono::seconds(5));

    constexpr int max_retries = 3;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        auto res = cli.Get(path.c_str());

        if (res && res->status == 200) {
            try {
                return nlohmann::json::parse(res->body);
            } catch (const nlohmann::json::exception& e) {
                throw source_error("Failed to parse AUR response: " + std::string(e.what()));
            }
        }

        if (res && res->status == 429) {
            // Rate limited — wait and retry
            std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
            continue;
        }

        if (attempt < max_retries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        auto status = res ? std::to_string(res->status) : "connection failed";
        throw source_error("AUR request failed with status " + status);
    }

    throw source_error("AUR request failed after retries");
}

/// Path to the shared bare mirror of the AUR Git repository.
static std::filesystem::path get_aur_mirror_path() {
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache && std::string(xdg_cache).size() > 0)
        return std::filesystem::path{xdg_cache} / "pacmkr" / "aur.git";
    const char* home = std::getenv("HOME");
    if (!home) return std::filesystem::temp_directory_path() / "pacmkr" / "aur.git";
    return std::filesystem::path{home} / ".cache" / "pacmkr" / "aur.git";
}

/// Initialize or update the shared bare AUR mirror.
static void ensure_aur_mirror() {
    auto mirror_path = get_aur_mirror_path();
    std::filesystem::create_directories(mirror_path);

    const bool exists = std::filesystem::exists(mirror_path / ".git");
    if (!exists) {
        terminal::info("Initializing AUR Git mirror...");
        if (run_process({"git", "init", "--bare", mirror_path.string()}) != 0)
            throw source_error("Failed to initialize AUR Git mirror");
    }

    // Update the bare mirror from AUR.
    // If this is a fresh clone, use --mirror for full fetch; otherwise just fetch.
    const bool was_empty = exists && std::filesystem::is_empty(mirror_path);
    if (was_empty) {
        terminal::info("Cloning AUR Git mirror (this may take a while)...");
        if (run_process({"git", "clone", "--mirror", "--quiet",
                         "https://aur.archlinux.org/aur.git",
                         mirror_path.string()}) != 0)
            throw source_error("Failed to clone AUR Git mirror");
    } else {
        terminal::info("Updating AUR Git mirror...");
        if (run_process({"git", "-C", mirror_path.string(), "fetch", "--prune", "--quiet",
                         "https://aur.archlinux.org/aur.git", "+refs/*:refs/*"}) != 0) {
            // Mirror update failure is non-fatal — we can still checkout from it
            std::cerr << "warning: AUR Git mirror update failed, using stale mirror\n";
        }
    }
}

/// Checkout a specific package branch from the bare mirror into a working directory.
static std::filesystem::path checkout_from_mirror(const std::string& pkgname,
                                                   const std::filesystem::path& dest) {
    auto mirror_path = get_aur_mirror_path();
    auto pkgdir = dest / pkgname;

    // If we already have this package, fast-forward from the mirror.
    if (std::filesystem::exists(pkgdir)) {
        if (!std::filesystem::is_directory(pkgdir / ".git"))
            throw source_error("Existing path is not an AUR Git repository: " + pkgdir.string());
        terminal::info("Updating " + pkgname);
        // Update remote to point at the mirror, then fetch and merge.
        if (run_process({"git", "-C", pkgdir.string(), "remote", "set-url",
                         "origin", mirror_path.string()}) != 0)
            throw source_error("Failed to update AUR repository origin");
        if (run_process({"git", "-C", pkgdir.string(), "pull", "--ff-only"}) != 0) {
            // If fast-forward fails, do a fresh checkout from mirror.
            std::filesystem::remove_all(pkgdir);
            goto fresh_checkout;
        }
    } else {
fresh_checkout:
        terminal::info("Cloning " + pkgname);
        // Clone from the bare mirror (local operation, very fast).
        if (run_process({"git", "clone", "--depth", "1",
                         "file://" + mirror_path.string(),
                         pkgdir.string()}) != 0)
            throw source_error("Failed to clone AUR repository for " + pkgname);
    }

    auto pkgbuild_path = pkgdir / "PKGBUILD";
    if (!std::filesystem::exists(pkgbuild_path)) {
        throw source_error("No PKGBUILD found in AUR package '" + pkgname + "'");
    }
    return pkgbuild_path;
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

std::vector<AurPackage> search(const std::string& query) {
    auto data = aur_get("/rpc?v=5&type=search&by=name-desc&arg=" + detail::url_encode(query));

    if (!data.contains("results")) return {};

    std::vector<AurPackage> results;
    for (auto& item : data["results"]) {
        results.push_back(pkg_from_json(item));
    }

    // Apply fuzzy scoring to rank by relevance to query
    for (auto& pkg : results) {
        std::string text = pkg.name;
        if (pkg.desc.has_value()) {
            text += " " + *pkg.desc;
        }
        auto score = fuzzy::score_match(query, text);
        pkg.relevance_score = score.score;
    }

    // Sort by relevance (primary), then popularity, then votes
    // Relevance handles typos and partial matches; popularity/votes break ties
    std::sort(results.begin(), results.end(),
              [](const AurPackage& a, const AurPackage& b) {
                  if (a.relevance_score != b.relevance_score)
                      return a.relevance_score > b.relevance_score;
                  if (a.popularity != b.popularity)
                      return a.popularity > b.popularity;
                  return a.numvotes > b.numvotes;
              });

    return results;
}

AurPackageInfo fetch(const std::string& pkgname) {
    auto data = aur_get("/rpc?v=5&type=info&arg[]=" + detail::url_encode(pkgname));

    if (!data.contains("results") || data["results"].empty()) {
        throw source_error("Package '" + pkgname + "' not found in AUR");
    }

    return info_from_json(data["results"][0]);
}

// Maximum package names per AUR RPC request. The AUR rejects over-long URLs
// (roughly 8 KB); 150 names keeps every request comfortably under that even
// with long package names, and matches what other helpers use.
static constexpr size_t kRpcBatchSize = 150;

std::vector<AurPackageInfo> fetch_batch(const std::vector<std::string>& pkgnames) {
    if (pkgnames.empty()) return {};

    std::vector<AurPackageInfo> results;
    results.reserve(pkgnames.size());
    bool any_ok = false;
    std::string last_error;

    for (size_t offset = 0; offset < pkgnames.size(); offset += kRpcBatchSize) {
        const size_t end = std::min(offset + kRpcBatchSize, pkgnames.size());

        // Build chunk path: /rpc?v=5&type=info&arg[]=foo&arg[]=bar...
        std::string path = "/rpc?v=5&type=info";
        for (size_t i = offset; i < end; ++i) {
            path += "&arg[]=" + detail::url_encode(pkgnames[i]);
        }

        nlohmann::json data;
        try {
            data = aur_get(path);
        } catch (const source_error& e) {
            last_error = e.what();
            continue;  // keep going — a later chunk may still succeed
        }
        any_ok = true;

        if (!data.contains("results") || !data["results"].is_array()) continue;
        for (auto& item : data["results"]) {
            if (item.contains("Name") && item["Name"].is_string()) {
                results.push_back(info_from_json(item));
            }
        }
    }

    // Only fail outright when every chunk failed; a partial result is still
    // useful for dependency resolution.
    if (!any_ok) {
        throw source_error(last_error.empty() ? "AUR batch info request failed"
                                              : last_error);
    }
    return results;
}

std::vector<std::string> providers(const std::string& dep) {
    std::string name = dep;
    for (const char sep : {'>', '<', '='}) {
        if (const auto pos = name.find(sep); pos != std::string::npos) {
            name = name.substr(0, pos);
        }
    }
    if (name.empty()) return {};

    nlohmann::json data;
    try {
        data = aur_get("/rpc?v=5&type=search&by=provides&arg=" + detail::url_encode(name));
    } catch (const source_error&) {
        return {};
    }
    if (!data.contains("results") || !data["results"].is_array()) return {};

    struct Candidate { std::string base; unsigned int votes; bool exact; };
    std::vector<Candidate> candidates;
    for (auto& item : data["results"]) {
        auto pkg = pkg_from_json(item);
        const std::string& base = pkg.pkgname.empty() ? pkg.name : pkg.pkgname;
        if (base.empty()) continue;
        candidates.push_back({base, pkg.numvotes, pkg.name == name});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.exact != b.exact) return a.exact;
                  return a.votes > b.votes;
              });

    std::vector<std::string> bases;
    for (const auto& candidate : candidates) {
        if (std::find(bases.begin(), bases.end(), candidate.base) == bases.end()) {
            bases.push_back(candidate.base);
        }
    }
    return bases;
}

std::filesystem::path download_pkgbuild(const std::string& pkgname,
                                         const std::filesystem::path& dest) {
    // Initialize the shared bare mirror (first time or update).
    ensure_aur_mirror();
    
    // Ensure destination exists
    std::filesystem::create_directories(dest);
    return checkout_from_mirror(pkgname, dest);
}

bool is_official_package(const std::string& pkgname) {
    try {
        auto pkg = alpm::get_sync_package(pkgname);
        return pkg.has_value();
    } catch (...) {
        return false;
    }
}

bool is_aur_package(const std::string& pkgname) {
    try {
        fetch(pkgname);
        return true;
    } catch (...) {
        return false;
    }
}

void search_aur(const std::string& query, unsigned int limit, bool json_mode) {
    auto results = search(query);

    if (results.empty()) {
        if (json_mode) {
            json j;
            j["query"] = query;
            j["results"] = json::array();
            std::cout << json_output::serialize(j) << "\n";
        } else {
            std::cout << "No results found for '" << query << "'\n";
        }
        return;
    }

    // JSON mode: output structured data and skip interactive picker
    if (json_mode) {
        json j;
        j["query"] = query;
        j["results"] = json_output::aur_search_results_to_json(results);
        std::cout << json_output::serialize(j) << "\n";
        return;
    }

    // Check if we're in TTY mode
    bool is_tty = isatty(STDOUT_FILENO) != 0;

    if (is_tty && !results.empty()) {
        // Interactive picker
        std::cout << "\n==> Searching AUR for '" << query << "':\n";
        std::cout << "     Type a number to select, or Esc to cancel\n\n";

        unsigned int count = std::min(limit, static_cast<unsigned int>(results.size()));
        for (unsigned int i = 0; i < count; ++i) {
            auto desc = results[i].desc.value_or("No description");
            if (desc.size() > 60) desc = desc.substr(0, 57) + "...";
            std::cout << "  " << (i + 1) << ". " << results[i].name
                      << " — " << desc << " (votes: " << results[i].numvotes
                      << ", pop: " << results[i].popularity
                      << ", rel: " << results[i].relevance_score << ")\n";
        }

        std::cout << "\nSelect: ";
        std::string input;
        std::getline(std::cin, input);

        try {
            const size_t idx = std::stoul(input) - 1;
            if (idx < count) {
                auto& pkg = results[idx];
                std::cout << "\n==> Selected: " << pkg.name << "\n";
                std::cout << "     PKGBUILD will be downloaded.\n";
                std::cout << "     To build: pacmkr --aur " << pkg.name << "\n";
            } else {
                std::cout << "Invalid selection.\n";
            }
        } catch (...) {
            std::cout << "No package selected.\n";
        }
    } else {
        // List mode
        std::cout << "\n" << results.size() << " result(s) for '" << query << "':\n\n";
        unsigned int count = std::min(limit, static_cast<unsigned int>(results.size()));
        for (unsigned int i = 0; i < count; ++i) {
            auto desc = results[i].desc.value_or("No description");
            if (desc.size() > 60) desc = desc.substr(0, 57) + "...";
            std::cout << "  " << (i + 1) << ". " << results[i].name
                      << " — " << desc << " (votes: " << results[i].numvotes
                      << ", pop: " << results[i].popularity
                      << ", rel: " << results[i].relevance_score << ")\n";
        }
        std::cout << "\nTo build a package: pacmkr --aur <package-name>\n";
    }
}

void hybrid_sync_search(const std::string& query, unsigned int limit, bool json_mode) {
    auto repo_results = alpm::search_sync(query);

    // Query AUR
    std::vector<AurPackage> aur_results;
    try {
        aur_results = search(query);
    } catch (...) {
        // AUR query failed — still show repo results
    }

    // JSON mode: output structured data
    if (json_mode) {
        json j = json_output::hybrid_search_to_json(repo_results, aur_results);
        j["query"] = query;
        std::cout << json_output::serialize(j) << "\n";
        return;
    }

    // Display combined results
    bool has_repo = !repo_results.empty();
    bool has_aur = !aur_results.empty();

    if (!has_repo && !has_aur) {
        std::cout << "No results found for '" << query << "'\n";
        return;
    }

    unsigned int aur_count = std::min(limit, static_cast<unsigned int>(aur_results.size()));

    if (has_repo) {
        for (const auto& pkg : repo_results) {
            std::cout << pkg.origin_db << "/" << pkg.name << " " << pkg.version << "\n"
                      << "    " << pkg.desc << "\n";
        }
    }

    if (has_aur) {
        if (has_repo) std::cout << "\n";
        std::cout << "==> AUR results:\n";
        for (unsigned int i = 0; i < aur_count; ++i) {
            auto desc = aur_results[i].desc.value_or("No description");
            if (desc.size() > 60) desc = desc.substr(0, 57) + "...";
            std::cout << "  aur/" << aur_results[i].name
                      << " — " << desc << " (votes: " << aur_results[i].numvotes
                      << ", pop: " << aur_results[i].popularity
                      << ", rel: " << aur_results[i].relevance_score << ")\n";
        }
    }
}

void hybrid_sync_info(const std::string& pkgname, bool json_mode) {
    // JSON mode
    if (json_mode) {
        json j;
        j["name"] = pkgname;
        auto repo_pkg = alpm::get_sync_package(pkgname);
        if (repo_pkg) {
            j["source"] = "repository";
            j["package"] = json_output::alpm_package_to_json(*repo_pkg);
            std::cout << json_output::serialize(j) << "\n";
            return;
        }
        // Try AUR
        try {
            auto info = fetch(pkgname);
            j["source"] = "aur";
            j["package"] = json_output::aur_package_info_to_json(info);
            std::cout << json_output::serialize(j) << "\n";
            return;
        } catch (const source_error&) {
            j["source"] = "not_found";
            j["error"] = "package not found in repos or AUR";
            std::cout << json_output::serialize(j) << "\n";
            std::cerr << "error: package '" << pkgname << "' not found in repos or AUR\n";
        }
        return;
    }

    // Text mode
    if (auto pkg = alpm::get_sync_package(pkgname)) {
        std::cout << "Repository      : " << pkg->origin_db << "\n"
                  << "Name            : " << pkg->name << "\n"
                  << "Version         : " << pkg->version << "\n"
                  << "Description     : " << pkg->desc << "\n"
                  << "Architecture    : " << pkg->arch << "\n"
                  << "URL             : " << pkg->url << "\n";
        return;
    }

    // Try AUR
    try {
        auto info = fetch(pkgname);
        std::cout << "     Name          : " << info.name << "\n";
        std::cout << "     Description   : " << (info.desc.value_or("N/A")) << "\n";
        std::cout << "     URL           : " << (info.url.value_or("N/A")) << "\n";
        std::cout << "     License       : ";
        for (size_t i = 0; i < info.license.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << info.license[i];
        }
        std::cout << "\n";
        std::cout << "     Dependencies  : ";
        for (size_t i = 0; i < info.depends.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << info.depends[i];
        }
        std::cout << "\n";
        std::cout << "     Make Deps     : ";
        for (size_t i = 0; i < info.makedepends.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << info.makedepends[i];
        }
        std::cout << "\n";
        std::cout << "     Check Deps    : ";
        for (size_t i = 0; i < info.checkdepends.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << info.checkdepends[i];
        }
        std::cout << "\n";
        std::cout << "     AUR Package   : " << info.name << "\n";
    } catch (const source_error& e) {
        std::cerr << "error: package '" << pkgname << "' not found in repos or AUR\n";
    }
}

} // namespace pacmkr::aur
