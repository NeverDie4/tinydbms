#include "compiler_fake.hpp"

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tinydbms::testing::fake_compiler {
namespace {

State fake_state;

// 与公共契约一致：\n 与 \r\n 各算一次换行，Tab 与独立的 \r 都按一字节一列。
SourceLocation location_at(std::string_view text, std::size_t offset) {
    int line = 1;
    std::size_t column = 1;
    std::size_t index = 0;
    while (index < offset) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            ++line;
            column = 1;
            index += 2;
            continue;
        }
        if (text[index] == '\n') {
            ++line;
            column = 1;
            ++index;
            continue;
        }
        ++column;
        ++index;
    }
    return SourceLocation{line, static_cast<int>(column)};
}

}  // namespace

State& state() {
    return fake_state;
}

void reset() {
    fake_state = State{};
}

void set_split_override(std::optional<std::vector<compiler::SplitStatement>> statements) {
    fake_state.split_override = std::move(statements);
}

void set_compile_results(std::deque<compiler::CompileResult> results) {
    fake_state.compile_results = std::move(results);
}

SourceRange make_range(
    std::string_view text,
    std::size_t begin_offset,
    std::size_t end_offset) {
    if (begin_offset > text.size()) {
        begin_offset = text.size();
    }
    if (end_offset > text.size()) {
        end_offset = text.size();
    }
    if (end_offset < begin_offset) {
        end_offset = begin_offset;
    }
    return SourceRange{
        location_at(text, begin_offset),
        location_at(text, end_offset),
        begin_offset,
        end_offset};
}

SourceRange make_range_of(std::string_view text, std::string_view needle) {
    const std::size_t position = needle.empty() ? std::string_view::npos : text.find(needle);
    if (position == std::string_view::npos) {
        return make_range(text, 0, text.size());
    }
    return make_range(text, position, position + needle.size());
}

compiler::CompileError make_compile_error(
    CompileStage stage,
    std::string_view statement_sql,
    std::size_t begin_offset,
    std::size_t end_offset,
    std::string message,
    std::optional<std::string> suggestion,
    std::optional<FixIt> fix_it) {
    return compiler::CompileError{
        stage,
        make_range(statement_sql, begin_offset, end_offset),
        std::move(message),
        std::move(suggestion),
        std::move(fix_it)};
}

}  // namespace tinydbms::testing::fake_compiler

namespace tinydbms::compiler {

SplitStatementsResult split_statements(std::string_view text) {
    auto& fake = testing::fake_compiler::state();
    ++fake.split_calls;
    if (fake.throw_on_split) {
        throw std::runtime_error{"fake split_statements failure"};
    }
    if (fake.split_override.has_value()) {
        std::vector<SplitStatement> override_statements = *fake.split_override;
        return SplitStatementsResult{std::move(override_statements)};
    }
    if (text.size() > kMaxSqlBytes) {
        return SplitStatementsResult{CompileError{
            CompileStage::kLex,
            SourceRange{SourceLocation{1, 1}, SourceLocation{1, 1}, 0, 0},
            "SQL text exceeds maximum length",
            std::nullopt,
            std::nullopt}};
    }

    std::vector<SplitStatement> statements;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }

        const std::size_t semicolon = text.find(';', cursor);
        const std::size_t end = semicolon == std::string_view::npos ? text.size() : semicolon + 1;
        statements.push_back(SplitStatement{
            std::string{text.substr(cursor, end - cursor)},
            testing::fake_compiler::make_range(text, cursor, end)});

        if (semicolon == std::string_view::npos) {
            break;
        }
        cursor = semicolon + 1;
    }
    return SplitStatementsResult{std::move(statements)};
}

CompileResult compile(const CompileRequest& request) {
    auto& fake = testing::fake_compiler::state();
    ++fake.compile_calls;
    if (fake.throw_on_compile_call != 0 && fake.compile_calls == fake.throw_on_compile_call) {
        throw std::runtime_error{"fake compile failure"};
    }
    fake.catalog_sizes.push_back(request.catalog.tables.size());
    std::vector<TableId> table_ids;
    table_ids.reserve(request.catalog.tables.size());
    for (const TableMeta& table : request.catalog.tables) {
        table_ids.push_back(table.table_id);
    }
    fake.catalog_table_ids.push_back(std::move(table_ids));

    auto& results = fake.compile_results;
    if (results.empty()) {
        return CompileResult{CompileError{
            CompileStage::kSyntax,
            testing::fake_compiler::make_range(request.sql, 0, 0),
            "fake compiler has no configured result",
            std::nullopt,
            std::nullopt}};
    }

    CompileResult result = std::move(results.front());
    results.pop_front();
    return result;
}

}  // namespace tinydbms::compiler
