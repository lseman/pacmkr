#include "pacmkr/build/build.h"
#include "pacmkr/core/optimize.h"
#include "pacmkr/core/disk.h"
#include "pacmkr/core/error.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/app/terminal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace pacmkr::build {

namespace fs = std::filesystem;

namespace {

bool check_tool(const std::string& name) {
    return optimize::find_tool(name).has_value();
}

bool valid_environment_name(const std::string& name) {
    if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name.front())) &&
                         name.front() != '_')) return false;
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    });
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

    // ─── Start build timer ───────────────────────────────────────
    auto build_start = std::chrono::steady_clock::now();

    // ─── Set up build log directory ──────────────────────────────
    namespace fs = std::filesystem;
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path cache_base = xdg_cache ? fs::path(xdg_cache) / "pacmkr"
        : (home ? fs::path(home) / ".cache" / "pacmkr"
                : fs::temp_directory_path() / "pacmkr");
    const fs::path logs_dir = cache_base / "logs";
    const std::string ts = pkgbuild.pkgbase() + "-" +
        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    log_dir_ = logs_dir / ts;
    fs::create_directories(log_dir_);

    // Write a build manifest (metadata for log reconstruction)
    {
        std::ofstream meta(log_dir_ / "manifest.json");
        if (meta) {
            meta << "{\n"
                 << "  \"pkgbase\": \"" << pkgbuild.pkgbase() << "\",\n"
                 << "  \"version\": \"" << pkgbuild.full_version() << "\",\n"
                 << "  \"timestamp\": " << std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count() << ",\n"
                 << "  \"compiler\": \"" << (cli.cc.has_value()
                     ? (cli.cc == cli::Compiler::Clang ? "clang" : "gcc")
                     : "auto") << "\",\n"
                 << "  \"workdir\": \"" << workdir.string() << "\"\n"
                 << "}\n";
        }
    }

    terminal::section("Build: " + pkgbuild.pkgbase());
    terminal::info("Log: " + log_dir_.string());

    // ─── Sync missing build dependencies (makedepends/checkdepends) ─
    if (cli.syncdeps) {
        terminal::info("Syncing build dependencies...");
        const int rc = alpm::install_sync_packages(pkgbuild.makedepends, pkgbuild.checkdepends, cli.noconfirm);
        if (rc != 0) {
            std::cerr << "error: failed to sync build dependencies\n";
            return rc;
        }
    }

    // ─── Set up environment with optimization flags ──────────────
    setup_environment();

    // ─── Delegate to makepkg ─────────────────────────────────────
    // All source acquisition, checksum verification, prepare/build/check/
    // package functions, and archive creation are handled by the real
    // makepkg. We only inject our optimization flags and collect post-
    // build results.
    std::vector<std::string> args;
    
    // Determine PKGBUILD path
    const auto pkgbuild_path = pkgbuild.source_path.empty()
        ? (cli.packagefile.empty() ? "PKGBUILD" : cli.packagefile.string())
        : pkgbuild.source_path.string();

    // Build makepkg command line from CLI flags
    if (cli.cleanbuild)  args.push_back("-C");
    if (cli.clean)       args.push_back("-c");
    if (cli.ignorearch)  args.push_back("--ignorearch");
    if (cli.install)     args.push_back("-i");
    if (cli.log)         args.push_back("--log");
    if (cli.nobuild)     args.push_back("-o");
    if (cli.noextract)   args.push_back("-e");
    if (cli.noprepare)   args.push_back("--noprepare");
    if (cli.nocheck)     args.push_back("--nocheck");
    if (cli.check)       args.push_back("--check");
    if (cli.force)       args.push_back("-f");
    if (cli.holdver)     args.push_back("--holdver");
    if (cli.noarchive)   args.push_back("--noarchive");
    if (cli.sign && !cli.nosign) {
        args.push_back("--sign");
        if (cli.key) args.push_back("--key");
        if (cli.key) args.push_back(*cli.key);
    }
    if (cli.nosign)      args.push_back("--nosign");
    if (cli.skipchecksums) args.push_back("--skipchecksums");
    if (cli.skipinteg)   args.push_back("--skipinteg");
    if (cli.verifysource) args.push_back("--verifysource");
    
    // Pass through mflags
    for (const auto& flag : cli.mflags) {
        args.push_back("--mflags");
        args.push_back(flag);
    }

    // Add PKGBUILD path as last argument
    args.push_back(pkgbuild_path);

    // Execute makepkg and capture output to log file
    const auto log_file = log_dir_ / "makepkg.log";
    int exit_code = execute_makepipe(args, log_file);
    
    if (exit_code != 0) {
        std::cerr << "==> makepkg failed with exit code " << exit_code << "\n";
        std::cerr << "==> Log: " << log_file.string() << "\n";
        return exit_code;
    }

    // ─── Post-build: install if requested ────────────────────────
    if (cli.install) {
        // Find the built package archive
        const auto destination = std::filesystem::absolute(config.pkgdest);
        std::vector<std::string> archives;
        
        for (const auto& entry : std::filesystem::directory_iterator(destination)) {
            if (entry.is_regular_file() && entry.path().extension() == ".pkg.tar.zst") {
                archives.push_back(entry.path().string());
            }
        }
        
        if (!archives.empty()) {
            alpm::install_files(archives, cli.noconfirm);
        }
    }

    // ─── Report build timing ─────────────────────────────────────
    auto build_end = std::chrono::steady_clock::now();
    total_duration_ = std::chrono::duration_cast<std::chrono::milliseconds>(build_end - build_start);
    
    int secs = total_duration_.count() / 1000;
    int ms = total_duration_.count() % 1000;
    std::cout << "==> Build completed in " << secs << "s" << (ms > 0 ? " +" + std::to_string(ms) + "ms" : "") << "\n";
    
    terminal::success("Build completed successfully");
    return 0;
}

