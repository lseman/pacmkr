#include "pacmkr/build/source.h"
#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/core/error.h"
#include "pacmkr/app/terminal.h"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <array>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>

namespace pacmkr::source {

namespace {

struct SourceSpec { std::string name, location, fragment; };

std::string vcs_kind(const std::string& location) {
    for (const std::string kind : {"git", "hg", "svn", "bzr"}) {
        if (detail::starts_with(location, kind + "+")) return kind;
    }
    return {};
}

SourceSpec parse_source(const std::string& source) {
    SourceSpec result;
    auto location = source;
    if (const auto alias = location.find("::"); alias != std::string::npos) {
        result.name = location.substr(0, alias);
        location.erase(0, alias + 2);
    }
    if (const auto fragment = location.find('#'); fragment != std::string::npos) {
        result.fragment = location.substr(fragment + 1);
        location.erase(fragment);
    }
    result.location = location;
    return result;
}

std::string get_filename(const std::string& source) {
    auto parsed = parse_source(source);
    if (!parsed.name.empty()) return parsed.name;
    // Strip VCS prefixes
    auto s = parsed.location;
    for (auto prefix : {"git+", "hg+", "svn+", "bzr+", "file://", "https://", "http://"}) {
        auto pos = s.find(prefix);
        if (pos != std::string::npos) s = s.substr(pos + strlen(prefix));
    }

    // Extract filename from URL/path
    auto slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);

