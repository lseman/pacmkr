#include "pacmkr/app/terminal.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace pacmkr::terminal {
namespace {
std::mutex output_mutex;

// True while an interactive progress line has been drawn but not yet
// terminated with a newline. Any other output must break the line first so it
// does not land appended to the half-drawn bar. Guarded by output_mutex.
bool progress_line_open = false;

const char* cyan()  { return interactive() ? "\033[1;36m" : ""; }
const char* green() { return interactive() ? "\033[1;32m" : ""; }
const char* yellow(){ return interactive() ? "\033[1;33m" : ""; }
const char* dim()   { return interactive() ? "\033[2m" : ""; }
const char* reset() { return interactive() ? "\033[0m" : ""; }

// Move off an open progress line (clearing it) so the caller can print on a
// fresh line. Must be called with output_mutex held.
void end_progress_line() {
    if (!progress_line_open) return;
    std::cout << "\r\033[2K" << std::flush;
    progress_line_open = false;
}
}

bool interactive() { return isatty(STDOUT_FILENO) != 0; }

void title(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    if (!interactive()) { std::cout << "==> " << text << "\n"; return; }
    end_progress_line();
    std::cout << "\n" << cyan() << "  ◆ " << text << reset() << "\n";
}

void section(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    if (!interactive()) { std::cout << "==> " << text << "\n"; return; }
    end_progress_line();
    std::cout << "\n" << cyan() << ":: " << reset() << text << "\n";
}

void info(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    if (!interactive()) { std::cout << "==> " << text << "\n"; return; }
    end_progress_line();
    std::cout << dim() << "   → " << reset() << text << "\n";
}

void success(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    if (!interactive()) { std::cout << "==> " << text << "\n"; return; }
    end_progress_line();
    std::cout << green() << "   ✓ " << reset() << text << "\n";
}

void warning(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    if (!interactive()) { std::cerr << "warning: " << text << "\n"; return; }
    end_progress_line();
    std::cerr << yellow() << "warning: " << reset() << text << "\n";
}

void progress(const std::string& label, int percent, std::size_t current,
              std::size_t total, bool done, const std::string& suffix) {
    std::lock_guard<std::mutex> lock(output_mutex);
    percent = std::max(0, std::min(100, percent));
    if (!interactive()) {
        if (done) std::cout << "   " << label << ": " << (suffix.empty() ? "done" : suffix) << "\n";
        return;
    }
    // Fixed columns so every bar lines up regardless of label length:
    //    <label:24>  [<bar:26>] <pct:4>  <detail>
    constexpr int label_col = 24;
    constexpr int bar_width = 26;

    std::string name = label;
    if (name.size() > static_cast<std::size_t>(label_col))
        name = name.substr(0, label_col - 1) + "…";

    const int filled = percent * bar_width / 100;
    std::string bar(filled, '=');
    if (filled < bar_width) bar += ">" + std::string(bar_width - filled - 1, ' ');

    std::string detail = suffix;
    if (detail.empty() && total) {
        std::ostringstream count;
        count << current << "/" << total;
        detail = count.str();
    }

    std::cout << "\r\033[2K   " << std::left << std::setw(label_col) << name
              << "  " << cyan() << "[" << bar << "]" << reset()
              << " " << std::right << std::setw(3) << percent << "%";
    if (!detail.empty()) std::cout << "  " << dim() << detail << reset();
    if (done) std::cout << "\n";
    std::cout << std::flush;
    progress_line_open = !done;
}
} // namespace pacmkr::terminal
