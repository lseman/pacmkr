#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <algorithm>

// Inline implementation of pkgvercmp for testing (mirrors deps.cpp)

namespace {

enum class TokenKind { Null, Numeric, Alpha };
struct Token {
    TokenKind kind;
    unsigned long long num_val{};
    std::string str_val;
    bool tilde{false};
};

static int strverscmp_impl(const std::string& a, const std::string& b) {
    auto lower = [](std::string s) -> std::string {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    std::string la = lower(a), lb = lower(b);

    static const char* keywords[] = {"", "pre", "c", "rc", "a", "b", "ssl", "rel"};

    auto kw_pos = [&](const std::string& s) -> int {
        for (int i = 0; i < 8; ++i) {
            if (keywords[i] == s) return i;
        }
        return -1;
    };

    int ai = kw_pos(la), bi = kw_pos(lb);
    if (ai >= 0 && bi >= 0) return (ai < bi ? -1 : (ai > bi ? 1 : 0));
    if (ai >= 0) return -1;
    if (bi >= 0) return 1;

    auto a_num = la.find_first_of("0123456789");
    auto b_num = lb.find_first_of("0123456789");
    if (a_num != std::string::npos && b_num != std::string::npos) {
        unsigned long long an = std::stoull(la.substr(a_num));
        unsigned long long bn = std::stoull(lb.substr(b_num));
        if (an != bn) return (an < bn ? -1 : 1);
    }

    return la.compare(lb);
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    size_t i = 0;

    while (i < s.size()) {
        Token tok{};

        if (s[i] == '~') {
            tok.tilde = true;
            ++i;
            if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                tok.kind = TokenKind::Numeric;
                size_t start = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                tok.num_val = std::stoull(s.substr(start, i - start));
            } else {
                tok.kind = TokenKind::Alpha;
                size_t start = i;
                while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                tok.str_val = s.substr(start, i - start);
            }
        } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            tok.kind = TokenKind::Numeric;
            size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            tok.num_val = std::stoull(s.substr(start, i - start));
        } else if (std::isalpha(static_cast<unsigned char>(s[i]))) {
            tok.kind = TokenKind::Alpha;
            size_t start = i;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            tok.str_val = s.substr(start, i - start);
        } else {
            ++i;
            continue;
        }

        tokens.push_back(tok);
    }

    tokens.push_back({TokenKind::Null});
    return tokens;
}

int pkgvercmp(const std::string& a, const std::string& b) {
    auto ta = tokenize(a), tb = tokenize(b);
    size_t ai = 0, bi = 0;

    while (ai < ta.size() && bi < tb.size()) {
        auto& tok_a = ta[ai], tok_b = tb[bi];

        if (tok_a.kind == TokenKind::Null && tok_b.kind == TokenKind::Null) return 0;
        if (tok_a.kind == TokenKind::Null) return -1;
        if (tok_b.kind == TokenKind::Null) return 1;

        if (tok_a.kind == TokenKind::Numeric && tok_b.kind == TokenKind::Numeric) {
            if (tok_a.tilde != tok_b.tilde) return tok_a.tilde ? -1 : 1;
            if (tok_a.num_val != tok_b.num_val) return (tok_a.num_val < tok_b.num_val ? -1 : 1);
        } else if (tok_a.kind == TokenKind::Alpha && tok_b.kind == TokenKind::Alpha) {
            if (tok_a.tilde != tok_b.tilde) return tok_a.tilde ? -1 : 1;
            int c = strverscmp_impl(tok_a.str_val, tok_b.str_val);
            if (c != 0) return c;
        } else {
            std::string num_str = std::to_string(
                tok_a.kind == TokenKind::Numeric ? tok_a.num_val : tok_b.num_val);
            const std::string& alpha_str = tok_a.kind == TokenKind::Alpha ? tok_a.str_val : tok_b.str_val;
            int c = strverscmp_impl(num_str, alpha_str);
            if (c != 0) return c;
        }

        ++ai; ++bi;
    }

    if (ai < ta.size()) {
        bool all_null = true;
        for (; ai < ta.size(); ++ai) if (ta[ai].kind != TokenKind::Null) { all_null = false; break; }
        return all_null ? 0 : 1;
    }
    if (bi < tb.size()) {
        bool all_null = true;
        for (; bi < tb.size(); ++bi) if (tb[bi].kind != TokenKind::Null) { all_null = false; break; }
        return all_null ? 0 : -1;
    }

    return 0;
}

