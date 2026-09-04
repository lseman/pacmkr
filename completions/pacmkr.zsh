#compdef pacmkr
# zsh completion for pacmkr
#
# Installed as $prefix/share/zsh/site-functions/_pacmkr

_pacmkr_sync_pkgs() {
    local -a pkgs
    pkgs=( ${(f)"$(pacman -Slq 2>/dev/null)"} )
    _describe -t packages 'package' pkgs
}

_pacmkr_installed_pkgs() {
    local -a pkgs
    pkgs=( ${(f)"$(pacman -Qq 2>/dev/null)"} )
    _describe -t packages 'installed package' pkgs
}

_pacmkr() {
    local curcontext="$curcontext" state line
    typeset -A opt_args

    local -a operations
    operations=(
        '-S[Sync: install or query repositories and the AUR]'
        '-R[Remove installed packages]'
        '-Q[Query the local package database]'
        '-U[Upgrade from a package file or URL]'
        '-F[Query the files database]'
        '-D[Database / install-reason operations]'
        '-T[Dependency test]'
        '-B[Build the PKGBUILD in the current directory]'
    )

    local -a common
    common=(
        '(-h --help)'{-h,--help}'[Show help]'
        '(-V --version)'{-V,--version}'[Show version]'
        '--flags[Print compiler and flags used for builds]'
        '--config[Alternate config file]:config file:_files'
        '(-n --noconfirm)'{-n,--noconfirm}'[Skip all confirmation prompts]'
        '--color[Force color output]'
        '--nocolor[Disable color output]'
        '--debug[Verbose debug output]'
        '--json[Machine-readable JSON output]'
        '--dry-run[Preview an upgrade without executing it]'
    )

    local -a aur
    aur=(
        '--aur[Operate on the AUR]'
        '--aur-deps[Resolve and build AUR dependencies]'
        '--noreview[Explicitly skip review of AUR file changes]'
        '--search[Search the AUR]:query:'
        '--limit[Limit search results]:count:'
        '--aur-dir[AUR clone directory]:directory:_files -/'
        '(-u --refresh)'{-u,--refresh}'[Upgrade out-of-date AUR packages]'
        '--no-deps-resolve[Do not resolve AUR dependencies]'
        '(-G --getpkgbuild)'{-G,--getpkgbuild}'[Download PKGBUILD only]'
        '--list-foreign[List installed AUR/local packages]'
        '--orphans[List orphaned packages]'
        '--devel[Include -git/-svn packages in upgrades]'
        '--nodevel[Skip -git/-svn packages in upgrades]'
        '--cleanup[Remove old AUR sources and build logs]'
        '--cleanup-age[Age threshold in days]:days:'
        '--sortby[Search sort key]:key:(votes popular updated)'
        '--pgpfetch[Auto-fetch missing PGP keys]'
        '--keepsrc[Preserve sources after build]'
        '--cleanafter[Clean build dir after each package]'
        '--answerclean[Answer for clean-dir prompts]:answer:'
        '--answerupgrade[Answer for upgrade prompts]:answer:'
    )

    local -a build
    build=(
        '(-p --packagefile)'{-p,--packagefile}'[Alternate build script]:file:_files'
        '(-N --nodeps)'{-N,--nodeps}'[Skip dependency checks]'
        '(-s --syncdeps)'{-s,--syncdeps}'[Install missing dependencies]'
        '(-o --nobuild)'{-o,--nobuild}'[Download and extract only]'
        '(-C --cleanbuild)'{-C,--cleanbuild}'[Remove existing src/ dir]'
        '(-c --clean)'{-c,--clean}'[Clean work files after build]'
        '(-f --force)'{-f,--force}'[Overwrite an existing package]'
        '(-i --install)'{-i,--install}'[Install after a successful build]'
        '(-g --geninteg)'{-g,--geninteg}'[Generate source checksums]'
        '(-e --noextract)'{-e,--noextract}'[Do not extract sources]'
        '--check[Run the check() function]'
        '--nocheck[Skip the check() function]'
        '--noprepare[Skip the prepare() function]'
        '--verifysource[Download and verify sources only]'
        '--skipchecksums[Skip checksum verification]'
        '--skipinteg[Skip all integrity checks]'
        '--skippgpcheck[Skip PGP signature checks]'
        '--sign[Sign the built package]'
        '--nosign[Do not sign the built package]'
        '--key[GPG key to sign with]:key:'
        '(-r --rmdeps)'{-r,--rmdeps}'[Remove build-only deps afterward]'
        '--printsrcinfo[Print the generated .SRCINFO]'
        '--holdver[Do not update VCS sources]'
        '--mflags[Build-tool parallelism flags]:flags:'
        '--graphite[GCC Graphite loop optimizer]::mode:(yes no auto)'
        '--polly[LLVM Polly optimizer]::mode:(yes no auto)'
        '--lto[Link-time optimization]::mode:(yes no auto)'
        '--mold[Use the mold linker]::mode:(yes no auto)'
        '--cc[Compiler override]:compiler:(gcc clang)'
    )

    _arguments -s -C \
        "${operations[@]}" \
        "${common[@]}" \
        "${aur[@]}" \
        "${build[@]}" \
        '--needed[Skip up-to-date targets]' \
        '--ignore[Ignore a package during upgrade]:package:_pacmkr_installed_pkgs' \
        '--ignoregroup[Ignore a group during upgrade]:group:' \
        '--overwrite[Overwrite conflicting files]:glob:' \
        '(-w --downloadonly)'{-w,--downloadonly}'[Download packages only]' \
        '--asdeps[Mark installed packages as dependencies]' \
        '--asexplicit[Mark installed packages as explicit]' \
        '*::package:->pkgs'

    case $state in
        pkgs)
            if (( ${words[(I)-R*]} || ${words[(I)-Q*]} )); then
                _pacmkr_installed_pkgs
            elif (( ${words[(I)-U]} )); then
                _files -g '*.pkg.tar*'
            else
                _pacmkr_sync_pkgs
            fi
            ;;
    esac
}

_pacmkr "$@"
