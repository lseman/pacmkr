# bash completion for pacmkr-repo                          -*- shell-script -*-

_pacmkr_repo() {
    local cur prev
    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=( $(compgen -W 'create add remove list delete help' -- "$cur") )
        return
    fi

    local sub=${COMP_WORDS[1]}
    case $sub in
        create)
            [[ $COMP_CWORD -ge 3 ]] && { compgen -d -- "$cur" >/dev/null && COMPREPLY=( $(compgen -d -- "$cur") ); }
            ;;
        add)
            if [[ $COMP_CWORD -ge 3 ]]; then
                COMPREPLY=( $(compgen -f -X '!*.pkg.tar*' -- "$cur") $(compgen -d -- "$cur") )
            fi
            ;;
        delete|remove)
            ;;
    esac
}

complete -F _pacmkr_repo pacmkr-repo
