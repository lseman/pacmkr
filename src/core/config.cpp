#include "pacmkr/core/config.h"
#include "pacmkr/core/error.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace pacmkr::config {

namespace {

std::string trim(const std::string &s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return {};
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string &s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return r;
}

bool parse_bool(const std::string &s) {
  auto l = to_lower(trim(s));
  return l == "true" || l == "yes" || l == "1" || l == "on";
}

std::string strip_quotes(const std::string &s) {
  auto t = trim(s);
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                        (t.front() == '\'' && t.back() == '\''))) {
    return t.substr(1, t.size() - 2);
  }
  return t;
}

std::vector<std::string> parse_array(const std::string &s) {
  auto t = trim(s);
  if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
    t = t.substr(1, t.size() - 2);
  } else {
    return {strip_quotes(t)};
  }

  std::vector<std::string> result;
  std::string current;
  bool in_quote = false;
  char qc = '\0';

  for (char ch : t) {
    if (!in_quote) {
      if (ch == '"' || ch == '\'') {
        in_quote = true;
        qc = ch;
      } else if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!current.empty()) {
          result.push_back(current);
          current.clear();
        }
      } else {
        current += ch;
      }
    } else if (ch == qc) {
      in_quote = false;
    } else {
      current += ch;
    }
  }
  if (!current.empty())
    result.push_back(current);
  return result;
}

std::string parse_array_or_single(const std::string &s) {
  auto parts = parse_array(s);
  std::string result;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0)
      result += ' ';
    result += parts[i];
  }
  return result;
}

} // namespace

// ─── UserConfig ──────────────────────────────────────────────────────

std::filesystem::path UserConfig::config_path() {
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
    return std::filesystem::path{xdg} / "pacmkr" / "config";
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path{home} / ".config" / "pacmkr" / "config";
  return "pacmkr.conf";
}

UserConfig UserConfig::load() {
  auto path = config_path();
  try {
    return from_file(path);
  } catch (...) {
    return {}; // defaults on failure
  }
}

UserConfig UserConfig::from_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    throw config_error("Cannot read " + path.string());

  UserConfig cfg;
  std::string line;

  while (std::getline(in, line)) {
    auto t = trim(line);
    if (t.empty() || t.front() == '#')
      continue;

    auto eq = t.find('=');
    if (eq == std::string::npos)
      continue;

    auto key = to_lower(trim(t.substr(0, eq)));
    auto value = trim(t.substr(eq + 1));

    if (key == "aur_dir")
      cfg.aur_dir = strip_quotes(value);
    else if (key == "sync_deps")
      cfg.sync_deps = parse_bool(value);
    else if (key == "install")
      cfg.install = parse_bool(value);
    else if (key == "lto")
      cfg.lto = parse_bool(value);
    else if (key == "mold")
      cfg.mold = parse_bool(value);
    else if (key == "graphite")
      cfg.graphite = parse_bool(value);
    else if (key == "polly")
      cfg.polly = parse_bool(value);
    else if (key == "cc")
      cfg.cc = strip_quotes(value);
    else if (key == "search_limit") {
      try {
        cfg.search_limit = std::stoul(value);
      } catch (...) {
      }
    } else if (key == "clean_build")
      cfg.clean_build = parse_bool(value);
    else if (key == "clean")
      cfg.clean = parse_bool(value);
    else if (key == "skip_checksums")
      cfg.skip_checksums = parse_bool(value);
    else if (key == "makeflags")
      cfg.makeflags = strip_quotes(value);
  }

  return cfg;
}

// ─── Config ──────────────────────────────────────────────────────────

Config Config::load(const std::filesystem::path *path) {
  auto p = path ? *path : std::filesystem::path{"/etc/pacmkr.conf"};
  if (std::filesystem::exists(p))
    return from_file(p);
  return defaults();
}

Config Config::from_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    throw config_error("Cannot read " + path.string());

  const auto base = defaults();
  std::string pkgdest{base.pkgdest.string()}, srcdest{base.srcdest.string()},
      srcpkgdest{base.srcpkgdest.string()}, logdest{base.logdest.string()};
  std::string builddir{base.builddir.string()}, packager{base.packager},
      cflags{base.cflags}, cxxflags{base.cxxflags}, ldflags{base.ldflags},
      makeflags{base.makeflags};
  std::vector<std::string> buildenv{base.buildenv}, options{base.options},
      validpgpkeys{base.validpgpkeys};

  std::string line;
  while (std::getline(in, line)) {
    auto t = trim(line);
    if (t.empty() || t.front() == '#')
      continue;

    auto eq = t.find('=');
    if (eq == std::string::npos)
      continue;

    auto key = trim(t.substr(0, eq));
    auto value = t.substr(eq + 1);

    if (key == "PKGDEST")
      pkgdest = strip_quotes(value);
    else if (key == "SRCDEST")
      srcdest = strip_quotes(value);
    else if (key == "SRCPKGDEST")
      srcpkgdest = strip_quotes(value);
    else if (key == "LOGDEST")
      logdest = strip_quotes(value);
    else if (key == "BUILDDIR")
      builddir = strip_quotes(value);
    else if (key == "PACKAGER")
      packager = strip_quotes(value);
    else if (key == "CFLAGS")
      cflags = parse_array_or_single(value);
    else if (key == "CXXFLAGS")
      cxxflags = parse_array_or_single(value);
    else if (key == "LDFLAGS")
      ldflags = parse_array_or_single(value);
    else if (key == "MAKEFLAGS")
      makeflags = parse_array_or_single(value);
    else if (key == "BUILDENV")
      buildenv = parse_array(value);
    else if (key == "OPTIONS")
      options = parse_array(value);
    else if (key == "VALIDPGPKEYS")
      validpgpkeys = parse_array(value);
  }

  return {pkgdest,  srcdest, srcpkgdest,  logdest, builddir,
          packager, cflags,  cxxflags,    ldflags, makeflags,
          buildenv, options, validpgpkeys};
}

Config Config::defaults() {
  return {".",
          ".",
          ".",
          ".",
          "/tmp",
          "pacmkr <none>",
          "-march=native -mtune=native -O3 -pipe",
          "-march=native -mtune=native -O3 -pipe",
          "",
          "-j$(nproc)",
          {"color", "nice", "distcc"},
          {"strip", "docs", "lto"},
          {}};
}

} // namespace pacmkr::config
