#include "pacmkr/backend/hybrid_search.h"
#include "pacmkr/backend/alpm.h"
#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/core/fuzzy_search.h"

#include <algorithm>
#include <vector>
#include <utility>

namespace pacmkr::hybrid_search {

namespace {

/// Score a single result (repo or AUR) by relevance to query.
double score_result(const std::string& query, const std::string& name, 
                    const std::optional<std::string>& desc) {
    // Build text from name and description
    std::string text = name;
    if (desc.has_value() && !desc->empty()) {
        text += " " + *desc;
    }
    
    auto score = fuzzy::score_match(query, text);
    return score.score;
}

/// Convert alpm::Package to hybrid search result.
Result make_repo_result(const alpm::Package& pkg, double relevance) {
    Result r{};
    r.source = Source::Repository;
    r.name = pkg.name;
    r.version = pkg.version;
    r.desc = pkg.desc.empty() ? std::nullopt : std::optional<std::string>(pkg.desc);
    r.origin_db = pkg.origin_db;
    r.relevance_score = relevance;
    return r;
}

/// Convert aur_cache::SearchResult to hybrid search result.
Result make_aur_result(const aur_cache::SearchResult& sr, double relevance) {
    Result r{};
    r.source = Source::AUR;
    r.name = sr.name;
    r.version = "";  // AUR cache doesn't store version in SearchResult
    r.desc = sr.desc.empty() ? std::nullopt : std::optional<std::string>(sr.desc);
    r.origin_db = "aur";
    r.relevance_score = relevance;
    r.numvotes = sr.numvotes;
    r.popularity = sr.popularity;
    return r;
}

} // anonymous

std::vector<Result> search(
    const std::string& query,
    unsigned int limit) {
    
    if (query.empty()) return {};
    
    // Search repositories
    auto repo_pkgs = alpm::search_sync(query);
    
    // Search AUR cache
    auto aur_results = aur_cache::search(query, limit * 2);
    
    std::vector<Result> results;
    results.reserve(repo_pkgs.size() + aur_results.size());
    
    // Score and convert repo results
    for (auto& pkg : repo_pkgs) {
        double score = score_result(query, pkg.name, pkg.desc.empty() ? std::nullopt : std::optional<std::string>(pkg.desc));
        if (score >= 0.4) {  // Minimum relevance threshold
            results.push_back(make_repo_result(pkg, score));
        }
    }
    
    // Score and convert AUR results
    for (auto& sr : aur_results) {
        double score = score_result(query, sr.name, sr.desc.empty() ? std::nullopt : std::optional<std::string>(sr.desc));
        if (score >= 0.4) {  // Minimum relevance threshold
            results.push_back(make_aur_result(sr, score));
        }
    }
    
    // Sort by relevance score (primary), then by source priority (repo first for same score),
    // then by popularity/votes for AUR packages
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) {
                  if (a.relevance_score != b.relevance_score)
                      return a.relevance_score > b.relevance_score;
                  
                  // Same relevance: prefer repository matches
                  if (a.source != b.source) {
                      if (a.source == Source::Repository) return true;
                      return false;
                  }
                  
                  // Same source and relevance: sort by popularity/votes for AUR
                  if (a.source == Source::AUR) {
                      if (a.popularity != b.popularity)
                          return a.popularity > b.popularity;
                      return a.numvotes > b.numvotes;
                  }
                  
                  // Same source and relevance: sort by name for repos
                  return a.name < b.name;
              });
    
    // Limit results
    if (results.size() > limit) {
        results.resize(limit);
    }
    
    return results;
}

} // namespace pacmkr::hybrid_search
