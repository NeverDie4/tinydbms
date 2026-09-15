#ifndef TINYDBMS_GUI_SESSION_STATE_HPP
#define TINYDBMS_GUI_SESSION_STATE_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include <QString>

#include "tinydbms/core.hpp"
#include "tinydbms/diagnostic.hpp"

namespace tinydbms::gui {

// GUI 自身的连接状态，只由结构化结果驱动，不从错误文本推断。
enum class SessionState {
    kClosed,       // 尚未打开，或 open 失败
    kOpen,         // 正常可用
    kNeedsReopen   // 致命中止或 cleanup-pending：只能重开或关闭窗口
};

// 一条语句在结果区的归类，与 StatementStatus 一一对应且保留部分成功语义。
enum class StatementDisplay {
    kQuery,
    kCommand,
    kPartialCommand,
    kExecutionError,
    kCompileError,
    kAnalysisError,
    kAnalysisOnly,
    kSkipped,
    kCancelled,
    kIndeterminate
};

struct StatementView {
    std::size_t index = 0;
    StatementDisplay display = StatementDisplay::kSkipped;
    QString summary;                                   // 结果区一行摘要
    QString detail;                                    // 诊断详情，可多行
    QString stage;                                     // 诊断列表的阶段标签
    std::optional<tinydbms::SourceRange> range;        // 可定位范围
    std::optional<tinydbms::FixIt> fix_it;             // 可应用的修复
    const tinydbms::core::QueryResult* query = nullptr;  // 仅 kQuery，生命周期由调用方保证
};

// 只有致命中止（kInternal）与失效会话上的调用（kExecute）会让会话失效；
// script_error 为 kCompile 时会话仍然健康，可以修改文本后直接重跑。
SessionState next_session_state(
    SessionState current,
    const tinydbms::core::ExecuteScriptResult& result) noexcept;

// 把一次执行结果转成界面条目；返回的 query 指针指向 result 内部，
// 调用方必须保证 result 在视图使用期间存活。
std::vector<StatementView> build_statement_views(
    const tinydbms::core::ExecuteScriptResult& result);

QString error_kind_label(const tinydbms::core::Error& error);
QString compile_stage_label(tinydbms::CompileStage stage);
QString format_range(const tinydbms::SourceRange& range);

// 脚本级横幅：无 script_error 时返回空字符串。
QString script_error_banner(const tinydbms::core::ExecuteScriptResult& result);

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SESSION_STATE_HPP
