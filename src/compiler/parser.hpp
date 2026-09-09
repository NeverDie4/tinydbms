#ifndef TINYDBMS_COMPILER_PARSER_HPP
#define TINYDBMS_COMPILER_PARSER_HPP

#include <variant>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace tinydbms::compiler::internal {

struct ParseResult {
    std::variant<StatementAst, CompileError> outcome;
};

ParseResult parse(const std::vector<Token>& tokens);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_PARSER_HPP
