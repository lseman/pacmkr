#include "pacmkr/core/fuzzy_search.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <sstream>

namespace pacmkr::fuzzy {

namespace {

/// Convert string to lowercase.
static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

/// Check if character is alphanumeric or underscore (word boundary).
static bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// Split query into words, handling common patterns like "foo-bar" -> ["foo", "bar"]
static std::vector<std::string> split_words(const std::string& query) {
    std::vector<std::string> words;
    std::string current;
    
    for (char c : query) {
        if (is_word_char(c)) {
            current += c;
        } else {
            if (!current.empty()) {
                words.push_back(to_lower(current));
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        words.push_back(to_lower(current));
    }
    
    return words;
}

/// Compute Levenshtein distance using optimized space O(min(m,n)) algorithm.
static int levenshtein_dist(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    
    // Early exit: if one string is empty, distance is the other's length
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);
    
    // Use two rows instead of full matrix for space efficiency
    std::vector<int> prev(n + 1), curr(n + 1);
    
    // Initialize first row
    for (size_t j = 0; j <= n; ++j) {
        prev[j] = static_cast<int>(j);
    }
    
    // Fill matrix row by row
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] != b[j - 1]) ? 1 : 0;
            curr[j] = std::min({
                prev[j] + 1,           // deletion
                curr[j - 1] + 1,       // insertion
                prev[j - 1] + cost     // substitution
            });
        }
        std::swap(prev, curr);
    }
    
    return prev[n];
}

/// Check if query is a substring of text (case-insensitive).
static bool contains_substring(const std::string& query, const std::string& text) {
    return text.find(query) != std::string::npos;
}

/// Score an exact substring match with position bonus.
static double score_substring(const std::string& query, const std::string& text) {
    size_t pos = text.find(query);
    if (pos == std::string::npos) return 0.0;
    
    // Base score from length ratio (shorter queries relative to text score higher)
    double base_score = 1.0 - (static_cast<double>(query.size()) / static_cast<double>(text.size() + query.size()));
    
    // Position bonus: matches at start score higher
    double position_bonus = (pos == 0) ? 0.3 : std::max(0.0, 0.2 - (pos * 0.01));
    
    return std::min(1.5, base_score + position_bonus);
}

/// Score a fuzzy match using Levenshtein distance, normalized by string length.
static double score_fuzzy(const std::string& query, const std::string& text) {
    // Only attempt fuzzy matching if strings are similar in length (within 50%)
    double len_ratio = static_cast<double>(std::min(query.size(), text.size())) / 
                       static_cast<double>(std::max(query.size(), text.size()));
    
    if (len_ratio < 0.5) return 0.0;  // Too different lengths
    
    int dist = levenshtein_distance(to_lower(query), to_lower(text));
    double max_dist = std::max(query.size(), text.size());
    
    // Normalize distance to score: 1.0 for identical, 0.0 for completely different
    double normalized_score = 1.0 - (static_cast<double>(dist) / max_dist);
    
    // Apply minimum threshold: require at least 60% similarity
    if (normalized_score < 0.6) return 0.0;
    
    // Position bonus for matches starting at beginning
    double position_bonus = 0.0;
    if (!text.empty() && !query.empty()) {
        size_t match_len = std::min(query.size(), text.size());
        bool prefix_match = true;
        for (size_t i = 0; i < match_len; ++i) {
            if (std::tolower(static_cast<unsigned char>(query[i])) != 
                std::tolower(static_cast<unsigned char>(text[i]))) {
                prefix_match = false;
                break;
            }
        }
        if (prefix_match) position_bonus = 0.2;
    }
    
    return std::min(1.2, normalized_score + position_bonus);
}

/// Score a multi-word query against text.
/// Each word must match separately; final score is the average of word scores.
static double score_multi_word(const std::vector<std::string>& words, const std::string& text) {
    if (words.empty()) return 0.0;
    
    std::string text_lower = to_lower(text);
    std::vector<double> word_scores;
    
    for (const auto& word : words) {
        // Try exact substring first
        double score = score_substring(word, text_lower);
        
        // If no exact match, try fuzzy
        if (score == 0.0) {
            score = score_fuzzy(word, text_lower);
        }
        
        // Word must have some score to contribute
        if (score > 0.0) {
            word_scores.push_back(score);
        } else {
            return 0.0;  // All words must match
        }
    }
    
    // Average of all word scores
    double total = std::accumulate(word_scores.begin(), word_scores.end(), 0.0);
    return total / static_cast<double>(word_scores.size());
}

/// Score a single query against text (handles both single and multi-word).
static double score_single_query(const std::string& query, const std::string& text) {
    auto words = split_words(query);
    
    if (words.empty()) return 0.0;
    
    std::string text_lower = to_lower(text);
    
    // If single word, use optimized path
    if (words.size() == 1) {
        double score = score_substring(words[0], text_lower);
        if (score > 0.0) return score;
        
        return score_fuzzy(words[0], text_lower);
    }
    
    // Multi-word: each word must match
    return score_multi_word(words, text_lower);
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

int levenshtein_distance(const std::string& a, const std::string& b) {
    return levenshtein_dist(a, b);
}

MatchScore score_match(const std::string& query, const std::string& text) {
    MatchScore result{0.0, false};
    
    if (query.empty() || text.empty()) return result;
    
    // Try exact substring first (fast path)
    double score = score_single_query(query, text);
    
    if (score > 0.0) {
        result.score = score;
        result.matched = true;
    }
    
    return result;
}

std::vector<FuzzyResult> search_fuzzy(
    const std::vector<std::pair<std::string, std::string>>& items,
    const std::string& query,
    unsigned int limit) {
    
    std::vector<FuzzyResult> results;
    
    for (const auto& [name, desc] : items) {
        // Score name match (higher weight)
        double name_score = score_single_query(query, name);
        
        // Score description match (lower weight)
        double desc_score = 0.0;
        if (!desc.empty()) {
            desc_score = score_single_query(query, desc) * 0.7;  // 30% penalty for description matches
        }
        
        // Use the best score
        double best_score = std::max(name_score, desc_score);
        
        // Only include if score is above threshold (0.4 minimum)
        if (best_score >= 0.4) {
            results.push_back({name, desc, best_score});
        }
    }
    
    // Sort by score descending
    std::sort(results.begin(), results.end(),
              [](const FuzzyResult& a, const FuzzyResult& b) {
                  return a.score > b.score;
              });
    
    // Limit results
    if (results.size() > limit) {
        results.resize(limit);
    }
    
    return results;
}

} // namespace pacmkr::fuzzy
