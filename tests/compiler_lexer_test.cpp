#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lexer.hpp"

namespace {

using tinydbms::SourceLocation;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::internal::LexResult;
using tinydbms::compiler::internal::Token;
using tinydbms::compiler::internal::TokenKind;
using tinydbms::compiler::internal::tokenize;

struct ExpectedToken {
    TokenKind kind;
    std::string_view lexeme;
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

const std::vector<Token>* successful_tokens(
    TestContext& test,
    const LexResult& result,
    std::string_view case_name) {
    const auto* tokens = std::get_if<std::vector<Token>>(&result.outcome);
    test.expect(tokens != nullptr, std::string{case_name} + ": expected success");
    return tokens;
}

void expect_tokens(
    TestContext& test,
    std::string_view case_name,
    std::string_view input,
    std::initializer_list<ExpectedToken> expected) {
    const auto result = tokenize(input);
    const auto* tokens = successful_tokens(test, result, case_name);
    if (tokens == nullptr) {
        return;
    }

    const std::string prefix{case_name};
    test.expect(tokens->size() == expected.size() + 1, prefix + ": token count including end");
    if (tokens->size() != expected.size() + 1) {
        return;
    }

    std::size_t index = 0;
    for (const auto& item : expected) {
        test.expect((*tokens)[index].kind == item.kind, prefix + ": kind[" + std::to_string(index) + "]");
        test.expect(
            (*tokens)[index].lexeme == item.lexeme,
            prefix + ": lexeme[" + std::to_string(index) + "]");
        ++index;
    }

    test.expect(tokens->back().kind == TokenKind::kEnd, prefix + ": final token is end");
    test.expect(tokens->back().lexeme.empty(), prefix + ": end lexeme is empty");
}

void expect_location(
    TestContext& test,
    std::string_view case_name,
    std::string_view input,
    std::size_t token_index,
    SourceLocation expected) {
    const auto result = tokenize(input);
    const auto* tokens = successful_tokens(test, result, case_name);
    if (tokens == nullptr) {
        return;
    }

    const std::string prefix{case_name};
    test.expect(token_index < tokens->size(), prefix + ": token index exists");
    if (token_index >= tokens->size()) {
        return;
    }

    test.expect((*tokens)[token_index].location.line == expected.line, prefix + ": line");
    test.expect((*tokens)[token_index].location.column == expected.column, prefix + ": column");
}

void expect_lex_error(
    TestContext& test,
    std::string_view case_name,
    std::string_view input,
    SourceLocation expected,
    std::string_view message_part) {
    const auto result = tokenize(input);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{case_name};
    test.expect(error != nullptr, prefix + ": expected lex error");
    if (error == nullptr) {
        return;
    }

    test.expect(error->kind == CompileErrorKind::kLex, prefix + ": error kind");
    test.expect(error->location.line == expected.line, prefix + ": error line");
    test.expect(error->location.column == expected.column, prefix + ": error column");
    test.expect(error->message.find(message_part) != std::string::npos, prefix + ": error message");
}

std::string quoted_bytes(std::initializer_list<unsigned char> bytes) {
    std::string value{"'"};
    for (const unsigned char byte : bytes) {
        value.push_back(static_cast<char>(byte));
    }
    value.push_back('\'');
    return value;
}

}  // namespace

int main() {
    TestContext test;

    expect_tokens(
        test,
        "keywords",
        "CREATE TABLE INSERT INTO VALUES SELECT FROM WHERE DELETE AND OR NOT INT VARCHAR",
        {
            {TokenKind::kCreate, "create"},
            {TokenKind::kTable, "table"},
            {TokenKind::kInsert, "insert"},
            {TokenKind::kInto, "into"},
            {TokenKind::kValues, "values"},
            {TokenKind::kSelect, "select"},
            {TokenKind::kFrom, "from"},
            {TokenKind::kWhere, "where"},
            {TokenKind::kDelete, "delete"},
            {TokenKind::kAnd, "and"},
            {TokenKind::kOr, "or"},
            {TokenKind::kNot, "not"},
            {TokenKind::kInt, "int"},
            {TokenKind::kVarchar, "varchar"},
        });
    expect_tokens(
        test,
        "keyword case",
        "SeLeCt FROM where",
        {{TokenKind::kSelect, "select"}, {TokenKind::kFrom, "from"}, {TokenKind::kWhere, "where"}});
    expect_tokens(
        test,
        "identifiers",
        "student student_1 Student_Name selecta",
        {
            {TokenKind::kIdentifier, "student"},
            {TokenKind::kIdentifier, "student_1"},
            {TokenKind::kIdentifier, "student_name"},
            {TokenKind::kIdentifier, "selecta"},
        });

    const std::string identifier_64(64, 'a');
    const std::string identifier_65(65, 'a');
    expect_tokens(test, "identifier length 64", identifier_64, {{TokenKind::kIdentifier, identifier_64}});
    expect_lex_error(test, "identifier length 65", identifier_65, {1, 1}, "identifier");
    expect_lex_error(test, "identifier invalid start", "_abc", {1, 1}, "identifier");

    expect_tokens(
        test,
        "integers",
        "0 123 2147483647",
        {
            {TokenKind::kIntegerLiteral, "0"},
            {TokenKind::kIntegerLiteral, "123"},
            {TokenKind::kIntegerLiteral, "2147483647"},
        });
    expect_lex_error(test, "integer overflow", "2147483648", {1, 1}, "integer");
    expect_lex_error(test, "negative integer unsupported", "-1", {1, 1}, "character");

    expect_tokens(test, "string", "'Alice'", {{TokenKind::kStringLiteral, "Alice"}});
    expect_tokens(test, "empty string", "''", {{TokenKind::kStringLiteral, ""}});
    expect_tokens(test, "escaped string", "'Tom''s book'", {{TokenKind::kStringLiteral, "Tom's book"}});
    expect_tokens(
        test,
        "string special characters",
        "'a;b -- /* */'",
        {{TokenKind::kStringLiteral, "a;b -- /* */"}});
    expect_lex_error(test, "unterminated string", "'abc", {1, 1}, "unterminated string");

    const std::string varchar_1024 = "'" + std::string(tinydbms::kMaxVarcharBytes, 'a') + "'";
    const std::string varchar_1025 = "'" + std::string(tinydbms::kMaxVarcharBytes + 1, 'a') + "'";
    expect_tokens(
        test,
        "varchar maximum",
        varchar_1024,
        {{TokenKind::kStringLiteral, std::string_view{varchar_1024}.substr(1, tinydbms::kMaxVarcharBytes)}});
    expect_lex_error(test, "varchar too long", varchar_1025, {1, 1}, "VARCHAR");

    expect_tokens(
        test,
        "valid utf8",
        "'中药材' SELECT",
        {{TokenKind::kStringLiteral, "中药材"}, {TokenKind::kSelect, "select"}});
    expect_location(test, "utf8 byte column", "'中药材' SELECT", 1, {1, 13});
    expect_lex_error(
        test,
        "utf8 invalid continuation",
        quoted_bytes({0xC2, 0x41}),
        {1, 1},
        "UTF-8");
    expect_lex_error(test, "utf8 truncated", quoted_bytes({0xE2, 0x82}), {1, 1}, "UTF-8");
    expect_lex_error(test, "utf8 overlong", quoted_bytes({0xC0, 0xAF}), {1, 1}, "UTF-8");
    expect_lex_error(test, "utf8 surrogate", quoted_bytes({0xED, 0xA0, 0x80}), {1, 1}, "UTF-8");
    expect_lex_error(
        test,
        "utf8 above unicode maximum",
        quoted_bytes({0xF4, 0x90, 0x80, 0x80}),
        {1, 1},
        "UTF-8");

    expect_tokens(
        test,
        "operators",
        "= != < <= > >=",
        {
            {TokenKind::kEq, "="},
            {TokenKind::kNe, "!="},
            {TokenKind::kLt, "<"},
            {TokenKind::kLe, "<="},
            {TokenKind::kGt, ">"},
            {TokenKind::kGe, ">="},
        });
    expect_lex_error(test, "standalone bang", "!", {1, 1}, "character");
    expect_tokens(
        test,
        "delimiters",
        "(),;",
        {
            {TokenKind::kLeftParen, "("},
            {TokenKind::kRightParen, ")"},
            {TokenKind::kComma, ","},
            {TokenKind::kSemicolon, ";"},
        });
    expect_tokens(
        test,
        "select star",
        "SELECT * FROM t;",
        {
            {TokenKind::kSelect, "select"},
            {TokenKind::kStar, "*"},
            {TokenKind::kFrom, "from"},
            {TokenKind::kIdentifier, "t"},
            {TokenKind::kSemicolon, ";"},
        });

    expect_tokens(test, "whitespace", " \t\r\nSELECT", {{TokenKind::kSelect, "select"}});
    expect_location(test, "whitespace location", " \t\r\nSELECT", 0, {2, 1});
    expect_tokens(
        test,
        "line comment",
        "SELECT -- hello\nname FROM t;",
        {
            {TokenKind::kSelect, "select"},
            {TokenKind::kIdentifier, "name"},
            {TokenKind::kFrom, "from"},
            {TokenKind::kIdentifier, "t"},
            {TokenKind::kSemicolon, ";"},
        });
    expect_tokens(
        test,
        "block comment",
        "SELECT /* hello */ name FROM t;",
        {
            {TokenKind::kSelect, "select"},
            {TokenKind::kIdentifier, "name"},
            {TokenKind::kFrom, "from"},
            {TokenKind::kIdentifier, "t"},
            {TokenKind::kSemicolon, ";"},
        });
    expect_tokens(
        test,
        "comment special characters",
        "-- ; ' /*\n/* ; ' -- */ SELECT",
        {{TokenKind::kSelect, "select"}});
    expect_lex_error(test, "unterminated block comment", "SELECT /* abc", {1, 8}, "block comment");

    const std::string multiline = "SELECT\nname\nFROM t;";
    expect_location(test, "location select", multiline, 0, {1, 1});
    expect_location(test, "location name", multiline, 1, {2, 1});
    expect_location(test, "location from", multiline, 2, {3, 1});
    expect_location(test, "location identifier", multiline, 3, {3, 6});
    expect_location(test, "location semicolon", multiline, 4, {3, 7});
    expect_location(test, "location end", multiline, 5, {3, 8});
    expect_location(test, "crlf location", "SELECT\r\nname", 1, {2, 1});

    expect_lex_error(test, "invalid at", "SELECT @ FROM t;", {1, 8}, "character");
    expect_lex_error(test, "invalid backtick", "`name`", {1, 1}, "character");
    expect_lex_error(test, "invalid double quote", "\"name\"", {1, 1}, "character");
    expect_lex_error(test, "invalid backslash", "\\", {1, 1}, "character");
    expect_tokens(
        test,
        "missing semicolon",
        "SELECT * FROM t",
        {
            {TokenKind::kSelect, "select"},
            {TokenKind::kStar, "*"},
            {TokenKind::kFrom, "from"},
            {TokenKind::kIdentifier, "t"},
        });
    expect_location(test, "missing semicolon end", "SELECT * FROM t", 4, {1, 16});
    expect_tokens(
        test,
        "syntax is not lexer's job",
        "SELECT SELECT SELECT;",
        {
            {TokenKind::kSelect, "select"},
            {TokenKind::kSelect, "select"},
            {TokenKind::kSelect, "select"},
            {TokenKind::kSemicolon, ";"},
        });
    expect_tokens(test, "empty has end", "", {});
    expect_location(test, "empty end location", "", 0, {1, 1});

    if (test.failures() != 0) {
        std::cerr << test.failures() << " lexer test assertion(s) failed\n";
        return 1;
    }

    return 0;
}
