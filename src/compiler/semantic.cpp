#include "semantic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tinydbms::compiler::internal {
namespace {

enum class SemanticType {
    kInt,
    kVarchar,
    kBool
};

struct AnalyzedExpression {
    BoundExprPtr expression;
    SemanticType type;
};

using ExpressionResult = std::variant<AnalyzedExpression, CompileError>;

[[nodiscard]] CompileError make_semantic_error(SourceLocation location, std::string message) {
    return CompileError{CompileErrorKind::kSemantic, location, std::move(message)};
}

[[nodiscard]] SemanticResult semantic_error(SourceLocation location, std::string message) {
    return SemanticResult{make_semantic_error(location, std::move(message))};
}

[[nodiscard]] const TableMeta* find_table(CatalogView catalog, const std::string& name) {
    const auto table = std::find_if(
        catalog.tables.begin(),
        catalog.tables.end(),
        [&name](const TableMeta& candidate) { return candidate.table_name == name; });
    return table == catalog.tables.end() ? nullptr : &*table;
}

[[nodiscard]] std::optional<ColumnId> find_column(
    const TableMeta& table,
    const std::string& name) {
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (table.columns[index].name == name) {
            return static_cast<ColumnId>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] const char* type_name(Type type) {
    return type == Type::kInt ? "INT" : "VARCHAR";
}

[[nodiscard]] Type value_type(const Value& value) {
    return std::holds_alternative<std::int32_t>(value.data) ? Type::kInt : Type::kVarchar;
}

[[nodiscard]] SemanticType semantic_type(Type type) {
    return type == Type::kInt ? SemanticType::kInt : SemanticType::kVarchar;
}

[[nodiscard]] SemanticType semantic_type(const Value& value) {
    return std::holds_alternative<std::int32_t>(value.data)
        ? SemanticType::kInt
        : SemanticType::kVarchar;
}

[[nodiscard]] const char* semantic_type_name(SemanticType type) {
    switch (type) {
        case SemanticType::kInt:
            return "INT";
        case SemanticType::kVarchar:
            return "VARCHAR";
        case SemanticType::kBool:
            return "BOOL";
    }
    return "unknown";
}

[[nodiscard]] CmpOp bound_compare_op(AstCompareOp op) {
    switch (op) {
        case AstCompareOp::kEq:
            return CmpOp::kEq;
        case AstCompareOp::kNe:
            return CmpOp::kNe;
        case AstCompareOp::kLt:
            return CmpOp::kLt;
        case AstCompareOp::kLe:
            return CmpOp::kLe;
        case AstCompareOp::kGt:
            return CmpOp::kGt;
        case AstCompareOp::kGe:
            return CmpOp::kGe;
    }
    return CmpOp::kEq;
}

[[nodiscard]] LogicOp bound_logic_op(AstLogicOp op) {
    return op == AstLogicOp::kAnd ? LogicOp::kAnd : LogicOp::kOr;
}

[[nodiscard]] const char* compare_symbol(AstCompareOp op) {
    switch (op) {
        case AstCompareOp::kEq:
            return "=";
        case AstCompareOp::kNe:
            return "!=";
        case AstCompareOp::kLt:
            return "<";
        case AstCompareOp::kLe:
            return "<=";
        case AstCompareOp::kGt:
            return ">";
        case AstCompareOp::kGe:
            return ">=";
    }
    return "?";
}

[[nodiscard]] const char* logic_symbol(AstLogicOp op) {
    return op == AstLogicOp::kAnd ? "AND" : "OR";
}

[[nodiscard]] bool is_equality(AstCompareOp op) {
    return op == AstCompareOp::kEq || op == AstCompareOp::kNe;
}

[[nodiscard]] SourceLocation expression_location(const AstExpr& expression) {
    if (const auto* identifier = std::get_if<AstIdentifierExpr>(&expression.kind)) {
        return identifier->location;
    }
    if (const auto* literal = std::get_if<AstLiteralExpr>(&expression.kind)) {
        return literal->location;
    }
    if (const auto* binary = std::get_if<AstBinaryExpr>(&expression.kind)) {
        return binary->location;
    }
    return std::get<AstUnaryExpr>(expression.kind).location;
}

[[nodiscard]] ExpressionResult analyze_expression(
    const AstExpr& expression,
    const TableMeta& table) {
    if (const auto* identifier = std::get_if<AstIdentifierExpr>(&expression.kind)) {
        const std::optional<ColumnId> column_id = find_column(table, identifier->name);
        if (!column_id.has_value()) {
            return make_semantic_error(
                identifier->location,
                "column '" + identifier->name + "' does not exist in table '" +
                    table.table_name + "'");
        }
        const ColumnMeta& column = table.columns[*column_id];
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(BoundColumnRef{*column_id}),
            semantic_type(column.type)
        };
    }

    if (const auto* literal = std::get_if<AstLiteralExpr>(&expression.kind)) {
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(BoundLiteral{literal->value}),
            semantic_type(literal->value)
        };
    }

