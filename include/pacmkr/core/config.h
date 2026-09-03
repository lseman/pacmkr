#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pacmkr::config {

/// User configuration (~/.config/pacmkr/config).
struct UserConfig {
  std::optional<std::filesystem::path> aur_dir;
  bool sync_deps{true};
  bool install{false};
  bool lto{true};
  bool mold{true};
  bool graphite{false};
  bool polly{false};
  std::optional<std::string> cc;
  unsigned int search_limit{10};
  bool clean_build{false};
  bool clean{false};
  bool skip_checksums{false};
  std::optional<std::string> makeflags;

  static UserConfig load();

private:
  static UserConfig from_file(const std::filesystem::path &path);
  static std::filesystem::path config_path();
};

/// System/makepkg configuration (/etc/pacmkr.conf).
struct Config {
  std::filesystem::path pkgdest{"."};
  std::filesystem::path srcdest{"."};
  std::filesystem::path srcpkgdest{"."};
  std::filesystem::path logdest{"."};
  std::filesystem::path builddir{"/tmp"};
  std::string packager{"pacmkr <none>"};
  std::string cflags{"-march=native -mtune=native -O3 -pipe"};
  std::string cxxflags{"-march=native -mtune=native -O3 -pipe"};
  std::string ldflags;
  std::string makeflags{"-j$(nproc)"};
  std::vector<std::string> buildenv{"color", "nice", "distcc"};
  std::vector<std::string> options{"strip", "docs", "lto"};
  std::vector<std::string> validpgpkeys;

  static Config load(const std::filesystem::path *path = nullptr);

private:
  static Config from_file(const std::filesystem::path &path);
  static Config defaults();
};

} // namespace pacmkr::config
