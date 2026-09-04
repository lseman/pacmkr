#compdef pacmkr-repo
# zsh completion for pacmkr-repo

_pacmkr_repo() {
    local -a subcommands
    subcommands=(
        'create:Register a repository directory'
        'add:Copy package archives into a repository and index them'
        'remove:Drop packages from a repository'
        'list:List registered repositories'
        'delete:Unregister a repository (files preserved)'
        'help:Show usage'
    )

    if (( CURRENT == 2 )); then
        _describe -t commands 'pacmkr-repo command' subcommands
        return
    fi

    case ${words[2]} in
        create)
            case $CURRENT in
                3) _message 'repository name' ;;
                4) _files -/ ;;
            esac
            ;;
        add)
            if (( CURRENT == 3 )); then
                _message 'repository name'
            else
                _files -g '*.pkg.tar*'
            fi
            ;;
        remove|delete)
            (( CURRENT == 3 )) && _message 'repository name'
            ;;
    esac
}

_pacmkr_repo "$@"