    if (const auto* binary = std::get_if<AstBinaryExpr>(&expression.kind)) {
        ExpressionResult lhs_result = analyze_expression(*binary->lhs, table);
        if (const auto* error = std::get_if<CompileError>(&lhs_result)) {
            return *error;
        }
        AnalyzedExpression lhs = std::get<AnalyzedExpression>(std::move(lhs_result));

        ExpressionResult rhs_result = analyze_expression(*binary->rhs, table);
        if (const auto* error = std::get_if<CompileError>(&rhs_result)) {
            return *error;
        }
        AnalyzedExpression rhs = std::get<AnalyzedExpression>(std::move(rhs_result));

        if (const auto* comparison = std::get_if<AstCompareOp>(&binary->op)) {
            const std::string op = compare_symbol(*comparison);
            if (lhs.type != rhs.type) {
                return make_semantic_error(
                    binary->location,
                    "operator '" + op + "' cannot be applied to " +
                        semantic_type_name(lhs.type) + " and " +
                        semantic_type_name(rhs.type));
            }
            if (lhs.type == SemanticType::kBool) {
                return make_semantic_error(
                    binary->location,
                    "operator '" + op + "' cannot be applied to BOOL");
            }
            if (lhs.type == SemanticType::kVarchar && !is_equality(*comparison)) {
                return make_semantic_error(
                    binary->location,
                    "operator '" + op + "' cannot be applied to VARCHAR");
            }
            return AnalyzedExpression{
                std::make_unique<BoundExpr>(BoundBinaryExpr{
                    bound_compare_op(*comparison),
                    std::move(lhs.expression),
                    std::move(rhs.expression)
                }),
                SemanticType::kBool
            };
        }

        const AstLogicOp logic = std::get<AstLogicOp>(binary->op);
        if (lhs.type != SemanticType::kBool || rhs.type != SemanticType::kBool) {
            return make_semantic_error(
                binary->location,
                "operator '" + std::string{logic_symbol(logic)} + "' requires BOOL operands");
        }
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(BoundBinaryExpr{
                bound_logic_op(logic),
                std::move(lhs.expression),
                std::move(rhs.expression)
            }),
            SemanticType::kBool
        };
    }

    const auto& unary = std::get<AstUnaryExpr>(expression.kind);
    ExpressionResult operand_result = analyze_expression(*unary.operand, table);
    if (const auto* error = std::get_if<CompileError>(&operand_result)) {
        return *error;
    }
    AnalyzedExpression operand = std::get<AnalyzedExpression>(std::move(operand_result));
    if (operand.type != SemanticType::kBool) {
        return make_semantic_error(
            unary.location,
            "operator 'NOT' requires a BOOL operand");
    }
    return AnalyzedExpression{
        std::make_unique<BoundExpr>(BoundUnaryExpr{std::move(operand.expression)}),
        SemanticType::kBool
    };
}

[[nodiscard]] SemanticResult analyze_create(const CreateTableAst& create, CatalogView catalog) {
    if (find_table(catalog, create.table_name) != nullptr) {
        return semantic_error(
            create.table_location,
            "table '" + create.table_name + "' already exists");
    }

    for (std::size_t index = 0; index < create.columns.size(); ++index) {
        const auto& column = create.columns[index];
        const auto duplicate = std::find_if(
            create.columns.begin(),
            create.columns.begin() + static_cast<std::ptrdiff_t>(index),
            [&column](const AstColumnDef& previous) { return previous.name == column.name; });
        if (duplicate != create.columns.begin() + static_cast<std::ptrdiff_t>(index)) {
            return semantic_error(
                column.location,
                "duplicate column '" + column.name + "'");
        }
    }

    BoundCreateTable bound;
    bound.table_name = create.table_name;
    bound.columns.reserve(create.columns.size());
    for (const auto& column : create.columns) {
        bound.columns.push_back(ColumnMeta{column.name, column.type});
    }
    return SemanticResult{BoundStatement{std::move(bound)}};
}

