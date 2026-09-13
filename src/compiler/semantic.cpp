#include "semantic.hpp"

#include "diagnostic_suggestion.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tinydbms::compiler::internal {
namespace {

enum class SemanticType {
    kInt,
    kBigInt,
    kDouble,
    kVarchar,
    kBool,
    kNull,
    kUnsupported
};

struct AnalyzedExpression {
    BoundExprPtr expression;
    SemanticType type;
};

using ExpressionResult = std::variant<AnalyzedExpression, CompileError>;
using AssignmentResult = std::variant<Value, CompileError>;
using RelationScope = std::vector<const TableMeta*>;

struct ResolvedColumn {
    BoundColumnRef reference;
    const ColumnMeta* metadata;
};

using ResolveColumnResult = std::variant<ResolvedColumn, CompileError>;
using BoundAggregateResult = std::variant<BoundAggregateCall, CompileError>;

[[nodiscard]] CompileError make_semantic_error(
    SourceLocation location,
    std::string message,
    std::optional<std::string> suggestion = std::nullopt) {
    CompileError error{CompileErrorKind::kSemantic, location, std::move(message)};
    error.suggestion = std::move(suggestion);
    return error;
}

[[nodiscard]] SemanticResult semantic_error(
    SourceLocation location,
    std::string message,
    std::optional<std::string> suggestion = std::nullopt) {
    return SemanticResult{make_semantic_error(
        location, std::move(message), std::move(suggestion))};
}

[[nodiscard]] const TableMeta* find_table(CatalogView catalog, const std::string& name) {
    const auto table = std::find_if(
        catalog.tables.begin(),
        catalog.tables.end(),
        [&name](const TableMeta& candidate) { return candidate.table_name == name; });
    return table == catalog.tables.end() ? nullptr : &*table;
}

[[nodiscard]] std::optional<std::string> table_suggestion(
    CatalogView catalog,
    std::string_view name) {
    std::vector<SuggestionCandidate> candidates;
    candidates.reserve(catalog.tables.size());
    for (const TableMeta& table : catalog.tables) {
        candidates.push_back(SuggestionCandidate{table.table_name, table.table_name});
    }
    const std::optional<std::string> candidate = best_suggestion(name, candidates);
    return candidate.has_value()
        ? std::optional<std::string>{suggestion_message(*candidate)}
        : std::nullopt;
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

[[nodiscard]] std::optional<std::string> qualified_column_suggestion(
    const TableMeta& table,
    std::string_view qualifier,
    std::string_view name) {
    std::vector<std::string> displays;
    displays.reserve(table.columns.size());
    for (const ColumnMeta& column : table.columns) {
        displays.push_back(std::string{qualifier} + "." + column.name);
    }
    std::vector<SuggestionCandidate> candidates;
    candidates.reserve(table.columns.size());
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        candidates.push_back(SuggestionCandidate{table.columns[index].name, displays[index]});
    }
    const std::optional<std::string> candidate = best_suggestion(name, candidates);
    return candidate.has_value()
        ? std::optional<std::string>{suggestion_message(*candidate)}
        : std::nullopt;
}

[[nodiscard]] std::optional<std::string> unqualified_column_suggestion(
    const RelationScope& relations,
    std::string_view name) {
    std::vector<std::string_view> unique_names;
    for (const TableMeta* table : relations) {
        for (const ColumnMeta& column : table->columns) {
            const std::size_t occurrences = static_cast<std::size_t>(std::count_if(
                relations.begin(), relations.end(), [&column](const TableMeta* relation) {
                    return std::any_of(
                        relation->columns.begin(), relation->columns.end(),
                        [&column](const ColumnMeta& candidate) {
                            return candidate.name == column.name;
                        });
                }));
            if (occurrences == 1U &&
                std::find(unique_names.begin(), unique_names.end(), column.name) ==
                    unique_names.end()) {
                unique_names.push_back(column.name);
            }
        }
    }
    std::vector<SuggestionCandidate> candidates;
    candidates.reserve(unique_names.size());
    for (const std::string_view candidate : unique_names) {
        candidates.push_back(SuggestionCandidate{candidate, candidate});
    }
    const std::optional<std::string> candidate = best_suggestion(name, candidates);
    return candidate.has_value()
        ? std::optional<std::string>{suggestion_message(*candidate)}
        : std::nullopt;
}

[[nodiscard]] std::optional<std::string> qualifier_suggestion(
    const RelationScope& relations,
    std::string_view qualifier,
    std::string_view column_name) {
    std::vector<std::string> displays;
    std::vector<std::string_view> spellings;
    displays.reserve(relations.size());
    spellings.reserve(relations.size());
    for (const TableMeta* table : relations) {
        if (find_column(*table, std::string{column_name}).has_value()) {
            spellings.push_back(table->table_name);
            displays.push_back(table->table_name + "." + std::string{column_name});
        }
    }
    std::vector<SuggestionCandidate> candidates;
    candidates.reserve(spellings.size());
    for (std::size_t index = 0; index < spellings.size(); ++index) {
        candidates.push_back(SuggestionCandidate{spellings[index], displays[index]});
    }
    const std::optional<std::string> candidate = best_suggestion(qualifier, candidates);
    return candidate.has_value()
        ? std::optional<std::string>{suggestion_message(*candidate)}
        : std::nullopt;
}

[[nodiscard]] ResolveColumnResult resolve_column(
    const RelationScope& relations,
    const std::optional<std::string>& qualifier,
    const std::string& name,
    SourceLocation location) {
    if (qualifier.has_value()) {
        const auto relation = std::find_if(
            relations.begin(),
            relations.end(),
            [&qualifier](const TableMeta* table) {
                return table->table_name == *qualifier;
            });
        if (relation == relations.end()) {
            return make_semantic_error(
                location,
                "table qualifier '" + *qualifier + "' is not present in query",
                qualifier_suggestion(relations, *qualifier, name));
        }
        const std::optional<ColumnId> column_id = find_column(**relation, name);
        if (!column_id.has_value()) {
            return make_semantic_error(
                location,
                "column '" + name + "' does not exist in table '" + *qualifier + "'",
                qualified_column_suggestion(**relation, *qualifier, name));
        }
        return ResolvedColumn{
            BoundColumnRef{(*relation)->table_id, *column_id},
            &(*relation)->columns[*column_id]};
    }

    std::optional<ResolvedColumn> match;
    for (const TableMeta* table : relations) {
        const std::optional<ColumnId> column_id = find_column(*table, name);
        if (!column_id.has_value()) {
            continue;
        }
        if (match.has_value()) {
            return make_semantic_error(location, "column '" + name + "' is ambiguous");
        }
        match = ResolvedColumn{
            BoundColumnRef{table->table_id, *column_id},
            &table->columns[*column_id]};
    }
    if (!match.has_value()) {
        if (relations.size() == 1U) {
            return make_semantic_error(
                location,
                "column '" + name + "' does not exist in table '" +
                    relations.front()->table_name + "'",
                unqualified_column_suggestion(relations, name));
        }
        return make_semantic_error(
            location,
            "column '" + name + "' does not exist",
            unqualified_column_suggestion(relations, name));
    }
    return *match;
}

[[nodiscard]] const char* type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT";
        case Type::kBigInt:
            return "BIGINT";
        case Type::kDouble:
            return "DOUBLE";
        case Type::kBoolean:
            return "BOOLEAN";
        case Type::kVarchar:
            return "VARCHAR";
    }
    return "unknown";
}

