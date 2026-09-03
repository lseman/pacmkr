#pragma once

#include <string>
#include <vector>
#include <utility>

namespace pacmkr::fuzzy {

/// Compute Levenshtein edit distance between two strings.
/// Returns the minimum number of single-character edits (insertions, deletions,
/// or substitutions) required to change one string into the other.
int levenshtein_distance(const std::string& a, const std::string& b);

/// Score a fuzzy match between query and text.
/// Higher score = better match. Scoring factors:
///   - Exact substring match (highest weight)
///   - Fuzzy match with Levenshtein distance (scaled by string length)
///   - Position bonus: matches at start of string score higher
///   - Multi-word query: each word must match separately
struct MatchScore {
    double score;        // 0.0 to 1.0+ (can exceed 1.0 for strong matches)
    bool matched;        // true if any scoring threshold was met
};

MatchScore score_match(const std::string& query, const std::string& text);

/// Search a vector of (name, desc) pairs with fuzzy matching.
/// Returns top N results sorted by relevance score.
struct FuzzyResult {
    std::string name;
    std::string desc;
    double score;
};
std::vector<FuzzyResult> search_fuzzy(
    const std::vector<std::pair<std::string, std::string>>& items,
    const std::string& query,
    unsigned int limit = 10);

} // namespace pacmkr::fuzzy
