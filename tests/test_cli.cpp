#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "pacmkr/cli.h"
#include "pacmkr/operations.h"

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

} // namespace

int main() {
    std::cout << "Running cli tests...\n";

    test_sync_upgrade_detection();
    test_syu_parse_and_transaction();

    std::cout << "  PASSED: sync_upgrade_detection\n";
    std::cout << "  PASSED: syu_atomic_transaction\n";
    std::cout << "All cli tests passed.\n";
    return 0;
}
