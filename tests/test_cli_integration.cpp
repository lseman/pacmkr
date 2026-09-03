#include "pacmkr/backend/alpm.h"
#include "pacmkr/core/package.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static int run(const fs::path& config, const fs::path& cache,
               const std::vector<std::string>& arguments) {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        setenv("XDG_CACHE_HOME", cache.c_str(), 1);
        std::vector<std::string> storage{PACMKR_TEST_EXECUTABLE, "--config", config.string()};
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(PACMKR_TEST_EXECUTABLE, argv.data());
        _exit(126);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

int main() {
    const auto id = std::to_string(getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path base = fs::temp_directory_path() / ("pacmkr-cli-test-" + id);
    const fs::path root = base / "root";
    const fs::path db = base / "db";
    const fs::path package_dir = base / "package";
    const fs::path config = base / "pacman.conf";
    fs::create_directories(root);
    fs::create_directories(db);
    fs::create_directories(package_dir / "usr/share/pacmkr-cli-test");
    std::ofstream(package_dir / "usr/share/pacmkr-cli-test/owned") << "cli\n";
    std::ofstream metadata(package_dir / ".PKGINFO");
    metadata << "pkgname = pacmkr-cli-test\npkgbase = pacmkr-cli-test\npkgver = 1.0-1\n"
                "pkgdesc = isolated CLI fixture\nbuilddate = 1\npackager = tests\nsize = 4\narch = any\n";
    metadata.close();
    std::ofstream conf(config);
    conf << "[options]\nRootDir = " << root.string() << "\nDBPath = " << db.string()
         << "\nCacheDir = " << (base / "cache").string() << "\nLogFile = " << (base / "pacman.log").string()
         << "\nGPGDir = " << (base / "gnupg").string() << "\nSigLevel = Never\nLocalFileSigLevel = Never\n";
    conf.close();

    auto archive = pacmkr::package::Package::make("pacmkr-cli-test", "1.0-1", "any", base);
    archive.create_archive(package_dir);
    pacmkr::alpm::init(root.string(), db.string(), config.string());
    assert(pacmkr::alpm::install_files({archive.dest.string()}, true) == 0);
    pacmkr::alpm::shutdown();

    assert(run(config, base / "xdg", {"-Q", "pacmkr-cli-test"}) == 0);
    assert(run(config, base / "xdg", {"-Qo", "/usr/share/pacmkr-cli-test/owned"}) == 0);
    assert(run(config, base / "xdg", {"-T", "pacmkr-cli-test"}) == 0);
    assert(run(config, base / "xdg", {"-T", "definitely-missing>=1"}) == 127);
    assert(run(config, base / "xdg", {"-D", "--asdeps", "pacmkr-cli-test"}) == 0);
    assert(run(config, base / "xdg", {"-Qdt"}) == 0);
    assert(run(config, base / "xdg", {"-Qp", archive.dest.string()}) == 0);
    assert(run(config, base / "xdg", {"-Qip", archive.dest.string()}) == 0);
    assert(run(config, base / "xdg", {"-Qlp", archive.dest.string()}) == 0);
    assert(run(config, base / "xdg", {"-Qp", (base / "not-here.pkg.tar").string()}) == 1);
    assert(run(config, base / "xdg", {"-Sc", "--noconfirm"}) == 0);
    assert(run(config, base / "xdg", {"-Sw", "--noconfirm"}) == 1);   // no targets
    assert(run(config, base / "xdg", {"-Suw", "--noconfirm"}) == 0);  // nothing pending
    assert(run(config, base / "xdg", {"-R", "--noconfirm", "pacmkr-cli-test"}) == 0);
    assert(run(config, base / "xdg", {"-Q", "pacmkr-cli-test"}) == 1);

    fs::remove_all(base);
    std::cout << "isolated native CLI operations passed\n";
}
