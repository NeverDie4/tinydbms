#ifndef TINYDBMS_APP_ARGUMENTS_HPP
#define TINYDBMS_APP_ARGUMENTS_HPP

#include <iosfwd>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "output.hpp"

namespace tinydbms::app {

enum class CliAction {
    kRun,
    kHelp,
    kVersion
};

enum class ErrorPolicy {
    kStop,
    kAnalyze
};

struct ParsedArguments {
    CliAction action = CliAction::kRun;
    std::string data_dir;
    ErrorPolicy error_policy = ErrorPolicy::kStop;
    OutputFormat format = OutputFormat::kTable;
    // --format 是否显式出现：未出现时由 runner 按 stdout 是否终端选择默认格式。
    bool format_explicit = false;
    // --time：每次 execute_script 调用在 stderr 输出一行墙钟耗时。
    bool show_time = false;
    // --stats：每次 execute_script 调用在 stderr 输出一行 buffer pool 统计快照。
    bool show_stats = false;
    bool plan_only = false;
    // 未提供时为 nullopt；由 runner 决定默认值，app 参数层不复制 core 的常量。
    std::optional<std::size_t> max_query_rows;
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
