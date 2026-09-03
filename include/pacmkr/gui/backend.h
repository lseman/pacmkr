#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

namespace pacmkr::gui {

/// Individual search result row (exposed to UI frameworks)
struct SearchResultItem {
    SearchResultItem();
    ~SearchResultItem();

    std::string const& get_name() const;
    void set_name(std::string const&);

    std::string const& get_version() const;
    void set_version(std::string const&);

    std::string const& get_origin() const;
    void set_origin(std::string const&);

    std::string const& get_description() const;
    void set_description(std::string const&);

    bool get_is_out_of_date() const;
    void set_is_out_of_date(bool);

private:
    std::string name_;
    std::string version_;
    std::string origin_;
    std::string description_;
    bool is_out_of_date_{false};
};

/// Result from a package search (unified AUR + repo)
struct SearchResult {
    std::string name;
    std::string version;
    std::string origin;  // "aur" or repo name (core, extra, etc.)
    std::string description;
    bool is_out_of_date{false};
};

struct AvailableUpdate {
    std::string name;
    std::string installed_version;
    std::string available_version;
    std::string origin;
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
    std::vector<SearchResult> search(std::string const& query, unsigned int limit = 20);

    /// Refresh AUR cache
    void refresh_cache();

    // ─── Build Operations ─────────────────────────────────────────

    /// Start building packages (runs in background thread)
    void start_build(std::vector<SearchResult> const& targets,
                     ProgressCallback progress_cb,
                     LogCallback log_cb);

    /// Cancel ongoing build
    void cancel_build();

    /// Check if a build is currently in progress
    bool is_building() const;

    // ─── Package Info ─────────────────────────────────────────────

    /// Get detailed package information
    SearchResult get_package_info(std::string const& name);

    /// Get list of out-of-date packages (AUR only)
    std::vector<SearchResult> get_out_of_date_packages();

    /// Compare installed packages with the currently configured sync databases.
    std::vector<AvailableUpdate> get_available_updates();

    /// Run an approved full system upgrade. This may open sudo in the caller's terminal.
    int apply_system_updates();

private:
    // Background build worker
    void build_worker(std::vector<SearchResult> const& targets,
                      ProgressCallback progress_cb,
                      LogCallback log_cb);

    // State
    std::atomic<bool> building_{false};
    std::thread build_thread_;
};

} // namespace pacmkr::gui
