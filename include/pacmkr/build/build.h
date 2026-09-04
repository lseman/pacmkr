#pragma once

#include "pacmkr/app/cli.h"
#include "pacmkr/core/config.h"
#include "pacmkr/build/pkgbuild.h"

#include <string>
#include <vector>
#include <filesystem>

namespace pacmkr::build {

/// Orchestrate the build process via makepipe with our optimization flags.
struct BuildOrchestrator {
    const cli::Cli& cli;
    const config::Config& config;
    pkgbuild::Pkgbuild pkgbuild;

    BuildOrchestrator(const cli::Cli& cli, const config::Config& config,
                      pkgbuild::Pkgbuild pkgbuild);

    /// Run the full build pipeline (delegates to makepipe).
    int run();

    /// Get the package version string.
    std::string version() const;

    /// Total build duration in milliseconds (set by run()).
    int total_duration_ms() const { return static_cast<int>(total_duration_.count()); }

private:
    std::filesystem::path log_dir_{};
    std::chrono::milliseconds total_duration_;
    void setup_environment();
    
    /// Execute makepipe with the given arguments, capturing output to log_file.
    int execute_makepipe(const std::vector<std::string>& args,
                         const std::filesystem::path& log_file);
};

/// Check that required tools are available.
void check_requirements();

/// Run builds for multiple packages in parallel (limited by max_jobs).
/// Returns 0 on success, non-zero on failure.
int run_parallel_builds(const std::vector<std::string>& package_dirs,
                        const cli::Cli& base_cli, const config::Config& config,
                        int max_jobs = 4);

} // namespace pacmkr::build
