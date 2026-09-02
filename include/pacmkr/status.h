#pragma once

#include <string>
#include <vector>

namespace pacmkr::status {

/// Print a themed banner/header for the upgrade flow.
void print_upgrade_banner();

/// Print a "starting" message with personality.
void print_starting(const std::string& action);

/// Print a success/completion message.
void print_success(const std::string& msg, int count = 0);

/// Print a separator line.
void print_separator(char c = '-', int width = 50);

/// Print a section header with icon.
void print_section(const std::string& title);

/// Print the final summary after upgrade.
void print_summary(int repo_count, int aur_count, bool everything_up_to_date);

/// Print a status line (like "==> ..." but prettier).
void print_status(const std::string& msg);

/// Print a warning message.
void print_warning(const std::string& msg);

} // namespace pacmkr::status
