#pragma once

#include <istream>
#include <string>
#include <vector>

namespace pacmkr::pacman_config {

struct Options {
    std::string root_dir{"/"};
    std::string db_path{"/var/lib/pacman/"};
    std::vector<std::string> cache_dirs{"/var/cache/pacman/pkg/"};
    std::vector<std::string> hook_dirs{"/etc/pacman.d/hooks/", "/usr/share/libalpm/hooks/"};
    std::string log_file{"/var/log/pacman.log"};
    std::string gpg_dir{"/etc/pacman.d/gnupg/"};
    std::string architecture{"auto"};
    std::string download_user;
    unsigned int parallel_downloads{1};
    bool check_space{false};
    std::vector<std::string> hold_packages;
    std::vector<std::string> ignore_packages;
    std::vector<std::string> ignore_groups;
    std::vector<std::string> no_upgrade;
    std::vector<std::string> no_extract;
    std::vector<std::string> sig_level{"Required", "DatabaseOptional"};
    std::vector<std::string> local_file_sig_level{"Optional"};
    std::vector<std::string> remote_file_sig_level{"Required"};
};

struct Repository {
    std::string name;
    std::vector<std::string> sig_level;
    std::vector<std::string> usage{"All"};
};

struct Config {
    Options options;
    std::vector<Repository> repositories;
};

/// Parse the supported pacman.conf options and repository policy.
Config parse(std::istream& input);

/// Parse global [options]. Repeated list directives are accumulated.
Options parse_options(std::istream& input);

/// Parse repository section names from pacman.conf content.
/// The special [options] section is not a repository.
std::vector<std::string> repository_names(std::istream& input);

} // namespace pacmkr::pacman_config
