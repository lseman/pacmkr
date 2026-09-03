#pragma once

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

// C++17 compatibility: starts_with is C++20
namespace pacmkr::detail {
inline bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

/// URL-encode a string (only encode non-safe characters).
inline std::string url_encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}
} // namespace pacmkr::detail

namespace pacmkr {

/// Base exception for all pacmkr errors.
class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// PKGBUILD parsing failure.
class pkgbuild_error : public error {
public:
    pkgbuild_error(int line, std::string reason)
        : error("PKGBUILD parse error at line " + std::to_string(line) + ": " + reason),
          line_(line), reason_(std::move(reason)) {}

    int line() const noexcept { return line_; }
    const char* reason() const noexcept { return reason_.c_str(); }

private:
    int line_{};
    std::string reason_;
};

/// Required tool not found in PATH.
class missing_tool_error : public error {
public:
    explicit missing_tool_error(std::string name)
        : error("Missing required tool: " + name), name_(std::move(name)) {}
    const char* tool() const noexcept { return name_.c_str(); }

private:
    std::string name_;
};

/// External command failed.
class command_error : public error {
public:
    command_error(std::string cmd, int exit_code)
        : error("Command failed: " + cmd + " (exit code: " + std::to_string(exit_code) + ")"),
          cmd_(std::move(cmd)), exit_code_(exit_code) {}

    const char* command() const noexcept { return cmd_.c_str(); }
    int exit_code() const noexcept { return exit_code_; }

private:
    std::string cmd_;
    int exit_code_{};
};

/// Checksum verification failure.
class checksum_error : public error {
public:
    checksum_error(std::string file, std::string expected, std::string actual)
        : error("Checksum mismatch for " + file + ": expected " + expected + ", got " + actual),
          file_(std::move(file)), expected_(std::move(expected)), actual_(std::move(actual)) {}

    const char* file() const noexcept { return file_.c_str(); }
    const char* expected() const noexcept { return expected_.c_str(); }
    const char* actual() const noexcept { return actual_.c_str(); }

private:
    std::string file_, expected_, actual_;
};

/// Dependency resolution failure.
class dependency_error : public error {
public:
    explicit dependency_error(std::string msg) : error("Dependency resolution failed: " + msg) {}
};

/// Package creation failure.
class package_error : public error {
public:
    explicit package_error(std::string msg) : error("Package assembly error: " + msg) {}
};

/// Configuration file error.
class config_error : public error {
public:
    explicit config_error(std::string msg) : error("Configuration error: " + msg) {}
};

/// Invalid optimization combination.
class opt_error : public error {
public:
    explicit opt_error(std::string msg) : error("Invalid optimization combination: " + msg) {}
};

/// Source download / network failure.
class source_error : public error {
public:
    explicit source_error(std::string msg) : error("Source download failed: " + msg) {}
};

/// libalpm error.
class alpm_error : public error {
public:
    explicit alpm_error(std::string msg) : error("libalpm error: " + msg) {}
};

} // namespace pacmkr
