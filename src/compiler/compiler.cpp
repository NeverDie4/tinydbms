#include "tinydbms/compiler.h"

#include <cctype>

namespace tinydbms::compiler {
namespace {

bool is_space(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string trim(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

std::vector<SplitStatement> split_statements(std::string_view script) {
    std::vector<SplitStatement> statements;
    std::string current;
    SourceLocation start;
    bool started = false;
    bool single_quote = false;
    bool line_comment = false;
    bool block_comment = false;
    std::size_t line = 1;
    std::size_t column = 1;

    auto advance = [&](char value) {
        if (value == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    };

    auto finish = [&]() {
        const std::string sql = trim(current);
        if (started && !sql.empty()) {
            statements.push_back(SplitStatement{sql, start});
        }
        current.clear();
        started = false;
    };

    for (std::size_t index = 0; index < script.size(); ++index) {
        const char value = script[index];
        const char next = index + 1 < script.size() ? script[index + 1] : '\0';

        if (line_comment) {
            advance(value);
            if (value == '\n') {
                line_comment = false;
            }
            continue;
        }
        if (block_comment) {
            if (value == '*' && next == '/') {
                advance(value);
                advance(next);
                ++index;
                block_comment = false;
            } else {
                advance(value);
            }
            continue;
        }
        if (!single_quote && value == '-' && next == '-') {
            advance(value);
            advance(next);
            ++index;
            line_comment = true;
            continue;
        }
        if (!single_quote && value == '/' && next == '*') {
            advance(value);
            advance(next);
            ++index;
            block_comment = true;
            continue;
        }
        if (value == '\'') {
            single_quote = !single_quote;
        }
        if (!single_quote && value == ';') {
            advance(value);
            finish();
            continue;
        }
        if (!started && !is_space(value)) {
            start = SourceLocation{line, column};
            started = true;
        }
        if (started || !is_space(value)) {
            current.push_back(value);
        }
        advance(value);
    }
    finish();
    return statements;
}

CompileResult compile(const CompileRequest& request) {
    CompileError error;
    error.kind = CompileErrorKind::kSyntax;
    error.location = SourceLocation{1, 1};
    error.message = "SQL compiler is not implemented yet; the public contract is initialized";
    if (request.sql.empty()) {
        error.message = "empty SQL statement";
    }
    return CompileResult{std::move(error)};
}

}  // namespace tinydbms::compiler
