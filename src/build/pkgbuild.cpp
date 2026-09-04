#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/core/error.h"

#include <fstream>
#include <algorithm>
#include <array>
#include <cerrno>
#include <map>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace pacmkr::pkgbuild {

namespace {

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
    if (t.find("()") != std::string::npos) return false;  // function def
    if (detail::starts_with(t, "export ") || detail::starts_with(t, "local ") ||
        detail::starts_with(t, "declare ") || detail::starts_with(t, "if ") ||
        detail::starts_with(t, "for ") || detail::starts_with(t, "while ")) return false;

    auto eq = t.find('=');
    if (eq == std::string::npos) return false;

    const auto k = trim(t.substr(0, eq));
    if (k.empty() || (!std::isalpha(static_cast<unsigned char>(k.front())) && k.front() != '_'))
        return false;
    if (!std::all_of(k.begin() + 1, k.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        })) return false;

    return true;
}

std::pair<std::string, std::string> parse_var(const std::string& line) {
    auto t = trim(line);
    auto eq = t.find('=');
    std::string key = trim(t.substr(0, eq));
    std::string value = trim(t.substr(eq + 1));
    return {key, value};
}

struct EvaluatedMetadata {
    std::map<std::string, std::string> scalars;
    std::map<std::string, std::vector<std::string>> arrays;
    std::vector<std::string> functions;
};

std::vector<std::string> nul_fields(const std::string& data) {
    std::vector<std::string> fields;
    size_t begin = 0;
    while (begin < data.size()) {
        const auto end = data.find('\0', begin);
        if (end == std::string::npos) break;
        fields.push_back(data.substr(begin, end - begin));
        begin = end + 1;
    }
    return fields;
}

EvaluatedMetadata evaluate_file(const std::filesystem::path& path) {
    // PKGBUILDs are Bash programs, not declarative files. This evaluator is
    // owned by pacmkr and emits a NUL-delimited protocol so ordinary quoting,
    // whitespace, substitutions, and arrays retain their Bash semantics.
    static constexpr char script[] = R"BASH(
set -e
unset BASH_ENV ENV CDPATH
export CARCH="${CARCH:-$(uname -m)}"
export startdir="$PWD"
export srcdir="$PWD/src"
export pkgdir="$PWD/pkg"
source "$1"

emit_scalar() {
    declare -p "$1" >/dev/null 2>&1 || return 0
    printf 'S\0%s\0%s\0' "$1" "${!1}" >&3
}
emit_array() {
    declare -p "$1" >/dev/null 2>&1 || return 0
    printf 'Z\0%s\0\0' "$1" >&3
    local -n values="$1"
    local value
    for value in "${values[@]}"; do
        printf 'A\0%s\0%s\0' "$1" "$value" >&3
    done
}

for name in pkgbase epoch pkgver pkgrel pkgdesc url install changelog; do emit_scalar "$name"; done
for name in pkgname arch license depends makedepends checkdepends optdepends \
            options backup conflicts provides replaces groups source noextract validpgpkeys \
            md5sums sha1sums sha224sums sha256sums sha384sums sha512sums b2sums; do
    emit_array "$name"
done
for prefix in source md5sums sha1sums sha224sums sha256sums sha384sums sha512sums b2sums; do
    emit_array "${prefix}_${CARCH}"
done
while read -r _ _ name; do
    case "$name" in
        prepare|build|check|package|pkgver|package_*) printf 'F\0%s\0\0' "$name" >&3 ;;
    esac