[[nodiscard]] bool same_column(
    const BoundColumnRef& lhs,
    const BoundColumnRef& rhs) {
    return lhs.table_id == rhs.table_id && lhs.column_id == rhs.column_id;
}

[[nodiscard]] AggregateKind aggregate_kind(AstAggregateKind kind) {
    switch (kind) {
        case AstAggregateKind::kCount: return AggregateKind::kCount;
        case AstAggregateKind::kSum: return AggregateKind::kSum;
        case AstAggregateKind::kAvg: return AggregateKind::kAvg;
        case AstAggregateKind::kMin: return AggregateKind::kMin;
        case AstAggregateKind::kMax: return AggregateKind::kMax;
    }
    return AggregateKind::kCount;
}

[[nodiscard]] const char* aggregate_name(AstAggregateKind kind) {
    switch (kind) {
        case AstAggregateKind::kCount: return "COUNT";
        case AstAggregateKind::kSum: return "SUM";
        case AstAggregateKind::kAvg: return "AVG";
        case AstAggregateKind::kMin: return "MIN";
        case AstAggregateKind::kMax: return "MAX";
    }
    return "aggregate";
}

[[nodiscard]] BoundAggregateResult analyze_aggregate(
    const AstAggregateCall& aggregate,
    const RelationScope& relations) {
    if (!aggregate.argument.has_value()) {
        return BoundAggregateCall{
            AggregateKind::kCount, std::nullopt, Type::kBigInt, false};
    }

    const AstSelectColumn& argument = *aggregate.argument;
    ResolveColumnResult resolved = resolve_column(
        relations, argument.qualifier, argument.name, argument.location);
    if (const auto* error = std::get_if<CompileError>(&resolved)) {
        return *error;
    }
    const ResolvedColumn column = std::get<ResolvedColumn>(resolved);
    const Type input_type = column.metadata->type;
    const bool numeric = input_type == Type::kInt ||
        input_type == Type::kBigInt || input_type == Type::kDouble;

    Type output_type = input_type;
    bool nullable = true;
    switch (aggregate.kind) {
        case AstAggregateKind::kCount:
            output_type = Type::kBigInt;
            nullable = false;
            break;
        case AstAggregateKind::kSum:
            if (!numeric) {
                return make_semantic_error(
                    aggregate.location,
                    std::string{"SUM requires a numeric column, but "} +
                        type_name(input_type) + " found");
            }
            output_type = input_type == Type::kDouble ? Type::kDouble : Type::kBigInt;
            break;
        case AstAggregateKind::kAvg:
            if (!numeric) {
                return make_semantic_error(
                    aggregate.location,
                    std::string{"AVG requires a numeric column, but "} +
                        type_name(input_type) + " found");
            }
            output_type = Type::kDouble;
            break;
        case AstAggregateKind::kMin:
        case AstAggregateKind::kMax:
            if (!numeric && input_type != Type::kVarchar) {
                return make_semantic_error(
                    aggregate.location,
                    std::string{aggregate_name(aggregate.kind)} +
                        " does not support " + type_name(input_type));
            }
            break;
    }
    return BoundAggregateCall{
        aggregate_kind(aggregate.kind), column.reference, output_type, nullable};
}

