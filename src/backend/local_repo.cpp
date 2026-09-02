#include "pacmkr/local_repo.h"
#include "pacmkr/optimize.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/wait.h>

namespace pacmkr::local_repo {
namespace {
using Registry = std::map<std::string, std::filesystem::path>;

std::filesystem::path registry_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        return std::filesystem::path{xdg} / "pacmkr" / "repositories";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path{home} / ".config" / "pacmkr" / "repositories";
    throw std::runtime_error("HOME and XDG_CONFIG_HOME are not set");
}

bool valid_name(const std::string& name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

Registry load() {
    Registry repos;
    std::ifstream in(registry_path());
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab != std::string::npos && valid_name(line.substr(0, tab)))
            repos[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return repos;
}

void save(const Registry& repos) {
    auto path = registry_path();
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write repository registry");
        for (const auto& [name, directory] : repos)
            out << name << '\t' << directory.string() << '\n';
    }
    std::filesystem::rename(temporary, path);
}

std::string quote(const std::string& value) {
    std::string result{"'"};
    for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

int execute(const std::string& tool, const std::vector<std::string>& args) {
    auto path = optimize::find_tool(tool);
    if (!path) throw std::runtime_error(tool + " is required (install pacman)");
    std::string command = quote(*path);
    for (const auto& arg : args) command += " " + quote(arg);
    int status = std::system(command.c_str());
    return status == -1 ? 1 : WEXITSTATUS(status);
}

std::filesystem::path database(const std::string& name,
                               const std::filesystem::path& directory) {
    return directory / (name + ".db.tar.gz");
}

} // namespace

void print_help() {
    std::cout <<
        "Usage:\n"
        "  pacmkr repo create <name> <directory>\n"
        "  pacmkr repo add <name> <package.pkg.tar.zst>...\n"
        "  pacmkr repo remove <name> <package-name>...\n"
        "  pacmkr repo list\n"
        "  pacmkr repo delete <name>\n";
}

int run(const std::vector<std::string>& args) {
    try {
        if (args.size() < 2 || args[1] == "help" || args[1] == "--help") {
            print_help();
            return args.size() < 2 ? 1 : 0;
        }
        auto repos = load();
        const auto& action = args[1];

        if (action == "list") {
            for (const auto& [name, directory] : repos)
                std::cout << name << "\t" << directory << "\n";
            return 0;
        }
        if (args.size() < 3 || !valid_name(args[2]))
            throw std::runtime_error("a valid repository name is required");
        const auto& name = args[2];

        if (action == "create") {
            if (args.size() != 4) throw std::runtime_error("create requires a directory");
            if (repos.count(name)) throw std::runtime_error("repository already exists: " + name);
            auto directory = std::filesystem::absolute(args[3]).lexically_normal();
            std::filesystem::create_directories(directory);
            repos[name] = directory;
            save(repos);
            std::cout << "Registered " << name << " at " << directory << "\n";
            return 0;
        }
        auto found = repos.find(name);
        if (found == repos.end()) throw std::runtime_error("unknown repository: " + name);

        if (action == "delete") {
            if (args.size() != 3) throw std::runtime_error("delete accepts only a name");
            repos.erase(found);
            save(repos);
            std::cout << "Unregistered " << name << " (files were preserved)\n";
            return 0;
        }
        if ((action == "add" || action == "remove") && args.size() < 4)
            throw std::runtime_error(action + " requires at least one package");

        if (action == "add") {
            std::vector<std::string> tool_args{database(name, found->second).string()};
            for (auto it = args.begin() + 3; it != args.end(); ++it) {
                auto source = std::filesystem::absolute(*it).lexically_normal();
                if (!std::filesystem::is_regular_file(source))
                    throw std::runtime_error("package file not found: " + source.string());
                if (source.filename().string().find(".pkg.tar.") == std::string::npos)
                    throw std::runtime_error("not an Arch package archive: " + source.string());
                auto destination = found->second / source.filename();
                if (source != destination)
                    std::filesystem::copy_file(source, destination,
                                               std::filesystem::copy_options::overwrite_existing);
                tool_args.push_back(destination.string());
                auto signature = source;
                signature += ".sig";
                auto destination_signature = std::filesystem::path{destination.string() + ".sig"};
                if (std::filesystem::is_regular_file(signature) &&
                    signature != destination_signature)
                    std::filesystem::copy_file(signature, destination_signature,
                                               std::filesystem::copy_options::overwrite_existing);
            }
            return execute("repo-add", tool_args);
        }
        if (action == "remove") {
            std::vector<std::string> tool_args{database(name, found->second).string()};
            tool_args.insert(tool_args.end(), args.begin() + 3, args.end());
            return execute("repo-remove", tool_args);
        }
        throw std::runtime_error("unknown repo action: " + action);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}

} // namespace pacmkr::local_repo
