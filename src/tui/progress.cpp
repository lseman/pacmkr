#include "pacmkr/progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace pacmkr::progress {

// ─── Phase Labels & Icons ────────────────────────────────────────────

const char* phase_label(Phase p) {
    switch (p) {
        case Phase::Downloading:   return "Downloading";
        case Phase::Verifying:     return "Verifying";
        case Phase::Configuring:   return "Configuring";
        case Phase::Building:      return "Building";
        case Phase::Packaging:     return "Packaging";
        case Phase::Installing:    return "Installing";
        case Phase::Cleaning:      return "Cleaning";
    }
    return "Unknown";
}

const char* phase_icon(Phase p) {
    switch (p) {
        case Phase::Downloading:   return "\xe2\x86\xb3";  // ↓
        case Phase::Verifying:     return "\xe2\x9c\x93";  // ✓
        case Phase::Configuring:   return "\xe2\x9a\x99";  // ⚙
        case Phase::Building:      return "\xf0\x9f\x94\xa5";  // 🔥
        case Phase::Packaging:     return "\xf0\x9f\x93\xa6";  // 🦦 (package)
        case Phase::Installing:    return "\xe2\x86\x91";  // ↑
        case Phase::Cleaning:      return "\xe2\x9b\xbb";  // ♻
    }
    return "";
}

// ─── ANSI Helpers ────────────────────────────────────────────────────

void ProgressBar::ansi_erase_line() {
    std::cerr << "\033[2K\r";  // Erase entire line, move to start
}

void ProgressBar::ansi_move_up(int n) {
    std::cerr << "\033[" << n << "A";
}

void ProgressBar::ansi_show_cursor() {
    std::cerr << "\033[?25h";
}

void ProgressBar::ansi_hide_cursor() {
    std::cerr << "\033[?25l";
}

// ─── Gradient Color Calculation ──────────────────────────────────────

std::string ProgressBar::gradient_color(double t) {
    t = std::max(0.0, std::min(1.0, t));

    int r, g, b;

    if (t < 0.25) {
        // Cyan → Blue (0.0 - 0.25)
        double s = t / 0.25;
        r = static_cast<int>(0 + s * 0);
        g = static_cast<int>(238 * (1 - s * 0.3));
        b = static_cast<int>(255 * (1 - s * 0.15));
    } else if (t < 0.5) {
        // Blue → Purple (0.25 - 0.5)
        double s = (t - 0.25) / 0.25;
        r = static_cast<int>(0 + s * 128);
        g = static_cast<int>(168 * (1 - s * 0.5));
        b = static_cast<int>(217 + s * 38);
    } else if (t < 0.75) {
        // Purple → Pink (0.5 - 0.75)
        double s = (t - 0.25) / 0.25;
        r = static_cast<int>(128 + s * 127);
        g = static_cast<int>(84 * (1 - s));
        b = static_cast<int>(255 * (1 - s * 0.35));
    } else {
        // Pink → Warm Red (0.75 - 1.0)
        double s = (t - 0.75) / 0.25;
        r = static_cast<int>(255);
        g = static_cast<int>(84 * (1 - s * 0.6));
        b = static_cast<int>(166 * (1 - s));
    }

    std::ostringstream oss;
    oss << "\033[38;2;" << r << ";" << g << ";" << b << "m";
    return oss.str();
}

// ─── Byte Formatting ─────────────────────────────────────────────────

std::string ProgressBar::format_bytes_internal(uint64_t bytes) {
    if (bytes == 0) return "0 B";

    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }

    std::ostringstream oss;
    if (size >= 100.0) {
        oss << std::fixed << std::setprecision(0) << size << " " << units[unit];
    } else if (size >= 10.0) {
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    } else {
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    }
    return oss.str();
}

std::string ProgressBar::format_bytes(uint64_t bytes) {
    return format_bytes_internal(bytes);
}

// ─── Elapsed Time Formatting ─────────────────────────────────────────

