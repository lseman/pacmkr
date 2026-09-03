#include <cassert>
#include <iostream>
#include <string>

#include "pacmkr/core/optimize.h"

namespace {

void test_flags_extend_configured_baseline() {
    pacmkr::optimize::OptConfig config;
    config.compiler = pacmkr::cli::Compiler::Gcc;
    config.lto = true;
    config.mold = true;

    auto [cflags, cxxflags, ldflags] = config.apply_flags_to(
        "-march=native -O2", "-march=native -O2", "-Wl,-O1");
    assert(cflags == "-march=native -O2 -flto=auto");
    assert(cxxflags == "-march=native -O2 -flto=auto");
    assert(ldflags == "-Wl,-O1 -flto=auto -fuse-ld=mold");
}

void test_auto_does_not_override_disabled_config_defaults() {
    pacmkr::cli::Cli cli;
    auto config = pacmkr::optimize::OptConfig::from_cli(cli);
    assert(!config.graphite);
    assert(!config.polly);
    assert(!config.lto);
    assert(!config.mold);
}

void test_compiler_specific_optimizers() {
    pacmkr::cli::Cli gcc_cli;
    gcc_cli.cc = pacmkr::cli::Compiler::Gcc;
    gcc_cli.graphite = pacmkr::cli::OptMode::Enabled;
    gcc_cli.polly = pacmkr::cli::OptMode::Enabled;
    auto conflicting = pacmkr::optimize::OptConfig::from_cli(gcc_cli);
    assert(!conflicting.graphite && !conflicting.polly);

    pacmkr::cli::Cli clang_cli;
    clang_cli.cc = pacmkr::cli::Compiler::Clang;
    clang_cli.polly = pacmkr::cli::OptMode::Enabled;
    auto clang_config = pacmkr::optimize::OptConfig::from_cli(clang_cli);
    assert(clang_config.polly);
    assert(!clang_config.graphite);
}

} // namespace

int main() {
    test_flags_extend_configured_baseline();
    test_auto_does_not_override_disabled_config_defaults();
    test_compiler_specific_optimizers();
    std::cout << "All optimization tests passed.\n";
}
