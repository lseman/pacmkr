# fish completion for pacmkr
#
# Installed as $prefix/share/fish/vendor_completions.d/pacmkr.fish

function __pacmkr_sync_pkgs
    pacman -Slq 2>/dev/null
end

function __pacmkr_installed_pkgs
    pacman -Qq 2>/dev/null
end

function __pacmkr_using_op
    set -l tokens (commandline -opc)
    for t in $tokens[2..-1]
        switch $t
            case '-S*' --sync
                test $argv[1] = sync; and return 0
            case '-R*' --remove
                test $argv[1] = remove; and return 0
            case '-Q*' --query
                test $argv[1] = query; and return 0
            case -U --upgrade
                test $argv[1] = upgrade; and return 0
        end
    end
    return 1
end

# Operations
complete -c pacmkr -f
complete -c pacmkr -s S -d 'Sync: install/query repositories and the AUR'
complete -c pacmkr -s R -d 'Remove installed packages'
complete -c pacmkr -s Q -d 'Query the local package database'
complete -c pacmkr -s U -d 'Upgrade from a package file or URL'
complete -c pacmkr -s F -d 'Query the files database'
complete -c pacmkr -s D -d 'Database / install-reason operations'
complete -c pacmkr -s T -d 'Dependency test'
complete -c pacmkr -s B -l build -d 'Build the PKGBUILD in the current directory'

# Common
complete -c pacmkr -s h -l help -d 'Show help'
complete -c pacmkr -s V -l version -d 'Show version'
complete -c pacmkr -l flags -d 'Print compiler and flags used for builds'
complete -c pacmkr -l config -r -d 'Alternate config file'
complete -c pacmkr -s n -l noconfirm -d 'Skip all confirmation prompts'
complete -c pacmkr -l color -d 'Force color output'
complete -c pacmkr -l nocolor -d 'Disable color output'
complete -c pacmkr -l debug -d 'Verbose debug output'
complete -c pacmkr -l json -d 'Machine-readable JSON output'
complete -c pacmkr -l dry-run -d 'Preview an upgrade without executing it'

# AUR
complete -c pacmkr -l aur -d 'Operate on the AUR'
complete -c pacmkr -l aur-deps -d 'Resolve and build AUR dependencies'
complete -c pacmkr -l noreview -d 'Explicitly skip review of AUR file changes'
complete -c pacmkr -l search -r -d 'Search the AUR'
complete -c pacmkr -l limit -x -d 'Limit search results'
complete -c pacmkr -l aur-dir -r -d 'AUR clone directory'
complete -c pacmkr -s u -l refresh -d 'Upgrade out-of-date AUR packages'
complete -c pacmkr -l no-deps-resolve -d 'Do not resolve AUR dependencies'
complete -c pacmkr -s G -l getpkgbuild -d 'Download PKGBUILD only'
complete -c pacmkr -l list-foreign -d 'List installed AUR/local packages'
complete -c pacmkr -l orphans -d 'List orphaned packages'
complete -c pacmkr -l devel -d 'Include -git/-svn packages in upgrades'
complete -c pacmkr -l nodevel -d 'Skip -git/-svn packages in upgrades'
complete -c pacmkr -l cleanup -d 'Remove old AUR sources and build logs'
complete -c pacmkr -l cleanup-age -x -d 'Age threshold in days'
complete -c pacmkr -l sortby -x -a 'votes popular updated' -d 'Search sort key'
complete -c pacmkr -l pgpfetch -d 'Auto-fetch missing PGP keys'
complete -c pacmkr -l keepsrc -d 'Preserve sources after build'
complete -c pacmkr -l cleanafter -d 'Clean build dir after each package'
complete -c pacmkr -l answerclean -x -d 'Answer for clean-dir prompts'
complete -c pacmkr -l answerupgrade -x -d 'Answer for upgrade prompts'

# Build / PKGBUILD-compatible
complete -c pacmkr -s p -l packagefile -r -d 'Alternate build script'
complete -c pacmkr -s N -l nodeps -d 'Skip dependency checks'
complete -c pacmkr -s s -l syncdeps -d 'Install missing dependencies'
complete -c pacmkr -s o -l nobuild -d 'Download and extract only'
complete -c pacmkr -s C -l cleanbuild -d 'Remove existing src/ dir'
complete -c pacmkr -s c -l clean -d 'Clean work files after build'
complete -c pacmkr -s f -l force -d 'Overwrite an existing package'
complete -c pacmkr -s i -l install -d 'Install after a successful build'
complete -c pacmkr -s g -l geninteg -d 'Generate source checksums'
complete -c pacmkr -s e -l noextract -d 'Do not extract sources'
complete -c pacmkr -l check -d 'Run the check() function'
complete -c pacmkr -l nocheck -d 'Skip the check() function'
complete -c pacmkr -l noprepare -d 'Skip the prepare() function'
complete -c pacmkr -l verifysource -d 'Download and verify sources only'
complete -c pacmkr -l skipchecksums -d 'Skip checksum verification'
complete -c pacmkr -l skipinteg -d 'Skip all integrity checks'
complete -c pacmkr -l skippgpcheck -d 'Skip PGP signature checks'
complete -c pacmkr -l sign -d 'Sign the built package'
complete -c pacmkr -l nosign -d 'Do not sign the built package'
complete -c pacmkr -l key -x -d 'GPG key to sign with'
complete -c pacmkr -s r -l rmdeps -d 'Remove build-only deps afterward'
complete -c pacmkr -l printsrcinfo -d 'Print the generated .SRCINFO'
complete -c pacmkr -l holdver -d 'Do not update VCS sources'
complete -c pacmkr -l mflags -x -d 'Build-tool parallelism flags'
complete -c pacmkr -l graphite -x -a 'yes no auto' -d 'GCC Graphite loop optimizer'
complete -c pacmkr -l polly -x -a 'yes no auto' -d 'LLVM Polly optimizer'
complete -c pacmkr -l lto -x -a 'yes no auto' -d 'Link-time optimization'
complete -c pacmkr -l mold -x -a 'yes no auto' -d 'Use the mold linker'
complete -c pacmkr -l cc -x -a 'gcc clang' -d 'Compiler override'

# Transaction options
complete -c pacmkr -l needed -d 'Skip up-to-date targets'
complete -c pacmkr -l ignore -x -a '(__pacmkr_installed_pkgs)' -d 'Ignore a package during upgrade'
complete -c pacmkr -l ignoregroup -x -d 'Ignore a group during upgrade'
complete -c pacmkr -l overwrite -x -d 'Overwrite conflicting files (glob)'
complete -c pacmkr -s w -l downloadonly -d 'Download packages only'
complete -c pacmkr -l asdeps -d 'Mark installed packages as dependencies'
complete -c pacmkr -l asexplicit -d 'Mark installed packages as explicit'

# Package-name arguments, per operation
complete -c pacmkr -n '__pacmkr_using_op remove; or __pacmkr_using_op query' -f -a '(__pacmkr_installed_pkgs)'
complete -c pacmkr -n '__pacmkr_using_op sync' -f -a '(__pacmkr_sync_pkgs)'
complete -c pacmkr -n '__pacmkr_using_op upgrade' -F
