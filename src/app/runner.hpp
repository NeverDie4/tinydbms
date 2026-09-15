#ifndef TINYDBMS_APP_RUNNER_HPP
#define TINYDBMS_APP_RUNNER_HPP

#include <iosfwd>
#include <string_view>

#include "session.hpp"

namespace tinydbms::app {

struct CliEnvironment {
    std::istream& input;
    std::ostream& output;
    std::ostream& error;
    bool interactive = false;
    // 未显式指定 --format 时，stdout 是终端则默认 pretty，否则 table。
    // 测试与 GUI 不填该字段，默认 false，保证既有 golden 测试仍走 table。
    bool output_is_terminal = false;
};

// 安装 CLI 的 SIGINT 处理器：首次 Ctrl+C 请求取消当前脚本，空闲期或第二次 Ctrl+C
// 恢复默认处置并重新触发。不安装时（GUI、测试）行为与现状完全一致。
void install_sigint_handler() noexcept;

int run_cli(
    int argc,
    char* const argv[],
    std::string_view version,
    Session& session,
    CliEnvironment& environment);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_RUNNER_HPP