std::string ProgressBar::format_elapsed() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - state_.start_time).count();

    int hours = static_cast<int>(elapsed / 3600);
    int mins = static_cast<int>((elapsed % 3600) / 60);
    int secs = static_cast<int>(elapsed % 60);

    if (hours > 0) {
        std::ostringstream oss;
        oss << hours << ":"
            << std::setfill('0') << std::setw(2) << mins << ":"
            << std::setfill('0') << std::setw(2) << secs;
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << mins << ":"
            << std::setfill('0') << std::setw(2) << secs;
        return oss.str();
    }
}

// ─── ETA & Speed Computation ─────────────────────────────────────────

std::string ProgressBar::compute_eta() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - state_.start_time).count();

    if (elapsed == 0 || state_.current == 0) return "--:--";

    double progress_ratio = static_cast<double>(state_.current) / state_.total;
    if (progress_ratio <= 0) return "--:--";

    double total_estimated = elapsed / progress_ratio;
    double remaining = total_estimated - elapsed;

    if (remaining > 3600) {
        int hours = static_cast<int>(remaining / 3600);
        int mins = static_cast<int>(std::fmod(remaining, 3600) / 60);
        return std::to_string(hours) + "h " + std::to_string(mins) + "m";
    } else if (remaining > 60) {
        int mins = static_cast<int>(remaining / 60);
        int secs = static_cast<int>(std::fmod(remaining, 60));
        return std::to_string(mins) + "m " + std::to_string(secs) + "s";
    } else {
        return std::to_string(static_cast<int>(remaining)) + "s";
    }
}

std::string ProgressBar::compute_speed() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state_.last_update_time).count();

    if (elapsed_ms == 0) return "";

    // Prefer byte-level speed if available
    if (state_.bytes_total > 0 && state_.bytes_current > 0) {
        static uint64_t last_bytes = 0;
        uint64_t delta = state_.bytes_current - last_bytes;
        last_bytes = state_.bytes_current;

        double rate = (delta * 1000.0) / std::max(1.0, static_cast<double>(elapsed_ms));
        return format_bytes_internal(static_cast<uint64_t>(rate)) + "/s";
    }

    // Fallback: steps per second
    static int last_current = 0;
    int delta = state_.current - last_current;
    last_current = state_.current;

    double rate = (delta * 1000.0) / std::max(1.0, static_cast<double>(elapsed_ms));
    return std::to_string(static_cast<int>(rate)) + "/s";
}

// ─── Phase-Specific Bar Characters ───────────────────────────────────

/// Returns the filled block character for the current phase.
static const char* phase_filled_char(Phase p) {
    switch (p) {
        case Phase::Downloading:   return "\xe2\x96\x88";  // ■ full block
        case Phase::Verifying:     return "\xe2\x9c\x85";  // ✓ checkmark (smaller, use ░ for empty)
        case Phase::Configuring:   return "\xe2\x9a\x99";  // ⚙ gear
        case Phase::Building:      return "\xf0\x9f\x94\xa5";  // 🔥 fire
        case Phase::Packaging:     return "\xe2\x96\xb6";  // ◶ rhombus
        case Phase::Installing:    return "\xe2\x86\x91";  // ↑ arrow up
        case Phase::Cleaning:      return "\xe2\x9b\xbb";  // ♻ recycle
    }
    return "\xe2\x96\x88";  // fallback: full block
}

/// Returns the empty block character for the current phase.
static const char* phase_empty_char(Phase p) {
    switch (p) {
        case Phase::Verifying:     return "\xe2\x98\xaf";  // ☯ yin-yang (subtle empty)
        case Phase::Cleaning:      return "\xe2\x8b\xa1";  // ⋡ not parallel (subtle empty)
        default:                   return "\xe2\x96\x91";  // ░ light block
    }
}