int BuildOrchestrator::execute_makepipe(const std::vector<std::string>& args,
                                        const std::filesystem::path& log_file) {
    // Build full command: makepkg [flags] PKGBUILD
    std::vector<std::string> cmd = {"makepkg"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    cmd.push_back("--log");  // Enable makepkg's own logging to stderr

    // Open log file for writing
    std::ofstream log_out(log_file, std::ios::trunc);
    if (!log_out) {
        throw package_error("cannot open log file: " + log_file.string());
    }

    // Create pipe for capturing makepkg output
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        throw package_error("cannot create pipe for makepkg output");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw package_error("cannot create makepkg process");
    }

    if (child == 0) {
        // Child: redirect stdout/stderr to log file and pipe
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Also write directly to log file
        int log_fd = open(log_file.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        // Build argv
        std::vector<char*> argv;
        for (const auto& arg : cmd) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp("makepkg", argv.data());
        _exit(127);
    }

    // Parent: read from pipe and write to log file
    close(pipefd[1]);
    {
        char buf[8192];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            log_out.write(buf, static_cast<std::streamsize>(n));
            // Also write to terminal for real-time feedback
            write(STDOUT_FILENO, buf, static_cast<size_t>(n));
        }
    }
    close(pipefd[0]);

    // Wait for child
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void BuildOrchestrator::setup_environment() {
    for (const auto& assignment : cli.extra_env) {
        const auto separator = assignment.find('=');
        if (separator == std::string::npos) continue;
        const auto name = assignment.substr(0, separator);
        if (!valid_environment_name(name))
            throw package_error("invalid build environment variable: " + name);
        const auto value = assignment.substr(separator + 1);
        if (setenv(name.c_str(), value.c_str(), 1) != 0)
            throw package_error("cannot set build environment variable: " + name);
    }

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

    // Apply --mflags as MAKEFLAGS for the package's underlying build tool.
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
    } else if (std::getenv("MAKEFLAGS") == nullptr) {
        // Default aggressive parallelism: -j for CPU count, -l for load limit.
        // Uses hardware_concurrency() with a floor of 2 to avoid spawning
        // trivially-parallel builds on single-core machines.
        const unsigned int nproc = std::max(2u, std::thread::hardware_concurrency());
        const std::string default_mflags = "-j" + std::to_string(nproc) + " -l" + std::to_string(nproc);
        setenv("MAKEFLAGS", default_mflags.c_str(), 1);
        terminal::info("Makeflags: " + default_mflags + " (default)");
    }

    terminal::info("Compiler: " + std::string(opt.compiler == cli::Compiler::Gcc ? "gcc" : "clang"));
    terminal::info("Optimizations: " + opt.summary());
    if (!cflags.empty())   terminal::info("CFLAGS: " + cflags);
    if (!cxxflags.empty()) terminal::info("CXXFLAGS: " + cxxflags);
    if (!ldflags.empty())  terminal::info("LDFLAGS: " + ldflags);
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