done < <(declare -F)
)BASH";

    int descriptors[2];
    if (pipe(descriptors) < 0) throw pkgbuild_error(0, "Cannot create PKGBUILD evaluator pipe");
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw pkgbuild_error(0, "Cannot create PKGBUILD evaluator process");
    }
    if (child == 0) {
        close(descriptors[0]);
        if (descriptors[1] != 3) {
            if (dup2(descriptors[1], 3) < 0) _exit(127);
            close(descriptors[1]);
        }
        if (chdir(path.parent_path().c_str()) != 0) _exit(127);
        unsetenv("BASH_ENV");
        unsetenv("ENV");
        execlp("bash", "bash", "--noprofile", "--norc", "-c", script, "pacmkr-eval",
               path.filename().c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(descriptors[1]);
    std::string output;
    std::array<char, 8192> buffer{};
    for (ssize_t count; (count = read(descriptors[0], buffer.data(), buffer.size())) > 0;)
        output.append(buffer.data(), static_cast<size_t>(count));
    close(descriptors[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw pkgbuild_error(0, "Bash evaluation failed: " + path.string());

    EvaluatedMetadata result;
    const auto fields = nul_fields(output);
    if (fields.size() % 3 != 0)
        throw pkgbuild_error(0, "Invalid response from PKGBUILD evaluator");
    for (size_t index = 0; index < fields.size(); index += 3) {
        const auto& type = fields[index];
        const auto& name = fields[index + 1];
        const auto& value = fields[index + 2];
        if (type == "S") result.scalars[name] = value;
        else if (type == "Z") result.arrays[name].clear();
        else if (type == "A") result.arrays[name].push_back(value);
        else if (type == "F") result.functions.push_back(name);
        else throw pkgbuild_error(0, "Unknown response from PKGBUILD evaluator");
    }
    return result;
}

void apply_evaluated(Pkgbuild& pkg, const EvaluatedMetadata& evaluated) {
    auto scalar = [&](const std::string& name, std::string& target) {
        if (const auto it = evaluated.scalars.find(name); it != evaluated.scalars.end()) {
            target = it->second;
            pkg.variables[name] = it->second;
        }
    };
    auto array = [&](const std::string& name, std::vector<std::string>& target) {
        if (const auto it = evaluated.arrays.find(name); it != evaluated.arrays.end())
            target = it->second;
    };

    if (const auto it = evaluated.scalars.find("pkgbase"); it != evaluated.scalars.end())
        pkg.pkgbase_opt = it->second;
    if (const auto it = evaluated.scalars.find("epoch"); it != evaluated.scalars.end())
        pkg.epoch = it->second;
    scalar("pkgver", pkg.pkgver);
    scalar("pkgrel", pkg.pkgrel);
    scalar("pkgdesc", pkg.desc);
    scalar("url", pkg.url);
    scalar("install", pkg.install);
    scalar("changelog", pkg.changelog);
    array("pkgname", pkg.pkgname);
    array("arch", pkg.arch);
    array("license", pkg.license);
    array("depends", pkg.depends);
    array("makedepends", pkg.makedepends);
    array("checkdepends", pkg.checkdepends);
    array("optdepends", pkg.optdepends);
    array("options", pkg.options);
    array("backup", pkg.backup);
    array("conflicts", pkg.conflicts);
    array("provides", pkg.provides);
    array("replaces", pkg.replaces);
    array("groups", pkg.groups);
    array("source", pkg.source);
    array("noextract", pkg.noextract);
    array("validpgpkeys", pkg.validpgpkeys);
    array("md5sums", pkg.md5sums);
    array("sha1sums", pkg.sha1sums);
    array("sha224sums", pkg.sha224sums);
    array("sha256sums", pkg.sha256sums);
    array("sha384sums", pkg.sha384sums);
    array("sha512sums", pkg.sha512sums);
    array("b2sums", pkg.b2sums);

    struct utsname system_info{};
    if (uname(&system_info) == 0) {
        const std::string suffix = "_" + std::string(system_info.machine);
        auto append = [&](const std::string& name, std::vector<std::string>& target) {
            if (const auto it = evaluated.arrays.find(name + suffix); it != evaluated.arrays.end())
                target.insert(target.end(), it->second.begin(), it->second.end());
        };
        append("source", pkg.source);
        append("md5sums", pkg.md5sums);
        append("sha1sums", pkg.sha1sums);
        append("sha224sums", pkg.sha224sums);
        append("sha256sums", pkg.sha256sums);
        append("sha384sums", pkg.sha384sums);
        append("sha512sums", pkg.sha512sums);
        append("b2sums", pkg.b2sums);
    }

    pkg.functions = evaluated.functions;
    pkg.packages.clear();
    for (const auto& function : pkg.functions) {
        if (detail::starts_with(function, "package_") && function.size() > 8)
            pkg.packages.push_back({function.substr(8), {}});
    }
}

std::string evaluate_pkgver(const std::filesystem::path& path,
                            const std::filesystem::path& srcdir) {
    static constexpr char script[] = R"BASH(
set -e
unset BASH_ENV ENV CDPATH
export CARCH="${CARCH:-$(uname -m)}"
export startdir="$PWD"
export srcdir="$2"
export pkgdir="$PWD/pkg"
source "$1"
cd "$srcdir"
pkgver
)BASH";

    int descriptors[2];
    if (pipe(descriptors) < 0) throw pkgbuild_error(0, "Cannot create pkgver evaluator pipe");
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw pkgbuild_error(0, "Cannot create pkgver evaluator process");
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);
        close(descriptors[1]);
        if (chdir(path.parent_path().c_str()) != 0) _exit(127);
        unsetenv("BASH_ENV");
        unsetenv("ENV");
        execlp("bash", "bash", "--noprofile", "--norc", "-c", script, "pacmkr-pkgver",
               path.filename().c_str(), srcdir.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(descriptors[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    for (ssize_t count; (count = read(descriptors[0], buffer.data(), buffer.size())) > 0;)
        output.append(buffer.data(), static_cast<size_t>(count));
    close(descriptors[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw pkgbuild_error(0, "pkgver() failed: " + path.string());

    output = trim(output);
    if (output.empty() || output.find('/') != std::string::npos ||
        std::any_of(output.begin(), output.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        throw pkgbuild_error(0, "pkgver() returned an invalid version: " + output);
    }
    return output;
}

} // anonymous

// ─── Pkgbuild ────────────────────────────────────────────────────────

Pkgbuild Pkgbuild::parse(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw pkgbuild_error(0, "Cannot read file: " + path.string());

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    auto parsed = parse_content(content);
    parsed.source_path = std::filesystem::absolute(path);
    apply_evaluated(parsed, evaluate_file(parsed.source_path));
    return parsed;
}

Pkgbuild Pkgbuild::parse_content(const std::string& content) {
    Pkgbuild pkg{};
    pkg.raw_content = content;

    std::map<std::string, std::string> vars;
    std::istringstream stream(content);
    std::string line;
    std::string pending;
    int function_depth = 0;
    auto delta = [](const std::string& value, char open, char close) {
        int result = 0;
        bool quoted = false;
        char quote = 0;
        for (size_t i = 0; i < value.size(); ++i) {
            const char c = value[i];
            if (quoted) {
                if (c == quote && (i == 0 || value[i - 1] != '\\')) quoted = false;
            } else if (c == '\'' || c == '"') { quoted = true; quote = c; }
            else if (c == open) ++result;
            else if (c == close) --result;
        }
        return result;
    };

    while (std::getline(stream, line)) {
        auto t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        if (function_depth > 0) {
            function_depth += delta(t, '{', '}');
            continue;
        }
        if (t.find("()") != std::string::npos && t.find('{') != std::string::npos) {
            function_depth = std::max(0, delta(t, '{', '}'));
            continue;
        }
        if (!pending.empty()) pending += " " + t;
        else if (is_var_assignment(t)) pending = t;
        else continue;
        const auto equals = pending.find('=');
        if (equals != std::string::npos && delta(pending.substr(equals + 1), '(', ')') > 0) continue;
        auto [key, value] = parse_var(pending);
        vars[key] = value;
        pending.clear();
    }

    // Extract fields
    for (const auto& [name, value] : vars) pkg.variables[name] = strip_quotes(value);

    auto get_vec = [&](const std::string& key) -> std::vector<std::string> {
        auto it = vars.find(key);
        if (it == vars.end()) return {};
        auto v = it->second;
        if (!v.empty() && v[0] == '(') return parse_array(v);
        return {strip_quotes(v)};
    };

    auto get_str = [&](const std::string& key, const std::string& default_val = "") -> std::string {
        auto it = vars.find(key);
        if (it == vars.end()) return default_val;
        return strip_quotes(it->second);
    };

    // pkgbase from pkgname if not set
    if (auto base = get_str("pkgbase"); !base.empty()) pkg.pkgbase_opt = std::move(base);
    if (auto epoch = get_str("epoch"); !epoch.empty()) pkg.epoch = std::move(epoch);
    pkg.pkgname = get_vec("pkgname");
    pkg.pkgver = get_str("pkgver", "0");
    pkg.pkgrel = get_str("pkgrel", "1");
    pkg.desc = get_str("pkgdesc", "");
    pkg.url = get_str("url", "");
    pkg.install = get_str("install", "");
    pkg.changelog = get_str("changelog", "");
    pkg.arch = get_vec("arch");
    pkg.license = get_vec("license");
    pkg.depends = get_vec("depends");
    pkg.makedepends = get_vec("makedepends");
    pkg.checkdepends = get_vec("checkdepends");
    pkg.optdepends = get_vec("optdepends");
    pkg.options = get_vec("options");
    pkg.backup = get_vec("backup");
    pkg.conflicts = get_vec("conflicts");
    pkg.provides = get_vec("provides");
    pkg.replaces = get_vec("replaces");
    pkg.groups = get_vec("groups");
    pkg.source = get_vec("source");
    pkg.noextract = get_vec("noextract");
    pkg.md5sums = get_vec("md5sums");
    pkg.sha256sums = get_vec("sha256sums");
    pkg.sha512sums = get_vec("sha512sums");
    pkg.b2sums = get_vec("b2sums");
    pkg.sha384sums = get_vec("sha384sums");
    pkg.sha224sums = get_vec("sha224sums");
    pkg.sha1sums = get_vec("sha1sums");
    pkg.validpgpkeys = get_vec("validpgpkeys");

    struct utsname system_info{};
    if (uname(&system_info) == 0) {
        const std::string suffix = "_" + std::string(system_info.machine);
        auto append = [&](std::vector<std::string>& target, const std::string& key) {
            auto values = get_vec(key + suffix);
            target.insert(target.end(), values.begin(), values.end());
        };
        append(pkg.source, "source");
        append(pkg.md5sums, "md5sums");
        append(pkg.sha1sums, "sha1sums");
        append(pkg.sha224sums, "sha224sums");
        append(pkg.sha256sums, "sha256sums");
        append(pkg.sha384sums, "sha384sums");
        append(pkg.sha512sums, "sha512sums");
        append(pkg.b2sums, "b2sums");
    }

    std::istringstream functions(content);
    while (std::getline(functions, line)) {
        const auto value = trim(line);
        if (!detail::starts_with(value, "package_") || value.find("()") == std::string::npos) continue;
        const auto end = value.find('(');
        pkg.packages.push_back({value.substr(8, end - 8), {}});
    }

    return pkg;
}

std::string Pkgbuild::pkgbase() const {
    if (pkgbase_opt.has_value()) return *pkgbase_opt;
    if (!pkgname.empty()) return pkgname[0];  // single pkgname: use it as pkgbase
    return "unknown";
}

std::string Pkgbuild::full_version() const {
    return pkgbase() + "-" + version();
}

std::string Pkgbuild::version() const {
    const auto version = (pkgver == "0" || pkgver.empty())
        ? pkgrel : pkgver + "-" + pkgrel;
    if (epoch.has_value() && !epoch->empty() && *epoch != "0")
        return *epoch + ":" + version;
    return version;
}

void Pkgbuild::update_version(const std::filesystem::path& srcdir) {
    if (!has_function("pkgver")) return;
    if (source_path.empty()) throw pkgbuild_error(0, "pkgver() requires a PKGBUILD file");
    pkgver = evaluate_pkgver(source_path, std::filesystem::absolute(srcdir));
    variables["pkgver"] = pkgver;
}

void Pkgbuild::merge_function_metadata(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw pkgbuild_error(0, "Cannot read package metadata: " + path.string());
    const std::string data((std::istreambuf_iterator<char>(input)), {});
    EvaluatedMetadata evaluated;
    const auto fields = nul_fields(data);
    if (fields.size() % 3 != 0)
        throw pkgbuild_error(0, "Invalid package metadata: " + path.string());
    for (size_t index = 0; index < fields.size(); index += 3) {
        const auto& type = fields[index];
        const auto& name = fields[index + 1];
        const auto& value = fields[index + 2];
        if (type == "S") evaluated.scalars[name] = value;
        else if (type == "Z") evaluated.arrays[name].clear();
        else if (type == "A") evaluated.arrays[name].push_back(value);
        else throw pkgbuild_error(0, "Unknown package metadata record");
    }
    const auto package_names = pkgname;
    const auto package_functions = functions;
    const auto package_sections = packages;
    apply_evaluated(*this, evaluated);
    pkgname = package_names;
    functions = package_functions;
    packages = package_sections;
}

bool Pkgbuild::has_function(const std::string& name) const {
    if (std::find(functions.begin(), functions.end(), name) != functions.end()) return true;
    std::istringstream input(raw_content);
    for (std::string line; std::getline(input, line);) {
        const auto value = trim(line);
        if (detail::starts_with(value, name)) {
            const auto rest = trim(value.substr(name.size()));
            if (detail::starts_with(rest, "()")) return true;
        }
    }
    // package_name() sections
    for (auto& p : packages) {
        if (p.name == name) return true;
    }
    return false;
}

} // namespace pacmkr::pkgbuild
