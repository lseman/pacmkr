#pragma once

#include <string>
#include <vector>
#include <optional>

namespace pacmkr::hybrid_search {

/// Unified search result that can hold either a repo package or AUR package.
enum class Source { Repository, AUR };

struct Result {
    Source source;
    std::string name;
    std::string version;
    std::optional<std::string> desc;
    std::string origin_db;      // For repos: "core", "extra", etc.
    double relevance_score{0.0}; // Fuzzy match score (0.0 to 1.5+)
    unsigned int numvotes{0};    // For AUR packages
    double popularity{0.0};      // For AUR packages
};

/// Search both repositories and AUR, returning unified results sorted by relevance.
std::vector<Result> search(
    const std::string& query,
    unsigned int limit = 10);

} // namespace pacmkr::hybrid_search
