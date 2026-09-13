#include "tinydbms/compiler.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include "lexer.hpp"
#include "optimizer.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "semantic.hpp"

namespace {

enum class SplitState {
    kNormal,
    kInString,
    kInLineComment,
    kInBlockComment
};

constexpr bool is_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

}  // namespace

namespace tinydbms::compiler {

SplitStatementsResult split_statements(std::string_view text) {
    if (text.size() > kMaxSqlBytes) {
        const SourceLocation start{1, 1, 0};
        return SplitStatementsResult{CompileError{
            CompileStage::kLex,
            SourceRange{start, start},
            "SQL text exceeds maximum length"}};
    }

    std::vector<SplitStatement> statements;
    SplitState state = SplitState::kNormal;
    std::size_t segment_start = 0;
    SourceLocation segment_location{1, 1, 0};
    int line = 1;
    int column = 1;
    bool has_sql_content = false;

    const auto advance_location = [&line, &column](char value) {
        if (value == '\n') {
            ++line;
            column = 1;
        } else if (value != '\r') {
            ++column;
        }
    };

    const auto append_segment = [&statements, text, &segment_location](
                                    std::size_t begin,
                                    std::size_t end,
                                    SourceLocation end_location) {
        statements.push_back(SplitStatement{
            std::string{text.substr(begin, end - begin)},
            SourceRange{segment_location, end_location}
        });
    };

    std::size_t index = 0;
    while (index < text.size()) {
        const char current = text[index];
        const bool has_next = index + 1 < text.size();
        const char next = has_next ? text[index + 1] : '\0';

        switch (state) {
            case SplitState::kNormal:
                if (current == '\'') {
                    has_sql_content = true;
                    state = SplitState::kInString;
                    advance_location(current);
                    ++index;
                } else if (current == '-' && next == '-') {
                    state = SplitState::kInLineComment;
                    advance_location(current);
                    advance_location(next);
                    index += 2;
                } else if (current == '/' && next == '*') {
                    state = SplitState::kInBlockComment;
                    advance_location(current);
                    advance_location(next);
                    index += 2;
                } else if (current == ';') {
                    advance_location(current);
                    ++index;
                    const SourceLocation end_location{line, column, index};
                    if (has_sql_content) {
                        append_segment(segment_start, index, end_location);
                    }
                    segment_start = index;
                    segment_location = end_location;
                    has_sql_content = false;
                } else {
                    if (!is_whitespace(current)) {
                        has_sql_content = true;
                    }
                    advance_location(current);
                    ++index;
                }
                break;

            case SplitState::kInString:
                if (current == '\'' && next == '\'') {
                    advance_location(current);
                    advance_location(next);
                    index += 2;
                } else {
                    if (current == '\'') {
                        state = SplitState::kNormal;
                    }
                    advance_location(current);
                    ++index;
                }
                break;

            case SplitState::kInLineComment:
                advance_location(current);
                ++index;
                if (current == '\n') {
                    state = SplitState::kNormal;
                }
                break;

            case SplitState::kInBlockComment:
                if (current == '*' && next == '/') {
                    advance_location(current);
                    advance_location(next);
                    index += 2;
                    state = SplitState::kNormal;
                } else {
                    advance_location(current);
                    ++index;
                }
                break;
        }
    }

    const bool has_unterminated_input =
        state == SplitState::kInString || state == SplitState::kInBlockComment;
    if (has_sql_content || has_unterminated_input) {
        append_segment(
            segment_start,
            text.size(),
            SourceLocation{line, column, text.size()});
    }

    return SplitStatementsResult{std::move(statements)};
}

CompileResult compile(const CompileRequest& request) {
    if (request.sql.size() > kMaxSqlBytes) {
        const SourceLocation start{1, 1, 0};
        return CompileResult{CompileError{
            CompileStage::kLex,
            SourceRange{start, start},
            "SQL text exceeds maximum length"
        }};
    }

    internal::LexResult lexed = internal::tokenize(request.sql);
    if (auto* error = std::get_if<CompileError>(&lexed.outcome)) {
        return CompileResult{std::move(*error)};
    }
    auto tokens = std::get<std::vector<internal::Token>>(std::move(lexed.outcome));

    internal::ParseResult parsed = internal::parse(tokens);
    if (auto* error = std::get_if<CompileError>(&parsed.outcome)) {
        return CompileResult{std::move(*error)};
    }
    const auto& statement = std::get<internal::StatementAst>(parsed.outcome);

    internal::SemanticResult analyzed = internal::analyze(statement, request.catalog);
    if (auto* error = std::get_if<CompileError>(&analyzed.outcome)) {
        return CompileResult{std::move(*error)};
    }
    internal::BoundStatement bound =
        std::get<internal::BoundStatement>(std::move(analyzed.outcome));
    internal::BoundStatement optimized = internal::optimize(std::move(bound));

    internal::PlannerResult planned = std::visit(
        [&request](auto&& value) -> internal::PlannerResult {
            using StatementType = std::decay_t<decltype(value)>;
            if constexpr (
                std::is_same_v<StatementType, internal::BoundSelect> ||
                std::is_same_v<StatementType, internal::BoundDelete> ||
                std::is_same_v<StatementType, internal::BoundUpdate>) {
                return internal::generate_plan(std::move(value), request.catalog);
            } else {
                return internal::PlannerResult{
                    internal::generate_plan(std::move(value))};
            }
        },
        std::move(optimized.kind));
    if (auto* error = std::get_if<CompileError>(&planned.outcome)) {
        return CompileResult{std::move(*error)};
    }
    return CompileResult{std::get<Plan>(std::move(planned.outcome))};
}

}
