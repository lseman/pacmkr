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

namespace pacmkr::source {

namespace {

struct SourceSpec { std::string name, location, fragment; };

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
            for (const std::string token : {"${" + name + "}", "$" + name}) {
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

SourceHandler::SourceHandler(const std::filesystem::path& srcdest, bool skip_checksums)
    : srcdest(srcdest), skip_checksums(skip_checksums) {}

void SourceHandler::download_and_verify(const pkgbuild::Pkgbuild& pkgbuild,
                                         const std::filesystem::path& srcdir) const {
    std::filesystem::create_directories(srcdir);
    std::filesystem::create_directories(srcdest);

    for (const auto& source : pkgbuild.source) {
        download_source(expand_source(source, pkgbuild), srcdir);
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

    if (detail::starts_with(parsed.location, "git+")) {
        const auto destination = srcdir / filename;
        const auto url = parsed.location.substr(4);
        if (!std::filesystem::exists(destination) &&
            run_process({"git", "clone", "--", url, destination.string()}) != 0)
            throw source_error("Failed to clone: " + url);
        if (!parsed.fragment.empty()) {
            auto revision = parsed.fragment;
            const auto eq = revision.find('=');
            if (eq != std::string::npos) revision.erase(0, eq + 1);
            if (run_process({"git", "-C", destination.string(), "checkout", "--detach", revision}) != 0)
                throw source_error("Failed to check out revision " + revision);
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
        if (detail::starts_with(parsed.location, "git+")) continue;
        const auto filename = get_filename(expanded);
        if (!is_archive(filename)) continue;
        if (run_process({"bsdtar", "-xf", (srcdir / filename).string(), "-C", srcdir.string()}) != 0)
            throw source_error("Failed to extract: " + filename);
    }
}

void SourceHandler::verify_checksums(const pkgbuild::Pkgbuild& pkgbuild,
                                      const std::filesystem::path& srcdir) const {
    auto check_sums = [&](const std::vector<std::string>& sums, const std::string& algo) {
        for (size_t i = 0; i < sums.size() && i < pkgbuild.source.size(); ++i) {
            auto expected = sums[i];
            if (expected == "SKIP" || expected == "IGNORE") continue;

            auto filename = get_filename(expand_source(pkgbuild.source[i], pkgbuild));
            auto filepath = srcdir / filename;

            if (!std::filesystem::exists(filepath)) continue;

            auto actual = compute_hash(filepath, algo);
            if (!actual.empty() && actual != expected) {
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

    terminal::success("Source checksums verified");
}

std::string SourceHandler::compute_hash(const std::filesystem::path& file,
                                         const std::string& algo) const {
    static const std::vector<std::string> allowed{"md5", "sha1", "sha224", "sha256", "sha384", "sha512", "b2"};
    if (std::find(allowed.begin(), allowed.end(), algo) == allowed.end())
        throw source_error("Unsupported checksum algorithm: " + algo);
    std::string output;
    if (run_process({algo + "sum", file.string()}, &output) != 0 || output.empty()) return {};

    // Output format: hash  filename
    auto space = output.find(' ');
    if (space != std::string::npos) return output.substr(0, space);
    return output;
}

} // namespace pacmkr::source
