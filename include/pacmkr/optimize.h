#pragma once

#include "pacmkr/cli.h"
#include <string>
#include <optional>

namespace pacmkr::optimize {

struct OptConfig {
    bool graphite{false};
    bool polly{false};
    bool lto{false};
    bool mold{false};
    cli::Compiler compiler{cli::Compiler::Gcc};

    static OptConfig from_cli(const cli::Cli& cli);

    /// Apply optimization flags. Returns (CFLAGS, CXXFLAGS, LDFLAGS).
    std::tuple<std::string, std::string, std::string> apply_flags() const;

    /// Set CC/CXX environment variables.
    void apply_compiler_env() const;

    /// Summary string for .BUILDINFO.
    std::string summary() const;

    static bool compiler_supports_graphite(cli::Compiler c);
    static bool compiler_supports_polly(cli::Compiler c);
};

/// Check if a tool exists in PATH.
std::optional<std::string> find_tool(const std::string& name);

} // namespace pacmkr::optimize
