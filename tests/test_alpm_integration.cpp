#include "pacmkr/backend/alpm.h"
#include "pacmkr/build/pkgbuild.h"
#include "pacmkr/core/package.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

static fs::path make_archive(const fs::path& base, const fs::path& pkgdir,
                             const std::string& name, const std::string& version) {
    fs::create_directories(pkgdir / "usr/share/pacmkr-test");
    std::ofstream(pkgdir / "usr/share/pacmkr-test/owned") << "native alpm\n";
    std::ofstream info(pkgdir / ".PKGINFO");
    info << "pkgname = " << name << "\n"
            "pkgbase = " << name << "\n"
            "pkgver = " << version << "\n"
            "pkgdesc = isolated native transaction fixture\n"
            "builddate = 1\n"
            "packager = pacmkr tests\n"
            "size = 12\n"
            "arch = any\n";
    info.close();

    pacmkr::package::Package archive = pacmkr::package::Package::make(name, version, "any", base);
    archive.create_archive(pkgdir);
    return archive.dest;
}

int main() {
    const auto unique = std::to_string(getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path base = fs::temp_directory_path() / ("pacmkr-alpm-test-" + unique);
    const fs::path root = base / "root";
    const fs::path db = base / "db";
    const fs::path cache = base / "cache";
    const fs::path config = base / "pacman.conf";
    fs::create_directories(root);
    fs::create_directories(db);
    fs::create_directories(cache);

    // Package metadata must preserve word boundaries while normalizing
    // whitespace to the single-line .PKGINFO representation.
    {
        const auto metadata_dir = base / "metadata";
        fs::create_directories(metadata_dir);
        pacmkr::pkgbuild::Pkgbuild definition;
        definition.pkgname = {"metadata-test"};
        definition.pkgver = "1";
        definition.pkgrel = "1";
        definition.desc = "  native   optimized\npackage  ";
        const auto metadata = pacmkr::package::Package::make(
            "metadata-test", "1-1", "any", base);
        metadata.write_pkginfo(definition, metadata_dir, "pacmkr tests", 1);
        std::ifstream input(metadata_dir / ".PKGINFO");
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        assert(contents.find("pkgdesc = native optimized package\n") != std::string::npos);
    }

    std::ofstream(config)
        << "[options]\n"
        << "RootDir = " << root.string() << "\n"
        << "DBPath = " << db.string() << "\n"
        << "CacheDir = " << cache.string() << "\n"
        << "LogFile = " << (base / "pacman.log").string() << "\n"
        << "GPGDir = " << (base / "gnupg").string() << "\n"
        << "SigLevel = Never\nLocalFileSigLevel = Never\n";

    const fs::path installed_pkg = make_archive(base, base / "package", "pacmkr-alpm-test", "1.0-1");
    const fs::path ghost_pkg = make_archive(base, base / "ghost", "pacmkr-alpm-ghost", "1.0-1");

    try {
        pacmkr::alpm::init(root.string(), db.string(), config.string());
        assert(pacmkr::alpm::install_files({installed_pkg.string()}, true) == 0);
        const auto installed = pacmkr::alpm::get_local_package("pacmkr-alpm-test");
        assert(installed && installed->version == "1.0-1");
        const auto owner = pacmkr::alpm::find_file_owner("/usr/share/pacmkr-test/owned");
        assert(owner && *owner == "pacmkr-alpm-test");
        assert(pacmkr::alpm::missing_dependencies({"pacmkr-alpm-test", "missing>=1"}) ==
               std::vector<std::string>{"missing>=1"});

        // -Qp: metadata read straight from a package archive, with a file list.
        const auto from_file = pacmkr::alpm::load_package_file(installed_pkg.string());
        assert(from_file.name == "pacmkr-alpm-test" && from_file.version == "1.0-1");
        assert(std::find(from_file.files.begin(), from_file.files.end(),
                         "usr/share/pacmkr-test/owned") != from_file.files.end());
        bool load_failed = false;
        try { pacmkr::alpm::load_package_file((base / "absent.pkg.tar").string()); }
        catch (...) { load_failed = true; }
        assert(load_failed);

        // -Qc: fixture ships no changelog, so the read is empty rather than an error.
        assert(pacmkr::alpm::get_changelog("pacmkr-alpm-test").empty());

        // -Sc: keep the installed version, drop archives for packages that are gone.
        fs::copy_file(installed_pkg, cache / installed_pkg.filename());
        fs::copy_file(ghost_pkg, cache / ghost_pkg.filename());
        std::ofstream(cache / "not-a-package.txt") << "left alone by -Sc\n";
        const auto partial = pacmkr::alpm::clean_cache(/*all=*/false, /*no_confirm=*/true);
        assert(partial.files_removed == 1);
        assert(fs::exists(cache / installed_pkg.filename()));
        assert(!fs::exists(cache / ghost_pkg.filename()));
        assert(fs::exists(cache / "not-a-package.txt"));

        // -Scc: wipe every remaining cache file regardless of install state.
        const auto full = pacmkr::alpm::clean_cache(/*all=*/true, /*no_confirm=*/true);
        assert(full.files_removed == 2);
        assert(fs::is_empty(cache));

        // -Sw: a download-only transaction with nothing pending is a clean no-op;
        // an unknown target is rejected before any fetch is attempted.
        assert(pacmkr::alpm::download({}, /*sysupgrade=*/true, /*no_confirm=*/true) == 0);
        bool download_rejected = false;
        try { pacmkr::alpm::download({"pacmkr-not-in-any-repo"}, false, true); }
        catch (...) { download_rejected = true; }
        assert(download_rejected);

        assert(pacmkr::alpm::set_install_reason({"pacmkr-alpm-test"}, false) == 0);
        assert(pacmkr::alpm::get_local_package("pacmkr-alpm-test")->reason ==
               pacmkr::alpm::Package::Reason::Dependency);
        assert(pacmkr::alpm::remove({"pacmkr-alpm-test"}, false, false, false, true) == 0);
        assert(!pacmkr::alpm::get_local_package("pacmkr-alpm-test"));
        pacmkr::alpm::shutdown();
    } catch (...) {
        pacmkr::alpm::shutdown();
        fs::remove_all(base);
        throw;
    }
    fs::remove_all(base);
    std::cout << "isolated native libalpm transaction passed\n";

    // check_pending_repo_upgrades: the root-free upgrade check (used to
    // decide whether -Syu needs to ask for a password at all) against a
    // real, if synthetic, file:// repository.
    {
        const auto check_id = std::to_string(getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path check_base = fs::temp_directory_path() / ("pacmkr-checkdb-test-" + check_id);
        const fs::path check_root = check_base / "root";
        const fs::path check_db = check_base / "db";
        const fs::path check_cache = check_base / "cache";
        const fs::path check_repo_dir = check_base / "repo";
        const fs::path check_config = check_base / "pacman.conf";
        fs::create_directories(check_root);
        fs::create_directories(check_db);
        fs::create_directories(check_cache);
        fs::create_directories(check_repo_dir);

        const std::string repo_name = "checkrepo";
        std::ofstream(check_config)
            << "[options]\n"
            << "RootDir = " << check_root.string() << "\n"
            << "DBPath = " << check_db.string() << "\n"
            << "CacheDir = " << check_cache.string() << "\n"
            << "LogFile = " << (check_base / "pacman.log").string() << "\n"
            << "GPGDir = " << (check_base / "gnupg").string() << "\n"
            << "SigLevel = Never\nLocalFileSigLevel = Never\n"
            << "[" << repo_name << "]\n"
            << "Server = file://" << check_repo_dir.string() << "\n";

        // The installed version (1.0-1) and the newer version sitting in
        // the repo (2.0-1).
        const fs::path old_pkg = make_archive(check_base, check_base / "check-old", "checkpkg", "1.0-1");
        const fs::path new_pkg = make_archive(check_base, check_base / "check-new", "checkpkg", "2.0-1");
        fs::copy_file(new_pkg, check_repo_dir / new_pkg.filename());

        const std::string repo_add_cmd = "repo-add '" +
            (check_repo_dir / (repo_name + ".db.tar.gz")).string() + "' '" +
            (check_repo_dir / new_pkg.filename()).string() + "' >/dev/null 2>&1";
        assert(std::system(repo_add_cmd.c_str()) == 0);

        try {
            pacmkr::alpm::init(check_root.string(), check_db.string(), check_config.string());
            assert(pacmkr::alpm::install_files({old_pkg.string()}, true) == 0);

            // No sync has happened yet, so there is nothing cached to
            // compare against — the check still completes cleanly.
            auto before_sync = pacmkr::alpm::check_pending_repo_upgrades(false);
            assert(before_sync.checked);
            assert(before_sync.upgrades.empty());

            // refresh=true pulls the repo db into a private scratch dir and
            // finds the real pending upgrade.
            auto refreshed = pacmkr::alpm::check_pending_repo_upgrades(true);
            assert(refreshed.checked);
            assert(refreshed.upgrades.size() == 1);
            assert(refreshed.upgrades[0].name == "checkpkg");
            assert(refreshed.upgrades[0].installed_version == "1.0-1");
            assert(refreshed.upgrades[0].repo_version == "2.0-1");

            // As a non-root caller, the check must never write into the
            // real (test-fixture) dbpath — only its own private scratch dir.
            if (geteuid() != 0) {
                assert(!fs::exists(check_db / "sync" / (repo_name + ".db")));
            }

            pacmkr::alpm::shutdown();
        } catch (...) {
            pacmkr::alpm::shutdown();
            fs::remove_all(check_base);
            throw;
        }
        fs::remove_all(check_base);
        std::cout << "root-free upgrade check passed\n";
    }
}
