#include "compiler_fake.hpp"

#include <cctype>
#include <string>
#include <utility>

namespace tinydbms::testing::fake_compiler {
namespace {

State fake_state;

}  // namespace

State& state() {
    return fake_state;
}

void reset() {
    fake_state = State{};
}

void set_compile_results(std::deque<compiler::CompileResult> results) {
    fake_state.compile_results = std::move(results);
}

}  // namespace tinydbms::testing::fake_compiler

namespace tinydbms::compiler {
namespace {

SourceLocation location_at(std::string_view text, std::size_t offset) {
    int line = 1;
    int column = 1;
    for (std::size_t index = 0; index < offset; ++index) {
        if (text[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return SourceLocation{line, column};
}

}  // namespace

std::vector<SplitStatement> split_statements(std::string_view text) {
    ++testing::fake_compiler::state().split_calls;
    std::vector<SplitStatement> statements;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }

        const std::size_t semicolon = text.find(';', cursor);
        const std::size_t end = semicolon == std::string_view::npos ? text.size() : semicolon + 1;
        statements.push_back(SplitStatement{
            std::string{text.substr(cursor, end - cursor)},
            location_at(text, cursor)});

        if (semicolon == std::string_view::npos) {
            break;
        }
        cursor = semicolon + 1;
    }
    return statements;
}

CompileResult compile(const CompileRequest& request) {
    ++testing::fake_compiler::state().compile_calls;
    auto& fake = testing::fake_compiler::state();
    fake.catalog_sizes.push_back(request.catalog.tables.size());
    auto& results = fake.compile_results;
    if (results.empty()) {
        return CompileResult{CompileError{
            CompileErrorKind::kSyntax,
            SourceLocation{1, 1},
            "fake compiler has no configured result"}};
    }

    CompileResult result = std::move(results.front());
    results.pop_front();
    return result;
}

}  // namespace tinydbms::compiler
