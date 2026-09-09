#include "arguments.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <vector>
#endif

namespace tinydbms::app {
namespace {

ArgumentError make_error(std::string message) {
    return ArgumentError{std::move(message)};
}

}  // namespace

ParseArgumentsResult parse_arguments(int argc, char* const argv[]) {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return make_error("invalid command-line arguments");
    }

    ParsedArguments result{CliAction::kRun, std::string{kDefaultDataDir}};
    bool data_dir_seen = false;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return make_error("invalid command-line argument");
        }

        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            if (argc != 2) {
                return make_error("--help must be used alone");
            }
            result.action = CliAction::kHelp;
            return result;
        }
        if (argument == "--version") {
            if (argc != 2) {
                return make_error("--version must be used alone");
            }
            result.action = CliAction::kVersion;
            return result;
        }
        if (argument != "--data-dir") {
            return make_error("unknown command-line argument");
        }
        if (data_dir_seen) {
            return make_error("--data-dir may appear only once");
        }
        data_dir_seen = true;

        if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0') {
            return make_error("--data-dir requires a non-empty value");
        }
        const std::string_view value{argv[index + 1]};
        if (value.size() >= 2 && value[0] == '-' && value[1] == '-') {
            return make_error("--data-dir requires a non-empty value");
        }
        result.data_dir = argv[++index];
    }

    return result;
}

bool write_help(std::ostream& output) {
    output << "Usage: tinydbms [--help|--version|--data-dir DIR]\n"
           << "\n"
           << "Read SQL from an interactive terminal or stdin.\n"
           << "Default data directory: " << kDefaultDataDir << "\n";
    return static_cast<bool>(output);
}

bool write_version(std::ostream& output, std::string_view version) {
    output << "tinydbms " << version << '\n';
    return static_cast<bool>(output);
}

#ifdef _WIN32

std::optional<std::vector<std::string>> convert_windows_arguments(
    int argc,
    wchar_t* const argv[]) {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return std::nullopt;
    }

    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return std::nullopt;
        }

        const int required = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            argv[index],
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 0) {
            return std::nullopt;
        }

        std::vector<char> buffer(static_cast<std::size_t>(required));
        const int written = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            argv[index],
            -1,
            buffer.data(),
            required,
            nullptr,
            nullptr);
        if (written <= 0) {
            return std::nullopt;
        }
        result.emplace_back(buffer.data(), static_cast<std::size_t>(written - 1));
    }
    return result;
}

#endif

}  // namespace tinydbms::app