/// Returns the partial-fill edge character for the current phase.
static const char* phase_edge_char(Phase p, double frac) {
    switch (p) {
        case Phase::Downloading:
            // Smooth gradient edge: ▁▂▃▄▅▆▇
            if (frac > 0.875) return "\xe2\x96\x81";  // ▁
            if (frac > 0.625) return "\xe2\x96\x82";  // ▂
            if (frac > 0.375) return "\xe2\x96\x83";  // ▃
            return "\xe2\x96\x84";                       // ▄
        case Phase::Building:
            // Blocky edge for compilation steps
            if (frac > 0.5) return "\xe2\x96\x8c";  // ◌ half block right
            return "\xe2\x96\x8e";                     // ◎ quarter block right
        case Phase::Verifying:
            // Dashed edge for verification
            if (frac > 0.5) return "╌";               // ╌ dash
            return "┄";                                 // ┄ light dash
        default:
            // Generic smooth edge
            if (frac > 0.875) return "\xe2\x96\x81";
            if (frac > 0.625) return "\xe2\x96\x82";
            if (frac > 0.375) return "\xe2\x96\x83";
            return "\xe2\x96\x84";
    }
}

// ─── ProgressBar Implementation ──────────────────────────────────────

ProgressBar::ProgressBar(std::string package_name, int total)
    : state_{std::move(package_name), total, 0, Phase::Building, "", false, false,
             std::chrono::steady_clock::now(), std::chrono::steady_clock::now()} {
    if (isatty(STDERR_FILENO)) ansi_hide_cursor();
}

ProgressBar::~ProgressBar() {
    // Always restore the cursor if an exception or early return interrupts a bar.
    if (isatty(STDERR_FILENO)) ansi_show_cursor();
}

void ProgressBar::set_phase(Phase p) {
    state_.phase = p;
    render();
}

void ProgressBar::update(int value, bool absolute) {
    if (state_.done) return;

    if (absolute) {
        state_.current = std::max(0, std::min(value, state_.total));
    } else {
        int new_current = static_cast<int>(std::round(state_.total * value / 100.0));
        state_.current = std::max(0, std::min(new_current, state_.total));
    }

    state_.last_update_time = std::chrono::steady_clock::now();
    render();
}

void ProgressBar::advance(int steps) {
    if (state_.done) return;
    state_.current = std::max(0, std::min(state_.current + steps, state_.total));
    state_.last_update_time = std::chrono::steady_clock::now();
    render();
}

void ProgressBar::set_bytes(uint64_t current_bytes, uint64_t total_bytes) {
    if (state_.done) return;

    state_.bytes_current = current_bytes;
    if (total_bytes > 0) {
        state_.bytes_total = total_bytes;
        // Sync step-based progress with byte progress
        double ratio = static_cast<double>(current_bytes) / total_bytes;
        state_.current = static_cast<int>(std::round(state_.total * ratio));
    }

    state_.last_update_time = std::chrono::steady_clock::now();
    render();
}

void ProgressBar::set_status(std::string msg) {
    state_.status = std::move(msg);
    render();
}

void ProgressBar::finish() {
    state_.current = state_.total;
    render();
    state_.done = true;

    std::cerr << "\n";
    if (isatty(STDERR_FILENO)) ansi_show_cursor();
    rendered_ = false;
}

void ProgressBar::cancel() {
    state_.cancelled = true;
    state_.done = true;
    if (isatty(STDERR_FILENO)) {
        ansi_erase_line();
        std::cerr << "\033[33m[CANCELLED]\033[0m\n";
        ansi_show_cursor();
    } else {
        std::cerr << "==> [cancelled] " << state_.package_name << "\n";
    }
    rendered_ = false;
}

void ProgressBar::redraw() {
    render();
}

// ─── Compact Single-Line Render (Pacman Style) ──────────────────────