[[nodiscard]] SemanticResult analyze_insert(const InsertAst& insert, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, insert.table_name);
    if (table == nullptr) {
        return semantic_error(
            insert.table_location,
            "table '" + insert.table_name + "' does not exist");
    }

    std::vector<ColumnId> bound_columns;
    bound_columns.reserve(insert.columns.size());
    for (const auto& column : insert.columns) {
        const std::optional<ColumnId> column_id = find_column(*table, column.name);
        if (!column_id.has_value()) {
            return semantic_error(
                column.location,
                "column '" + column.name + "' does not exist in table '" +
                    table->table_name + "'");
        }
        if (std::find(bound_columns.begin(), bound_columns.end(), *column_id) != bound_columns.end()) {
            return semantic_error(
                column.location,
                "duplicate column '" + column.name + "'");
        }
        bound_columns.push_back(*column_id);
    }

    if (!insert.columns.empty() && bound_columns.size() != table->columns.size()) {
        for (std::size_t index = 0; index < table->columns.size(); ++index) {
            const ColumnId column_id = static_cast<ColumnId>(index);
            if (std::find(bound_columns.begin(), bound_columns.end(), column_id) == bound_columns.end()) {
                return semantic_error(
                    insert.table_location,
                    "explicit INSERT column list must contain all columns; missing column '" +
                        table->columns[index].name + "'");
            }
        }
    }

    BoundInsert bound{table->table_id, bound_columns, {}};
    bound.rows.reserve(insert.rows.size());
    const std::size_t expected_count = table->columns.size();

    for (const auto& row : insert.rows) {
        if (row.size() != expected_count) {
            const SourceLocation location = row.size() > expected_count
                ? row[expected_count].location
                : (row.empty() ? insert.table_location : row.front().location);
            return semantic_error(location, "VALUES row value count does not match target column count");
        }

        std::vector<Value> bound_row;
        bound_row.reserve(row.size());
        for (std::size_t index = 0; index < row.size(); ++index) {
            const ColumnId target_id = bound_columns.empty()
                ? static_cast<ColumnId>(index)
                : bound_columns[index];
            const ColumnMeta& target = table->columns[target_id];
            const Type actual_type = value_type(row[index].value);
            if (target.type != actual_type) {
                return semantic_error(
                    row[index].location,
                    "column '" + target.name + "' expects " + type_name(target.type) +
                        ", but " + type_name(actual_type) + " found");
            }
            bound_row.push_back(row[index].value);
        }
        bound.rows.push_back(std::move(bound_row));
    }

    return SemanticResult{BoundStatement{std::move(bound)}};
}

[[nodiscard]] SemanticResult analyze_select(const SelectAst& select, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, select.table_name);
    if (table == nullptr) {
        return semantic_error(
            select.table_location,
            "table '" + select.table_name + "' does not exist");
    }

    std::vector<ColumnId> outputs;
    if (select.select_all) {
        outputs.reserve(table->columns.size());
        for (std::size_t index = 0; index < table->columns.size(); ++index) {
            outputs.push_back(static_cast<ColumnId>(index));
        }
    } else {
        outputs.reserve(select.columns.size());
        for (const auto& column : select.columns) {
            const std::optional<ColumnId> column_id = find_column(*table, column.name);
            if (!column_id.has_value()) {
                return semantic_error(
                    column.location,
                    "column '" + column.name + "' does not exist in table '" +
                        table->table_name + "'");
            }
            outputs.push_back(*column_id);
        }
    }

    BoundExprPtr predicate;
    if (select.predicate != nullptr) {
        ExpressionResult result = analyze_expression(*select.predicate, *table);
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool) {
            return semantic_error(
                expression_location(*select.predicate),
                "WHERE predicate must be BOOL");
        }
        predicate = std::move(analyzed.expression);
    }

    return SemanticResult{BoundStatement{BoundSelect{
        table->table_id,
        std::move(outputs),
        std::move(predicate)
    }}};
}

[[nodiscard]] SemanticResult analyze_delete(const DeleteAst& deletion, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, deletion.table_name);
    if (table == nullptr) {
        return semantic_error(
            deletion.table_location,
            "table '" + deletion.table_name + "' does not exist");
    }

    BoundExprPtr predicate;
    if (deletion.predicate != nullptr) {
        ExpressionResult result = analyze_expression(*deletion.predicate, *table);
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool) {
            return semantic_error(
                expression_location(*deletion.predicate),
                "WHERE predicate must be BOOL");
        }
        predicate = std::move(analyzed.expression);
    }

    return SemanticResult{BoundStatement{BoundDelete{
        table->table_id,
        std::move(predicate)
    }}};
}

}  // namespace

SemanticResult analyze(const StatementAst& statement, CatalogView catalog) {
    if (const auto* create = std::get_if<CreateTableAst>(&statement.kind)) {
        return analyze_create(*create, catalog);
    }
    if (const auto* insert = std::get_if<InsertAst>(&statement.kind)) {
        return analyze_insert(*insert, catalog);
    }
    if (const auto* select = std::get_if<SelectAst>(&statement.kind)) {
        return analyze_select(*select, catalog);
    }
    const auto& deletion = std::get<DeleteAst>(statement.kind);
    return analyze_delete(deletion, catalog);
}

}  // namespace tinydbms::compiler::internal
