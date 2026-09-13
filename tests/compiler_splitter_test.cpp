#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace {

struct ExpectedStatement {
    std::string_view sql;
    int line;
    int column;
};

tinydbms::SourceLocation location_at(std::string_view text, std::size_t offset) {
    int line = 1;
    int column = 1;
    for (std::size_t index = 0; index < offset; ++index) {
        if (text[index] == '\n') {
            ++line;
            column = 1;
        } else if (text[index] != '\r') {
            ++column;
        }
    }
    return tinydbms::SourceLocation{line, column, offset};
}

bool between_crlf(std::string_view text, std::size_t offset) {
    return offset > 0 && offset < text.size() &&
        text[offset - 1] == '\r' && text[offset] == '\n';
}

class TestContext {
public:
    void expect(bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] int failures() const {
        return failures_;
    }

private:
    int failures_{0};
};

void expect_split(
    TestContext& test,
    std::string_view case_name,
    std::string_view input,
    std::initializer_list<ExpectedStatement> expected) {
    const auto result = tinydbms::compiler::split_statements(input);
    const std::string prefix{case_name};
    const auto* actual =
        std::get_if<std::vector<tinydbms::compiler::SplitStatement>>(&result.outcome);

    test.expect(actual != nullptr, prefix + ": split success");
    if (actual == nullptr) {
        return;
    }

    test.expect(actual->size() == expected.size(), prefix + ": statement count");
    if (actual->size() != expected.size()) {
        return;
    }

    std::size_t index = 0;
    std::size_t search_offset = 0;
    for (const auto& item : expected) {
        const auto& statement = (*actual)[index];
        const std::size_t begin_offset = input.find(item.sql, search_offset);
        test.expect(begin_offset != std::string_view::npos, prefix + ": source exists");
        if (begin_offset == std::string_view::npos) {
            return;
        }
        const std::size_t end_offset = begin_offset + item.sql.size();
        const auto expected_begin = location_at(input, begin_offset);
        const auto expected_end = location_at(input, end_offset);
        test.expect(statement.sql == item.sql, prefix + ": sql[" + std::to_string(index) + "]");
        test.expect(
            statement.source.begin.line == item.line,
            prefix + ": line[" + std::to_string(index) + "]");
        test.expect(
            statement.source.begin.column == item.column,
            prefix + ": column[" + std::to_string(index) + "]");
        test.expect(
            statement.source.begin.byte_offset == expected_begin.byte_offset,
            prefix + ": begin byte offset[" + std::to_string(index) + "]");
        test.expect(
            statement.source.end.line == expected_end.line &&
                statement.source.end.column == expected_end.column &&
                statement.source.end.byte_offset == expected_end.byte_offset,
            prefix + ": end location[" + std::to_string(index) + "]");
        test.expect(
            input.substr(
                statement.source.begin.byte_offset,
                statement.source.end.byte_offset - statement.source.begin.byte_offset) ==
                statement.sql,
            prefix + ": source/sql correspondence[" + std::to_string(index) + "]");
        test.expect(
            !between_crlf(input, statement.source.begin.byte_offset) &&
                !between_crlf(input, statement.source.end.byte_offset),
            prefix + ": endpoints do not split CRLF[" + std::to_string(index) + "]");
        search_offset = end_offset;
        ++index;
    }
}

}  // namespace

