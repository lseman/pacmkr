#include "pacmkr/backend/local_repo.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const auto id = std::to_string(getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path base = fs::temp_directory_path() / ("pacmkr-repo-test-" + id);
    const fs::path config_home = base / "config";
    const fs::path repository = base / "packages";
    fs::create_directories(config_home);
    assert(setenv("XDG_CONFIG_HOME", config_home.c_str(), 1) == 0);

    assert(pacmkr::local_repo::run({"create", "custom", repository.string()}) == 0);
    assert(fs::is_directory(repository));

    const fs::path registry = config_home / "pacmkr" / "repositories";
    std::ifstream input(registry);
    std::string entry;
    assert(std::getline(input, entry));
    assert(entry == "custom\t" + fs::absolute(repository).lexically_normal().string());

    assert(pacmkr::local_repo::run({"list"}) == 0);
    assert(pacmkr::local_repo::run({"create", "custom", repository.string()}) == 1);
    assert(pacmkr::local_repo::run({"delete", "custom"}) == 0);
    assert(fs::is_directory(repository));
    assert(pacmkr::local_repo::run({"delete", "custom"}) == 1);

    fs::remove_all(base);
    std::cout << "local repository commands passed\n";
}
