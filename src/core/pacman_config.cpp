#include "pacmkr/core/pacman_config.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <sstream>

namespace pacmkr::pacman_config {

namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

} // namespace

Config parse(std::istream& input) {
    Config config;
    auto& result = config.options;
    std::string line, section;
    auto words = [](const std::string& value) {
        std::vector<std::string> out;
        std::istringstream stream(value);
        for (std::string word; stream >> word;) out.push_back(std::move(word));
        return out;
    };
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(std::move(line));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (!section.empty() && section != "options") {
                const auto found = std::find_if(config.repositories.begin(), config.repositories.end(),
                    [&](const Repository& repo) { return repo.name == section; });
                if (found == config.repositories.end())
                    config.repositories.push_back(Repository{section, {}, {"All"}});
            }
            continue;
        }
        const auto eq = line.find('=');
        const auto key = trim(line.substr(0, eq));
        const auto value = eq == std::string::npos ? std::string{} : trim(line.substr(eq + 1));
        if (section != "options") {
            auto found = std::find_if(config.repositories.begin(), config.repositories.end(),
                [&](const Repository& repo) { return repo.name == section; });
            if (found != config.repositories.end()) {
                if (key == "SigLevel") found->sig_level = words(value);
                else if (key == "Usage") found->usage = words(value);
            }
            continue;
        }
        if (key == "RootDir") result.root_dir = value;
        else if (key == "DBPath") result.db_path = value;
        else if (key == "CacheDir") { if (result.cache_dirs.size() == 1 && result.cache_dirs[0] == "/var/cache/pacman/pkg/") result.cache_dirs.clear(); auto v = words(value); result.cache_dirs.insert(result.cache_dirs.end(), v.begin(), v.end()); }
        else if (key == "HookDir") { if (result.hook_dirs.size() == 2) result.hook_dirs.clear(); auto v = words(value); result.hook_dirs.insert(result.hook_dirs.end(), v.begin(), v.end()); }
        else if (key == "LogFile") result.log_file = value;
        else if (key == "GPGDir") result.gpg_dir = value;
        else if (key == "Architecture") result.architecture = value;
        else if (key == "DownloadUser") result.download_user = value;
        else if (key == "ParallelDownloads") {
            try {
                const auto parsed = std::stoul(value);
                if (parsed <= std::numeric_limits<unsigned int>::max())
                    result.parallel_downloads = static_cast<unsigned int>(parsed);
            } catch (...) {}
        }
        else if (key == "CheckSpace") result.check_space = true;
        else if (key == "HoldPkg") { auto v = words(value); result.hold_packages.insert(result.hold_packages.end(), v.begin(), v.end()); }
        else if (key == "IgnorePkg") { auto v = words(value); result.ignore_packages.insert(result.ignore_packages.end(), v.begin(), v.end()); }
        else if (key == "IgnoreGroup") { auto v = words(value); result.ignore_groups.insert(result.ignore_groups.end(), v.begin(), v.end()); }
        else if (key == "NoUpgrade") { auto v = words(value); result.no_upgrade.insert(result.no_upgrade.end(), v.begin(), v.end()); }
        else if (key == "NoExtract") { auto v = words(value); result.no_extract.insert(result.no_extract.end(), v.begin(), v.end()); }
        else if (key == "SigLevel") result.sig_level = words(value);
        else if (key == "LocalFileSigLevel") result.local_file_sig_level = words(value);
        else if (key == "RemoteFileSigLevel") result.remote_file_sig_level = words(value);
    }
    return config;
}

Options parse_options(std::istream& input) {
    return parse(input).options;
}

std::vector<std::string> repository_names(std::istream& input) {
    std::vector<std::string> result;
    for (const auto& repo : parse(input).repositories) result.push_back(repo.name);
    return result;
}

} // namespace pacmkr::pacman_config