[[nodiscard]] std::optional<Type> value_type(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data)) {
        return Type::kInt;
    }
    if (std::holds_alternative<std::int64_t>(value.data)) {
        return Type::kBigInt;
    }
    if (std::holds_alternative<double>(value.data)) {
        return Type::kDouble;
    }
    if (std::holds_alternative<bool>(value.data)) {
        return Type::kBoolean;
    }
    if (std::holds_alternative<std::string>(value.data)) {
        return Type::kVarchar;
    }
    return std::nullopt;
}

[[nodiscard]] AssignmentResult coerce_assignment(
    const AstLiteral& literal,
    const ColumnMeta& target) {
    if (std::holds_alternative<std::monostate>(literal.value.data)) {
        if (!target.nullable) {
            return make_semantic_error(
                literal.location,
                "column '" + target.name + "' is NOT NULL, but NULL found");
        }
        return literal.value;
    }

    const std::optional<Type> actual_type = value_type(literal.value);
    const bool widens_int_to_bigint =
        target.type == Type::kBigInt && actual_type == Type::kInt;
    const bool widens_to_double = target.type == Type::kDouble &&
        (actual_type == Type::kInt || actual_type == Type::kBigInt);
    if (!actual_type.has_value() ||
        (target.type != *actual_type && !widens_int_to_bigint && !widens_to_double)) {
        return make_semantic_error(
            literal.location,
            "column '" + target.name + "' expects " + type_name(target.type) +
                ", but " +
                (actual_type.has_value() ? type_name(*actual_type) : "unsupported") +
                " found");
    }
    if (widens_int_to_bigint) {
        return Value{static_cast<std::int64_t>(
            std::get<std::int32_t>(literal.value.data))};
    }
    if (widens_to_double && *actual_type == Type::kInt) {
        return Value{static_cast<double>(
            std::get<std::int32_t>(literal.value.data))};
    }
    if (widens_to_double) {
        return Value{static_cast<double>(
            std::get<std::int64_t>(literal.value.data))};
    }
    return literal.value;
}

