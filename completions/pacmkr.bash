# bash completion for pacmkr                               -*- shell-script -*-
#
# Installed as $prefix/share/bash-completion/completions/pacmkr

_pacmkr_repo_names() {
    pacman-conf --repo-list 2>/dev/null
}

_pacmkr_sync_pkgs() {
    # All packages available in the sync repositories (+ AUR is not listed here;
    # pacmkr resolves AUR names at request time).
    pacman -Slq 2>/dev/null
}

_pacmkr_installed_pkgs() {
    pacman -Qq 2>/dev/null
}

_pacmkr_foreign_pkgs() {
    pacman -Qmq 2>/dev/null
}

_pacmkr() {
    local cur prev words cword
    if declare -F _init_completion >/dev/null 2>&1; then
        _init_completion || return
    else
        COMPREPLY=()
        cur=${COMP_WORDS[COMP_CWORD]}
        prev=${COMP_WORDS[COMP_CWORD-1]}
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
    fi

    local common_opts='
        --help --version --flags --config --noconfirm --color --nocolor
        --debug --json --dry-run
    '
    local pkgbuild_opts='
        --packagefile --log --nodeps --syncdeps --nobuild --noextract --check
        --nocheck --noprepare --cleanbuild --clean --force --install --noarchive
        --geninteg --skipchecksums --skipinteg --skippgpcheck --verifysource
        --allsource --sign --nosign --key --repackage --rmdeps --ignorearch
        --dir --packagelist --printsrcinfo --holdver --mflags
    '
    local opt_opts='--graphite --polly --lto --mold --cc'
    local aur_opts='
        --aur --aur-deps --noreview --search --limit --aur-dir --refresh --no-deps-resolve
        --getpkgbuild --list-foreign --orphans --nodevel --devel --cleanup
        --cleanup-age --sortby --answerclean --answerupgrade --pgpfetch --keepsrc
        --cleanafter
    '
    local sync_opts='
        --sysupgrade --refresh --search --info --list --groups --downloadonly
        --clean --needed --ignore --ignoregroup --overwrite --nodownload
        --asdeps --asexplicit --all-deps --build
    '

    # Value-taking options: complete the argument, not a new flag.
    case $prev in
        --config|--packagefile|-p|--dir|-D|--aur-dir|--key)
            _filedir 2>/dev/null || COMPREPLY=( $(compgen -f -- "$cur") )
            return
            ;;
        --cc)
            COMPREPLY=( $(compgen -W 'gcc clang' -- "$cur") )
            return
            ;;
        --lto|--mold|--graphite|--polly)
            COMPREPLY=( $(compgen -W 'yes no auto' -- "$cur") )
            return
            ;;
        --sortby)
            COMPREPLY=( $(compgen -W 'votes popular updated' -- "$cur") )
            return
            ;;
        --ignore|--ignoregroup)
            COMPREPLY=( $(compgen -W "$(_pacmkr_installed_pkgs)" -- "$cur") )
            return
            ;;
        --limit|--cleanup-age|--overwrite|--mflags|--answerclean|--answerupgrade|--search)
            return
            ;;
    esac

    # Figure out the primary operation from the argument list.
    local op=""
    local w
    for w in "${words[@]:1:cword-1}"; do
        case $w in
            -S*|--sync)     op=sync ;;
            -R*|--remove)   op=remove ;;
            -Q*|--query)    op=query ;;
            -U|--upgrade)   op=upgrade ;;
            -F*|--files)    op=files ;;
            -D|--database)  op=database ;;
            -B|--build)     op=build ;;
        esac
    done

    if [[ $cur == -* ]]; then
        local set="$common_opts"
        case $op in
            sync)    set="$set $sync_opts $aur_opts $opt_opts $pkgbuild_opts" ;;
            build)   set="$set $pkgbuild_opts $opt_opts" ;;
            remove)  set="$set --nosave --recursive --cascade --nodeps --unneeded" ;;
            query)   set="$set --info --list --owns --file --changelog --explicit --deps --unrequired --upgrades --search --quiet" ;;
            *)       set="$set $sync_opts $aur_opts $opt_opts $pkgbuild_opts" ;;
        esac
        COMPREPLY=( $(compgen -W "$set" -- "$cur") )
        return
    fi

    # Bare word: complete package names appropriate to the operation.
    case $op in
        remove|query)
            COMPREPLY=( $(compgen -W "$(_pacmkr_installed_pkgs)" -- "$cur") )
            ;;
        upgrade)
            _filedir 2>/dev/null
            ;;
        sync|"")
            COMPREPLY=( $(compgen -W "$(_pacmkr_sync_pkgs)" -- "$cur") )
            ;;
    esac
}

complete -F _pacmkr pacmkr
