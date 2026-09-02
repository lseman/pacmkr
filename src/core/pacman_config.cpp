#include "pacmkr/pacman_config.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace pacmkr::pacman_config {

namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

} // namespace

std::vector<std::string> repository_names(std::istream& input) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    std::string line;

    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(std::move(line));
        if (line.size() < 3 || line.front() != '[' || line.back() != ']') {
            continue;
        }

        auto section = trim(line.substr(1, line.size() - 2));
        if (section.empty() || section == "options" || !seen.insert(section).second) {
            continue;
        }
        result.push_back(std::move(section));
    }

    return result;
}

} // namespace pacmkr::pacman_config
