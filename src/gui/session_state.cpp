#include "session_state.hpp"

#include <QString>
#include <QStringList>

#include <utility>
#include <variant>

namespace tinydbms::gui {
namespace {

using tinydbms::core::CommandResult;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementResult;
using tinydbms::core::StatementStatus;
using tinydbms::SourceRange;

QString statement_prefix(std::size_t index) {
    return QStringLiteral("语句 %1：").arg(static_cast<qulonglong>(index + 1));
}

QString error_detail(const Error& error, const std::optional<SourceRange>& range) {
    QStringList lines;
    if (range.has_value()) {
        lines.append(QStringLiteral("位置 %1").arg(format_range(*range)));
    }
    QString headline = error_kind_label(error);
    if (error.source.has_value()) {
        headline += QStringLiteral("（%1）").arg(format_range(*error.source));
    }
    lines.append(headline);
    lines.append(QString::fromUtf8(error.message));
    if (error.suggestion.has_value()) {
        lines.append(QStringLiteral("建议：%1").arg(QString::fromUtf8(*error.suggestion)));
    }
    if (error.fix_it.has_value()) {
        lines.append(
            QStringLiteral("修复：把 %1 替换为 \"%2\"")
                .arg(
                    format_range(error.fix_it->range),
                    QString::fromUtf8(error.fix_it->replacement)));
    }
    return lines.join(QLatin1Char('\n'));
}

StatementDisplay display_of(const StatementResult& statement) {
    switch (statement.status()) {
    case StatementStatus::kExecuted: {
        const auto& outcome = statement.outcome();
        if (outcome.has_value() && std::holds_alternative<QueryResult>(outcome->outcome)) {
            return StatementDisplay::kQuery;
        }
        return StatementDisplay::kCommand;
    }
    // 计划模式的结果也是查询结果形状（单列 VARCHAR），按查询展示。
    case StatementStatus::kPlanOnly: {
        const auto& outcome = statement.outcome();
        if (outcome.has_value() && std::holds_alternative<QueryResult>(outcome->outcome)) {
            return StatementDisplay::kQuery;
        }
        return StatementDisplay::kExecutionError;
    }
    case StatementStatus::kCompileError:
        return StatementDisplay::kCompileError;
    case StatementStatus::kExecutionError: {
        const auto& outcome = statement.outcome();
        if (outcome.has_value()) {
            if (const CommandResult* command = std::get_if<CommandResult>(&outcome->outcome);
                command != nullptr) {
                return StatementDisplay::kPartialCommand;
            }
        }
        return StatementDisplay::kExecutionError;
    }
    case StatementStatus::kAnalysisError:
        return StatementDisplay::kAnalysisError;
    case StatementStatus::kAnalysisOnly:
        return StatementDisplay::kAnalysisOnly;
    case StatementStatus::kSkippedExecution:
        return StatementDisplay::kSkipped;
    case StatementStatus::kExecutionIndeterminate:
        return StatementDisplay::kIndeterminate;
    }
    return StatementDisplay::kSkipped;
}

QString stage_of(const StatementResult& statement) {
    const auto& outcome = statement.outcome();
    if (!outcome.has_value()) {
        return QString();
    }
    const Error* error = std::get_if<Error>(&outcome->outcome);
    if (error == nullptr) {
        return QString();
    }
    return error_kind_label(*error);
}

const Error* error_of(const StatementResult& statement) {
    const auto& outcome = statement.outcome();
    if (!outcome.has_value()) {
        return nullptr;
    }
    if (const Error* error = std::get_if<Error>(&outcome->outcome); error != nullptr) {
        return error;
    }
    if (const CommandResult* command = std::get_if<CommandResult>(&outcome->outcome);
        command != nullptr && command->error.has_value()) {
        return &*command->error;
    }
    return nullptr;
}

}  // namespace

SessionState next_session_state(SessionState current, const ExecuteScriptResult& result) noexcept {
    if (!result.script_error.has_value()) {
        return current;
    }
    switch (result.script_error->kind) {
    case ErrorKind::kCompile:
        // 分句失败与语句数超限不使会话失效，修改文本后可以直接重跑。
        return current;
    case ErrorKind::kExecute:
    case ErrorKind::kStorage:
    case ErrorKind::kAnalysis:
    case ErrorKind::kInternal:
        return SessionState::kNeedsReopen;
    }
    return SessionState::kNeedsReopen;
}

std::vector<StatementView> build_statement_views(const ExecuteScriptResult& result) {
    std::vector<StatementView> views;
    views.reserve(result.statements.size());

    for (const StatementResult& statement : result.statements) {
        StatementView view;
        view.index = statement.statement_index();
        view.display = display_of(statement);
        view.range = statement.source();
        view.stage = stage_of(statement);

        if (const Error* error = error_of(statement); error != nullptr) {
            if (error->fix_it.has_value()) {
                view.fix_it = error->fix_it;
            }
            view.detail = error_detail(*error, statement.source());
            if (view.stage.isEmpty()) {
                view.stage = error_kind_label(*error);
            }
        }

        switch (view.display) {
        case StatementDisplay::kQuery: {
            const auto& outcome = statement.outcome();
            const QueryResult& query = std::get<QueryResult>(outcome->outcome);
            view.query = &query;
            view.summary = statement_prefix(view.index) +
                QStringLiteral("查询返回 %1 行").arg(static_cast<qulonglong>(query.rows.size()));
            break;
        }
        case StatementDisplay::kCommand: {
            const auto& outcome = statement.outcome();
            const CommandResult& command = std::get<CommandResult>(outcome->outcome);
            view.summary = statement_prefix(view.index) +
                QStringLiteral("OK，影响 %1 行")
                    .arg(static_cast<qulonglong>(command.affected_rows));
            break;
        }
        case StatementDisplay::kPartialCommand: {
            const auto& outcome = statement.outcome();
            const CommandResult& command = std::get<CommandResult>(outcome->outcome);
            view.summary = statement_prefix(view.index) +
                QStringLiteral("已影响 %1 行后失败")
                    .arg(static_cast<qulonglong>(command.affected_rows));
            break;
        }
        case StatementDisplay::kExecutionError:
            view.summary = statement_prefix(view.index) + QStringLiteral("执行失败");
            break;
        case StatementDisplay::kCompileError:
            view.summary = statement_prefix(view.index) + QStringLiteral("编译失败");
            break;
        case StatementDisplay::kAnalysisError:
            view.summary = statement_prefix(view.index) + QStringLiteral("分析被拒绝");
            break;
        case StatementDisplay::kAnalysisOnly:
            view.summary = statement_prefix(view.index) + QStringLiteral("已分析，未执行");
            break;
        case StatementDisplay::kSkipped:
            view.summary = statement_prefix(view.index) + QStringLiteral("已跳过");
            break;
        case StatementDisplay::kIndeterminate:
            view.summary = statement_prefix(view.index) +
                QStringLiteral("状态未知，可能已产生副作用");
            break;
        }
        views.push_back(std::move(view));
    }

    return views;
}

QString error_kind_label(const Error& error) {
    switch (error.kind) {
    case ErrorKind::kCompile:
        if (error.compile_stage.has_value()) {
            return QStringLiteral("编译错误（%1）").arg(compile_stage_label(*error.compile_stage));
        }
        return QStringLiteral("编译错误");
    case ErrorKind::kExecute:
        return QStringLiteral("执行错误");
    case ErrorKind::kStorage:
        return QStringLiteral("存储错误");
    case ErrorKind::kAnalysis:
        return QStringLiteral("分析错误");
    case ErrorKind::kInternal:
        return QStringLiteral("内部错误");
    }
    return QStringLiteral("未知错误");
}

QString compile_stage_label(tinydbms::CompileStage stage) {
    switch (stage) {
    case tinydbms::CompileStage::kLex:
        return QStringLiteral("词法");
    case tinydbms::CompileStage::kSyntax:
        return QStringLiteral("语法");
    case tinydbms::CompileStage::kSemantic:
        return QStringLiteral("语义");
    }
    return QStringLiteral("未知");
}

QString format_range(const SourceRange& range) {
    return QStringLiteral("%1:%2-%3:%4")
        .arg(range.begin.line)
        .arg(range.begin.column)
        .arg(range.end.line)
        .arg(range.end.column);
}

QString script_error_banner(const ExecuteScriptResult& result) {
    if (!result.script_error.has_value()) {
        return QString();
    }
    const Error& error = *result.script_error;
    QString banner = error_kind_label(error);
    banner += QLatin1Char('\n');
    banner += QString::fromUtf8(error.message);
    if (error.source.has_value()) {
        banner += QStringLiteral("\n位置 %1").arg(format_range(*error.source));
    }
    if (error.suggestion.has_value()) {
        banner += QStringLiteral("\n建议：%1").arg(QString::fromUtf8(*error.suggestion));
    }
    return banner;
}

}  // namespace tinydbms::gui
