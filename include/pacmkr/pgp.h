#pragma once

#include <string>
#include <vector>

namespace pacmkr::pgp {

/// Fetch missing PGP keys from a keyserver.
/// Returns 0 on success, non-zero on failure.
int fetch_keys(const std::vector<std::string>& key_ids);

/// Import a single key from a keyserver.
int import_key(const std::string& key_id);

/// List local keys matching a pattern.
std::vector<std::string> list_keys(const std::string& pattern = "");

/// Trust a key fully (set ownertrust to 6).
int trust_key(const std::string& key_id);

} // namespace pacmkr::pgp
