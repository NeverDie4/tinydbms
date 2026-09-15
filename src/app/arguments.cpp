#include "arguments.hpp"

#include "tinydbms/core.hpp"

#include <charconv>
#include <cstddef>
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

    ParsedArguments result;
    result.data_dir = std::string{kDefaultDataDir};
    bool data_dir_seen = false;
    bool error_policy_seen = false;
    bool format_seen = false;
    bool time_seen = false;
    bool plan_seen = false;
    bool max_rows_seen = false;

    const auto take_value = [&](int index, std::string_view option, std::string& message) {
        if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0') {
            message = std::string{option} + " requires a non-empty value";
            return false;
        }
        const std::string_view value{argv[index + 1]};
        if (value.size() >= 2 && value[0] == '-' && value[1] == '-') {
            message = std::string{option} + " requires a non-empty value";
            return false;
        }
        return true;
    };

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
        if (argument == "--error-policy") {
            if (error_policy_seen) {
                return make_error("--error-policy may appear only once");
            }
            error_policy_seen = true;

            std::string message;
            if (!take_value(index, "--error-policy", message)) {
                return make_error(std::move(message));
            }
            const std::string_view value{argv[++index]};
            if (value == "stop") {
                result.error_policy = ErrorPolicy::kStop;
            } else if (value == "analyze") {
                result.error_policy = ErrorPolicy::kAnalyze;
            } else {
                return make_error("--error-policy must be stop or analyze");
            }
            continue;
        }
        if (argument == "--format") {
            if (format_seen) {
                return make_error("--format may appear only once");
            }
            format_seen = true;
            result.format_explicit = true;

            std::string message;
            if (!take_value(index, "--format", message)) {
                return make_error(std::move(message));
            }
            const std::string_view value{argv[++index]};
            if (value == "table") {
                result.format = OutputFormat::kTable;
            } else if (value == "pretty") {
                result.format = OutputFormat::kPretty;
            } else if (value == "json") {
                result.format = OutputFormat::kJson;
            } else {
                return make_error("--format must be table, pretty or json");
            }
            continue;
        }
        if (argument == "--time") {
            if (time_seen) {
                return make_error("--time may appear only once");
            }
            time_seen = true;
            result.show_time = true;
            continue;
        }
        if (argument == "--plan") {
            if (plan_seen) {
                return make_error("--plan may appear only once");
            }
            plan_seen = true;
            result.plan_only = true;
            continue;
        }
        if (argument == "--max-rows") {
            if (max_rows_seen) {
                return make_error("--max-rows may appear only once");
            }
            max_rows_seen = true;

            std::string message;
            if (!take_value(index, "--max-rows", message)) {
                return make_error(std::move(message));
            }
            const std::string_view value{argv[++index]};
            std::size_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || parsed == 0U) {
                return make_error("--max-rows must be a positive integer");
            }
            result.max_query_rows = parsed;
            continue;
        }
        if (argument != "--data-dir") {
            return make_error("unknown command-line argument");
        }
        if (data_dir_seen) {
            return make_error("--data-dir may appear only once");
        }
        data_dir_seen = true;

        std::string message;
        if (!take_value(index, "--data-dir", message)) {
            return make_error(std::move(message));
        }
        result.data_dir = argv[++index];
    }

    return result;
}

bool write_help(std::ostream& output) {
    output << "Usage: tinydbms [OPTIONS]\n"
           << "\n"
           << "Read SQL from an interactive terminal or stdin.\n"
           << "\n"
           << "  --help                 print this help and exit\n"
           << "  --version              print the version and exit\n"
           << "  --data-dir DIR         database directory (default: " << kDefaultDataDir << ")\n"
           << "  --error-policy POLICY  stop (default) or analyze; analyze only inspects later\n"
           << "                         statements and never executes them\n"
           << "  --format FORMAT        table, pretty or json; pretty is the default when\n"
           << "                         stdout is a terminal, table otherwise; json prints\n"
           << "                         one JSON object per line\n"
           << "  --time                 print the wall-clock time of each executed script or\n"
           << "                         REPL line to stderr\n"
           << "  --plan                 compile only and print the execution plan; nothing is\n"
           << "                         executed and no data is modified\n"
           << "  --max-rows N           maximum rows materialized for one statement\n"
           << "                         (default: " << tinydbms::core::kMaxQueryRows << ")\n";
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