[[nodiscard]] SemanticType semantic_type(Type type) {
    switch (type) {
        case Type::kInt:
            return SemanticType::kInt;
        case Type::kBigInt:
            return SemanticType::kBigInt;
        case Type::kDouble:
            return SemanticType::kDouble;
        case Type::kVarchar:
            return SemanticType::kVarchar;
        case Type::kBoolean:
            return SemanticType::kBool;
    }
    return SemanticType::kUnsupported;
}

[[nodiscard]] SemanticType semantic_type(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data)) {
        return SemanticType::kInt;
    }
    if (std::holds_alternative<std::int64_t>(value.data)) {
        return SemanticType::kBigInt;
    }
    if (std::holds_alternative<double>(value.data)) {
        return SemanticType::kDouble;
    }
    if (std::holds_alternative<bool>(value.data)) {
        return SemanticType::kBool;
    }
    if (std::holds_alternative<std::string>(value.data)) {
        return SemanticType::kVarchar;
    }
    if (std::holds_alternative<std::monostate>(value.data)) {
        return SemanticType::kNull;
    }
    return SemanticType::kUnsupported;
}

[[nodiscard]] bool is_numeric(SemanticType type) {
    return type == SemanticType::kInt || type == SemanticType::kBigInt ||
        type == SemanticType::kDouble;
}