void ProgressBar::render() {
    if (state_.done) return;

    // Non-TTY: simple text output
    if (!isatty(STDERR_FILENO)) {
        int pct = static_cast<int>(std::round(100.0 * state_.current / state_.total));
        std::cerr << "==> [" << state_.package_name << "] "
                  << phase_icon(state_.phase) << " " << phase_label(state_.phase)
                  << " " << pct << "%\n";
        return;
    }

    double ratio = static_cast<double>(state_.current) / state_.total;
    int filled = static_cast<int>(std::round(bar_width_ * ratio));
    int empty = bar_width_ - filled;

    // Move up to overwrite previous line
    if (rendered_) {
        ansi_move_up(1);
    }

    // ─── Pacman-style compact single line ──────────────────────────
    // Format:  [████████░░] 2.4 MiB/s  00:12  75%

    ansi_erase_line();

    // Opening bracket
    std::string line = "  \033[90m[\033[0m";

    // Filled portion with gradient
    for (int i = 0; i < filled; ++i) {
        double t = static_cast<double>(i) / bar_width_;
        line += gradient_color(t);

        if (i == filled - 1) {
            // Edge character with partial fill
            double frac = (bar_width_ * ratio) - filled;
            if (frac > 0.01 && frac < 0.99) {
                line += phase_edge_char(state_.phase, frac);
            } else {
                line += phase_filled_char(state_.phase);
            }
        } else {
            line += phase_filled_char(state_.phase);
        }
    }
    line += "\033[0m";

    // Empty portion
    for (int i = 0; i < empty; ++i) {
        line += "\033[90m";
        line += phase_empty_char(state_.phase);
        line += "\033[0m";
    }

    // Closing bracket
    line += "\033[90m]\033[0m";

    // ─── Info fields (right-aligned) ───────────────────────────────

    // Speed (byte-level or step-based)
    std::string speed = compute_speed();
    if (!speed.empty()) {
        line += "  \033[36m" + speed + "\033[0m";
    }

    // Elapsed time
    auto elapsed = format_elapsed();
    line += "  \033[90m" + elapsed + "\033[0m";

    // Percentage (bold, right)
    int pct = static_cast<int>(std::round(ratio * 100));
    line += "  \033[1m" + std::to_string(pct) + "%\033[0m";

    // Phase-specific status suffix
    if (!state_.status.empty()) {
        line += "  \033[90m── \033[37m" + state_.status + "\033[0m";
    }

    // Byte info for downloads (when total is known)
    if (state_.bytes_total > 0 && state_.phase == Phase::Downloading) {
        line += "  \033[90m(";
        line += format_bytes_internal(state_.bytes_current);
        line += "/";
        line += format_bytes_internal(state_.bytes_total);
        line += ")\033[0m";
    }

    std::cerr << line;
    std::cerr.flush();

    rendered_ = true;
}

// ─── MultiProgress Implementation (Pacman-style compact) ─────────────

MultiProgress::MultiProgress(std::string title) : title_(std::move(title)), active_index_(-1) {
    if (isatty(STDERR_FILENO)) {
        ProgressBar::ansi_hide_cursor();
    }
}

int MultiProgress::add_package(std::string name) {
    packages_.push_back({std::move(name), 0, false});
    return static_cast<int>(packages_.size()) - 1;
}

void MultiProgress::update(int pkg_index, int value) {
    if (pkg_index < 0 || pkg_index >= static_cast<int>(packages_.size())) return;
    packages_[pkg_index].progress = std::max(0, std::min(value, 100));
    render();
}

void MultiProgress::complete(int pkg_index) {
    if (pkg_index < 0 || pkg_index >= static_cast<int>(packages_.size())) return;
    packages_[pkg_index].progress = 100;
    packages_[pkg_index].completed = true;
    render();
}

void MultiProgress::set_active(int pkg_index) {
    active_index_ = pkg_index;
    render();
}

void MultiProgress::set_status(std::string msg) {
    global_status_ = std::move(msg);
    render();
}

