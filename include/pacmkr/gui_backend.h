#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

// Forward declarations for Slint-generated types
namespace pacmkr::gui {

/// Result from a package search (unified AUR + repo)
struct SearchResult {
    std::string name;
    std::string version;
    std::string origin;  // "aur" or repo name (core, extra, etc.)
    std::string description;
    bool is_out_of_date{false};
};

/// Build progress callback signature
using ProgressCallback = std::function<void(int percent, const std::string& status)>;

/// Log output callback signature
using LogCallback = std::function<void(const std::string& line)>;

class Backend {
public:
    Backend();
    ~Backend();

    // Non-copyable, non-movable (manages threads and callbacks)
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // ─── Search Operations ────────────────────────────────────────

    /// Search for packages (AUR + repositories)
    std::vector<SearchResult> search(const std::string& query, unsigned int limit = 20);

    /// Refresh AUR cache
    void refresh_cache();

    // ─── Build Operations ─────────────────────────────────────────

    /// Start building packages (runs in background thread)
    void start_build(const std::vector<SearchResult>& targets,
                     ProgressCallback progress_cb,
                     LogCallback log_cb);

    /// Cancel ongoing build
    void cancel_build();

    /// Check if a build is currently in progress
    bool is_building() const;

    // ─── Package Info ─────────────────────────────────────────────

    /// Get detailed package information
    SearchResult get_package_info(const std::string& name);

    /// Get list of out-of-date packages (AUR only)
    std::vector<SearchResult> get_out_of_date_packages();

private:
    // Background build worker
    void build_worker(const std::vector<SearchResult>& targets,
                      ProgressCallback progress_cb,
                      LogCallback log_cb);

    // State
    std::atomic<bool> building_{false};
    std::thread build_thread_;
};

} // namespace pacmkr::gui
