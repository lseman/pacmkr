#pragma once

#include <cstddef>
#include <string>

namespace pacmkr::terminal {

bool interactive();
void title(const std::string& text);
void section(const std::string& text);
void info(const std::string& text);
void success(const std::string& text);
void warning(const std::string& text);
void progress(const std::string& label, int percent, std::size_t current = 0,
              std::size_t total = 0, bool done = false,
              const std::string& suffix = {});

} // namespace pacmkr::terminal
