#ifndef TINYDBMS_APP_ARGUMENTS_HPP
#define TINYDBMS_APP_ARGUMENTS_HPP

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tinydbms::app {

enum class CliAction {
    kRun,
    kHelp,
    kVersion
};

struct ParsedArguments {
    CliAction action = CliAction::kRun;
    std::string data_dir;
};

struct ArgumentError {
    std::string message;
};

using ParseArgumentsResult = std::variant<ParsedArguments, ArgumentError>;

inline constexpr std::string_view kDefaultDataDir = "./tinydbms-data";

ParseArgumentsResult parse_arguments(int argc, char* const argv[]);

bool write_help(std::ostream& output);
bool write_version(std::ostream& output, std::string_view version);

#ifdef _WIN32
std::optional<std::vector<std::string>> convert_windows_arguments(
    int argc,
    wchar_t* const argv[]);
#endif

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_ARGUMENTS_HPP
