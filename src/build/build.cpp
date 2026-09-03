#include "pacmkr/build/build.h"
#include "pacmkr/core/optimize.h"
#include "pacmkr/core/disk.h"
#include "pacmkr/build/source.h"
#include "pacmkr/core/error.h"
#include "pacmkr/core/package.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/app/terminal.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

namespace fs = std::filesystem;

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

// Maximum number of retries for a single build function.
static constexpr int kMaxRetries = 2;
// Base delay in milliseconds for exponential backoff.
static constexpr int kBaseRetryMs = 1000;

int BuildOrchestrator::run() {
    auto workdir = std::filesystem::current_path();
    auto srcdir = workdir / "src";
    auto pkgdir = workdir / "pkg";

    // ─── Set up build log directory ──────────────────────────────
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const fs::path cache_base = home ? fs::path(home) / ".cache" / "pacmkr" : fs::temp_directory_path() / "pacmkr";
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

    // Run prepare() if present and not skipped (with retry)
    if (!cli.noprepare && pkgbuild.has_function("prepare")) {
        if (run_with_retry("prepare", srcdir) != 0) return 1;
    }

    // Run build() if present (with retry)
    if (pkgbuild.has_function("build")) {
        if (run_with_retry("build", srcdir) != 0) return 1;
    }

    // Run check() if requested (with retry)
    if ((cli.check || !cli.nocheck) && pkgbuild.has_function("check")) {
        if (run_with_retry("check", srcdir) != 0) return 1;
    }

    std::vector<std::filesystem::path> archives;
    const auto destination = std::filesystem::absolute(config.pkgdest);
    std::filesystem::create_directories(destination);

    // ─── Disk space pre-check ────────────────────────────────────
    std::cout << "==> Checking disk space...\n";
    disk::report_disk_status(workdir, destination);
    if (!disk::check_space(workdir)) {
        std::cerr << "error: aborting build due to insufficient disk space\n";
        return 1;
    }

    for (const auto& name : pkgbuild.pkgname) {
        std::filesystem::remove_all(pkgdir);
        std::filesystem::create_directories(pkgdir);
        const std::string function = pkgbuild.has_function("package_" + name) ? "package_" + name : "package";
        if (!pkgbuild.has_function(function)) throw package_error("PKGBUILD has no " + function + "() function");
        if (run_with_retry(function, srcdir, pkgdir, true) != 0) return 1;
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

    // ─── Redirect stdout/stderr to per-function log file ─────────
    const fs::path log_file = log_dir_ / (func_name + ".log");
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        throw package_error("cannot create pipe for build logging");
    }

    const pid_t child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); throw package_error("cannot create build process"); }
    if (child == 0) {
        // Child: redirect stdout/stderr to log file, then exec
        close(pipefd[0]);  // close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

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

    // Parent: read from pipe and write to log file, then wait for child
    close(pipefd[1]);  // close write end
    {
        char buf[8192];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            fs::path log_path = log_dir_ / (func_name + ".log");
            std::ofstream log_out(log_path, std::ios::app);
            if (log_out) log_out.write(buf, n);
        }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    if (rc != 0) {
        std::cerr << "==> " << func_name << "() failed. Log: " << log_file.string() << "\n";
        return rc;
    }

    return 0;
}

std::string BuildOrchestrator::version() const {
    return pkgbuild.full_version();
}

int BuildOrchestrator::run_with_retry(const std::string& func_name,
                                       const std::filesystem::path& srcdir,
                                       const std::filesystem::path& pkgdir,
                                       bool use_fakeroot) {
    int max_attempts = kMaxRetries + 1;  // initial + retries
    
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        int rc = run_function(func_name, srcdir, pkgdir, use_fakeroot);
        if (rc == 0) return 0;
        
        // Retryable: transient failures (network, OOM, GPG issues)
        // Non-retryable: syntax errors in PKGBUILD, missing tools
        bool retryable = (rc == 127 || rc == 126);  // command not found / permission denied
        
        if (!retryable && attempt == 0) {
            // For non-transient errors, try once more then give up
            std::cout << "==> Retrying " << func_name << "() (attempt 2/" << max_attempts << ")...\n";
            continue;
        }
        
        if (attempt < max_attempts - 1) {
            // Exponential backoff: 1s, 3s, 9s...
            int delay_ms = kBaseRetryMs * (1 << attempt);
            std::cout << "==> " << func_name << "() failed (exit " << rc << "). "
                      << "Retrying in " << (delay_ms / 1000) << "s (attempt " 
                      << (attempt + 2) << "/" << max_attempts << ")...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            retries_++;
        }
    }
    
    return 1;
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
