#include <iostream>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "tinydbms/core.h"

namespace {

void print_help(std::ostream& output) {
    output << "Usage: tinydbms [--help|--version|--data-dir DIR]\n"
           << "\n"
           << "Educational DBMS development scaffold.\n"
           << "The initialized phase provides database lifecycle and SQL statement splitting.\n";
}

void print_error(const tinydbms::core::Error& error) {
    std::cerr << "error at " << error.location.line << ':' << error.location.column << ": "
              << error.message << '\n';
}

int run_script(const std::string& script) {
    const auto result = tinydbms::core::execute_script({script});
    for (const auto& outcome : result.outcomes) {
        if (const auto* error = std::get_if<tinydbms::core::Error>(&outcome.value)) {
            print_error(*error);
            return 1;
        }
    }
    return 0;
}

bool is_interactive_input() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string data_dir = "tinydbms-data";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--version") {
            std::cout << "tinydbms " << TINYDBMS_VERSION << '\n';
            return 0;
        }
        if (argument == "--help") {
            print_help(std::cout);
            return 0;
        }
        if (argument == "--data-dir") {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for --data-dir\n";
                return 2;
            }
            data_dir = argv[++index];
            continue;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        print_help(std::cerr);
        return 2;
    }

    const auto opened = tinydbms::core::open_database({data_dir});
    if (opened.error.has_value()) {
        print_error(*opened.error);
        return 1;
    }

    std::cout << "tinydbms ready; data directory: " << std::filesystem::path(data_dir).string()
              << '\n';
    std::cout << "Enter SQL terminated by ';'. EOF exits.\n";

    int exit_code = 0;
    if (is_interactive_input()) {
        std::string line;
        while (std::cout << "tinydbms> " && std::getline(std::cin, line)) {
            if (run_script(line) != 0) {
                exit_code = 1;
            }
        }
    } else {
        std::ostringstream script;
        script << std::cin.rdbuf();
        if (run_script(script.str()) != 0) {
            exit_code = 1;
        }
    }

    const auto closed = tinydbms::core::close_database();
    if (closed.error.has_value()) {
        print_error(*closed.error);
        exit_code = 1;
    }
    return exit_code;
}
