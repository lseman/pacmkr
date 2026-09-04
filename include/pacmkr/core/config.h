#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pacmkr::config {

// ─── makepkg.conf types ──────────────────────────────────────────────

/// DLAGENTS: protocol → download command template (%u = URL, %o = output)
using DlagentsMap = std::map<std::string, std::string>;
/// VCSCLIENTS: protocol → package name
using VcsClientsMap = std::map<std::string, std::string>;

/// Parsed values from /etc/makepkg.conf.
struct MakepkgConf {
  // Architecture & host
  std::string carch;            // CARCH (e.g., x86_64)
  std::string chost;            // CHOST
  std::optional<int> nproc;     // NPROC (optional, may be commented out)

  // Download tools
  DlagentsMap dagents;          // DLAGENTS
  VcsClientsMap vcs_clients;    // VCSCLIENTS

  // Compiler flags (raw, unexpanded)
  std::string cflags;
  std::string cxxflags;
  std::string ldflags;
  std::string cppflags;
  std::string ltoflags;
  std::string makeflags;
  std::string ninjaflags;

  // Debug flags
  std::string debug_cflags;
  std::string debug_cxxflags;

  // Build environment & options (from makepkg.conf defaults)
  std::vector<std::string> buildenv_defaults;  // default BUILDENV values
  std::vector<std::string> options_defaults;   // default OPTIONS values

  // Integrity checks
  std::vector<std::string> integrity_check;    // INTEGRITY_CHECK

  // Stripping
  std::string strip_binaries;
  std::string strip_shared;
  std::string strip_static;

  // Directories for purge/zipman/docs
  std::string dbgsrcdir;
  std::vector<std::string> man_dirs;
  std::vector<std::string> doc_dirs;
  std::vector<std::string> purge_targets;
  std::vector<std::string> lib_dirs;

  // Compression commands (used when creating archives)
  std::vector<std::string> compress_gz;
  std::vector<std::string> compress_bz2;
  std::vector<std::string> compress_xz;
  std::vector<std::string> compress_zst;
  std::vector<std::string> compress_lrz;
  std::vector<std::string> compress_lzo;
  std::vector<std::string> compress_z;
  std::vector<std::string> compress_lz4;
  std::vector<std::string> compress_lz;

  // Output extensions
  std::string pkgext;   // PKGEXT (e.g., .pkg.tar.zst)
  std::string srcent;   // SRCEXT (e.g., .src.tar.gz)

  // Signing & packager
  std::optional<std::string> gpgkey;
  std::string packager;

  /// Expand makepkg.conf variable references (${VAR}, $(cmd), $VAR).
  static std::string expand(const std::string& value,
                            const std::map<std::string, std::string>& vars);

  /// Parse /etc/makepkg.conf and return the result.
  static MakepkgConf parse(const std::filesystem::path& path);
};

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