    const auto query = s.find('?');
    if (query != std::string::npos) s.erase(query);
    if (s.size() > 4 && s.substr(s.size() - 4) == ".git") s.erase(s.size() - 4);
    return s;
}

int run_process(const std::vector<std::string>& args, std::string* output = nullptr) {
    if (args.empty()) return 127;
    int descriptors[2]{-1, -1};
    if (output && pipe(descriptors) < 0) return 127;
    const pid_t child = fork();
    if (child < 0) return 127;
    if (child == 0) {
        if (output) {
            close(descriptors[0]);
            dup2(descriptors[1], STDOUT_FILENO);
            close(descriptors[1]);
        }
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (output) {
        close(descriptors[1]);
        std::array<char, 4096> buffer{};
        for (ssize_t count; (count = read(descriptors[0], buffer.data(), buffer.size())) > 0;)
            output->append(buffer.data(), static_cast<size_t>(count));
        close(descriptors[0]);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

bool is_archive(const std::string& name) {
    static const std::vector<std::string> endings = {
        ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz",
        ".tar.zst", ".zip", ".deb"
    };
    return std::any_of(endings.begin(), endings.end(), [&](const auto& ending) {
        return name.size() >= ending.size() && name.compare(name.size() - ending.size(), ending.size(), ending) == 0;
    });
}

std::string expand_source(std::string value, const pkgbuild::Pkgbuild& pkgbuild) {
    const std::string package = pkgbuild.pkgname.empty() ? pkgbuild.pkgbase() : pkgbuild.pkgname.front();
    auto variables = pkgbuild.variables;
    variables["pkgname"] = package;
    variables["pkgbase"] = pkgbuild.pkgbase();
    variables["pkgver"] = pkgbuild.pkgver;
    variables["pkgrel"] = pkgbuild.pkgrel;

    // Values may refer to earlier variables (for example _pkgname="code" and
    // source=("${_pkgname}-bin.sh")). A small fixed-point loop handles those
    // references without attempting to execute arbitrary PKGBUILD shell code.
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto& [name, replacement] : variables) {
            for (const std::string& token : {"${" + name + "}", "$" + name}) {
                for (auto at = value.find(token); at != std::string::npos;) {
                    value.replace(at, token.size(), replacement);
                    at = value.find(token, at + replacement.size());
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    return value;
}

} // anonymous

SourceHandler::SourceHandler(const std::filesystem::path& srcdest, bool skip_checksums,
                             bool hold_version)
    : srcdest(srcdest), skip_checksums(skip_checksums), hold_version(hold_version) {}

void SourceHandler::download_and_verify(const pkgbuild::Pkgbuild& pkgbuild,
                                         const std::filesystem::path& srcdir) const {
    std::filesystem::create_directories(srcdir);
    std::filesystem::create_directories(srcdest);

    // Separate VCS sources from archive sources.
    // VCS operations must be sequential (git/hg/svn commands can conflict).
    // Archive downloads can be parallelized safely.
    std::vector<std::string> vcs_sources;
    std::vector<std::string> archive_sources;

    for (const auto& source : pkgbuild.source) {
        const auto expanded = expand_source(source, pkgbuild);
        const auto parsed = parse_source(expanded);
        if (!vcs_kind(parsed.location).empty()) {
            vcs_sources.push_back(expanded);
        } else {
            archive_sources.push_back(expanded);
        }
    }

    // Download VCS sources sequentially.
    for (const auto& source : vcs_sources) {
        download_source(source, srcdir);
    }

    // Download archive sources in parallel.
    if (!archive_sources.empty()) {
        std::mutex log_mutex;
        const unsigned int num_threads = std::min(
            static_cast<unsigned int>(std::thread::hardware_concurrency()),
            static_cast<unsigned int>(archive_sources.size()));

        auto worker = [&](const std::vector<std::string>& batch) {
            for (const auto& source : batch) {
                try {
                    download_source(source, srcdir);
                } catch (const source_error& e) {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    std::cerr << "error: Failed to download: " << e.what() << "\n";
                }
            }
        };

        // Split archive sources across threads.
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        size_t start = 0;
        while (start < archive_sources.size()) {
            const size_t end = std::min(start + (archive_sources.size() + num_threads - 1) / num_threads,
                                        archive_sources.size());
            std::vector<std::string> batch(archive_sources.begin() + start,
                                           archive_sources.begin() + end);
            threads.emplace_back(worker, std::move(batch));
            start = end;
        }

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

    if (!skip_checksums) {
        verify_checksums(pkgbuild, srcdir);
    }
    extract_sources(pkgbuild, srcdir);
}

void SourceHandler::download_source(const std::string& source_spec,
                                     const std::filesystem::path& srcdir) const {
    const auto parsed = parse_source(source_spec);
    const auto filename = get_filename(source_spec);
    if (filename.empty() || filename == "." || filename == ".." || filename.find('/') != std::string::npos)
        throw source_error("Invalid source filename: " + filename);

    const auto kind = vcs_kind(parsed.location);
    if (!kind.empty()) {
        const auto destination = srcdir / filename;
        const auto url = parsed.location.substr(kind.size() + 1);
        const auto separator = parsed.fragment.find('=');
        const auto fragment_type = separator == std::string::npos
            ? std::string{} : parsed.fragment.substr(0, separator);
        const auto revision = separator == std::string::npos
            ? std::string{} : parsed.fragment.substr(separator + 1);
        if (!parsed.fragment.empty() && revision.empty())
            throw source_error("Unsupported VCS fragment: " + parsed.fragment);

        if (!std::filesystem::exists(destination)) {
            std::vector<std::string> clone;
            if (kind == "git") clone = {"git", "clone", "--", url, destination.string()};
            else if (kind == "hg") clone = {"hg", "clone", url, destination.string()};
            else if (kind == "svn") clone = {"svn", "checkout", url, destination.string()};
            else clone = {"bzr", "branch", url, destination.string()};
            if (run_process(clone) != 0) throw source_error("Failed to clone: " + url);
        } else if (!hold_version) {
            int update = 0;
            if (kind == "git")
                update = run_process({"git", "-C", destination.string(), "fetch", "--all", "--tags", "--prune"});
            else if (kind == "hg")
                update = run_process({"hg", "-R", destination.string(), "pull"});
            else if (kind == "svn")
                update = run_process({"svn", "update", destination.string()});
            else
                update = run_process({"bzr", "pull", "-d", destination.string()});
            if (update != 0) throw source_error("Failed to update VCS source: " + url);
        }

        if (!revision.empty()) {
            int checkout = 0;
            if (kind == "git") {
                const auto target = fragment_type == "branch" ? "origin/" + revision : revision;
                checkout = run_process({"git", "-C", destination.string(), "checkout", "--detach", target});
            } else if (kind == "hg") {
                checkout = run_process({"hg", "-R", destination.string(), "update", "-r", revision});
            } else if (kind == "svn") {
                checkout = run_process({"svn", "update", "-r", revision, destination.string()});
            } else {
                checkout = run_process({"bzr", "update", "-r", revision, "-d", destination.string()});
            }
            if (checkout != 0) throw source_error("Failed to check out revision " + revision);
        } else if (!hold_version && kind == "git" && std::filesystem::exists(destination / ".git")) {
            if (run_process({"git", "-C", destination.string(), "pull", "--ff-only"}) != 0)
                throw source_error("Failed to update Git worktree: " + url);
        }
        return;
    }
    auto cached = srcdest / filename;

    if (!std::filesystem::exists(cached)) {
        if (parsed.location.find("://") != std::string::npos) {
            terminal::info("Downloading " + filename);
            if (run_process({"curl", "--fail", "--location", "--output", cached.string(), parsed.location}) != 0)
                throw source_error("Failed to download: " + parsed.location);
        } else {
            auto local = std::filesystem::absolute(parsed.location);
            if (!std::filesystem::exists(local)) throw source_error("Local source not found: " + parsed.location);
            std::filesystem::copy_file(local, cached, std::filesystem::copy_options::overwrite_existing);
        }
    } else {
        terminal::info("Using cached source: " + filename);
    }

    // Copy to srcdir if not there
    auto dest = srcdir / filename;
    if (!std::filesystem::exists(dest)) {
        std::filesystem::copy_file(cached, dest, std::filesystem::copy_options::overwrite_existing);
    }
}

void SourceHandler::extract_sources(const pkgbuild::Pkgbuild& pkgbuild,
                                    const std::filesystem::path& srcdir) const {
    for (const auto& source : pkgbuild.source) {
        const auto expanded = expand_source(source, pkgbuild);
        const auto parsed = parse_source(expanded);
        if (!vcs_kind(parsed.location).empty()) continue;
        const auto filename = get_filename(expanded);
        if (std::find(pkgbuild.noextract.begin(), pkgbuild.noextract.end(), filename) !=
                pkgbuild.noextract.end() ||
            std::find(pkgbuild.noextract.begin(), pkgbuild.noextract.end(), source) !=
                pkgbuild.noextract.end()) continue;
        if (!is_archive(filename)) continue;
        if (run_process({"bsdtar", "-xf", (srcdir / filename).string(), "-C", srcdir.string()}) != 0)
            throw source_error("Failed to extract: " + filename);
    }
}

void SourceHandler::verify_checksums(const pkgbuild::Pkgbuild& pkgbuild,
                                      const std::filesystem::path& srcdir) const {
    bool has_integrity_data = false;
    auto check_sums = [&](const std::vector<std::string>& sums, const std::string& algo) {
        if (sums.empty()) return;
        has_integrity_data = true;
        if (sums.size() != pkgbuild.source.size())
            throw source_error("Integrity array length does not match source array for " + algo);
        for (size_t i = 0; i < sums.size(); ++i) {
            auto expected = sums[i];
            if (expected == "SKIP" || expected == "IGNORE") continue;

            auto filename = get_filename(expand_source(pkgbuild.source[i], pkgbuild));
            auto filepath = srcdir / filename;

            if (!std::filesystem::is_regular_file(filepath))
                throw source_error("Cannot verify non-file source: " + filename);

            auto actual = compute_hash(filepath, algo);
            if (actual != expected) {
                throw checksum_error{filename, expected, actual};
            }
        }
    };

    check_sums(pkgbuild.sha256sums, "sha256");
    check_sums(pkgbuild.md5sums, "md5");
    check_sums(pkgbuild.sha1sums, "sha1");
    check_sums(pkgbuild.sha224sums, "sha224");
    check_sums(pkgbuild.sha384sums, "sha384");
    check_sums(pkgbuild.sha512sums, "sha512");
    check_sums(pkgbuild.b2sums, "b2");

    if (!pkgbuild.source.empty() && !has_integrity_data)
        throw source_error("PKGBUILD has sources but no integrity array");

    terminal::success("Source checksums verified");
}

std::string SourceHandler::compute_hash(const std::filesystem::path& file,
                                         const std::string& algo) const {
    static const std::vector<std::string> allowed{"md5", "sha1", "sha224", "sha256", "sha384", "sha512", "b2"};
    if (std::find(allowed.begin(), allowed.end(), algo) == allowed.end())
        throw source_error("Unsupported checksum algorithm: " + algo);
    std::string output;
    if (run_process({algo + "sum", "--", file.string()}, &output) != 0 || output.empty())
        throw source_error("Failed to calculate " + algo + " checksum for " + file.string());

    // Output format: hash  filename
    auto space = output.find(' ');
    if (space != std::string::npos) return output.substr(0, space);
    throw source_error("Invalid " + algo + " checksum output for " + file.string());
}

} // namespace pacmkr::source
