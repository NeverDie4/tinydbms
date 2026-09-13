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
    kJoin,
    kInner,
    kOn,
    kWhere,
    kOrder,
    kBy,
    kGroup,
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
    kAsc,
    kDesc,
    kDelete,
    kUpdate,
    kSet,
    kAnd,
    kOr,
    kNot,
    kInt,
    kBigInt,
    kDouble,
    kBoolean,
    kVarchar,
    kTrue,
    kFalse,
    kNull,
    kIs,
    kIdentifier,
    kIntegerLiteral,
    kDoubleLiteral,
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
    kDot,
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