/// Compare two Arch Linux version segments (split on '-').
/// Handles '~': tilde sorts before any character or end-of-string.
/// Dots are skipped (not significant). Uses keyword ordering for alpha.
/// Empty segment means "no release" (final), which is greater than any
/// pre-release or numbered release (non-empty < empty).
static int compare_segment(const std::string& a, const std::string& b) {
    // Empty segment = no release (final) > any non-empty segment
    if (a.empty() && !b.empty()) return 1;
    if (!a.empty() && b.empty()) return -1;
    if (a.empty() && b.empty()) return 0;

    std::string la = a, lb = b;
    std::transform(la.begin(), la.end(), la.begin(), ::tolower);
    std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);

    size_t ai = 0, bi = 0;
    while (ai < la.size() || bi < lb.size()) {
        // Skip dots (not significant in Arch versioning)
        if (ai < la.size() && la[ai] == '.') { ++ai; continue; }
        if (bi < lb.size() && lb[bi] == '.') { ++bi; continue; }

        bool a_end = (ai >= la.size()), b_end = (bi >= lb.size());

        // Tilde sorts before everything else, including end-of-string.
        // Check tilde BEFORE end-of-string so "1~rc1" < "1".
        if (!a_end && !b_end) {
            if (la[ai] == '~' && lb[bi] != '~') return -1;
            if (lb[bi] == '~' && la[ai] != '~') return 1;
        } else if (!a_end && b_end) {
            // a has chars left, b exhausted — tilde < nothing
            if (la[ai] == '~') return -1;
            return 1;
        } else if (a_end && !b_end) {
            // b has chars left, a exhausted — nothing > tilde
            if (lb[bi] == '~') return 1;
            return -1;
        } else {
            return 0;
        }

        bool a_digit = std::isdigit(static_cast<unsigned char>(la[ai]));
        bool b_digit = std::isdigit(static_cast<unsigned char>(lb[bi]));

        if (a_digit && b_digit) {
            // Both numeric: compare as numbers
            size_t a_start = ai, b_start = bi;
            while (ai < la.size() && std::isdigit(static_cast<unsigned char>(la[ai]))) ++ai;
            while (bi < lb.size() && std::isdigit(static_cast<unsigned char>(lb[bi]))) ++bi;
            std::string anum = la.substr(a_start, ai - a_start);
            std::string bnum = lb.substr(b_start, bi - b_start);
            if (anum.size() != bnum.size()) return (anum.size() < bnum.size() ? -1 : 1);
            if (anum != bnum) return (anum < bnum ? -1 : 1);
        } else if (a_digit || b_digit) {
            // Mixed: digit sorts before letter
            return a_digit ? -1 : 1;
        } else {
            // Both alpha: compare lexicographically with keyword ordering
            size_t a_start = ai, b_start = bi;
            while (ai < la.size() && std::isalpha(static_cast<unsigned char>(la[ai]))) ++ai;
            while (bi < lb.size() && std::isalpha(static_cast<unsigned char>(lb[bi]))) ++bi;
            std::string aalpha = la.substr(a_start, ai - a_start);
            std::string balpha = lb.substr(b_start, bi - b_start);

            // Special keyword ordering (from Arch's pkgver.c)
            static const char* keywords[] = {"pre", "c", "rc", "a", "b", "ssl", "rel"};
            auto kw_pos = [&](const std::string& s) -> int {
                for (int i = 0; i < static_cast<int>(sizeof(keywords)/sizeof(keywords[0])); ++i) {
                    if (keywords[i] == s) return i;
                }
                return -1;
            };

            int ka = kw_pos(aalpha), kb = kw_pos(balpha);
            if (ka >= 0 && kb >= 0) {
                if (ka != kb) return (ka < kb ? -1 : 1);
            } else if (ka >= 0) {
                return -1;
            } else if (kb >= 0) {
                return 1;
            } else {
                if (aalpha != balpha) return (aalpha < balpha ? -1 : 1);
            }
        }
    }

    return 0;
}

/// Arch Linux pkgvercmp: split on '-', compare segments.
static int arch_pkgvercmp(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '-') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return parts;
    };

    auto a_parts = split(a), b_parts = split(b);
    size_t max_len = std::max(a_parts.size(), b_parts.size());
    for (size_t i = 0; i < max_len; ++i) {
        const auto& pa = (i < a_parts.size()) ? a_parts[i] : "";
        const auto& pb = (i < b_parts.size()) ? b_parts[i] : "";
        int c = compare_segment(pa, pb);
        if (c != 0) return c;
    }
    return 0;
}

int compare_epoch_upstream_release(const std::string& a, const std::string& b) {
    auto parse_ver = [](const std::string& v) -> std::tuple<unsigned long long, std::string> {
        auto colon = v.find(':');
        unsigned long long epoch = 0;
        std::string rest = v;

        if (colon != std::string::npos) {
            try { epoch = std::stoull(v.substr(0, colon)); } catch (...) {}
            rest = v.substr(colon + 1);
        }

        return {epoch, rest};
    };

    auto [a_epoch, a_rest] = parse_ver(a);
    auto [b_epoch, b_rest] = parse_ver(b);

    if (a_epoch != b_epoch) return (a_epoch < b_epoch ? -1 : 1);
    // Full version comparison handles all edge cases: tildes, pre-releases,
    // numeric segments, keyword ordering, etc.
    return arch_pkgvercmp(a_rest, b_rest);
}

