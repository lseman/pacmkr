#pragma once

#include "pacmkr/app/cli.h"
#include "pacmkr/core/config.h"
#include "pacmkr/build/pkgbuild.h"

namespace pacmkr::build {

/// Orchestrate the build process: prepare, build, check.
struct BuildOrchestrator {
    const cli::Cli& cli;
    const config::Config& config;
    pkgbuild::Pkgbuild pkgbuild;

    BuildOrchestrator(const cli::Cli& cli, const config::Config& config,
                      pkgbuild::Pkgbuild pkgbuild);

    /// Run the full build pipeline.
    int run();

    /// Get the package version string.
    std::string version() const;

private:
    void setup_environment();
    int run_function(const std::string& func_name, const std::filesystem::path& srcdir,
                     const std::filesystem::path& pkgdir = {}, bool fakeroot = false);
};

/// Check that required tools are available.
void check_requirements();

/// Run builds for multiple packages in parallel (limited by max_jobs).
/// Returns 0 on success, non-zero on failure.
int run_parallel_builds(const std::vector<std::string>& package_dirs,
                        const cli::Cli& base_cli, const config::Config& config,
                        int max_jobs = 4);

} // namespace pacmkr::build
