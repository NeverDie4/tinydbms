#ifndef TINYDBMS_COMPILER_LEXER_HPP
#define TINYDBMS_COMPILER_LEXER_HPP

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

enum class TokenKind {
    kCreate,
    kTable,
    kInsert,
    kInto,
    kValues,
    kSelect,
    kFrom,
    kWhere,
    kDelete,
    kAnd,
    kOr,
    kNot,
    kInt,
    kVarchar,
    kIdentifier,
    kIntegerLiteral,
    kStringLiteral,
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
    kLeftParen,
    kRightParen,
    kComma,
    kSemicolon,
    kStar,
    kEnd
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    SourceLocation location;
};

struct LexResult {
    std::variant<std::vector<Token>, CompileError> outcome;
};

LexResult tokenize(std::string_view sql);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_LEXER_HPP
