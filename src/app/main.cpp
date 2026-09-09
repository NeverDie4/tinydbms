#include "arguments.hpp"
#include "output.hpp"
#include "runner.hpp"
#include "session.hpp"
#include "terminal.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int run_application(int argc, char* const argv[]) {
    try {
#ifdef TINYDBMS_ENABLE_REAL_MODULES
        tinydbms::app::CoreSession session;
#else
        tinydbms::app::UnavailableSession session;
#endif
        tinydbms::app::CliEnvironment environment{
            std::cin,
            std::cout,
            std::cerr,
            tinydbms::app::stdin_is_terminal()};
        return tinydbms::app::run_cli(
            argc,
            argv,
            std::string_view{TINYDBMS_VERSION},
            session,
            environment);
    } catch (const std::exception& exception) {
        try {
            std::cerr << "ERROR internal application exception: "
                      << tinydbms::app::escape_text(
                             exception.what() == nullptr ? "unknown exception" : exception.what())
                      << '\n';
        } catch (...) {
            // There is no further recovery path if stderr itself is unavailable.
        }
        return 1;
    } catch (...) {
        try {
            std::cerr << "ERROR internal application raised an unknown exception\n";
        } catch (...) {
            // There is no further recovery path if stderr itself is unavailable.
        }
        return 1;
    }
}

#ifdef _WIN32

int run_windows_application(int argc, wchar_t* const argv[]) {
    try {
        const auto converted = tinydbms::app::convert_windows_arguments(argc, argv);
        if (!converted.has_value()) {
            std::cerr << "ERROR internal failed to convert command-line arguments to UTF-8\n";
            return 1;
        }

        std::vector<char*> narrow_argv;
        narrow_argv.reserve(converted->size());
        for (std::string& argument : *converted) {
            narrow_argv.push_back(argument.data());
        }
        return run_application(
            static_cast<int>(narrow_argv.size()),
            narrow_argv.data());
    } catch (const std::exception& exception) {
        std::cerr << "ERROR internal application exception: "
                  << tinydbms::app::escape_text(
                         exception.what() == nullptr ? "unknown exception" : exception.what())
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "ERROR internal application raised an unknown exception\n";
        return 1;
    }
}

#endif

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    return run_windows_application(argc, argv);
}
#else
int main(int argc, char* argv[]) {
    return run_application(argc, argv);
}
#endif
