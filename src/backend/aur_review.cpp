#include "pacmkr/backend/aur_review.h"

#include "pacmkr/app/terminal.h"
#include "pacmkr/core/error.h"

#include <cerrno>
#include <iostream>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace pacmkr::aur_review {
namespace {

struct CommandResult {
    int status{};
    std::string output;
};

CommandResult git(const std::filesystem::path& repository,
                  const std::vector<std::string>& arguments) {
    int output_pipe[2];
    if (pipe(output_pipe) != 0) throw source_error("cannot create Git review pipe");

    const pid_t child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        throw source_error("cannot start Git review process");
    }
    if (child == 0) {
        close(output_pipe[0]);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[1]);

        std::vector<std::string> storage{"git", "-C", repository.string()};
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    close(output_pipe[1]);
    std::string output;
    char buffer[8192];
    for (;;) {
        const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) output.append(buffer, static_cast<size_t>(count));
        else if (count == 0) break;
        else if (errno != EINTR) break;
    }
    close(output_pipe[0]);

    int wait_status = 0;
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {}
    const int status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128;
    return {status, std::move(output)};
}

std::string require_git(const std::filesystem::path& repository,
                        const std::vector<std::string>& arguments,
                        std::string_view action) {
    auto result = git(repository, arguments);
    if (result.status != 0) {
        throw source_error(std::string(action) + ": " + result.output);
    }
    while (!result.output.empty() &&
           (result.output.back() == '\n' || result.output.back() == '\r')) {
        result.output.pop_back();
    }
    return result.output;
}

std::vector<std::string> changed_files(const std::string& name_status) {
    std::vector<std::string> files;
    std::istringstream lines(name_status);
    for (std::string line; std::getline(lines, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        // Renames have two paths. The final path is the file the user is about
        // to execute or package, so it is the useful review identity.
        const auto final_tab = line.rfind('\t');
        files.push_back(line.substr(final_tab + 1));
    }
    return files;
}

bool added_line_contains(const std::string& patch, std::string_view needle) {
    std::istringstream lines(patch);
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with("+++") || line.empty() || line.front() != '+') continue;
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> risk_warnings(const std::vector<std::string>& files,
                                       const std::string& patch) {
    std::vector<std::string> warnings;
    for (const auto& file : files) {
        if (file.ends_with(".install")) {
            warnings.emplace_back("install script changed; it can run as root during transactions");
            break;
        }
    }
    if (added_line_contains(patch, "SKIP"))
        warnings.emplace_back("a checksum verification entry containing SKIP was added");
    if (added_line_contains(patch, "curl ") || added_line_contains(patch, "wget "))
        warnings.emplace_back("a direct network command was added to build files");
    if (added_line_contains(patch, "sudo ") || added_line_contains(patch, " su "))
        warnings.emplace_back("a privilege-changing command was added to build files");
    return warnings;
}

} // namespace

Review inspect(const std::filesystem::path& repository) {
    if (!std::filesystem::is_directory(repository / ".git"))
        throw source_error("cannot review a non-Git AUR directory: " + repository.string());

    Review review;
    review.repository = repository;
    review.package = repository.filename().string();
    review.commit = require_git(repository, {"rev-parse", "HEAD"}, "cannot identify AUR commit");

    auto previous = git(repository, {"config", "--local", "--get", "pacmkr.reviewedCommit"});
    if (previous.status == 0) {
        while (!previous.output.empty() &&
               (previous.output.back() == '\n' || previous.output.back() == '\r')) {
            previous.output.pop_back();
        }
        auto valid = git(repository, {"cat-file", "-e", previous.output + "^{commit}"});
        if (!previous.output.empty() && valid.status == 0) review.previous_commit = previous.output;
    }

    if (review.already_reviewed()) return review;

    std::vector<std::string> diff_args;
    std::vector<std::string> names_args;
    if (review.previous_commit) {
        diff_args = {"diff", "--no-ext-diff", "--find-renames",
                     *review.previous_commit, review.commit, "--"};
        names_args = {"diff", "--name-status", "--find-renames",
                      *review.previous_commit, review.commit, "--"};
    } else {
        diff_args = {"show", "--root", "--format=", "--no-ext-diff",
                     "--find-renames", review.commit, "--"};
        names_args = {"show", "--root", "--format=", "--name-status",
                      "--find-renames", review.commit, "--"};
    }

    review.patch = require_git(repository, diff_args, "cannot create AUR review diff");
    review.changed_files = changed_files(
        require_git(repository, names_args, "cannot list changed AUR files"));
    review.warnings = risk_warnings(review.changed_files, review.patch);
    return review;
}

bool confirm(const Review& review, bool skip_review) {
    if (skip_review || review.already_reviewed()) return true;

    terminal::section("Review AUR files: " + review.package);
    std::cout << "Commit: " << review.commit << "\n";
    if (review.previous_commit) std::cout << "Previously reviewed: " << *review.previous_commit << "\n";
    else std::cout << "Previously reviewed: never (showing complete initial contents)\n";

    if (!review.changed_files.empty()) {
        std::cout << "Changed files:\n";
        for (const auto& file : review.changed_files) std::cout << "  " << file << "\n";
    }
    for (const auto& warning : review.warnings) terminal::warning(warning);

    std::cout << "\n" << review.patch;
    if (!review.patch.empty() && review.patch.back() != '\n') std::cout << '\n';

    if (!isatty(STDIN_FILENO)) {
        throw source_error("AUR review requires a terminal; inspect interactively or pass --noreview explicitly");
    }
    std::cout << "\nApprove this exact AUR commit? [y/N] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

void mark_reviewed(const Review& review) {
    require_git(review.repository,
                {"config", "--local", "pacmkr.reviewedCommit", review.commit},
                "cannot record reviewed AUR commit");
}

} // namespace pacmkr::aur_review
