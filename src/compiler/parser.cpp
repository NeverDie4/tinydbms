#include "parser.hpp"

#include "diagnostic_suggestion.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydbms::compiler::internal {
namespace {

[[nodiscard]] std::optional<Value> parse_integer_literal(std::string_view lexeme) {
    std::int64_t value = 0;
    const char* const begin = lexeme.data();
    const char* const end = begin + lexeme.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    if (value <= std::numeric_limits<std::int32_t>::max()) {
        return Value{static_cast<std::int32_t>(value)};
    }
    return Value{value};
}

[[nodiscard]] std::optional<Value> parse_double_literal(std::string_view lexeme) {
    double value = 0.0;
    const char* const begin = lexeme.data();
    const char* const end = begin + lexeme.size();
    const auto [parsed_end, error] =
        std::from_chars(begin, end, value, std::chars_format::fixed);
    if (error != std::errc{} || parsed_end != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return Value{value};
}

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens)
        : tokens_{tokens}, fallback_end_{TokenKind::kEnd, "", fallback_location(tokens)} {}

    ParseResult run() {
        if (match(TokenKind::kCreate)) {
            return parse_create_table();
        }
        if (match(TokenKind::kInsert)) {
            return parse_insert();
        }
        if (match(TokenKind::kSelect)) {
            return parse_select();
        }
        if (match(TokenKind::kDelete)) {
            return parse_delete();
        }
        if (match(TokenKind::kUpdate)) {
            return parse_update();
        }

        static constexpr SuggestionCandidate candidates[]{
            {"create", "CREATE"}, {"insert", "INSERT"}, {"select", "SELECT"},
            {"delete", "DELETE"}, {"update", "UPDATE"}};
        return keyword_error(
            "expected CREATE, INSERT, SELECT, DELETE, or UPDATE", candidates);
    }

private:
    ParseResult parse_create_table() {
        if (!match(TokenKind::kTable)) {
            return keyword_error("expected TABLE after CREATE", {{{"table", "TABLE"}}});
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier");
        }

        std::string table_name = peek().lexeme;
        const SourceRange table_location = peek().source;
        advance();

        if (!match(TokenKind::kLeftParen)) {
            return syntax_error("expected '(' after table identifier");
        }

        std::vector<AstColumnDef> columns;
        while (true) {
            if (!check(TokenKind::kIdentifier)) {
                return syntax_error("expected column identifier");
            }

            std::string column_name = peek().lexeme;
            const SourceRange column_location = peek().source;
            advance();

            Type column_type;
            if (match(TokenKind::kInt)) {
                column_type = Type::kInt;
            } else if (match(TokenKind::kBigInt)) {
                column_type = Type::kBigInt;
            } else if (match(TokenKind::kDouble)) {
                column_type = Type::kDouble;
            } else if (match(TokenKind::kBoolean)) {
                column_type = Type::kBoolean;
            } else if (match(TokenKind::kVarchar)) {
                column_type = Type::kVarchar;
            } else {
                return syntax_error(
                    "expected INT, BIGINT, DOUBLE, BOOLEAN, or VARCHAR after column identifier");
            }

            bool nullable = true;
            if (match(TokenKind::kNull)) {
                nullable = true;
            } else if (match(TokenKind::kNot)) {
                if (!match(TokenKind::kNull)) {
                return keyword_error(
                    "expected NULL after NOT in column definition", {{{"null", "NULL"}}});
                }
                nullable = false;
            }

            columns.push_back(AstColumnDef{
                std::move(column_name),
                column_type,
                nullable,
                column_location
            });

            if (!match(TokenKind::kComma)) {
                if (check(TokenKind::kIdentifier)) {
                    return insertion_error(
                        "expected ')' after column definitions", ",");
                }
                break;
            }
        }

        if (!match(TokenKind::kRightParen)) {
            return right_parenthesis_error("expected ')' after column definitions");
        }
        if (!match(TokenKind::kSemicolon)) {
            return semicolon_error("expected ';' after CREATE TABLE statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }

        CreateTableAst create{
            std::move(table_name),
            table_location,
            std::move(columns)
        };
        return ParseResult{StatementAst{
            std::move(create)
        }};
    }

    ParseResult parse_insert() {
        if (!match(TokenKind::kInto)) {
            return keyword_error("expected INTO after INSERT", {{{"into", "INTO"}}});
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier");
        }

        std::string table_name = peek().lexeme;
        const SourceRange table_location = peek().source;
        advance();

        std::vector<AstInsertColumn> columns;
        if (match(TokenKind::kLeftParen)) {
            while (true) {
                if (!check(TokenKind::kIdentifier)) {
                    return syntax_error("expected column identifier");
                }

                columns.push_back(AstInsertColumn{peek().lexeme, peek().source});
                advance();
                if (!match(TokenKind::kComma)) {
                    break;
                }
            }

            if (!match(TokenKind::kRightParen)) {
                return right_parenthesis_error("expected ')' after column list");
            }
        }

        if (!match(TokenKind::kValues)) {
            return keyword_error(
                "expected VALUES after table or column list", {{{"values", "VALUES"}}});
        }

        std::vector<std::vector<AstLiteral>> rows;
        while (true) {
            if (!match(TokenKind::kLeftParen)) {
                return syntax_error("expected '(' before VALUES row");
            }

            std::vector<AstLiteral> row;
            while (true) {
                const Token& literal = peek();
                if (literal.kind == TokenKind::kIntegerLiteral) {
                    const std::optional<Value> value = parse_integer_literal(literal.lexeme);
                    if (!value.has_value()) {
                        return syntax_error("invalid integer literal");
                    }
                    row.push_back(AstLiteral{*value, literal.source});
                    advance();
                } else if (literal.kind == TokenKind::kDoubleLiteral) {
                    const std::optional<Value> value = parse_double_literal(literal.lexeme);
                    if (!value.has_value()) {
                        return syntax_error("invalid floating literal");
                    }
                    row.push_back(AstLiteral{*value, literal.source});
                    advance();
                } else if (literal.kind == TokenKind::kStringLiteral) {
                    row.push_back(AstLiteral{Value{literal.lexeme}, literal.source});
                    advance();
                } else if (literal.kind == TokenKind::kTrue ||
                           literal.kind == TokenKind::kFalse) {
                    row.push_back(AstLiteral{
                        Value{literal.kind == TokenKind::kTrue},
                        literal.source
                    });
                    advance();
                } else if (literal.kind == TokenKind::kNull) {
                    row.push_back(AstLiteral{Value{std::monostate{}}, literal.source});
                    advance();
                } else {
                    return syntax_error(
                        "expected integer, floating, BOOLEAN, NULL, or string literal");
                }

                if (!match(TokenKind::kComma)) {
                    if (starts_literal(peek().kind)) {
                        return insertion_error(
                            "expected ')' after VALUES row", ",");
                    }
                    break;
                }
            }

            if (!match(TokenKind::kRightParen)) {
                return right_parenthesis_error("expected ')' after VALUES row");
            }
            rows.push_back(std::move(row));

            if (!match(TokenKind::kComma)) {
                break;
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            return semicolon_error("expected ';' after INSERT statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }

        InsertAst insert{
            std::move(table_name),
            table_location,
            std::move(columns),
            std::move(rows)
        };
        return ParseResult{StatementAst{
            std::move(insert)
        }};
    }

    ParseResult parse_select() {
        bool select_all = false;
        std::vector<AstSelectItem> items;
        if (match(TokenKind::kStar)) {
            select_all = true;
        } else {
            bool first_item = true;
            while (true) {
                if (check(TokenKind::kCount) || check(TokenKind::kSum) ||
                    check(TokenKind::kAvg) || check(TokenKind::kMin) ||
                    check(TokenKind::kMax)) {
                    const Token function = peek();
                    AstAggregateKind kind = AstAggregateKind::kCount;
                    switch (function.kind) {
                        case TokenKind::kCount: kind = AstAggregateKind::kCount; break;
                        case TokenKind::kSum: kind = AstAggregateKind::kSum; break;
                        case TokenKind::kAvg: kind = AstAggregateKind::kAvg; break;
                        case TokenKind::kMin: kind = AstAggregateKind::kMin; break;
                        case TokenKind::kMax: kind = AstAggregateKind::kMax; break;
                        default: break;
                    }
                    advance();
                    if (!match(TokenKind::kLeftParen)) {
                        return syntax_error("expected '(' after aggregate function");
                    }
                    std::optional<AstSelectColumn> argument;
                    if (match(TokenKind::kStar)) {
                        if (kind != AstAggregateKind::kCount) {
                            return syntax_error("only COUNT accepts '*' as an argument");
                        }
                    } else {
                        std::optional<AstIdentifierExpr> column =
                            parse_column_reference("expected aggregate column argument");
                        if (!column.has_value()) {
                            return take_expression_error();
                        }
                        argument = AstSelectColumn{
                            std::move(column->qualifier),
                            std::move(column->name),
                            column->source};
                    }
                    if (!match(TokenKind::kRightParen)) {
                        return right_parenthesis_error("expected ')' after aggregate argument");
                    }
                    items.emplace_back(AstAggregateCall{kind, std::move(argument), function.source});
                } else {
                    std::optional<AstIdentifierExpr> column =
                        parse_column_reference(first_item
                            ? "expected SELECT column or '*'"
                            : "expected column identifier or aggregate after ','");
                    if (!column.has_value()) {
                        return take_expression_error();
                    }
                    items.emplace_back(AstSelectColumn{
                        std::move(column->qualifier),
                        std::move(column->name),
                        column->source});
                }
                first_item = false;
                if (!match(TokenKind::kComma)) {
                    break;
                }
            }
        }

        if (!match(TokenKind::kFrom)) {
            if (check(TokenKind::kIdentifier) && peek().lexeme == "as") {
                return syntax_error("SELECT aliases are not supported");
            }
            if (check(TokenKind::kIdentifier) && peek_next().kind == TokenKind::kFrom) {
                return insertion_error("expected FROM after SELECT list", ",");
            }
            return keyword_error(
                "expected FROM after SELECT list", {{{"from", "FROM"}}});
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier after FROM");
        }

        std::string table_name = peek().lexeme;
        const SourceRange table_location = peek().source;
        advance();
        if (auto alias_error = reject_table_alias(); alias_error.has_value()) {
            return std::move(*alias_error);
        }

        std::vector<AstJoin> joins;
        while (check(TokenKind::kJoin) || check(TokenKind::kInner)) {
            if (match(TokenKind::kInner) && !match(TokenKind::kJoin)) {
                return keyword_error("expected JOIN after INNER", {{{"join", "JOIN"}}});
            }
            if (check(TokenKind::kJoin)) {
                advance();
            }
            if (!check(TokenKind::kIdentifier)) {
                return syntax_error("expected table identifier after JOIN");
            }
            std::string joined_table = peek().lexeme;
            const SourceRange joined_location = peek().source;
            advance();
            if (auto alias_error = reject_table_alias(); alias_error.has_value()) {
                return std::move(*alias_error);
            }
            if (!match(TokenKind::kOn)) {
                return keyword_error("expected ON after joined table", {{{"on", "ON"}}});
            }
            AstExprPtr condition = parse_expression();
            if (condition == nullptr) {
                return take_expression_error();
            }
            joins.push_back(AstJoin{
                std::move(joined_table),
                joined_location,
                std::move(condition)});
        }

        AstExprPtr predicate;
        if (match(TokenKind::kWhere)) {
            predicate = parse_expression();
            if (predicate == nullptr) {
                return take_expression_error();
            }
        }

        std::vector<AstSelectColumn> group_by;
        if (match(TokenKind::kGroup)) {
            if (!match(TokenKind::kBy)) {
                return keyword_error("expected BY after GROUP", {{{"by", "BY"}}});
            }
            while (true) {
                std::optional<AstIdentifierExpr> column =
                    parse_column_reference("expected column identifier in GROUP BY");
                if (!column.has_value()) {
                    return take_expression_error();
                }
                group_by.push_back(AstSelectColumn{
                    std::move(column->qualifier),
                    std::move(column->name),
                    column->source});
                if (!match(TokenKind::kComma)) {
                    if (check(TokenKind::kIdentifier)) {
                        return insertion_error(
                            "expected ';' after SELECT statement", ",");
                    }
                    break;
                }
            }
        }

        std::vector<AstSortKey> order_by;
        if (match(TokenKind::kOrder)) {
            if (!match(TokenKind::kBy)) {
                return keyword_error("expected BY after ORDER", {{{"by", "BY"}}});
            }
            while (true) {
                std::optional<AstIdentifierExpr> column =
                    parse_column_reference("expected column identifier in ORDER BY");
                if (!column.has_value()) {
                    return take_expression_error();
                }

                AstSortDirection direction = AstSortDirection::kAsc;
                if (match(TokenKind::kAsc)) {
                    direction = AstSortDirection::kAsc;
                } else if (match(TokenKind::kDesc)) {
                    direction = AstSortDirection::kDesc;
                }
                order_by.push_back(AstSortKey{
                    std::move(column->qualifier),
                    std::move(column->name),
                    column->source,
                    direction});
                if (!match(TokenKind::kComma)) {
                    if (check(TokenKind::kIdentifier)) {
                        return insertion_error(
                            "expected ';' after SELECT statement", ",");
                    }
                    break;
                }
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            static constexpr SuggestionCandidate candidates[]{
                {"join", "JOIN"}, {"inner", "INNER"}, {"where", "WHERE"},
                {"group", "GROUP"}, {"order", "ORDER"}};
            if (check(TokenKind::kIdentifier)) {
                ParseResult keyword = keyword_error(
                    "expected ';' after SELECT statement", candidates);
                const auto* error = std::get_if<CompileError>(&keyword.outcome);
                if (error != nullptr && error->suggestion.has_value()) {
                    return keyword;
                }
            }
            return semicolon_error("expected ';' after SELECT statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }

        SelectAst select{
            std::move(table_name),
            table_location,
            std::move(joins),
            select_all,
            std::move(items),
            std::move(predicate),
            std::move(group_by),
            std::move(order_by)
        };
        return ParseResult{StatementAst{
            std::move(select)
        }};
    }

    ParseResult parse_delete() {
        if (!match(TokenKind::kFrom)) {
            return keyword_error("expected FROM after DELETE", {{{"from", "FROM"}}});
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier after FROM");
        }

        std::string table_name = peek().lexeme;
        const SourceRange table_location = peek().source;
        advance();

        AstExprPtr predicate;
        if (match(TokenKind::kWhere)) {
            predicate = parse_expression();
            if (predicate == nullptr) {
                return take_expression_error();
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            return semicolon_error("expected ';' after DELETE statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }

        DeleteAst deletion{
            std::move(table_name),
            table_location,
            std::move(predicate)
        };
        return ParseResult{StatementAst{
            std::move(deletion)
        }};
    }

    ParseResult parse_update() {
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier after UPDATE");
        }
        std::string table_name = peek().lexeme;
        const SourceRange table_location = peek().source;
        advance();
        if (!match(TokenKind::kSet)) {
            return keyword_error("expected SET after table identifier", {{{"set", "SET"}}});
        }

        std::vector<AstUpdateAssignment> assignments;
        while (true) {
            if (!check(TokenKind::kIdentifier)) {
                return syntax_error("expected column identifier in SET clause");
            }
            std::string column_name = peek().lexeme;
            const SourceRange column_location = peek().source;
            advance();
            if (!match(TokenKind::kEq)) {
                return syntax_error("expected '=' after SET column");
            }

            const Token& token = peek();
            std::optional<Value> value;
            if (token.kind == TokenKind::kIntegerLiteral) {
                value = parse_integer_literal(token.lexeme);
            } else if (token.kind == TokenKind::kDoubleLiteral) {
                value = parse_double_literal(token.lexeme);
            } else if (token.kind == TokenKind::kStringLiteral) {
                value = Value{token.lexeme};
            } else if (token.kind == TokenKind::kTrue || token.kind == TokenKind::kFalse) {
                value = Value{token.kind == TokenKind::kTrue};
            } else if (token.kind == TokenKind::kNull) {
                value = Value{std::monostate{}};
            }
            if (!value.has_value()) {
                return syntax_error(
                    "expected integer, floating, BOOLEAN, NULL, or string literal after '='");
            }
            assignments.push_back(AstUpdateAssignment{
                std::move(column_name),
                column_location,
                AstLiteral{std::move(*value), token.source}});
            advance();
            if (!match(TokenKind::kComma)) {
                if (check(TokenKind::kIdentifier)) {
                    return insertion_error("expected ';' after UPDATE statement", ",");
                }
                break;
            }
        }

        AstExprPtr predicate;
        if (match(TokenKind::kWhere)) {
            predicate = parse_expression();
            if (predicate == nullptr) {
                return take_expression_error();
            }
        }
        if (!match(TokenKind::kSemicolon)) {
            return semicolon_error("expected ';' after UPDATE statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }
        return ParseResult{StatementAst{UpdateAst{
            std::move(table_name),
            table_location,
            std::move(assignments),
            std::move(predicate)}}};
    }

    AstExprPtr parse_expression() {
        return parse_or();
    }

    AstExprPtr parse_or() {
        AstExprPtr lhs = parse_and();
        if (lhs == nullptr) {
            return nullptr;
        }

        while (check(TokenKind::kOr)) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            const SourceRange location = peek().source;
            advance();
            AstExprPtr rhs = parse_and();
            if (rhs == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<AstExpr>(AstBinaryExpr{
                AstLogicOp::kOr,
                location,
                std::move(lhs),
                std::move(rhs)
            });
        }
        return lhs;
    }

    AstExprPtr parse_and() {
        AstExprPtr lhs = parse_not();
        if (lhs == nullptr) {
            return nullptr;
        }

        while (check(TokenKind::kAnd)) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            const SourceRange location = peek().source;
            advance();
            AstExprPtr rhs = parse_not();
            if (rhs == nullptr) {
                return nullptr;
            }
            lhs = std::make_unique<AstExpr>(AstBinaryExpr{
                AstLogicOp::kAnd,
                location,
                std::move(lhs),
                std::move(rhs)
            });
        }
        return lhs;
    }

    AstExprPtr parse_not() {
        if (!check(TokenKind::kNot)) {
            return parse_comparison();
        }
        if (!consume_expression_budget()) {
            return nullptr;
        }

        const SourceRange location = peek().source;
        advance();
        AstExprPtr operand = parse_not();
        if (operand == nullptr) {
            return nullptr;
        }
        return std::make_unique<AstExpr>(AstUnaryExpr{location, std::move(operand)});
    }

    AstExprPtr parse_comparison() {
        AstExprPtr lhs = parse_primary();
        if (lhs == nullptr) {
            return nullptr;
        }

        if (check(TokenKind::kIs)) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            const SourceRange location = peek().source;
            advance();
            const bool negated = match(TokenKind::kNot);
            if (!match(TokenKind::kNull)) {
                return expression_error(
                    negated ? "expected NULL after IS NOT" : "expected NULL after IS");
            }
            return std::make_unique<AstExpr>(AstNullTestExpr{
                negated ? AstNullTestOp::kIsNotNull : AstNullTestOp::kIsNull,
                location,
                std::move(lhs)
            });
        }

        const auto op = comparison_operator(peek().kind);
        if (!op.has_value()) {
            return lhs;
        }
        if (!consume_expression_budget()) {
            return nullptr;
        }

        const SourceRange location = peek().source;
        advance();
        AstExprPtr rhs = parse_primary();
        if (rhs == nullptr) {
            return nullptr;
        }
        return std::make_unique<AstExpr>(AstBinaryExpr{
            *op,
            location,
            std::move(lhs),
            std::move(rhs)
        });
    }

    AstExprPtr parse_primary() {
        const Token& token = peek();
        if (token.kind == TokenKind::kIdentifier) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            std::optional<AstIdentifierExpr> column =
                parse_column_reference("expected column identifier");
            if (!column.has_value()) {
                return nullptr;
            }
            return std::make_unique<AstExpr>(std::move(*column));
        }

        if (token.kind == TokenKind::kIntegerLiteral) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            const std::optional<Value> value = parse_integer_literal(token.lexeme);
            if (!value.has_value()) {
                return expression_error("invalid integer literal in expression");
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                *value,
                token.source
            });
            advance();
            return expression;
        }

        if (token.kind == TokenKind::kDoubleLiteral) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            const std::optional<Value> value = parse_double_literal(token.lexeme);
            if (!value.has_value()) {
                return expression_error("invalid floating literal in expression");
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                *value,
                token.source
            });
            advance();
            return expression;
        }

        if (token.kind == TokenKind::kStringLiteral) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                Value{token.lexeme},
                token.source
            });
            advance();
            return expression;
        }

        if (token.kind == TokenKind::kTrue || token.kind == TokenKind::kFalse) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                Value{token.kind == TokenKind::kTrue},
                token.source
            });
            advance();
            return expression;
        }

