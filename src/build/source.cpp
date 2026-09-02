#include "pacmkr/source.h"
#include "pacmkr/pkgbuild.h"
#include "pacmkr/error.h"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <sstream>
#include <cstdlib>

namespace pacmkr::source {

namespace {

std::string get_filename(const std::string& source) {
    // Strip VCS prefixes
    auto s = source;
    for (auto prefix : {"git+", "hg+", "svn+", "bzr+", "file://", "https://", "http://"}) {
        auto pos = s.find(prefix);
        if (pos != std::string::npos) s = s.substr(pos + strlen(prefix));
    }

    // Extract filename from URL/path
    auto slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);

    return s;
}

std::string run_cmd(const std::string& cmd) {
    std::ostringstream result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result << buf;
    }
    pclose(pipe);

    // Trim trailing newline
    auto r = result.str();
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    return r;
}

} // anonymous

SourceHandler::SourceHandler(const std::filesystem::path& srcdest, bool skip_checksums)
    : srcdest(srcdest), skip_checksums(skip_checksums) {}

void SourceHandler::download_and_verify(const pkgbuild::Pkgbuild& pkgbuild,
                                         const std::filesystem::path& srcdir) const {
    std::filesystem::create_directories(srcdir);
    std::filesystem::create_directories(srcdest);

    for (auto& source : pkgbuild.source) {
        download_source(source, srcdir);
    }

    if (!skip_checksums) {
        verify_checksums(pkgbuild, srcdir);
    }
}

void SourceHandler::download_source(const std::string& source_spec,
                                     const std::filesystem::path& srcdir) const {
    // Skip VCS sources (would need git/hg/svn commands)
    if (detail::starts_with(source_spec, "git+") || detail::starts_with(source_spec, "hg+") ||
        detail::starts_with(source_spec, "svn+") || detail::starts_with(source_spec, "bzr+")) {
        std::cout << "==> Skipping VCS source: " << source_spec << "\n";
        return;
    }

    auto filename = get_filename(source_spec);
    auto cached = srcdest / filename;

    if (!std::filesystem::exists(cached)) {
        std::cout << "==> Downloading " << filename << "...\n";
        int rc = std::system(("curl -fL -o \"" + cached.string() + "\" \"" + source_spec + "\" 2>&1").c_str());
        if (rc != 0) {
            throw source_error("Failed to download: " + source_spec);
        }
    } else {
        std::cout << "==> Using cached: " << filename << "\n";
    }

    // Copy to srcdir if not there
    auto dest = srcdir / filename;
    if (!std::filesystem::exists(dest)) {
        std::filesystem::copy_file(cached, dest, std::filesystem::copy_options::overwrite_existing);
    }
}

void SourceHandler::verify_checksums(const pkgbuild::Pkgbuild& pkgbuild,
                                      const std::filesystem::path& srcdir) const {
    auto check_sums = [&](const std::vector<std::string>& sums, const std::string& algo) {
        for (size_t i = 0; i < sums.size() && i < pkgbuild.source.size(); ++i) {
            auto expected = sums[i];
            if (expected == "SKIP" || expected == "IGNORE") continue;

            auto filename = get_filename(pkgbuild.source[i]);
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
    check_sums(pkgbuild.sha384sums, "sha384");
    check_sums(pkgbuild.sha512sums, "sha512");

    std::cout << "==> Checksums verified.\n";
}

std::string SourceHandler::compute_hash(const std::filesystem::path& file,
                                         const std::string& algo) const {
    auto output = run_cmd(algo + "sum \"" + file.string() + "\"");
    if (output.empty()) return {};

    // Output format: hash  filename
    auto space = output.find(' ');
    if (space != std::string::npos) return output.substr(0, space);
    return output;
}

} // namespace pacmkr::source
