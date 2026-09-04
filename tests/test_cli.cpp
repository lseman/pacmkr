#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "pacmkr/app/cli.h"
#include "pacmkr/app/operations.h"
#include "pacmkr/core/pacman_config.h"

#include <sstream>

// Minimal CLI test — just verifies that we can parse a basic set of args.
// Full argparse testing would require linking the main binary.

namespace {

pacmkr::cli::Cli parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) argv.push_back(arg.data());
    return pacmkr::cli::parse(static_cast<int>(argv.size()), argv.data());
}

void test_sync_upgrade_detection() {
    using pacmkr::operations::is_sync_upgrade;
    assert(is_sync_upgrade({"-Syu"}));
    assert(is_sync_upgrade({"-Su"}));
    assert(is_sync_upgrade({"--sync", "--sysupgrade"}));
    assert(!is_sync_upgrade({"-Sy"}));
    assert(!is_sync_upgrade({"-Qu"}));
    assert(!is_sync_upgrade({"-U", "package.pkg.tar.zst"}));
}

void test_root_requirement_detection() {
    using pacmkr::operations::requires_root;
    assert(requires_root({"-S", "visual-studio-code-bin"}));
    assert(requires_root({"-Syu"}));
    assert(requires_root({"-Rns", "unused-package"}));
    assert(requires_root({"-U", "package.pkg.tar.zst"}));
    assert(requires_root({"--aur", "pkg", "--install"}));
    assert(requires_root({"--refresh"}));
    assert(requires_root({"-u"}));
    assert(!requires_root({"-Ss", "editor"}));
    assert(!requires_root({"-Si", "bash"}));
    assert(!requires_root({"-Q", "bash"}));
    assert(!requires_root({"--aur", "pkg"}));
}

void test_syu_parse_and_transaction() {
    auto cli = parse({"pacmkr", "-Syu"});
    assert(cli.operation == pacmkr::cli::Cli::Op::Sync);
    assert(cli.refresh_db);
    assert(cli.sysupgrade);

    auto args = pacmkr::operations::repo_upgrade_args(cli.refresh_db,
                                                       cli.noconfirm);
    assert((args == std::vector<std::string>{"-Syu"}));

    auto confirmed = pacmkr::operations::repo_upgrade_args(true, true);
    assert((confirmed == std::vector<std::string>{"-Syu", "--noconfirm"}));
}

void test_foreign_package_query() {
    using pacmkr::operations::is_foreign_package_query;
    using pacmkr::operations::is_quiet_query;

    assert(is_foreign_package_query({"-Qm"}));
    assert(is_foreign_package_query({"-Qmq"}));
    assert(is_foreign_package_query({"--list-foreign"}));
    assert(!is_foreign_package_query({"-Q"}));
    assert(!is_foreign_package_query({"-Qe"}));
    assert(is_quiet_query({"-Qmq"}));
    assert(is_quiet_query({"--list-foreign", "--quiet"}));

    auto alias = parse({"pacmkr", "--list-foreign"});
    assert(alias.operation == pacmkr::cli::Cli::Op::Query);
    assert(alias.query_mirrors);
}

void test_pacman_repository_parsing() {
    std::istringstream config{
        "[options]\n"
        "HoldPkg = pacman glibc\n"
        "# [disabled]\n"
        "[core]\n"
        "Include = /etc/pacman.d/mirrorlist\n"
        "[extra] # inline comment\n"
        "[cachyos-v3]\n"
        "[core]\n"
    };
    auto repositories = pacmkr::pacman_config::repository_names(config);
    assert((repositories == std::vector<std::string>{
        "core", "extra", "cachyos-v3"
    }));
}

void test_pacman_options_parsing() {
    std::istringstream config{"[options]\nArchitecture = auto\nParallelDownloads = 7\nCheckSpace\nIgnorePkg = linux foo\nNoUpgrade = etc/passwd\nDownloadUser = alpm\n[core]\nServer = ignored\n"};
    auto options = pacmkr::pacman_config::parse_options(config);
    assert(options.architecture == "auto");
    assert(options.parallel_downloads == 7);
    assert(options.check_space);
    assert((options.ignore_packages == std::vector<std::string>{"linux", "foo"}));
    assert((options.no_upgrade == std::vector<std::string>{"etc/passwd"}));
    assert(options.download_user == "alpm");
}

void test_pacman_repository_policy_parsing() {
    std::istringstream input{"[options]\nSigLevel = Required DatabaseOptional\nLocalFileSigLevel = Optional\n[staging]\nUsage = Sync Search\nSigLevel = PackageRequired DatabaseNever\n[core]\nServer = https://example/$repo/os/$arch\n"};
    auto config = pacmkr::pacman_config::parse(input);
    assert((config.options.sig_level == std::vector<std::string>{"Required", "DatabaseOptional"}));
    assert((config.options.local_file_sig_level == std::vector<std::string>{"Optional"}));
    assert(config.repositories.size() == 2);
    assert(config.repositories[0].name == "staging");
    assert((config.repositories[0].usage == std::vector<std::string>{"Sync", "Search"}));
    assert((config.repositories[0].sig_level == std::vector<std::string>{"PackageRequired", "DatabaseNever"}));
}