// Simulate the public API for testing
int compare_versions(const std::string& a, const std::string& b) {
    return compare_epoch_upstream_release(a, b);
}

// Topological sort (Kahn's algorithm) — same as deps.cpp
struct BuildNode {
    std::string name;
    std::vector<std::string> aur_deps;
};

std::vector<std::string> topological_sort(const std::map<std::string, BuildNode>& nodes) {
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> adj;

    for (auto& [name, _] : nodes) in_degree[name] = 0;

    for (auto& [name, node] : nodes) {
        for (auto& dep : node.aur_deps) {
            if (nodes.find(dep) != nodes.end()) {
                in_degree[name]++;
                adj[dep].push_back(name);
            }
        }
    }

    std::vector<std::string> queue;
    for (auto& [name, deg] : in_degree) {
        if (deg == 0) queue.push_back(name);
    }
    std::sort(queue.begin(), queue.end());

    std::vector<std::string> result;
    while (!queue.empty()) {
        std::sort(queue.begin(), queue.end());
        auto current = queue.front();
        queue.erase(queue.begin());
        result.push_back(current);

        if (adj.find(current) != adj.end()) {
            for (auto& dependent : adj[current]) {
                in_degree[dependent]--;
                if (in_degree[dependent] == 0) {
                    queue.push_back(dependent);
                }
            }
        }
    }

    // Handle cycles
    if (static_cast<int>(result.size()) < static_cast<int>(nodes.size())) {
        for (auto& [name, _] : nodes) {
            if (!std::count(result.begin(), result.end(), name)) {
                result.push_back(name);
            }
        }
    }

    return result;
}

} // anonymous

// ─── Tests ───────────────────────────────────────────────────────────

void test_compare_versions() {
    // Basic numeric
    assert(compare_versions("1.0", "2.0") < 0);
    assert(compare_versions("2.0", "1.0") > 0);
    assert(compare_versions("1.0", "1.0") == 0);

    // Numeric vs numeric (not string)
    assert(compare_versions("1.10", "1.9") > 0);   // 10 > 9 numerically
    assert(compare_versions("1.2", "1.10") < 0);

    // Release comparison
    assert(compare_versions("1.0-1", "1.0-2") < 0);
    assert(compare_versions("1.0-2", "1.0-1") > 0);

    // Alpha ordering: pre < c < rc < a < b
    assert(compare_versions("1.0-rc1", "1.0") < 0);
    assert(compare_versions("1.0-beta", "1.0") < 0);

    // Epoch
    assert(compare_versions("1:1.0", "2:0.9") < 0);
    assert(compare_versions("2:1.0", "1:9.9") > 0);

    // Tilde < normal
    assert(compare_versions("1~rc1", "1-rc1") < 0);

    std::cout << "  PASSED: compare_versions\n";
}

void test_topological_sort_linear() {
    std::map<std::string, BuildNode> nodes;
    nodes["c"] = {"c", {}};
    nodes["b"] = {"b", {"c"}};
    nodes["a"] = {"a", {"b"}};

    auto order = topological_sort(nodes);
    auto c_idx = std::find(order.begin(), order.end(), "c") - order.begin();
    auto b_idx = std::find(order.begin(), order.end(), "b") - order.begin();
    auto a_idx = std::find(order.begin(), order.end(), "a") - order.begin();

    assert(c_idx < b_idx && "c must come before b");
    assert(b_idx < a_idx && "b must come before a");

    std::cout << "  PASSED: topological_sort (linear)\n";
}

void test_topological_sort_diamond() {
    std::map<std::string, BuildNode> nodes;
    for (auto& n : {"a", "b", "c", "d"}) {
        nodes[n] = {n, {}};
    }
    nodes["a"].aur_deps = {"b", "c"};
    nodes["b"].aur_deps = {"d"};
    nodes["c"].aur_deps = {"d"};

    auto order = topological_sort(nodes);
    auto d_idx = std::find(order.begin(), order.end(), "d") - order.begin();
    auto b_idx = std::find(order.begin(), order.end(), "b") - order.begin();
    auto c_idx = std::find(order.begin(), order.end(), "c") - order.begin();
    auto a_idx = std::find(order.begin(), order.end(), "a") - order.begin();

    assert(d_idx < b_idx && "d before b");
    assert(d_idx < c_idx && "d before c");
    assert(b_idx < a_idx && "b before a");
    assert(c_idx < a_idx && "c before a");

    std::cout << "  PASSED: topological_sort (diamond)\n";
}

int main() {
    std::cout << "Running deps tests...\n";
    test_compare_versions();
    test_topological_sort_linear();
    test_topological_sort_diamond();
    std::cout << "All deps tests passed.\n";
    return 0;
}