int main() {
    TestContext test;

    expect_split(test, "empty", "", {});
    expect_split(test, "whitespace", "   \n\t ", {});
    expect_split(test, "line comment only", "-- hello", {});
    expect_split(test, "block comment only", "/* hello */", {});
    expect_split(test, "single statement", "SELECT * FROM t;", {{"SELECT * FROM t;", 1, 1}});
    expect_split(
        test,
        "two statements",
        "SELECT * FROM a;SELECT * FROM b;",
        {{"SELECT * FROM a;", 1, 1}, {"SELECT * FROM b;", 1, 17}});
    expect_split(
        test,
        "semicolon in string",
        "INSERT INTO t VALUES ('a;b');",
        {{"INSERT INTO t VALUES ('a;b');", 1, 1}});
    expect_split(
        test,
        "escaped quote and semicolon",
        "INSERT INTO t VALUES ('Tom''s;book');",
        {{"INSERT INTO t VALUES ('Tom''s;book');", 1, 1}});
    expect_split(
        test,
        "semicolon in line comment",
        "-- ;;; comment\nSELECT * FROM t;",
        {{"-- ;;; comment\nSELECT * FROM t;", 1, 1}});
    expect_split(
        test,
        "semicolon in block comment",
        "/* ; ; ; */\nSELECT * FROM t;",
        {{"/* ; ; ; */\nSELECT * FROM t;", 1, 1}});
    expect_split(
        test,
        "comment before next statement",
        "SELECT * FROM a; -- comment\nSELECT * FROM b;",
        {{"SELECT * FROM a;", 1, 1}, {" -- comment\nSELECT * FROM b;", 1, 17}});
    expect_split(
        test,
        "empty statements",
        ";;;SELECT * FROM t;;;;",
        {{"SELECT * FROM t;", 1, 4}});
    expect_split(test, "missing final semicolon", "SELECT * FROM t", {{"SELECT * FROM t", 1, 1}});
    expect_split(
        test,
        "unterminated string",
        "INSERT INTO t VALUES ('abc",
        {{"INSERT INTO t VALUES ('abc", 1, 1}});
    expect_split(
        test,
        "unterminated block comment",
        "SELECT * FROM t; /* unfinished",
        {{"SELECT * FROM t;", 1, 1}, {" /* unfinished", 1, 17}});
    expect_split(
        test,
        "crlf",
        "SELECT * FROM a;\r\nSELECT * FROM b;",
        {{"SELECT * FROM a;", 1, 1}, {"\r\nSELECT * FROM b;", 1, 17}});
    expect_split(
        test,
        "lf",
        "SELECT * FROM a;\nSELECT * FROM b;",
        {{"SELECT * FROM a;", 1, 1}, {"\nSELECT * FROM b;", 1, 17}});
    expect_split(
        test,
        "leading whitespace",
        "   SELECT * FROM t;",
        {{"   SELECT * FROM t;", 1, 1}});
    expect_split(
        test,
        "leading comment",
        "/* hello */ SELECT * FROM t;",
        {{"/* hello */ SELECT * FROM t;", 1, 1}});
    expect_split(
        test,
        "comment and empty segments",
        "; /* one */ ;\n-- two\n;\t/* three */",
        {});
    expect_split(
        test,
        "utf8 byte column",
        "中;SELECT * FROM t;",
        {{"中;", 1, 1}, {"SELECT * FROM t;", 1, 5}});
    expect_split(
        test,
        "tab and utf8 offsets",
        "\t中;\r\n\tSELECT * FROM t;",
        {{"\t中;", 1, 1}, {"\r\n\tSELECT * FROM t;", 1, 6}});

    const std::string too_long(tinydbms::kMaxSqlBytes + 1, 'x');
    const auto split_error_result = tinydbms::compiler::split_statements(too_long);
    const auto* split_error =
        std::get_if<tinydbms::compiler::CompileError>(&split_error_result.outcome);
    test.expect(split_error != nullptr, "oversized script: split failure");
    if (split_error != nullptr) {
        test.expect(split_error->stage == tinydbms::CompileStage::kLex, "oversized script: stage");
        test.expect(
            split_error->source.begin.byte_offset == 0 &&
                split_error->source.end.byte_offset == 0,
            "oversized script: empty insertion point");
    }

    for (const std::string_view incomplete : {"SELECT 'abc", "/* unfinished"}) {
        const auto split = tinydbms::compiler::split_statements(incomplete);
        const auto* statements =
            std::get_if<std::vector<tinydbms::compiler::SplitStatement>>(&split.outcome);
        test.expect(statements != nullptr && statements->size() == 1, "incomplete input: preserved");
        if (statements != nullptr && statements->size() == 1) {
            const auto compiled = tinydbms::compiler::compile(
                tinydbms::compiler::CompileRequest{statements->front().sql, {}});
            const auto* error = std::get_if<tinydbms::compiler::CompileError>(&compiled.outcome);
            test.expect(
                error != nullptr && error->stage == tinydbms::CompileStage::kLex,
                "incomplete input: compile reports lex error");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " splitter test assertion(s) failed\n";
        return 1;
    }

    return 0;
}