[[nodiscard]] const char* semantic_type_name(SemanticType type) {
    switch (type) {
        case SemanticType::kInt:
            return "INT";
        case SemanticType::kBigInt:
            return "BIGINT";
        case SemanticType::kDouble:
            return "DOUBLE";
        case SemanticType::kVarchar:
            return "VARCHAR";
        case SemanticType::kBool:
            return "BOOL";
        case SemanticType::kNull:
            return "NULL";
        case SemanticType::kUnsupported:
            return "unsupported";
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
    if (const auto* null_test = std::get_if<AstNullTestExpr>(&expression.kind)) {
        return null_test->location;
    }
    return std::get<AstUnaryExpr>(expression.kind).location;
}

[[nodiscard]] ExpressionResult analyze_expression(
    const AstExpr& expression,
    const RelationScope& relations) {
    if (const auto* identifier = std::get_if<AstIdentifierExpr>(&expression.kind)) {
        ResolveColumnResult resolved = resolve_column(
            relations, identifier->qualifier, identifier->name, identifier->location);
        if (const auto* error = std::get_if<CompileError>(&resolved)) {
            return *error;
        }
        const ResolvedColumn column = std::get<ResolvedColumn>(resolved);
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(column.reference),
            semantic_type(column.metadata->type)
        };
    }

    if (const auto* literal = std::get_if<AstLiteralExpr>(&expression.kind)) {
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(BoundLiteral{literal->value}),
            semantic_type(literal->value)
        };
    }

    if (const auto* binary = std::get_if<AstBinaryExpr>(&expression.kind)) {
        ExpressionResult lhs_result = analyze_expression(*binary->lhs, relations);
        if (const auto* error = std::get_if<CompileError>(&lhs_result)) {
            return *error;
        }
        AnalyzedExpression lhs = std::get<AnalyzedExpression>(std::move(lhs_result));

        ExpressionResult rhs_result = analyze_expression(*binary->rhs, relations);
        if (const auto* error = std::get_if<CompileError>(&rhs_result)) {
            return *error;
        }
        AnalyzedExpression rhs = std::get<AnalyzedExpression>(std::move(rhs_result));

        if (const auto* comparison = std::get_if<AstCompareOp>(&binary->op)) {
            const std::string op = compare_symbol(*comparison);
            const bool lhs_null = lhs.type == SemanticType::kNull;
            const bool rhs_null = rhs.type == SemanticType::kNull;
            if (lhs_null || rhs_null) {
                const SemanticType concrete = lhs_null ? rhs.type : lhs.type;
                if (concrete == SemanticType::kUnsupported) {
                    return make_semantic_error(
                        binary->location,
                        "operator '" + op + "' cannot be applied to unsupported values");
                }
                if (!is_equality(*comparison) &&
                    (concrete == SemanticType::kBool || concrete == SemanticType::kVarchar)) {
                    return make_semantic_error(
                        binary->location,
                        "operator '" + op + "' cannot be applied to " +
                            semantic_type_name(concrete));
                }
                return AnalyzedExpression{
                    std::make_unique<BoundExpr>(BoundBinaryExpr{
                        bound_compare_op(*comparison),
                        std::move(lhs.expression),
                        std::move(rhs.expression)}),
                    SemanticType::kBool};
            }
            if (lhs.type != rhs.type &&
                !(is_numeric(lhs.type) && is_numeric(rhs.type))) {
                return make_semantic_error(
                    binary->location,
                    "operator '" + op + "' cannot be applied to " +
                        semantic_type_name(lhs.type) + " and " +
                        semantic_type_name(rhs.type));
            }
            if (lhs.type == SemanticType::kUnsupported ||
                rhs.type == SemanticType::kUnsupported) {
                return make_semantic_error(
                    binary->location,
                    "operator '" + op + "' cannot be applied to unsupported values");
            }
            if (lhs.type == SemanticType::kBool && !is_equality(*comparison)) {
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
        const bool lhs_truth = lhs.type == SemanticType::kBool || lhs.type == SemanticType::kNull;
        const bool rhs_truth = rhs.type == SemanticType::kBool || rhs.type == SemanticType::kNull;
        if (!lhs_truth || !rhs_truth) {
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

    if (const auto* null_test = std::get_if<AstNullTestExpr>(&expression.kind)) {
        ExpressionResult operand_result = analyze_expression(*null_test->operand, relations);
        if (const auto* error = std::get_if<CompileError>(&operand_result)) {
            return *error;
        }
        AnalyzedExpression operand = std::get<AnalyzedExpression>(std::move(operand_result));
        if (operand.type == SemanticType::kUnsupported) {
            return make_semantic_error(
                null_test->location,
                "IS NULL operand has an unsupported type");
        }
        return AnalyzedExpression{
            std::make_unique<BoundExpr>(BoundNullTestExpr{
                null_test->op == AstNullTestOp::kIsNull
                    ? NullTestOp::kIsNull
                    : NullTestOp::kIsNotNull,
                std::move(operand.expression)}),
            SemanticType::kBool};
    }

    const auto& unary = std::get<AstUnaryExpr>(expression.kind);
    ExpressionResult operand_result = analyze_expression(*unary.operand, relations);
    if (const auto* error = std::get_if<CompileError>(&operand_result)) {
        return *error;
    }
    AnalyzedExpression operand = std::get<AnalyzedExpression>(std::move(operand_result));
    if (operand.type != SemanticType::kBool && operand.type != SemanticType::kNull) {
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
        bound.columns.push_back(ColumnMeta{column.name, column.type, column.nullable});
    }
    return SemanticResult{BoundStatement{std::move(bound)}};
}

[[nodiscard]] SemanticResult analyze_insert(const InsertAst& insert, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, insert.table_name);
    if (table == nullptr) {
        return semantic_error(
            insert.table_location,
            "table '" + insert.table_name + "' does not exist",
            table_suggestion(catalog, insert.table_name));
    }

    std::vector<ColumnId> bound_columns;
    bound_columns.reserve(insert.columns.size());
    for (const auto& column : insert.columns) {
        const std::optional<ColumnId> column_id = find_column(*table, column.name);
        if (!column_id.has_value()) {
            return semantic_error(
                column.location,
                "column '" + column.name + "' does not exist in table '" +
                    table->table_name + "'",
                qualified_column_suggestion(*table, table->table_name, column.name));
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
            AssignmentResult coerced = coerce_assignment(row[index], target);
            if (const auto* error = std::get_if<CompileError>(&coerced)) {
                return SemanticResult{*error};
            }
            bound_row.push_back(std::get<Value>(std::move(coerced)));
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
            "table '" + select.table_name + "' does not exist",
            table_suggestion(catalog, select.table_name));
    }

    RelationScope relations{table};
    std::vector<BoundJoin> joins;
    joins.reserve(select.joins.size());
    for (const AstJoin& join : select.joins) {
        const TableMeta* joined_table = find_table(catalog, join.table_name);
        if (joined_table == nullptr) {
            return semantic_error(
                join.table_location,
                "table '" + join.table_name + "' does not exist",
                table_suggestion(catalog, join.table_name));
        }
        const bool duplicate_relation = std::any_of(
            relations.begin(), relations.end(), [joined_table](const TableMeta* relation) {
                return relation->table_id == joined_table->table_id;
            });
        if (duplicate_relation) {
            return semantic_error(
                join.table_location,
                "table '" + join.table_name +
                    "' is already present in query; aliases are not supported");
        }
        relations.push_back(joined_table);

        ExpressionResult result = analyze_expression(*join.condition, relations);
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool && analyzed.type != SemanticType::kNull) {
            return semantic_error(
                expression_location(*join.condition),
                "JOIN ON predicate must be BOOL");
        }
        joins.push_back(BoundJoin{joined_table->table_id, std::move(analyzed.expression)});
    }

    std::vector<BoundColumnRef> group_by;
    group_by.reserve(select.group_by.size());
    for (const AstSelectColumn& key : select.group_by) {
        ResolveColumnResult resolved = resolve_column(
            relations, key.qualifier, key.name, key.location);
        if (const auto* error = std::get_if<CompileError>(&resolved)) {
            return SemanticResult{*error};
        }
        const BoundColumnRef reference = std::get<ResolvedColumn>(resolved).reference;
        if (std::none_of(
                group_by.begin(), group_by.end(),
                [&reference](const BoundColumnRef& existing) {
                    return same_column(existing, reference);
                })) {
            group_by.push_back(reference);
        }
    }

    const bool has_aggregate = std::any_of(
        select.items.begin(), select.items.end(), [](const AstSelectItem& item) {
            return std::holds_alternative<AstAggregateCall>(item);
        });
    const bool aggregate_query = has_aggregate || !group_by.empty();
    if (select.select_all && aggregate_query) {
        return semantic_error(
            select.table_location,
            "SELECT * is not allowed in an aggregate query");
    }

    std::vector<BoundSelectItem> items;
    if (select.select_all) {
        for (const TableMeta* relation : relations) {
            for (std::size_t index = 0; index < relation->columns.size(); ++index) {
                items.emplace_back(BoundColumnRef{
                    relation->table_id, static_cast<ColumnId>(index)});
            }
        }
    } else {
        items.reserve(select.items.size());
        for (const AstSelectItem& item : select.items) {
            if (const auto* column = std::get_if<AstSelectColumn>(&item)) {
                ResolveColumnResult resolved = resolve_column(
                    relations, column->qualifier, column->name, column->location);
                if (const auto* error = std::get_if<CompileError>(&resolved)) {
                    return SemanticResult{*error};
                }
                const BoundColumnRef reference = std::get<ResolvedColumn>(resolved).reference;
                if (aggregate_query && std::none_of(
                        group_by.begin(), group_by.end(),
                        [&reference](const BoundColumnRef& key) {
                            return same_column(key, reference);
                        })) {
                    return semantic_error(
                        column->location,
                        "column '" + column->name +
                            "' is neither grouped nor aggregated");
                }
                items.emplace_back(reference);
                continue;
            }
            BoundAggregateResult aggregate = analyze_aggregate(
                std::get<AstAggregateCall>(item), relations);
            if (const auto* error = std::get_if<CompileError>(&aggregate)) {
                return SemanticResult{*error};
            }
            items.emplace_back(std::get<BoundAggregateCall>(std::move(aggregate)));
        }
    }

    BoundExprPtr predicate;
    if (select.predicate != nullptr) {
        ExpressionResult result = analyze_expression(*select.predicate, relations);
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool && analyzed.type != SemanticType::kNull) {
            return semantic_error(
                expression_location(*select.predicate),
                "WHERE predicate must be BOOL");
        }
        predicate = std::move(analyzed.expression);
    }

    std::vector<BoundSortKey> order_by;
    order_by.reserve(select.order_by.size());
    for (const AstSortKey& key : select.order_by) {
        ResolveColumnResult resolved = resolve_column(
            relations, key.qualifier, key.column_name, key.location);
        if (const auto* error = std::get_if<CompileError>(&resolved)) {
            return SemanticResult{*error};
        }
        const BoundColumnRef reference = std::get<ResolvedColumn>(resolved).reference;
        if (aggregate_query && std::none_of(
                group_by.begin(), group_by.end(),
                [&reference](const BoundColumnRef& group_key) {
                    return same_column(group_key, reference);
                })) {
            return semantic_error(
                key.location,
                "ORDER BY column '" + key.column_name +
                    "' is neither grouped nor aggregated");
        }
        order_by.push_back(BoundSortKey{
            reference,
            key.direction == AstSortDirection::kAsc
                ? SortDirection::kAsc
                : SortDirection::kDesc});
    }

    return SemanticResult{BoundStatement{BoundSelect{
        table->table_id,
        std::move(joins),
        std::move(items),
        std::move(predicate),
        std::move(group_by),
        std::move(order_by)
    }}};
}

