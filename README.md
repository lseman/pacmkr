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
tool. Read-only repository operations use libalpm, system mutations are handed
to pacman, and AUR packages are resolved and built in dependency order.

- Pacman-style commands, including `-Syu`, `-Ss`, `-Q`, and `-Rns`
- Atomic repository refresh and upgrade through `pacman -Syu`
- Unified official repository and AUR search
- Transitive AUR dependency resolution with topological build ordering
- PKGBUILD parsing, source verification, packaging, and installation
- Optional LTO, mold, GCC Graphite, and LLVM Polly build optimization
- Interactive terminal output with multi-package progress
- Experimental Slint GUI sources

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

## Usage

```bash
# Refresh databases and upgrade repository + AUR packages
pacmkr -Syu

# Search official repositories and the AUR
pacmkr -Ss terminal

# Install a repository package, falling back to the AUR when needed
pacmkr -S package-name

# Fetch and build an AUR package with its dependencies
pacmkr --aur --aur-deps package-name

# Inspect foreign packages and remove a package cleanly
pacmkr -Qm
pacmkr -Rns package-name
```

`pacmkr -Qm` (or `pacmkr --list-foreign`) lists every installed package
that is absent from the configured repositories. This includes AUR packages
and packages installed from custom/local PKGBUILDs. Use `-Qmq` for names only.

Run `pacmkr --help` for the complete option reference.

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
CLI / TUI
    │
    ├── read operations ──► libalpm ──► repository + local databases
    ├── write operations ─► pacman  ──► privileged system transaction
    └── AUR operations ───► resolver ─► PKGBUILD ─► package ─► pacman -U
```

pacmkr keeps repository upgrades atomic: `pacmkr -Syu` invokes a single
`pacman -Syu` transaction before resolving out-of-date AUR packages. This
avoids the partial-upgrade window created by splitting refresh and upgrade into
separate commands.

## Project layout

```text
include/pacmkr/  C++ interfaces
src/app/         Entry point and application orchestration
src/backend/     libalpm, pacman, repository, and AUR integration
src/build/       PKGBUILD parsing and package build pipeline
src/core/        Configuration, package models, shared infrastructure
src/tui/         Argument parsing, status output, and progress UI
src/gui/         Experimental Slint desktop UI
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
