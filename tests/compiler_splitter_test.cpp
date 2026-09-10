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
    for (const auto& item : expected) {
        const auto& statement = (*actual)[index];
        test.expect(statement.sql == item.sql, prefix + ": sql[" + std::to_string(index) + "]");
        test.expect(
            statement.start.line == item.line,
            prefix + ": line[" + std::to_string(index) + "]");
        test.expect(
            statement.start.column == item.column,
            prefix + ": column[" + std::to_string(index) + "]");
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

    if (test.failures() != 0) {
        std::cerr << test.failures() << " splitter test assertion(s) failed\n";
        return 1;
    }

    return 0;
}