[[nodiscard]] SemanticResult analyze_delete(const DeleteAst& deletion, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, deletion.table_name);
    if (table == nullptr) {
        return semantic_error(
            deletion.table_location,
            "table '" + deletion.table_name + "' does not exist",
            table_suggestion(catalog, deletion.table_name));
    }

    BoundExprPtr predicate;
    if (deletion.predicate != nullptr) {
        ExpressionResult result = analyze_expression(
            *deletion.predicate, RelationScope{table});
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool && analyzed.type != SemanticType::kNull) {
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

[[nodiscard]] SemanticResult analyze_update(const UpdateAst& update, CatalogView catalog) {
    const TableMeta* table = find_table(catalog, update.table_name);
    if (table == nullptr) {
        return semantic_error(
            update.table_location,
            "table '" + update.table_name + "' does not exist",
            table_suggestion(catalog, update.table_name));
    }

    BoundUpdate bound{table->table_id, {}, nullptr};
    bound.assignments.reserve(update.assignments.size());
    for (const AstUpdateAssignment& assignment : update.assignments) {
        const std::optional<ColumnId> column_id = find_column(*table, assignment.column_name);
        if (!column_id.has_value()) {
            return semantic_error(
                assignment.column_location,
                "column '" + assignment.column_name + "' does not exist in table '" +
                    table->table_name + "'",
                qualified_column_suggestion(
                    *table, table->table_name, assignment.column_name));
        }
        const auto duplicate = std::find_if(
            bound.assignments.begin(),
            bound.assignments.end(),
            [column_id](const BoundUpdateAssignment& candidate) {
                return candidate.column_id == *column_id;
            });
        if (duplicate != bound.assignments.end()) {
            return semantic_error(
                assignment.column_location,
                "duplicate column '" + assignment.column_name + "'");
        }
        AssignmentResult coerced = coerce_assignment(
            assignment.literal,
            table->columns[*column_id]);
        if (const auto* error = std::get_if<CompileError>(&coerced)) {
            return SemanticResult{*error};
        }
        bound.assignments.push_back(BoundUpdateAssignment{
            *column_id,
            std::get<Value>(std::move(coerced))});
    }

    if (update.predicate != nullptr) {
        ExpressionResult result = analyze_expression(
            *update.predicate, RelationScope{table});
        if (const auto* error = std::get_if<CompileError>(&result)) {
            return SemanticResult{*error};
        }
        AnalyzedExpression analyzed = std::get<AnalyzedExpression>(std::move(result));
        if (analyzed.type != SemanticType::kBool && analyzed.type != SemanticType::kNull) {
            return semantic_error(
                expression_location(*update.predicate),
                "WHERE predicate must be BOOL");
        }
        bound.predicate = std::move(analyzed.expression);
    }
    return SemanticResult{BoundStatement{std::move(bound)}};
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
    if (const auto* deletion = std::get_if<DeleteAst>(&statement.kind)) {
        return analyze_delete(*deletion, catalog);
    }
    return analyze_update(std::get<UpdateAst>(statement.kind), catalog);
}

}  // namespace tinydbms::compiler::internal