void test_remove_flag_semantics() {
    auto plain = parse({"pacmkr", "-R", "foo"});
    assert(plain.operation == pacmkr::cli::Cli::Op::Remove);
    assert(!plain.remove_deps && !plain.remove_configs && !plain.nosave);
    auto recursive = parse({"pacmkr", "-Rns", "foo"});
    assert(recursive.remove_deps && recursive.nosave);
    assert(!recursive.remove_configs);
}

void test_native_operation_parsing() {
    assert(parse({"pacmkr", "-T", "glibc>=2"}).operation == pacmkr::cli::Cli::Op::Deptest);
    auto files = parse({"pacmkr", "-Fs", "libfoo.so"});
    assert(files.operation == pacmkr::cli::Cli::Op::Files && files.files_search);
    auto reason = parse({"pacmkr", "-D", "--asdeps", "foo"});
    assert(reason.operation == pacmkr::cli::Cli::Op::Database && reason.database_remove);
}

void test_query_file_and_changelog_parsing() {
    auto file_query = parse({"pacmkr", "-Qp", "./pkg-1.0-1-x86_64.pkg.tar.zst"});
    assert(file_query.operation == pacmkr::cli::Cli::Op::Query);
    assert(file_query.query_file && !file_query.query_info);
    assert((file_query.packages == std::vector<std::string>{"./pkg-1.0-1-x86_64.pkg.tar.zst"}));

    auto file_info = parse({"pacmkr", "-Qip", "./pkg.pkg.tar.zst"});
    assert(file_info.query_file && file_info.query_info);

    auto changelog = parse({"pacmkr", "-Qc", "bash"});
    assert(changelog.operation == pacmkr::cli::Cli::Op::Query && changelog.query_changelog);
}

void test_sync_clean_parsing() {
    auto once = parse({"pacmkr", "-Sc"});
    assert(once.operation == pacmkr::cli::Cli::Op::Sync && once.sync_clean);
    auto twice = parse({"pacmkr", "-Scc"});
    assert(twice.operation == pacmkr::cli::Cli::Op::Sync && twice.sync_clean);
    // -Sc must not be mistaken for a package target.
    assert(once.packages.empty() && twice.packages.empty());
}

void test_sync_download_parsing() {
    auto plain = parse({"pacmkr", "-Sw", "curl"});
    assert(plain.operation == pacmkr::cli::Cli::Op::Sync && plain.sync_download);
    assert((plain.packages == std::vector<std::string>{"curl"}));

    auto refresh = parse({"pacmkr", "-Swy", "curl"});
    assert(refresh.sync_download && refresh.refresh_db);

    auto upgrade = parse({"pacmkr", "-Suw"});
    assert(upgrade.sync_download && upgrade.sysupgrade);

    auto long_form = parse({"pacmkr", "-S", "--downloadonly", "curl"});
    assert(long_form.operation == pacmkr::cli::Cli::Op::Sync && long_form.sync_download);
}

void test_aur_review_policy_parsing() {
    auto defaults = parse({"pacmkr", "--aur", "review-test"});
    assert(defaults.aur);
    assert(!defaults.no_review);

    auto opted_out = parse({"pacmkr", "--aur", "--noreview", "review-test"});
    assert(opted_out.no_review);
}

void test_build_directory_parsing() {
    auto directory = parse({"pacmkr", "--build", "--dir", "/tmp/build-here"});
    assert(directory.dir == "/tmp/build-here");
}

} // namespace

int main() {
    std::cout << "Running cli tests...\n";

    test_sync_upgrade_detection();
    test_root_requirement_detection();
    test_syu_parse_and_transaction();
    test_foreign_package_query();
    test_pacman_repository_parsing();
    test_pacman_options_parsing();
    test_pacman_repository_policy_parsing();
    test_remove_flag_semantics();
    test_native_operation_parsing();
    test_query_file_and_changelog_parsing();
    test_sync_clean_parsing();
    test_sync_download_parsing();
    test_aur_review_policy_parsing();
    test_build_directory_parsing();

    std::cout << "  PASSED: sync_upgrade_detection\n";
    std::cout << "  PASSED: syu_atomic_transaction\n";
    std::cout << "  PASSED: foreign_package_query\n";
    std::cout << "  PASSED: pacman_repository_parsing\n";
    std::cout << "All cli tests passed.\n";
    return 0;
}