        if (token.kind == TokenKind::kNull) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                Value{std::monostate{}},
                token.source
            });
            advance();
            return expression;
        }

        if (check(TokenKind::kLeftParen)) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            advance();
            AstExprPtr expression = parse_expression();
            if (expression == nullptr) {
                return nullptr;
            }
            if (!match(TokenKind::kRightParen)) {
                return expression_error("expected ')' after expression");
            }
            return expression;
        }

        return expression_error("expected expression");
    }

    std::optional<AstIdentifierExpr> parse_column_reference(std::string message) {
        if (!check(TokenKind::kIdentifier)) {
            expression_error(std::move(message));
            return std::nullopt;
        }

        std::optional<std::string> qualifier;
        std::string name = peek().lexeme;
        const SourceRange location = peek().source;
        advance();
        if (match(TokenKind::kDot)) {
            qualifier = std::move(name);
            if (!check(TokenKind::kIdentifier)) {
                expression_error("expected column identifier after '.'");
                return std::nullopt;
            }
            name = peek().lexeme;
            advance();
        }
        return AstIdentifierExpr{std::move(qualifier), std::move(name), location};
    }

    bool consume_expression_budget() {
        if (expression_complexity_ >= kMaxExpressionComplexity) {
            expression_error("expression exceeds maximum supported complexity");
            return false;
        }
        ++expression_complexity_;
        return true;
    }

    static std::optional<AstCompareOp> comparison_operator(TokenKind kind) {
        switch (kind) {
            case TokenKind::kEq:
                return AstCompareOp::kEq;
            case TokenKind::kNe:
                return AstCompareOp::kNe;
            case TokenKind::kLt:
                return AstCompareOp::kLt;
            case TokenKind::kLe:
                return AstCompareOp::kLe;
            case TokenKind::kGt:
                return AstCompareOp::kGt;
            case TokenKind::kGe:
                return AstCompareOp::kGe;
            default:
                return std::nullopt;
        }
    }

    AstExprPtr expression_error(std::string message) {
        if (!expression_error_.has_value()) {
            expression_error_ = CompileError{
                CompileStage::kSyntax,
                peek().source,
                std::move(message)
            };
        }
        return nullptr;
    }

    ParseResult take_expression_error() {
        if (!expression_error_.has_value()) {
            return syntax_error("expected expression");
        }
        return ParseResult{std::move(*expression_error_)};
    }

    static SourceRange fallback_location(const std::vector<Token>& tokens) {
        if (tokens.empty()) {
            const SourceLocation start{1, 1, 0};
            return SourceRange{start, start};
        }
        return tokens.back().source;
    }

    [[nodiscard]] const Token& peek() const {
        return index_ < tokens_.size() ? tokens_[index_] : fallback_end_;
    }

    [[nodiscard]] const Token& peek_next() const {
        const std::size_t next = index_ + 1U;
        return next < tokens_.size() ? tokens_[next] : fallback_end_;
    }

    [[nodiscard]] bool check(TokenKind kind) const {
        return peek().kind == kind;
    }

    const Token& advance() {
        const Token& token = peek();
        if (index_ < tokens_.size() && token.kind != TokenKind::kEnd) {
            ++index_;
        }
        return token;
    }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
    }

    [[nodiscard]] ParseResult syntax_error(std::string message) const {
        return ParseResult{CompileError{
            CompileStage::kSyntax,
            peek().source,
            std::move(message)
        }};
    }

    [[nodiscard]] std::optional<ParseResult> reject_table_alias() {
        if (!check(TokenKind::kIdentifier)) {
            return std::nullopt;
        }
        if (peek().lexeme == "as") {
            advance();
            return check(TokenKind::kIdentifier)
                ? std::optional{syntax_error("table aliases are not supported")}
                : std::optional{syntax_error("expected table alias after AS")};
        }
        const TokenKind follower = peek_next().kind;
        if (follower == TokenKind::kSemicolon || follower == TokenKind::kJoin ||
            follower == TokenKind::kInner || follower == TokenKind::kOn ||
            follower == TokenKind::kWhere || follower == TokenKind::kGroup ||
            follower == TokenKind::kOrder) {
            return syntax_error("table aliases are not supported");
        }
        return std::nullopt;
    }

    [[nodiscard]] ParseResult keyword_error(
        std::string message,
        std::span<const SuggestionCandidate> candidates) const {
        CompileError error{
            CompileStage::kSyntax,
            peek().source,
            std::move(message)};
        if (peek().kind == TokenKind::kIdentifier) {
            const std::optional<std::string> candidate =
                best_suggestion(peek().lexeme, candidates);
            if (candidate.has_value()) {
                error.suggestion = suggestion_message(*candidate);
                error.fix_it = FixIt{peek().source, *candidate};
            }
        }
        return ParseResult{std::move(error)};
    }

    [[nodiscard]] ParseResult insertion_error(
        std::string message,
        std::string replacement) const {
        CompileError error{
            CompileStage::kSyntax,
            peek().source,
            std::move(message)};
        error.fix_it = FixIt{
            SourceRange{peek().source.begin, peek().source.begin},
            std::move(replacement)};
        return ParseResult{std::move(error)};
    }

    [[nodiscard]] ParseResult semicolon_error(std::string message) const {
        return check(TokenKind::kEnd)
            ? insertion_error(std::move(message), ";")
            : syntax_error(std::move(message));
    }

    [[nodiscard]] ParseResult right_parenthesis_error(std::string message) const {
        return check(TokenKind::kSemicolon) || check(TokenKind::kEnd) ||
               check(TokenKind::kFrom) || check(TokenKind::kWhere) ||
               check(TokenKind::kGroup) || check(TokenKind::kOrder)
            ? insertion_error(std::move(message), ")")
            : syntax_error(std::move(message));
    }

    [[nodiscard]] static bool starts_literal(TokenKind kind) noexcept {
        return kind == TokenKind::kIntegerLiteral || kind == TokenKind::kDoubleLiteral ||
               kind == TokenKind::kStringLiteral || kind == TokenKind::kTrue ||
               kind == TokenKind::kFalse || kind == TokenKind::kNull;
    }

    const std::vector<Token>& tokens_;
    Token fallback_end_;
    std::size_t index_{0};
    std::size_t expression_complexity_{0};
    std::optional<CompileError> expression_error_;
};

}  // namespace

ParseResult parse(const std::vector<Token>& tokens) {
    return Parser{tokens}.run();
}

}  // namespace tinydbms::compiler::internal