void MultiProgress::render() {
    if (!isatty(STDERR_FILENO)) return;

    size_t total_lines = packages_.size() + 2;

    for (size_t i = 0; i < total_lines; ++i) {
        ProgressBar::ansi_move_up(1);
        ProgressBar::ansi_erase_line();
    }
    std::cerr << "\r";

    // Title line
    std::cerr << "  \033[1m\033[36m" << title_ << "\033[0m\n";

    // Package lines (compact pacman-style)
    for (size_t i = 0; i < packages_.size(); ++i) {
        auto& pkg = packages_[i];

        // Status indicator
        if (static_cast<int>(i) == active_index_) {
            std::cerr << "  \033[36m▶\033[0m ";
        } else if (pkg.completed) {
            std::cerr << "  \033[32m✓\033[0m ";
        } else {
            std::cerr << "  \033[90m·\033[0m ";
        }

        // Package name (bold if active)
        if (static_cast<int>(i) == active_index_) {
            std::cerr << "\033[1m" << pkg.name << "\033[0m";
        } else {
            std::cerr << pkg.name;
        }

        // Compact mini progress bar (20 chars)
        int filled = static_cast<int>(std::round(20.0 * pkg.progress / 100.0));
        std::cerr << " [\033[90m";
        for (int j = 0; j < filled; ++j) {
            std::cerr << "\033[0m" << ProgressBar::gradient_color(static_cast<double>(j) / 20);
            std::cerr << "■";
        }
        if (filled < 20 && pkg.progress < 100) {
            double frac = (20.0 * pkg.progress / 100.0) - filled;
            if (frac > 0.75) std::cerr << "\033[0m▁";
            else if (frac > 0.5) std::cerr << "\033[0m▂";
            else if (frac > 0.25) std::cerr << "\033[0m▃";
            else std::cerr << "\033[0m▄";
        }
        for (int j = filled; j < 20; ++j) {
            std::cerr << "░";
        }
        std::cerr << "\033[90m]\033[0m";

        // Percentage
        std::cerr << " \033[90m" << pkg.progress << "%\033[0m";
        std::cerr << "\n";
    }

    // Global status line
    if (!global_status_.empty()) {
        std::cerr << "  \033[90m── \033[37m" << global_status_ << "\033[0m\n";
    }

    std::cerr.flush();
}

void MultiProgress::finish() {
    for (auto& p : packages_) {
        p.progress = 100;
        p.completed = true;
    }
    render();
    if (isatty(STDERR_FILENO)) {
        ProgressBar::ansi_show_cursor();
    }
}

// ─── Spinner Animation ───────────────────────────────────────────────

int animated_spinner(const char* text, int frame) {
    static const char* frames[] = {
        "\xe2\x87\xbf",  // ⟿
        "\xe2\x86\xbb",  // ↻
        "\xe2\x87\xbb",  // ⟻
        "\xe2\x86\xba",  // ↺
    };
    static const int num_frames = 4;

    if (!isatty(STDERR_FILENO)) {
        std::cerr << "==> " << text << "...\n";
        return 0;
    }

    std::ostringstream oss;
    oss << "\r  \033[36m" << frames[frame % num_frames] << "\033[0m"
        << " \033[1m" << text << "\033[0m";
    std::cerr << oss.str();
    std::cerr.flush();

    return frame + 1;
}

// ─── Convenience Print Functions ─────────────────────────────────────

void print_success(const std::string& msg) {
    if (!isatty(STDERR_FILENO)) {
        std::cerr << "[OK] " << msg << "\n";
        return;
    }
    std::cerr << "  \033[32m\xe2\x9c\x85\033[0m \033[32m" << msg << "\033[0m\n";
}

void print_warning(const std::string& msg) {
    if (!isatty(STDERR_FILENO)) {
        std::cerr << "[WARN] " << msg << "\n";
        return;
    }
    std::cerr << "  \033[33m\xe2\x9a\xa0\033[0m \033[33m" << msg << "\033[0m\n";
}

void print_error(const std::string& msg) {
    if (!isatty(STDERR_FILENO)) {
        std::cerr << "[ERROR] " << msg << "\n";
        return;
    }
    std::cerr << "  \033[31m\xe2\x9c\x97\033[0m \033[31m" << msg << "\033[0m\n";
}

} // namespace pacmkr::progress
