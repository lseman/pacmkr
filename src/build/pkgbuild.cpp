#include "pacmkr/pkgbuild.h"
#include "pacmkr/error.h"

#include <fstream>
#include <algorithm>

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

    auto key = t.substr(0, eq);
    // Trim and validate key
    auto k = trim(key);
    if (k.empty() || k.front() == '(' || k[0] == '_' || detail::starts_with(k, "_")) return false;

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
    return parse_content(content);
}

Pkgbuild Pkgbuild::parse_content(const std::string& content) {
    Pkgbuild pkg{};

    // Find package function ranges to skip during var extraction
    struct FuncRange { size_t start, end; };
    std::vector<FuncRange> func_ranges;
    {
        std::istringstream stream(content);
        std::string line;
        size_t line_idx = 0;
        while (std::getline(stream, line)) {
            auto t = trim(line);
            if (detail::starts_with(t, "package_") && t.find("()") != std::string::npos) {
                FuncRange r{line_idx, line_idx + 1};
                size_t brace_pos = t.rfind('{');
                int depth = 0;

                if (brace_pos != std::string::npos) {
                    for (size_t j = brace_pos; j < t.size(); ++j) {
                        if (t[j] == '{') depth++;
                        else if (t[j] == '}') depth--;
                    }
                }

                if (depth > 0 || (brace_pos != std::string::npos && !(trim(line).empty()) && trim(line).back() == '{')) {
                    for (size_t k = line_idx + 1; k < static_cast<size_t>(-1); ++k) {
                        if (k >= static_cast<size_t>(100000)) break;  // safety
                        // We need to re-read lines — let's use a different approach
                        break;
                    }
                }

                func_ranges.push_back(r);
            }
            ++line_idx;
        }
    }

    // Simpler approach: just parse variables, skipping function definitions
    std::map<std::string, std::string> vars;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        auto t = trim(line);

        // Skip empty lines and comments
        if (t.empty() || t.front() == '#') continue;

        // Skip function definitions (but not package_name())
        if (t.find("()") != std::string::npos && !detail::starts_with(t, "package_")) continue;

        // Parse variable assignments
        if (!is_var_assignment(t)) continue;

        auto [key, value] = parse_var(t);

        auto it = vars.find(key);
        if (it == vars.end()) {
            vars[key] = value;
        } else {
            // Array append: check if previous line also had no leading whitespace
            it->second += ' ' + value;
        }
    }

    // Extract fields
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
    pkg.pkgbase_opt = get_str("pkgbase");
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

    // Extract package() sections for split packages
    {
        std::istringstream s2(content);
        std::string l;
        size_t idx = 0;
        while (std::getline(s2, l)) {
            auto t = trim(l);
            if (!detail::starts_with(t, "package_")) { ++idx; continue; }
            if (t.find("()") == std::string::npos) { ++idx; continue; }

            // Extract name
            auto fp = t.substr(0, t.find('('));
            std::string name = trim(fp);
            if (detail::starts_with(name, "package_")) name = name.substr(8);

            // Find body
            PackageSection section{name};
            ++idx;
            bool found_open = false;
            for (; idx < static_cast<size_t>(-1) && !found_open; ++idx) {
                if (idx >= 100000) break;
                auto tl = trim(content);  // simplified — just grab what we can
                section.body += l + "\n";
                if (l.find('{') != std::string::npos) found_open = true;
            }

            pkg.packages.push_back(std::move(section));
        }
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
    // Check for standalone functions (build, check, prepare)
    if (name == "prepare" || name == "build" || name == "check") {
        return false;  // Would need to scan raw content — simplified
    }
    // package_name() sections
    for (auto& p : packages) {
        if (p.name == name) return true;
    }
    return false;
}

} // namespace pacmkr::pkgbuild
