#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <optional>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "pacmkr/build/pkgbuild.h"

// Inline PKGBUILD parser for testing (mirrors pkgbuild.cpp)

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string strip_quotes(const std::string& s) {
    auto t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                           (t.front() == '\'' && t.back() == '\''))) {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

std::vector<std::string> parse_array(const std::string& s) {
    auto t = trim(s);
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
        t = t.substr(1, t.size() - 2);
    } else {
        return {strip_quotes(t)};
    }

    std::vector<std::string> result;
    std::string current;
    bool in_quote = false;
    char qc = '\0';

    for (char ch : t) {
        if (!in_quote) {
            if (ch == '"' || ch == '\'') {
                in_quote = true;
                qc = ch;
            } else if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!current.empty()) { result.push_back(current); current.clear(); }
            } else {
                current += ch;
            }
        } else if (ch == qc) {
            in_quote = false;
        } else {
            current += ch;
        }
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

bool is_var_assignment(const std::string& line) {
    auto t = trim(line);
    if (t.empty() || t.front() == '#') return false;
    if (t.find("()") != std::string::npos) return false;
    if (starts_with(t, "export ") || starts_with(t, "local ") ||
        starts_with(t, "declare ") || starts_with(t, "if ") ||
        starts_with(t, "for ") || starts_with(t, "while ")) return false;

    auto eq = t.find('=');
    if (eq == std::string::npos) return false;

    auto key = trim(t.substr(0, eq));
    if (key.empty() || key.front() == '(' || key[0] == '_') return false;

    return true;
}

std::pair<std::string, std::string> parse_var(const std::string& line) {
    auto t = trim(line);
    auto eq = t.find('=');
    std::string key = trim(t.substr(0, eq));
    std::string value = trim(t.substr(eq + 1));
    return {key, value};
}

struct Pkgbuild {
    std::vector<std::string> pkgname;
    std::optional<std::string> pkgbase_opt;
    std::string pkgver{"0"}, pkgrel{"1"};
    std::string desc, url;
    std::vector<std::string> license, depends, makedepends, checkdepends, optdepends;
    std::vector<std::string> source;
    std::vector<std::string> sha256sums;

    std::string get_pkgbase() const {
        if (pkgbase_opt.has_value()) return *pkgbase_opt;
        if (!pkgname.empty()) return pkgname[0];
        return "unknown";
    }

    std::string full_version() const {
        if (pkgver == "0" || pkgver.empty()) {
            return get_pkgbase() + "-" + pkgrel;
        }
        return get_pkgbase() + "-" + pkgver + "-" + pkgrel;
    }
};

Pkgbuild parse_content(const std::string& content) {
    Pkgbuild pkg{};
    std::map<std::string, std::string> vars;

    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        auto t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        if (t.find("()") != std::string::npos && !starts_with(t, "package_")) continue;
        if (!is_var_assignment(t)) continue;

        auto [key, value] = parse_var(t);
        auto it = vars.find(key);
        if (it == vars.end()) {
            vars[key] = value;
        } else {
            it->second += ' ' + value;
        }
    }

    auto get_vec = [&](const std::string& key) -> std::vector<std::string> {
        auto it = vars.find(key);
        if (it == vars.end()) return {};
        auto v = it->second;
        if (v.size() > 0 && v[0] == '(') return parse_array(v);
        return {strip_quotes(v)};
    };

    auto get_str = [&](const std::string& key, const std::string& default_val = "") -> std::string {
        auto it = vars.find(key);
        if (it == vars.end()) return default_val;
        return strip_quotes(it->second);
    };

    auto pb_str = get_str("pkgbase");
    if (!pb_str.empty()) pkg.pkgbase_opt = pb_str;
    pkg.pkgname = get_vec("pkgname");
    pkg.pkgver = get_str("pkgver", "0");
    pkg.pkgrel = get_str("pkgrel", "1");
    pkg.desc = get_str("pkgdesc", "");
    pkg.url = get_str("url", "");
    pkg.license = get_vec("license");
    pkg.depends = get_vec("depends");
    pkg.makedepends = get_vec("makedepends");
    pkg.checkdepends = get_vec("checkdepends");
    pkg.optdepends = get_vec("optdepends");
    pkg.source = get_vec("source");
    pkg.sha256sums = get_vec("sha256sums");

    return pkg;
}

} // anonymous

