#ifndef TINYDBMS_COMPILER_SEMANTIC_HPP
#define TINYDBMS_COMPILER_SEMANTIC_HPP

#include <variant>

#include "ast.hpp"
#include "bound_ast.hpp"
#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

struct SemanticResult {
    std::variant<BoundStatement, CompileError> outcome;
};

[[nodiscard]] SemanticResult analyze(const StatementAst& statement, CatalogView catalog);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_SEMANTIC_HPP
