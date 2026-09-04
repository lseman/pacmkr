#include "pacmkr/backend/alpm.h"
#include "pacmkr/core/package.h"

#include <cassert>
#include <array>
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
        setenv("SOURCE_DATE_EPOCH", "1700000000", 1);
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

static std::string archive_member(const fs::path& archive, const std::string& member) {
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDOUT_FILENO) >= 0);
        close(descriptors[1]);
        execlp("bsdtar", "bsdtar", "-xOf", archive.c_str(), member.c_str(),
               static_cast<char*>(nullptr));
        _exit(126);
    }
    close(descriptors[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    for (ssize_t count; (count = read(descriptors[0], buffer.data(), buffer.size())) > 0;)
        output.append(buffer.data(), static_cast<size_t>(count));
    close(descriptors[0]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return output;
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

    // Native split-package build: package_*() metadata must be captured after
    // each function, and build environment overrides must reach every phase.
    const fs::path build_dir = base / "native-split-build";
    fs::create_directories(build_dir);
    std::ofstream(build_dir / "first.install") << "post_install() { :; }\n";
    std::ofstream(build_dir / "second.changelog") << "native split changelog\n";
    std::ofstream(build_dir / "PKGBUILD") << R"PKGBUILD(
pkgbase=pacmkr-native-split
pkgname=(pacmkr-native-first pacmkr-native-second)
pkgver=2.0
pkgrel=3
pkgdesc='global description'
arch=('any')
license=('MIT')
source=()
sha256sums=()

check() {
    [[ $PACMKR_TEST_FLAG == works ]]
}
package_pacmkr-native-first() {
    [[ $pkgname == pacmkr-native-first ]]
    pkgdesc='first split package'
    depends=('glibc')
    install=first.install
    install -Dm644 /dev/null "$pkgdir/usr/share/pacmkr-native/first"
}
package_pacmkr-native-second() {
    [[ $pkgname == pacmkr-native-second ]]
    pkgdesc='second split package'
    depends=('pacmkr-native-first=2.0-3')
    changelog=second.changelog
    install -Dm644 /dev/null "$pkgdir/usr/share/pacmkr-native/second"
}
)PKGBUILD";
    const std::vector<std::string> native_build_arguments{
        "--build", "--dir", build_dir.string(), "--check", "--skipchecksums",
        "--lto=no", "--mold=no", "--graphite=no", "--polly=no", "--",
        "PACMKR_TEST_FLAG=works"};
    assert(run(config, base / "xdg", native_build_arguments) == 0);

    const auto first_archive = build_dir / "pacmkr-native-first-2.0-3-any.pkg.tar.zst";
    const auto second_archive = build_dir / "pacmkr-native-second-2.0-3-any.pkg.tar.zst";
    assert(fs::is_regular_file(first_archive));
    assert(fs::is_regular_file(second_archive));
    std::ifstream first_bytes_input(first_archive, std::ios::binary);
    const std::string first_bytes((std::istreambuf_iterator<char>(first_bytes_input)), {});
    assert(run(config, base / "xdg", native_build_arguments) == 0);
    std::ifstream rebuilt_bytes_input(first_archive, std::ios::binary);
    const std::string rebuilt_bytes((std::istreambuf_iterator<char>(rebuilt_bytes_input)), {});
    assert(first_bytes == rebuilt_bytes);
    assert(fs::is_regular_file(build_dir / "pkg/.CHANGELOG"));
    pacmkr::alpm::init(root.string(), db.string(), config.string());
    const auto first_split = pacmkr::alpm::load_package_file(first_archive.string());
    const auto second_split = pacmkr::alpm::load_package_file(second_archive.string());
    assert(first_split.desc == "first split package");
    assert((first_split.depends == std::vector<std::string>{"glibc"}));
    assert(second_split.desc == "second split package");
    assert((second_split.depends ==
            std::vector<std::string>{"pacmkr-native-first=2.0-3"}));
    pacmkr::alpm::shutdown();
    assert(archive_member(first_archive, ".INSTALL").find("post_install") !=
           std::string::npos);
    assert(archive_member(second_archive, ".CHANGELOG") == "native split changelog\n");
    assert(archive_member(first_archive, ".BUILDINFO").find("buildtool = pacmkr\n") !=
           std::string::npos);
    assert(!archive_member(first_archive, ".MTREE").empty());

    assert(run(config, base / "xdg", {"-Sc", "--noconfirm"}) == 0);
    assert(run(config, base / "xdg", {"-Sw", "--noconfirm"}) == 1);   // no targets
    assert(run(config, base / "xdg", {"-Suw", "--noconfirm"}) == 0);  // nothing pending
    assert(run(config, base / "xdg", {"-R", "--noconfirm", "pacmkr-cli-test"}) == 0);
    assert(run(config, base / "xdg", {"-Q", "pacmkr-cli-test"}) == 1);

    fs::remove_all(base);
    std::cout << "isolated native CLI operations passed\n";
}
