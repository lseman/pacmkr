#include "pacmkr/core/optimize.h"
#include "pacmkr/core/error.h"

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <numeric>

namespace pacmkr::optimize {

namespace {

bool is_enabled(cli::OptMode mode) {
    return mode == cli::OptMode::Enabled;
}

bool mold_available() {
    return find_tool("mold").has_value();
}

} // anonymous

// ─── OptConfig ───────────────────────────────────────────────────────

bool OptConfig::compiler_supports_graphite(cli::Compiler c) {
    return c == cli::Compiler::Gcc;
}

bool OptConfig::compiler_supports_polly(cli::Compiler c) {
    return c == cli::Compiler::Clang;
}

/// Check if an optimization mode is active (explicitly enabled or auto + available).
static bool opt_active(cli::OptMode mode, bool available) {
    if (mode == cli::OptMode::Enabled) return true;
    if (mode == cli::OptMode::Disabled) return false;
    // Auto: enable only if the underlying tool is available.
    return available;
}

OptConfig OptConfig::from_cli(const cli::Cli& cli) {
    OptConfig cfg{};

    // Detect compiler
    if (cli.cc.has_value()) {
        cfg.compiler = *cli.cc;
    } else if (const char* cc_env = std::getenv("CC")) {
        std::string cc(cc_env);
        cfg.compiler = (cc.find("clang") != std::string::npos) ? cli::Compiler::Clang : cli::Compiler::Gcc;
    } else {
        cfg.compiler = cli::Compiler::Gcc;  // default
    }

    // Resolve graphite/polly based on compiler compatibility and availability.
    const bool graphite_avail = cfg.compiler == cli::Compiler::Gcc &&
                                find_tool("gcc").has_value();
    const bool polly_avail = cfg.compiler == cli::Compiler::Clang &&
                             find_tool("polly-opt").has_value();

    cfg.graphite = opt_active(cli.graphite, graphite_avail);
    cfg.polly = opt_active(cli.polly, polly_avail);

    if (cfg.graphite && cfg.polly) {
        std::cerr << "warning: graphite and polly are incompatible, disabling both\n";
        cfg.graphite = false;
        cfg.polly = false;
    }

    // LTO: auto-detect by checking if the compiler supports -flto=auto.
    cfg.lto = opt_active(cli.lto, find_tool(cfg.compiler == cli::Compiler::Gcc ? "gcc" : "clang").has_value());

    // Mold: enabled if explicitly set, auto, and tool available.
    cfg.mold = opt_active(cli.mold, mold_available());

    return cfg;
}

std::tuple<std::string, std::string, std::string> OptConfig::apply_flags() const {
    auto get_env = [](const char* var) -> std::string {
        auto* val = std::getenv(var);
        return val ? val : "";
    };

    return apply_flags_to(get_env("CFLAGS"), get_env("CXXFLAGS"), get_env("LDFLAGS"));
}

std::tuple<std::string, std::string, std::string> OptConfig::apply_flags_to(
    std::string cflags, std::string cxxflags, std::string ldflags) const {

    // Graphite optimizations (GCC polyhedral)
    if (graphite) {
        std::string gf = " -fgraphite-identity -floop-interchange -floop-nest-optimize"
                         " -ftree-loop-distribution -ftree-vectorize";
        cflags += gf;
        cxxflags += gf;
    }

    // Polly optimizations (Clang polyhedral)
    if (polly) {
        std::string pf = " -mllvm -polly";
        cflags += pf;
        cxxflags += pf;
        ldflags += " -mllvm -polly";
    }

    // LTO
    if (lto) {
        std::string lto_flag = (compiler == cli::Compiler::Gcc) ? "-flto=auto" : "-flto";
        cflags += " " + lto_flag;
        cxxflags += " " + lto_flag;
        ldflags += " " + lto_flag;
        // GCC needs the linker plugin for full LTO
        if (compiler == cli::Compiler::Gcc) {
            ldflags += " -fuse-linker-plugin";
        }
    }

    // Mold linker
    if (mold) {
        if (ldflags.find("-fuse-ld=") != std::string::npos) {
            size_t pos = ldflags.find("-fuse-ld=");
            auto end = ldflags.find(' ', pos);
            std::string before = ldflags.substr(0, pos);
            std::string after = (end != std::string::npos) ? ldflags.substr(end) : "";
            ldflags = before + " -fuse-ld=mold" + after;
        } else {
            ldflags += " -fuse-ld=mold";
        }
        // GDB index speeds up symbol lookup in large binaries
        ldflags += " -Wl,--gdb-index";
    }

    // Trim
    auto trim = [](std::string& s) {
        auto start = s.find_first_not_of(" \t");
        if (start != std::string::npos) s = s.substr(start);
        auto end = s.find_last_not_of(" \t");
        if (end != std::string::npos) s = s.substr(0, end + 1);
    };
    trim(cflags); trim(cxxflags); trim(ldflags);

    return {cflags, cxxflags, ldflags};
}

void OptConfig::apply_compiler_env() const {
    setenv("CC", compiler == cli::Compiler::Gcc ? "gcc" : "clang", 1);
    setenv("CXX", compiler == cli::Compiler::Gcc ? "g++" : "clang++", 1);
}

std::string OptConfig::summary() const {
    std::vector<std::string> parts;
    if (graphite) parts.push_back("graphite");
    if (polly) parts.push_back("polly");
    if (lto) parts.push_back("lto");
    if (mold) parts.push_back("mold");

    return parts.empty() ? "none" : std::accumulate(parts.begin(), parts.end(), std::string{},
        [](std::string a, const std::string& b) { return a.empty() ? b : a + "," + b; });
}

// ─── Tool lookup ─────────────────────────────────────────────────────

std::optional<std::string> find_tool(const std::string& name) {
    // Use which command
    std::ostringstream cmd;
    cmd << "which " << name << " 2>/dev/null";
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return std::nullopt;

    char buf[256];
    if (fgets(buf, sizeof(buf), pipe)) {
        std::string path(buf);
        auto end = path.find_last_of("\r\n");
        if (end != std::string::npos) path = path.substr(0, end);
        pclose(pipe);
        return path;
    }

    pclose(pipe);
    return std::nullopt;
}

} // namespace pacmkr::optimize
