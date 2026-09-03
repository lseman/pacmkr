#include "pacmkr/build/pgp.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

namespace pacmkr::pgp {

namespace {

/// Execute a command and return exit code.
int exec_cmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

/// Extract key IDs from gpg --list-keys output.
std::vector<std::string> extract_key_ids(const std::string& output) {
    std::vector<std::string> keys;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        // Look for lines like: pub   rsa4096 2020-01-01 [SC] [expires: 2025-01-01]
        //                       Key fingerprint = ABCD 1234 EFAB 5678
        if (line.find("pub") != std::string::npos || line.find("sub") != std::string::npos) {
            // Extract the key ID from the fingerprint line
            auto pos = output.find(line);
            if (pos != std::string::npos) {
                // Look ahead for fingerprint
                std::string rest = output.substr(pos);
                auto fp_pos = rest.find("Key fingerprint");
                if (fp_pos != std::string::npos) {
                    // Extract last 8 chars of fingerprint as key ID
                    std::string fp_line;
                    std::istringstream fp_stream(rest.substr(fp_pos));
                    std::getline(fp_stream, fp_line);

                    // Key fingerprint format: "Key fingerprint = ABCD 1234 EFAB 5678"
                    auto eq_pos = fp_line.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string fp = fp_line.substr(eq_pos + 1);
                        // Remove spaces and get last 8 characters
                        fp.erase(std::remove(fp.begin(), fp.end(), ' '), fp.end());
                        if (fp.size() >= 8) {
                            keys.push_back(fp.substr(fp.size() - 8));
                        }
                    }
                }
            }
        }
    }

    return keys;
}

} // anonymous

// ─── Public API ──────────────────────────────────────────────────────

int import_key(const std::string& key_id) {
    // Try multiple keyservers
    static const char* keyservers[] = {
        "keyserver.ubuntu.com",
        "pgp.mit.edu",
        "keys.openpgp.org"
    };

    for (auto* server : keyservers) {
        std::ostringstream cmd;
        cmd << "gpg --no-tty --batch --keyserver " << server
            << " --recv-keys " << key_id << " 2>&1";

        int rc = exec_cmd(cmd.str());
        if (rc == 0) {
            std::cout << "==> Imported key " << key_id << " from " << server << "\n";
            return 0;
        }
    }

    std::cerr << "error: failed to import key " << key_id << " from any keyservers\n";
    return 1;
}

int fetch_keys(const std::vector<std::string>& key_ids) {
    int failures = 0;

    for (auto& key_id : key_ids) {
        if (import_key(key_id) != 0) {
            ++failures;
        }
    }

    return failures;
}

std::vector<std::string> list_keys(const std::string& pattern) {
    std::ostringstream cmd;
    cmd << "gpg --no-tty --batch --list-keys " << (pattern.empty() ? "" : pattern);

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return {};

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    pclose(pipe);

    return extract_key_ids(output);
}

int trust_key(const std::string& key_id) {
    // Set ownertrust to 6 (ultimate trust)
    std::ostringstream cmd;
    cmd << "echo \"6:" << key_id << ":\" | gpg --no-tty --batch --import-ownertrust 2>&1";

    return exec_cmd(cmd.str());
}

} // namespace pacmkr::pgp
