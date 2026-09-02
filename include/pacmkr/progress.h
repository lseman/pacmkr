#pragma once

#include <string>
#include <chrono>
#include <vector>

namespace pacmkr::progress {

/// Build phases for the progress bar.
enum class Phase {
    Downloading,   // Fetching sources
    Verifying,     // Checking checksums
    Configuring,   // Running prepare()
    Building,      // Running build()
    Packaging,     // Running package()
    Installing,    // pacman -U
    Cleaning,      // Cleanup after build
};

/// Convert phase to a human-readable label.
const char* phase_label(Phase p);

/// Convert phase to an emoji/icon.
const char* phase_icon(Phase p);

/// A compact, animated progress bar in pacman style.
/// Usage:
///   ProgressBar pb("package-name", 100);
///   pb.set_phase(Phase::Building);
///   pb.update(25);
///   // ... after some work
///   pb.update(75);
///   // ... done
///   pb.finish();
class ProgressBar {
public:
    /// Create a progress bar for `total` steps.
    explicit ProgressBar(std::string package_name, int total = 100);
    ~ProgressBar();

    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

    /// Set the current phase.
    void set_phase(Phase p);

    /// Update progress to a specific value (0–100 or absolute steps).
    /// If `absolute` is true, treat `value` as an absolute step count.
    void update(int value, bool absolute = false);

    /// Advance by a number of steps.
    void advance(int steps = 1);

    // ─── Byte-level tracking (for downloads) ───────────────────────

    /// Enable byte-level progress tracking.
    /// `current_bytes` is how much has been transferred so far.
    /// `total_bytes` is the expected total; set to 0 for unknown.
    void set_bytes(uint64_t current_bytes, uint64_t total_bytes = 0);

    /// Format bytes into human-readable string (e.g., "2.4 MiB").
    static std::string format_bytes(uint64_t bytes);
    void set_status(std::string msg);

    /// Mark progress as complete and clear the bar.
    void finish();

    /// Cancel/abort the progress bar.
    void cancel();

    /// Check if the bar has been finished/cancelled.
    bool done() const { return state_.done; }

    /// Force a redraw (useful when external output occurs).
    void redraw();

    // Static helpers for ANSI terminal control
    static void ansi_erase_line();
    static void ansi_move_up(int n);
    static void ansi_show_cursor();
    static void ansi_hide_cursor();
    static std::string gradient_color(double t);  // t in [0, 1]

private:
    void render();
    std::string compute_eta() const;
    std::string compute_speed() const;
    std::string format_elapsed() const;
    static std::string format_bytes_internal(uint64_t bytes);

    struct State {
        std::string package_name;
        int total = 100;
        int current = 0;
        Phase phase = Phase::Building;
        std::string status;
        bool done = false;
        bool cancelled = false;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point last_update_time;

        // Byte-level tracking (optional, for downloads)
        uint64_t bytes_current = 0;
        uint64_t bytes_total = 0;
    } state_;

    bool rendered_ = false;
    int bar_width_ = 30;
};

/// A multi-progress tracker for building multiple packages.
/// Shows a list of packages with individual mini-bars.
class MultiProgress {
public:
    explicit MultiProgress(std::string title);

    /// Add a package to track. Returns an index.
    int add_package(std::string name);

    /// Update progress for a specific package.
    void update(int pkg_index, int value);

    /// Mark a package as complete.
    void complete(int pkg_index);

    /// Set the currently building package (highlights it).
    void set_active(int pkg_index);

    /// Set a global status message (shown at bottom).
    void set_status(std::string msg);

    /// Finish all and clear.
    void finish();

private:
    struct PackageState {
        std::string name;
        int progress = 0;
        bool completed = false;
        std::string status;
    };

    void render();

    std::string title_;
    std::vector<PackageState> packages_;
    int active_index_ = -1;
    std::string global_status_;
};

// ─── Convenience Functions ───────────────────────────────────────────

/// Create a simple spinner animation for quick operations.
/// Returns the frame index (caller should increment and redraw).
int animated_spinner(const char* text, int frame);

/// Print a success message with a checkmark.
void print_success(const std::string& msg);

/// Print a warning message with a warning icon.
void print_warning(const std::string& msg);

/// Print an error message with an X icon.
void print_error(const std::string& msg);

} // namespace pacmkr::progress
