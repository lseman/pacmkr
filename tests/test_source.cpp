#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/build/source.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const auto id = std::to_string(getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = fs::temp_directory_path() / ("pacmkr-source-test-" + id);
    const auto old_path = fs::current_path();
    fs::create_directories(base / "cache");
    fs::current_path(base);
    try {
        std::ofstream("input.txt") << "hello\n";
        pacmkr::pkgbuild::Pkgbuild pb;
        pb.pkgname = {"source-fixture"};
        pb.pkgver = "2.0";
        pb.source = {"renamed-${pkgver}.txt::input.txt"};
        pb.sha256sums = {"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"};
        pacmkr::source::SourceHandler{base / "cache"}.download_and_verify(pb, base / "src");
        assert(fs::exists(base / "src/renamed-2.0.txt"));

        std::ofstream("code-bin.sh") << "#!/bin/sh\n";
        pacmkr::pkgbuild::Pkgbuild variable_pb;
        variable_pb.pkgname = {"visual-studio-code-bin"};
        variable_pb.variables["_pkgname"] = "code";
        variable_pb.source = {"${_pkgname}-bin.sh"};
        pacmkr::source::SourceHandler{base / "variable-cache", true}
            .download_and_verify(variable_pb, base / "variable-src");
        assert(fs::exists(base / "variable-src/code-bin.sh"));

        fs::create_directories(base / "archive-content/project");
        std::ofstream(base / "archive-content/project/value") << "expanded\n";
        const std::string command = "bsdtar -czf " + (base / "project.tar.gz").string() +
                                    " -C " + (base / "archive-content").string() + " project";
        assert(std::system(command.c_str()) == 0);
        pacmkr::pkgbuild::Pkgbuild archive_pb;
        archive_pb.pkgname = {"archive-fixture"};
        archive_pb.source = {"project.tar.gz"};
        archive_pb.sha256sums = {"SKIP"};
        pacmkr::source::SourceHandler{base / "cache", true}.download_and_verify(archive_pb, base / "archive-src");
        assert(fs::exists(base / "archive-src/project/value"));

        fs::create_directories(base / "deb-content");
        std::ofstream(base / "deb-content/data.tar.xz") << "inner archive\n";
        const std::string deb_command = "bsdtar -cf " + (base / "fixture.deb").string() +
                                        " -C " + (base / "deb-content").string() + " data.tar.xz";
        assert(std::system(deb_command.c_str()) == 0);
        pacmkr::pkgbuild::Pkgbuild deb_pb;
        deb_pb.pkgname = {"deb-fixture"};
        deb_pb.source = {"fixture.deb"};
        pacmkr::source::SourceHandler{base / "deb-cache", true}
            .download_and_verify(deb_pb, base / "deb-src");
        assert(fs::exists(base / "deb-src/data.tar.xz"));
    } catch (...) {
        fs::current_path(old_path);
        fs::remove_all(base);
        throw;
    }
    fs::current_path(old_path);
    fs::remove_all(base);
    std::cout << "source expansion, verification, and extraction passed\n";
}
