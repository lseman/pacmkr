#include "pacmkr/backend/local_repo.h"

#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    return pacmkr::local_repo::run(arguments);
}
