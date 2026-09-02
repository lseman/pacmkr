#include "pacmkr/status.h"

#include <iostream>
#include <string>
#include <unistd.h>

namespace pacmkr::status {

// ─── Helpers ──────────────────────────────────────────────────────────

static bool is_tty() { return isatty(STDOUT_FILENO) != 0; }

static void color(int code, const std::string& msg) {
    if (!is_tty()) { std::cout << msg; return; }
    std::cout << "\033[" << code << "m" << msg << "\033[0m";
}

static void cyan(const std::string& msg)  { color(36, msg); }
static void green(const std::string& msg) { color(32, msg); }
static void white(const std::string& msg) { color(37, msg); }
static void bold(const std::string& msg)  { color(1, msg); }
static void dim(const std::string& msg)   { color(2, msg); }

// ─── Banner ───────────────────────────────────────────────────────────

void print_upgrade_banner() {
    if (!is_tty()) {
        std::cout << "==> Synchronizing full system...\n";
        return;
    }

    std::cout << "\n";
    dim("  \xe2\x95\xad\xe2\x94\x80 ");
    cyan("\xe2\x97\x88 PACMKR");
    dim(" / system upgrade\n");
    dim("  \xe2\x95\xb0\xe2\x94\x80 Prepare \xe2\x86\x92 Sync \xe2\x86\x92 Upgrade \xe2\x86\x92 AUR\n");
}

// ─── Starting Messages ────────────────────────────────────────────────

void print_starting(const std::string& action) {
    if (!is_tty()) {
        std::cout << "==> " << action << "\n";
        return;
    }

    dim("  \xe2\x86\x92 ");
    white(action);
    std::cout << "\n";
}

// ─── Success Messages ─────────────────────────────────────────────────

void print_success(const std::string& msg, int count) {
    if (!is_tty()) {
        std::cout << "==> " << msg;
        if (count >= 0) std::cout << " (" << count << ")";
        std::cout << "\n";
        return;
    }

    green("  \xe2\x9c\x93 ");
    white(msg);
    if (count >= 0) {
        dim(" (" + std::to_string(count) + ")");
    }
    std::cout << "\n";
}

// ─── Separators ───────────────────────────────────────────────────────

void print_separator(char c, int width) {
    if (!is_tty()) {
        std::cout << std::string(width, c) << "\n";
        return;
    }

    dim("  ");
    for (int i = 0; i < width - 2; ++i) {
        std::cout << c;
    }
    std::cout << "\n";
}

// ─── Section Headers ──────────────────────────────────────────────────

void print_section(const std::string& title) {
    if (!is_tty()) {
        std::cout << "==> " << title << "\n";
        return;
    }

    std::cout << "\n";
    cyan("  \xe2\x97\x86 ");
    bold(title);
    std::cout << "\n";
}

// ─── Summary ──────────────────────────────────────────────────────────

void print_summary(int repo_count, int aur_count, bool everything_up_to_date) {
    if (!is_tty()) {
        if (everything_up_to_date) {
            std::cout << "==> Nothing to do.\n";
        } else {
            std::cout << "==> Upgrade complete. Repos: " << repo_count
                      << ", AUR: " << aur_count << "\n";
        }
        return;
    }

    if (everything_up_to_date) {
        green("  Everything is up to date.\n");
    } else {
        std::cout << "\n";
        green("  Upgrade complete.");
        dim(" Repos: ");
        white(std::to_string(repo_count));
        dim(", AUR: ");
        white(std::to_string(aur_count));
        std::cout << "\n";
    }
}

// ─── Status Lines ─────────────────────────────────────────────────────

void print_status(const std::string& msg) {
    if (!is_tty()) {
        std::cout << "==> " << msg << "\n";
        return;
    }

    dim("  \xe2\x86\xb3 ");
    white(msg);
    std::cout << "\n";
}

void print_warning(const std::string& msg) {
    if (!is_tty()) {
        std::cout << "==> WARNING: " << msg << "\n";
        return;
    }

    color(33, "  \xe2\x9a\xa0 ");
    white(msg);
    std::cout << "\n";
}

} // namespace pacmkr::status
