#include "pacmkr/aur.h"
#include "pacmkr/alpm.h"
#include "pacmkr/error.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>

namespace pacmkr::aur {


namespace {

const char* AUR_API_BASE = "https://aur.archlinux.org/rpc?v=5";

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

std::string encode(const std::string& s) {
    // Simple URL encoding — only encode special characters
    static const std::string safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.";
    std::string result;
    for (char c : s) {
        if (safe.find(c) != std::string::npos) {
            result += c;
        } else {
            result += '%';
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned char>(c));
            result += hex;
        }
    }
    return result;
}

AurPackageInfo info_from_json(const nlohmann::json& obj) {
    AurPackageInfo info{};

    auto get_str = [&obj](const std::string& key) -> std::optional<std::string> {
        if (obj.is_object() && obj.find(key) != obj.end() && obj[key].is_string()) return obj[key].get<std::string>();
        return {};
    };

    auto get_vec = [&obj](const std::string& key) -> std::vector<std::string> {
        if (!obj.is_object() && obj.find(key) != obj.end() || !obj[key].is_array()) return {};
        std::vector<std::string> result;
        for (auto& item : obj[key]) {
            if (item.is_string()) result.push_back(item.get<std::string>());
        }
        return result;
    };

    info.name          = get_or(obj["Name"], std::string{});
    info.pkgname       = get_or(obj["PackageBase"], std::string{});
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
    pkg.outofdate    = get_optional<unsigned long long>(obj["OutOfDate"]);
    pkg.firstsubmitted = get_optional<unsigned long long>(obj["FirstSubmitted"]);
    pkg.lastmodified = get_or(obj["LastModified"], 0ULL);
    return pkg;
}

/// Make an HTTP GET request to the AUR API with retries.
nlohmann::json aur_get(const std::string& path) {
    static httplib::Client cli(AUR_API_BASE);
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(15));

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

std::filesystem::path clone_aur_repo(const std::string& pkgname,
                                      const std::filesystem::path& dest) {
    auto pkgdir = dest / pkgname;

    if (std::filesystem::exists(pkgdir)) {
        std::cout << "==> Using existing directory: " << pkgdir.string() << "\n";
    } else {
        std::cout << "==> Cloning AUR repository for " << pkgname << "...\n";
        std::ostringstream cmd;
        cmd << "git clone --depth 1 \"https://aur.archlinux.org/" << pkgname << ".git\" \""
            << pkgdir.string() << "\" 2>&1";

        int rc = std::system(cmd.str().c_str());
        if (rc != 0) {
            throw source_error("Failed to clone AUR repository for " + pkgname);
        }
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
    auto data = aur_get("/search?type=byname&arg=" + encode(query));

    if (!data.contains("results")) return {};

    std::vector<AurPackage> results;
    for (auto& item : data["results"]) {
        results.push_back(pkg_from_json(item));
    }

    // Sort by votes descending
    std::sort(results.begin(), results.end(),
              [](const AurPackage& a, const AurPackage& b) {
                  return a.numvotes > b.numvotes;
              });

    return results;
}

AurPackageInfo fetch(const std::string& pkgname) {
    auto data = aur_get("/info?arg=" + encode(pkgname));

    if (!data.contains("results") || data["results"].empty()) {
        throw source_error("Package '" + pkgname + "' not found in AUR");
    }

    return info_from_json(data["results"][0]);
}

std::filesystem::path download_pkgbuild(const std::string& pkgname,
                                         const std::filesystem::path& dest) {
    // Ensure destination exists
    std::filesystem::create_directories(dest);
    return clone_aur_repo(pkgname, dest);
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

void search_aur(const std::string& query, unsigned int limit) {
    auto results = search(query);

    if (results.empty()) {
        std::cout << "No results found for '" << query << "'\n";
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
                      << " — " << desc << " (votes: " << results[i].numvotes << ")\n";
        }

        std::cout << "\nSelect: ";
        std::string input;
        std::getline(std::cin, input);

        try {
            unsigned int idx = std::stoul(input) - 1;
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
                      << " — " << desc << " (votes: " << results[i].numvotes << ")\n";
        }
        std::cout << "\nTo build a package: pacmkr --aur <package-name>\n";
    }
}

void hybrid_sync_search(const std::string& query, unsigned int limit) {
    // Query official repos
    std::ostringstream repo_cmd;
    repo_cmd << "pacman -Ss \"" << query << "\" 2>/dev/null";
    FILE* repo_pipe = popen(repo_cmd.str().c_str(), "r");
    std::string repo_results;
    if (repo_pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), repo_pipe)) {
            repo_results += buf;
        }
        pclose(repo_pipe);
    }

    // Query AUR
    std::vector<AurPackage> aur_results;
    try {
        aur_results = search(query);
    } catch (...) {
        // AUR query failed — still show repo results
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
        std::cout << repo_results;
    }

    if (has_aur) {
        if (has_repo) std::cout << "\n";
        std::cout << "==> AUR results:\n";
        for (unsigned int i = 0; i < aur_count; ++i) {
            auto desc = aur_results[i].desc.value_or("No description");
            if (desc.size() > 60) desc = desc.substr(0, 57) + "...";
            std::cout << "  aur/" << aur_results[i].name
                      << " — " << desc << " (votes: " << aur_results[i].numvotes << ")\n";
        }
    }
}

void hybrid_sync_info(const std::string& pkgname) {
    // Try official repos first
    std::ostringstream repo_cmd;
    repo_cmd << "pacman -Si \"" << pkgname << "\" 2>/dev/null";
    FILE* repo_pipe = popen(repo_cmd.str().c_str(), "r");
    if (repo_pipe) {
        char buf[256];
        bool has_output = false;
        while (fgets(buf, sizeof(buf), repo_pipe)) {
            has_output = true;
            std::cout << buf;
        }
        int rc = pclose(repo_pipe);
        if (has_output && WEXITSTATUS(rc) == 0) {
            return; // Found in official repos
        }
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