// ─── Tests ───────────────────────────────────────────────────────────

void test_parse_simple_pkgbuild() {
    std::string content = R"(
pkgname='hello'
pkgver=1.0
pkgrel=1
pkgdesc='A hello world package'
url='https://example.com'
license=('GPL3')
depends=()
makedepends=()
source=("https://example.com/hello-${pkgver}.tar.gz")
sha256sums=('abc123')

build() {
    cd "$srcdir/$pkgname-$pkgver"
    ./configure
    make
}

package() {
    make DESTDIR="$pkgdir" install
}
)";

    auto pkg = parse_content(content);
    assert(pkg.pkgname.size() == 1 && pkg.pkgname[0] == "hello");
    assert(pkg.pkgver == "1.0");
    assert(pkg.pkgrel == "1");
    assert(pkg.desc == "A hello world package");
    assert(pkg.url == "https://example.com");
    assert(pkg.license.size() == 1 && pkg.license[0] == "GPL3");
    assert(pkg.depends.empty());
    assert(pkg.source.size() == 1);
    assert(pkg.sha256sums.size() == 1 && pkg.sha256sums[0] == "abc123");

    std::cout << "  PASSED: parse_simple_pkgbuild\n";
}

void test_parse_array() {
    assert(parse_array("('item1' 'item2')") == std::vector<std::string>{"item1", "item2"});
    assert(parse_array("(\"a\" \"b\" \"c\")") == std::vector<std::string>{"a", "b", "c"});
    assert(parse_array("('linux' 'arm64')") == std::vector<std::string>{"linux", "arm64"});

    std::cout << "  PASSED: parse_array\n";
}

void test_full_version() {
    Pkgbuild pkg;
    pkg.pkgname = {"foo"};
    pkg.pkgver = "1.0";
    pkg.pkgrel = "2";
    assert(pkg.full_version() == "foo-1.0-2");

    // When pkgver is 0 or empty, it's omitted from full_version
    pkg.pkgver = "0";
    assert(pkg.full_version() == "foo-2");

    std::cout << "  PASSED: full_version\n";
}

void test_pkgbase_from_pkgname() {
    Pkgbuild pkg;
    pkg.pkgname = {"my-package"};
    pkg.pkgver = "1.0";
    pkg.pkgrel = "1";
    assert(pkg.get_pkgbase() == "my-package");

    std::cout << "  PASSED: pkgbase_from_pkgname\n";
}

void test_real_parser_multiline_and_functions() {
    const auto parsed = pacmkr::pkgbuild::Pkgbuild::parse_content(R"(
pkgname=('one' 'two')
pkgver=3.2
pkgrel=4
_pkgname=code
source_x86_64=("code_${pkgver}_amd64.deb::https://example.test/code.deb")
sha256sums_x86_64=('arch-checksum')
source=(
  "https://example.test/${pkgname}-${pkgver}.tar.gz"
  'fix.patch'
)
depends=(
  'glibc'
  'zlib'
)
prepare() {
  depends=('must-not-leak')
}
package_one() { :; }
package_two() { :; }
)");
    assert((parsed.pkgname == std::vector<std::string>{"one", "two"}));
    assert((parsed.depends == std::vector<std::string>{"glibc", "zlib"}));
    assert(parsed.variables.at("_pkgname") == "code");
#if defined(__x86_64__)
    assert(parsed.source.size() == 3);
    assert(parsed.source.back().find("code_${pkgver}_amd64.deb") == 0);
    assert(parsed.sha256sums.back() == "arch-checksum");
#else
    assert(parsed.source.size() == 2);
#endif
    assert(parsed.has_function("prepare"));
    assert(parsed.has_function("package_one"));
    assert(parsed.pkgbase() == "one");
}

