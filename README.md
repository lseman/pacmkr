<p align="center">
  <img src="assets/pacmkr-logo.png" alt="pacmkr logo" width="180">
</p>

<h1 align="center">pacmkr</h1>

<p align="center">
  A fast, modern package manager and AUR helper for Arch Linux.
</p>

> [!WARNING]
> pacmkr is experimental. Review package transactions and PKGBUILDs before
> installing them, and do not use it as your only recovery path on a critical
> system.

## Why pacmkr?

pacmkr brings official repository and AUR workflows into one C++17 command-line
tool. Repository reads and system transactions use libalpm directly, and AUR
packages are resolved and built in dependency order.

- Pacman-style commands, including `-Syu`, `-Ss`, `-Q`, and `-Rns`
- Atomic native repository refresh and upgrade through libalpm
- Fuzzy search with Levenshtein distance scoring and typo tolerance
- Hybrid repo/AUR ranking: unified relevance-sorted results across both sources
- Transitive AUR dependency resolution with topological build ordering
- PKGBUILD parsing, source verification, packaging, and installation
- Disk space pre-check before builds (aborts early if insufficient)
- Build retry with exponential backoff for transient failures
- Optional LTO, mold, GCC Graphite, and LLVM Polly build optimization
- Plain-text command output suitable for terminals, logs, and scripts
- Optional GTK4 desktop dashboard

## Quick start

pacmkr currently targets Arch Linux and requires a C++17 compiler, CMake,
OpenSSL, libalpm, and the standard Arch packaging tools.

```bash
git clone https://github.com/lseman/pacmkr.git
cd pacmkr

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

sudo cmake --install build
```

With [`just`](https://just.systems/), the common build workflow is:

```bash
just
just test
just install
```

Use `just install-user` to install under `~/.local` without root privileges.
Build options can be passed through to Make, for example `just build GUI=OFF`
for a CLI-only build or `just install PREFIX=/usr/local` for a different
prefix.

The equivalent conventional Make workflow is:

```bash
make
make test
sudo make install
```

The install includes the `pacmkr` and `pacmkr-repo` command-line tools, GTK
application, desktop launcher, application metadata, icon, and license.

## Usage

```bash
# Refresh databases and upgrade repository + AUR packages
sudo pacmkr -Syu

# Search official repositories and the AUR
pacmkr -Ss terminal

# Install a repository package, falling back to the AUR when needed
sudo pacmkr -S package-name

# Fetch and build an AUR package with its dependencies
pacmkr --aur --aur-deps package-name

# Inspect foreign packages and remove a package cleanly
pacmkr -Qm
sudo pacmkr -Rns package-name

# Preview an upgrade without executing it
pacmkr --dry-run -Syu

# List orphan packages (foreign deps no longer required)
pacmkr --orphans
```

`pacmkr -Qm` (or `pacmkr --list-foreign`) lists every installed package
that is absent from the configured repositories. This includes AUR packages
and packages installed from custom/local PKGBUILDs. Use `-Qmq` for names only.

Run `pacmkr --help` for the complete option reference.

### Search behavior

**Fuzzy matching**: pacmkr scores search results using Levenshtein edit distance,
so typos like `"termnal"` still match `"terminal"` with a high relevance score.
Multi-word queries require every word to match, and name matches rank above
description matches.

**Unified ranking**: `-Ss` no longer shows repository results first and AUR
results second. Instead, repo and AUR packages are scored together and sorted
by relevance, with repository packages breaking ties when scores are equal.

**JSON output**: use `--json` for machine-readable search results:

```bash
pacmkr -Ss --json terminal | jq '.results[] | select(.source == "aur")'
```

### Build resilience

**Disk space pre-check**: before invoking makepkg, pacmkr verifies that the
build directory and package destination have at least 2 GB free. Use a custom
threshold by setting `min_space` in your config file.

**Retry with backoff**: transient build failures (network timeouts, OOM kills,
GPG keyring issues) are automatically retried up to 2 times with exponential
backoff (1 s → 3 s → 9 s). Build logs for every attempt are saved under
`~/.cache/pacmkr/logs/`.

**Dry-run upgrade preview**: `--dry-run` shows exactly what a `-Syu` would change
— repository upgrades, AUR out-of-date packages, dependency resolution plan, and
any conflicts — without executing any transaction or build.

### Local repositories

```bash
pacmkr-repo create myrepo ~/.local/share/pacmkr/myrepo
pacmkr-repo add myrepo ./package-1.0-1-x86_64.pkg.tar.zst
pacmkr-repo remove myrepo package
pacmkr-repo list
pacmkr-repo delete myrepo
```

`create` registers the directory; the database is generated when the first
package is added. `add` copies package archives (and adjacent signatures) into
the repository before indexing them. `delete` unregisters the repository but
deliberately preserves its database and package files. Add the generated
database to `pacman.conf` separately when you want it available system-wide
system-wide.

### Build optimization

Optimization flags are applied to packages built by pacmkr, not to prebuilt
repository packages.

```bash
pacmkr --aur --aur-deps --lto --mold package-name
pacmkr --aur --aur-deps --cc=clang --polly --lto package-name
pacmkr --aur --aur-deps --cc=gcc --graphite --lto package-name
```

## Configuration

Create `~/.config/pacmkr/config` to define persistent defaults. See
[`config.example`](config.example) for the available settings.

```ini
aur_dir = ~/.cache/pacmkr/aur
sync_deps = true
install = false
search_limit = 15

lto = true
mold = true
graphite = false
polly = false
```

## How it works

```text
CLI
    │
    ├── read operations ──► libalpm ──► repository + local databases
    ├── write operations ─► libalpm ──► privileged system transaction
    └── AUR operations ───► resolver ─► PKGBUILD ─► package ─► libalpm
```

pacmkr keeps repository upgrades atomic: `sudo pacmkr -Syu` invokes a single
native libalpm transaction before resolving out-of-date AUR packages. This
avoids the partial-upgrade window created by splitting refresh and upgrade into
separate commands.

## Project layout

```text
include/pacmkr/app/      CLI and terminal interfaces
include/pacmkr/backend/  Package-manager and AUR interfaces
include/pacmkr/build/    PKGBUILD and package-construction interfaces
include/pacmkr/core/     Configuration and shared domain interfaces
include/pacmkr/gui/      GUI adapter interfaces
src/app/         Entry point, argument parsing, and application orchestration
src/backend/     libalpm transactions, repositories, and AUR integration
src/build/       PKGBUILD parsing and package build pipeline
src/core/        Configuration, package models, shared infrastructure
src/gui/gtk/     GTK4 desktop UI and backend adapter
tests/           C++ unit and regression tests
```

## Development

```bash
cmake -S . -B build -DPACMKR_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Contributions and bug reports are welcome. When reporting package-management
failures, include the pacmkr command, relevant terminal output, and whether the
same operation succeeds with pacman.

## Security

AUR packages are user-produced build recipes. pacmkr avoids evaluating a
PKGBUILD merely to read its metadata, but building a package necessarily runs
its build functions. Inspect unfamiliar PKGBUILDs and their sources before
continuing.

## License

pacmkr is available under the [MIT License](LICENSE).
