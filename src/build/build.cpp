#include "pacmkr/build.h"
#include "pacmkr/optimize.h"
#include "pacmkr/source.h"
#include "pacmkr/error.h"
#include "pacmkr/progress.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <queue>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>

namespace pacmkr::build {

namespace {

bool check_tool(const std::string& name) {
    return optimize::find_tool(name).has_value();
}

} // anonymous

void check_requirements() {
    static const char* tools[] = {"bash", "fakeroot", "bsdtar", "gzip", "sha256sum"};
    for (auto* tool : tools) {
        if (!check_tool(tool)) {
            throw missing_tool_error(tool);
        }
    }
}

BuildOrchestrator::BuildOrchestrator(const cli::Cli& cli, const config::Config& config,
                                      pkgbuild::Pkgbuild pkgbuild)
    : cli(cli), config(config), pkgbuild(std::move(pkgbuild)) {}

int BuildOrchestrator::run() {
    auto workdir = std::filesystem::current_path();
    auto srcdir = workdir / "src";
    auto pkgdir = workdir / "pkg";

    // Clean if requested
    if (cli.cleanbuild && std::filesystem::exists(srcdir)) {
        std::cout << "==> Removing existing $srcdir/ directory...\n";
        std::filesystem::remove_all(srcdir);
    }

    if (cli.force && std::filesystem::exists(pkgdir)) {
        std::cout << "==> Removing existing $pkgdir/ directory...\n";
        std::filesystem::remove_all(pkgdir);
    }

    // Create working directories
    std::filesystem::create_directories(srcdir);
    std::filesystem::create_directories(pkgdir);

    // Set up environment with optimization flags
    setup_environment();

    // Run prepare() if present and not skipped
    if (!cli.noprepare && pkgbuild.has_function("prepare")) {
        run_function("prepare", srcdir);
    }

    // Run build() if present
    if (pkgbuild.has_function("build")) {
        run_function("build", srcdir);
    }

    // Run check() if requested
    if ((cli.check || !cli.nocheck) && pkgbuild.has_function("check")) {
        run_function("check", srcdir);
    }

    std::cout << "==> Build completed successfully.\n";
    return 0;
}

void BuildOrchestrator::setup_environment() {
    auto opt = optimize::OptConfig::from_cli(cli);
    opt.apply_compiler_env();

    auto [cflags, cxxflags, ldflags] = opt.apply_flags();

    // Merge with config defaults if env vars not set
    if (std::getenv("CFLAGS") == nullptr && !config.cflags.empty()) {
        setenv("CFLAGS", config.cflags.c_str(), 0);
    }
    if (std::getenv("CXXFLAGS") == nullptr && !config.cxxflags.empty()) {
        setenv("CXXFLAGS", config.cxxflags.c_str(), 0);
    }
    if (std::getenv("LDFLAGS") == nullptr && !config.ldflags.empty()) {
        setenv("LDFLAGS", config.ldflags.c_str(), 0);
    }

    // Apply --mflags: custom makepkg flags
    if (!cli.mflags.empty()) {
        std::ostringstream mflags_env;
        for (size_t i = 0; i < cli.mflags.size(); ++i) {
            if (i > 0) mflags_env << " ";
            mflags_env << cli.mflags[i];
        }
        setenv("MAKEFLAGS", mflags_env.str().c_str(), 1);
        std::cout << "==> Makeflags: " << mflags_env.str() << "\n";
    }

    // Print optimization summary
    std::cout << "==> Compiler: " << (opt.compiler == cli::Compiler::Gcc ? "gcc" : "clang") << "\n";
    std::cout << "==> Optimizations: " << opt.summary() << "\n";
    if (!cflags.empty())   std::cout << "==> CFLAGS: " << cflags << "\n";
    if (!cxxflags.empty()) std::cout << "==> CXXFLAGS: " << cxxflags << "\n";
    if (!ldflags.empty())  std::cout << "==> LDFLAGS: " << ldflags << "\n";
}

int BuildOrchestrator::run_function(const std::string& func_name,
                                     const std::filesystem::path& workdir) {
    if (!pkgbuild.has_function(func_name)) {
        std::cout << "==> No " << func_name << "() function found, skipping.\n";
        return 0;
    }

    std::cout << "==> Starting " << func_name << "()...\n";

    std::ostringstream cmd;
    cmd << "cd '" << workdir.string() << "' && " << func_name << "()";

    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << "==> " << func_name << "() failed.\n";
        return rc;
    }

    return 0;
}

std::string BuildOrchestrator::version() const {
    return pkgbuild.full_version();
}

// ─── Parallel Build Support ──────────────────────────────────────────

int run_parallel_builds(const std::vector<std::string>& package_dirs,
                        const cli::Cli& base_cli, const config::Config& config,
                        progress::MultiProgress& mp, int max_jobs) {
    if (package_dirs.empty()) return 0;

    // Build list of package names for progress tracking
    std::vector<std::string> pkg_names;
    for (auto& dir : package_dirs) {
        pkg_names.push_back(std::filesystem::path(dir).filename().string());
        mp.add_package(pkg_names.back());
    }

    std::atomic<int> failures{0};
    std::mutex mp_mutex; // Protect MultiProgress access

    auto build_single = [&](int pkg_idx, const std::string& dir) {
        try {
            cli::Cli cli = base_cli;
            cli.install = true;
            cli.nocheck = true;

            BuildOrchestrator orchestrator{cli, config, pkgbuild::Pkgbuild::parse(dir + "/PKGBUILD")};
            int rc = orchestrator.run();

            if (rc != 0) {
                failures++;
            }
        } catch (const std::exception& e) {
            std::cerr << "error building " << dir << ": " << e.what() << "\n";
            failures++;
        }
    };

    // Simple work-stealing queue
    std::vector<std::string> work_queue(package_dirs.begin(), package_dirs.end());
    std::atomic<size_t> next_job{0};
    std::vector<std::thread> workers;

    int num_workers = std::min(max_jobs, static_cast<int>(package_dirs.size()));
    for (int i = 0; i < num_workers; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = next_job.fetch_add(1);
                if (idx >= work_queue.size()) break;

                std::string dir = work_queue[idx];
                int pkg_idx = static_cast<int>(idx);

                // Update progress bar for this package
                {
                    std::lock_guard<std::mutex> lock(mp_mutex);
                    mp.set_active(pkg_idx);
                    mp.update(pkg_idx, 0);
                }

                build_single(pkg_idx, dir);

                {
                    std::lock_guard<std::mutex> lock(mp_mutex);
                    mp.complete(pkg_idx);
                }
            }
        });
    }

    // Join all worker threads
    for (auto& w : workers) {
        w.join();
    }

    return failures.load();
}

} // namespace pacmkr::build
