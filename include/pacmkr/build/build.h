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

    /// Run the full build pipeline with retry support.
    int run();

    /// Get the package version string.
    std::string version() const;

    /// Number of retries attempted (set by run()).
    int retries_attempted() const { return retries_; }

    /// Total build duration in milliseconds (set by run()).
    int total_duration_ms() const { return static_cast<int>(total_duration_.count()); }

private:
    std::filesystem::path log_dir_{};
    int retries_{0};
    std::chrono::milliseconds total_duration_;
    void setup_environment();
    int run_function(const std::string& func_name, const std::filesystem::path& srcdir,
                     const std::filesystem::path& pkgdir = {}, bool fakeroot = false);
    int run_with_retry(const std::string& func_name, const std::filesystem::path& srcdir,
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