void test_file_parser_uses_bash_semantics() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("pacmkr-pkgbuild-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "PKGBUILD";
    {
        std::ofstream output(path);
        output << R"PKGBUILD(
_base=runtime
pkgname=("${_base}" "${_base}-docs")
pkgver="$(printf 2.4)"
pkgrel=$((1 + 1))
pkgdesc="${_base} package"
arch=('x86_64')
depends=('glibc')
if [[ $CARCH == x86_64 ]]; then depends+=('zlib>=1'); fi
source=("archive-${pkgver}.tar.gz")
source_x86_64=("binary-${CARCH}")
sha256sums=('SKIP')
sha256sums_x86_64=('SKIP')
build() { :; }
package_runtime() { :; }
package_runtime-docs() { :; }
pkgver() { printf '5.7.r3.gabc123'; }
)PKGBUILD";
    }

    const auto parsed = pacmkr::pkgbuild::Pkgbuild::parse(path);
    assert((parsed.pkgname == std::vector<std::string>{"runtime", "runtime-docs"}));
    assert(parsed.pkgver == "2.4");
    assert(parsed.pkgrel == "2");
    assert(parsed.desc == "runtime package");
    assert((parsed.depends == std::vector<std::string>{"glibc", "zlib>=1"}));
    assert((parsed.source == std::vector<std::string>{
        "archive-2.4.tar.gz", "binary-x86_64"}));
    assert(parsed.has_function("build"));
    assert(parsed.has_function("package_runtime-docs"));
    assert(parsed.packages.size() == 2);
    std::filesystem::create_directories(directory / "src");
    auto versioned = parsed;
    versioned.update_version(directory / "src");
    assert(versioned.pkgver == "5.7.r3.gabc123");
    assert(versioned.full_version() == "runtime-5.7.r3.gabc123-2");
    auto epoched = versioned;
    epoched.epoch = "2";
    assert(epoched.version() == "2:5.7.r3.gabc123-2");
    assert(epoched.full_version() == "runtime-2:5.7.r3.gabc123-2");

    const auto metadata_path = directory / "package-metadata";
    {
        std::ofstream metadata(metadata_path, std::ios::binary);
        const auto record = [&](const std::string& type, const std::string& name,
                                const std::string& value) {
            metadata.write(type.data(), static_cast<std::streamsize>(type.size()));
            metadata.put('\0');
            metadata.write(name.data(), static_cast<std::streamsize>(name.size()));
            metadata.put('\0');
            metadata.write(value.data(), static_cast<std::streamsize>(value.size()));
            metadata.put('\0');
        };
        record("S", "pkgdesc", "documentation split");
        record("S", "install", "runtime-docs.install");
        record("Z", "depends", "");
        record("A", "depends", "runtime=5.7.r3.gabc123");
    }
    versioned.merge_function_metadata(metadata_path);
    assert(versioned.desc == "documentation split");
    assert(versioned.install == "runtime-docs.install");
    assert((versioned.depends ==
            std::vector<std::string>{"runtime=5.7.r3.gabc123"}));
    assert((versioned.pkgname == std::vector<std::string>{"runtime", "runtime-docs"}));

    std::filesystem::remove_all(directory);
}

int main() {
    std::cout << "Running pkgbuild tests...\n";
    test_parse_simple_pkgbuild();
    test_parse_array();
    test_full_version();
    test_pkgbase_from_pkgname();
    test_real_parser_multiline_and_functions();
    test_file_parser_uses_bash_semantics();
    std::cout << "All pkgbuild tests passed.\n";
    return 0;
}
