#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/core/error.h"

#include <fstream>
#include <algorithm>
#include <sys/utsname.h>

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

bool is_function_def(const std::string& line) {
    // Match: funcname() or funcname () — but not package_name()
    auto t = trim(line);
    if (t.find("()") == std::string::npos) return false;
    if (detail::starts_with(t, "package_")) return false;  // split packages are data, not functions to skip
    // Must have () in it and no assignment
    return true;
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

/// Find package function bodies (package_name() { ... }) and their line ranges.
struct FuncRange { size_t start, end; };

std::vector<FuncRange> find_package_functions(const std::string& content) {
    std::vector<FuncRange> ranges;
    std::vector<std::string> lines;

    // Split into lines with indices
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        auto t = trim(lines[i]);
        // Match package_name() or package_NAME()
        if (!detail::starts_with(t, "package_")) continue;
        if (t.find("()") == std::string::npos) continue;

        // Find opening brace
        size_t brace_pos = t.rfind('{');
        if (brace_pos == std::string::npos) {
            // Brace might be on next line
            if (i + 1 < lines.size()) {
                auto nt = trim(lines[i + 1]);
                if (nt.find('{') != std::string::npos) {
                    ++i;  // skip to brace line
                } else continue;
            } else continue;
        }

        FuncRange range{static_cast<size_t>(-1), static_cast<size_t>(-1)};
        int depth = 0;

        for (size_t j = brace_pos; j < lines[i].size(); ++j) {
            if (lines[i][j] == '{') depth++;
            else if (lines[i][j] == '}') depth--;
        }

        range.start = i;
        if (depth <= 0 && !(trim(lines[i]).size() > 0 && trim(lines[i]).back() == '{')) {
            range.end = i + 1;
        } else {
            for (size_t k = i + 1; k < lines.size(); ++k) {
                for (char ch : lines[k]) {
                    if (ch == '{') depth++;
                    else if (ch == '}') depth--;
                }
                range.end = k + 1;
                if (depth <= 0) break;
            }
        }

        // Extract name from function signature
        auto func_part = t.substr(0, t.find('('));
        auto name = trim(func_part);
        if (detail::starts_with(name, "package_")) {
            name = name.substr(8);
        }

        // Store body
        std::string body;
        for (size_t k = range.start; k < range.end && k < lines.size(); ++k) {
            body += lines[k] + "\n";
        }

        // We don't store the ranges here, we need them for var extraction
        // Store in a separate structure
        ranges.push_back(range);
    }

    return ranges;
}

bool overlaps(size_t line, const std::vector<FuncRange>& ranges) {
    for (auto& r : ranges) {
        if (line >= r.start && line < r.end) return true;
    }
    return false;
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
    pkg.pkgname = get_vec("pkgname");
    pkg.pkgver = get_str("pkgver", "0");
    pkg.pkgrel = get_str("pkgrel", "1");
    pkg.desc = get_str("pkgdesc", "");
    pkg.url = get_str("url", "");
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
    if (pkgver == "0" || pkgver.empty()) {
        return pkgbase() + "-" + pkgrel;
    }
    return pkgbase() + "-" + pkgver + "-" + pkgrel;
}

bool Pkgbuild::has_function(const std::string& name) const {
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
