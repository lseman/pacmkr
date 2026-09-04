#include "pacmkr/backend/aur_review.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string quote(const std::string& value) {
    std::string result{"'"};
    for (const char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

void run(const std::filesystem::path& repository, const std::string& arguments) {
    const std::string command = "git -C " + quote(repository.string()) + " " + arguments;
    assert(std::system(command.c_str()) == 0);
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    assert(output);
    output << content;
}

class TemporaryRepository {
public:
    TemporaryRepository() {
        char pattern[] = "/tmp/pacmkr-aur-review-XXXXXX";
        const char* created = mkdtemp(pattern);
        assert(created);
        path = created;
        run(path, "init -q");
        run(path, "config user.name 'pacmkr tests'");
        run(path, "config user.email 'tests@pacmkr.invalid'");
    }

    ~TemporaryRepository() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void test_initial_and_incremental_review() {
    TemporaryRepository repository;
    write(repository.path / "PKGBUILD",
          "pkgname=review-test\nsha256sums=('SKIP')\n");
    write(repository.path / "review-test.install", "post_install() { true; }\n");
    run(repository.path, "add PKGBUILD review-test.install");
    run(repository.path, "commit -qm initial");

    auto initial = pacmkr::aur_review::inspect(repository.path);
    assert(!initial.previous_commit.has_value());
    assert(!initial.commit.empty());
    assert(initial.patch.find("pkgname=review-test") != std::string::npos);
    assert(std::find(initial.changed_files.begin(), initial.changed_files.end(), "PKGBUILD") !=
           initial.changed_files.end());
    assert(initial.warnings.size() >= 2);

    pacmkr::aur_review::mark_reviewed(initial);
    auto unchanged = pacmkr::aur_review::inspect(repository.path);
    assert(unchanged.already_reviewed());
    assert(unchanged.patch.empty());

    write(repository.path / "PKGBUILD",
          "pkgname=review-test\nsha256sums=('0123456789')\n");
    run(repository.path, "add PKGBUILD");
    run(repository.path, "commit -qm checksums");

    auto incremental = pacmkr::aur_review::inspect(repository.path);
    assert(incremental.previous_commit == initial.commit);
    assert(incremental.commit != initial.commit);
    assert(incremental.changed_files == std::vector<std::string>{"PKGBUILD"});
    assert(incremental.patch.find("0123456789") != std::string::npos);
}

} // namespace

int main() {
    test_initial_and_incremental_review();
    std::cout << "All AUR review tests passed.\n";
    return 0;
}
