#include "pacmkr/build/build.h"
#include "pacmkr/core/optimize.h"
#include "pacmkr/build/source.h"
#include "pacmkr/core/error.h"
#include "pacmkr/core/package.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/app/terminal.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <queue>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace pacmkr::build {

namespace {

bool check_tool(const std::string& name) {
    return optimize::find_tool(name).has_value();
}

int sign_archive(const std::filesystem::path& archive, const std::optional<std::string>& key) {
    const pid_t child = fork();
    if (child < 0) throw package_error("cannot create signing process");
    if (child == 0) {
        if (key)
            execlp("gpg", "gpg", "--batch", "--yes", "--detach-sign", "--local-user",
                   key->c_str(), archive.c_str(), static_cast<char*>(nullptr));
        else
            execlp("gpg", "gpg", "--batch", "--yes", "--detach-sign",
                   archive.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

} // anonymous

void check_requirements() {
    static const char* tools[] = {"bash", "fakeroot", "bsdtar", "zstd", "sha256sum"};
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

    terminal::section("Build: " + pkgbuild.pkgbase());

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

    if (!cli.noextract) {
        source::SourceHandler sources{config.srcdest, cli.skipchecksums || cli.skipinteg};
        sources.download_and_verify(pkgbuild, srcdir);
    }
    if (cli.verifysource || cli.nobuild) return 0;

    // Run prepare() if present and not skipped
    if (!cli.noprepare && pkgbuild.has_function("prepare")) {
        if (run_function("prepare", srcdir) != 0) return 1;
    }

    // Run build() if present
    if (pkgbuild.has_function("build")) {
        if (run_function("build", srcdir) != 0) return 1;
    }

    // Run check() if requested
    if ((cli.check || !cli.nocheck) && pkgbuild.has_function("check")) {
        if (run_function("check", srcdir) != 0) return 1;
    }

    std::vector<std::filesystem::path> archives;
    const auto destination = std::filesystem::absolute(config.pkgdest);
    std::filesystem::create_directories(destination);
    for (const auto& name : pkgbuild.pkgname) {
        std::filesystem::remove_all(pkgdir);
        std::filesystem::create_directories(pkgdir);
        const std::string function = pkgbuild.has_function("package_" + name) ? "package_" + name : "package";
        if (!pkgbuild.has_function(function)) throw package_error("PKGBUILD has no " + function + "() function");
        if (run_function(function, srcdir, pkgdir, true) != 0) return 1;
        const std::string arch = pkgbuild.arch.empty() ? "any" : pkgbuild.arch.front();
        auto output = package::Package::make(name, pkgbuild.pkgver + "-" + pkgbuild.pkgrel, arch, destination);
        output.write_pkginfo(pkgbuild, pkgdir, config.packager,
            static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())));
        if (!cli.noarchive) {
            output.create_archive(pkgdir);
            if (cli.sign && !cli.nosign && sign_archive(output.dest, cli.key) != 0)
                throw package_error("failed to sign package archive " + output.dest.string());
            archives.push_back(output.dest);
        }
    }

    if (cli.install) {
        if (archives.empty()) throw package_error("cannot install when package archive creation is disabled");
        std::vector<std::string> paths;
        for (const auto& archive : archives) paths.push_back(archive.string());
        alpm::install_files(paths, cli.noconfirm);
    }

    terminal::success("Build completed successfully");
    return 0;
}

void BuildOrchestrator::setup_environment() {
    auto opt = optimize::OptConfig::from_cli(cli);
    opt.apply_compiler_env();

    auto baseline = [&](char const* name, std::string const& configured) {
        if (auto const* value = std::getenv(name)) return std::string(value);
        return configured;
    };
    auto [cflags, cxxflags, ldflags] = opt.apply_flags_to(
        baseline("CFLAGS", config.cflags), baseline("CXXFLAGS", config.cxxflags),
        baseline("LDFLAGS", config.ldflags));
    setenv("CFLAGS", cflags.c_str(), 1);
    setenv("CXXFLAGS", cxxflags.c_str(), 1);
    setenv("LDFLAGS", ldflags.c_str(), 1);

    // Apply --mflags: custom makepkg flags
    if (!cli.mflags.empty()) {
        std::ostringstream mflags_env;
        for (size_t i = 0; i < cli.mflags.size(); ++i) {
            if (i > 0) mflags_env << " ";
            mflags_env << cli.mflags[i];
        }
        setenv("MAKEFLAGS", mflags_env.str().c_str(), 1);
        terminal::info("Makeflags: " + mflags_env.str());
    } else if (std::getenv("MAKEFLAGS") == nullptr && !config.makeflags.empty()) {
        auto makeflags = config.makeflags;
        auto const marker = makeflags.find("$(nproc)");
        if (marker != std::string::npos)
            makeflags.replace(marker, 8, std::to_string(std::max(1u, std::thread::hardware_concurrency())));
        setenv("MAKEFLAGS", makeflags.c_str(), 1);
        terminal::info("Makeflags: " + makeflags);
    }

    terminal::info("Compiler: " + std::string(opt.compiler == cli::Compiler::Gcc ? "gcc" : "clang"));
    terminal::info("Optimizations: " + opt.summary());
    if (!cflags.empty())   terminal::info("CFLAGS: " + cflags);
    if (!cxxflags.empty()) terminal::info("CXXFLAGS: " + cxxflags);
    if (!ldflags.empty())  terminal::info("LDFLAGS: " + ldflags);
}

int BuildOrchestrator::run_function(const std::string& func_name,
                                     const std::filesystem::path& srcdir,
                                     const std::filesystem::path& pkgdir,
                                     bool use_fakeroot) {
    if (!pkgbuild.has_function(func_name)) {
        std::cout << "==> No " << func_name << "() function found, skipping.\n";
        return 0;
    }

    terminal::info("Running " + func_name + "()");

    const auto script = pkgbuild.source_path.empty()
        ? std::filesystem::absolute(cli.packagefile.empty() ? "PKGBUILD" : cli.packagefile)
        : pkgbuild.source_path;
    const std::string shell = "source \"$1\"; cd \"$2\"; \"$3\"";
    const pid_t child = fork();
    if (child < 0) throw package_error("cannot create build process");
    if (child == 0) {
        setenv("srcdir", srcdir.c_str(), 1);
        setenv("startdir", script.parent_path().c_str(), 1);
        if (!pkgdir.empty()) setenv("pkgdir", pkgdir.c_str(), 1);
        if (use_fakeroot)
            execlp("fakeroot", "fakeroot", "--", "bash", "-c", shell.c_str(), "pacmkr",
                   script.c_str(), srcdir.c_str(), func_name.c_str(), static_cast<char*>(nullptr));
        else
            execlp("bash", "bash", "-c", shell.c_str(), "pacmkr", script.c_str(),
                   srcdir.c_str(), func_name.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
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
                        int max_jobs) {
    if (package_dirs.empty()) return 0;

    std::atomic<int> failures{0};

    auto build_single = [&](const std::string& dir) {
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
                build_single(dir);
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
