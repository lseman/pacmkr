# fish completion for pacmkr-repo

function __pacmkr_repo_needs_command
    set -l cmd (commandline -opc)
    test (count $cmd) -eq 1
end

function __pacmkr_repo_using
    set -l cmd (commandline -opc)
    test (count $cmd) -ge 2; and test $cmd[2] = $argv[1]
end

complete -c pacmkr-repo -f
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a create -d 'Register a repository directory'
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a add    -d 'Copy package archives in and index them'
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a remove -d 'Drop packages from a repository'
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a list   -d 'List registered repositories'
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a delete -d 'Unregister a repository (files preserved)'
complete -c pacmkr-repo -n __pacmkr_repo_needs_command -a help   -d 'Show usage'

complete -c pacmkr-repo -n '__pacmkr_repo_using add' -k -a '(__fish_complete_suffix .pkg.tar.zst)'
complete -c pacmkr-repo -n '__pacmkr_repo_using create' -a '(__fish_complete_directories)'
