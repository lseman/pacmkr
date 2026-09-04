#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pacmkr::aur_review {

/// A reviewable change to one checked-out AUR Git repository.
struct Review {
    std::filesystem::path repository;
    std::string package;
    std::optional<std::string> previous_commit;
    std::string commit;
    std::vector<std::string> changed_files;
    std::vector<std::string> warnings;
    std::string patch;

    bool already_reviewed() const noexcept {
        return previous_commit.has_value() && *previous_commit == commit;
    }
};

/// Inspect all tracked changes since the last commit approved by pacmkr.
/// On first use, the complete contents introduced by the current commit are
/// returned. This function never sources or otherwise executes repository data.
Review inspect(const std::filesystem::path& repository);

/// Display the complete review and ask for explicit approval. `skip_review`
/// is an explicit trust-policy override; --noconfirm deliberately does not
/// bypass this prompt.
bool confirm(const Review& review, bool skip_review);

/// Persist the exact commit approved by the user in the repository-local Git
/// configuration. Call only after explicit approval.
void mark_reviewed(const Review& review);

} // namespace pacmkr::aur_review
