#include "parser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace tinydbms::compiler::internal {
namespace {

constexpr std::size_t kMaxExpressionComplexity = 256;

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

        return syntax_error("expected CREATE, INSERT, SELECT, or DELETE");
    }

private:
    ParseResult parse_create_table() {
        if (!match(TokenKind::kTable)) {
            return syntax_error("expected TABLE after CREATE");
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier");
        }

        std::string table_name = peek().lexeme;
        const SourceLocation table_location = peek().location;
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
            const SourceLocation column_location = peek().location;
            advance();

            Type column_type;
            if (match(TokenKind::kInt)) {
                column_type = Type::kInt;
            } else if (match(TokenKind::kVarchar)) {
                column_type = Type::kVarchar;
            } else {
                return syntax_error("expected INT or VARCHAR after column identifier");
            }

            columns.push_back(AstColumnDef{
                std::move(column_name),
                column_type,
                column_location
            });

            if (!match(TokenKind::kComma)) {
                break;
            }
        }

        if (!match(TokenKind::kRightParen)) {
            return syntax_error("expected ')' after column definitions");
        }
        if (!match(TokenKind::kSemicolon)) {
            return syntax_error("expected ';' after CREATE TABLE statement");
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
            std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst>{std::move(create)}
        }};
    }

    ParseResult parse_insert() {
        if (!match(TokenKind::kInto)) {
            return syntax_error("expected INTO after INSERT");
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier");
        }

        std::string table_name = peek().lexeme;
        const SourceLocation table_location = peek().location;
        advance();

        std::vector<AstInsertColumn> columns;
        if (match(TokenKind::kLeftParen)) {
            while (true) {
                if (!check(TokenKind::kIdentifier)) {
                    return syntax_error("expected column identifier");
                }

                columns.push_back(AstInsertColumn{peek().lexeme, peek().location});
                advance();
                if (!match(TokenKind::kComma)) {
                    break;
                }
            }

            if (!match(TokenKind::kRightParen)) {
                return syntax_error("expected ')' after column list");
            }
        }

        if (!match(TokenKind::kValues)) {
            return syntax_error("expected VALUES after table or column list");
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
                    std::int32_t value = 0;
                    const char* const begin = literal.lexeme.data();
                    const char* const end = begin + literal.lexeme.size();
                    const auto [parsed_end, error] = std::from_chars(begin, end, value);
                    if (error != std::errc{} || parsed_end != end) {
                        return syntax_error("invalid integer literal");
                    }
                    row.push_back(AstLiteral{Value{value}, literal.location});
                    advance();
                } else if (literal.kind == TokenKind::kStringLiteral) {
                    row.push_back(AstLiteral{Value{literal.lexeme}, literal.location});
                    advance();
                } else {
                    return syntax_error("expected integer or string literal");
                }

                if (!match(TokenKind::kComma)) {
                    break;
                }
            }

            if (!match(TokenKind::kRightParen)) {
                return syntax_error("expected ')' after VALUES row");
            }
            rows.push_back(std::move(row));

            if (!match(TokenKind::kComma)) {
                break;
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            return syntax_error("expected ';' after INSERT statement");
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
            std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst>{std::move(insert)}
        }};
    }

    ParseResult parse_select() {
        bool select_all = false;
        std::vector<AstSelectColumn> columns;
        if (match(TokenKind::kStar)) {
            select_all = true;
        } else {
            if (!check(TokenKind::kIdentifier)) {
                return syntax_error("expected SELECT column or '*'");
            }

            while (true) {
                columns.push_back(AstSelectColumn{peek().lexeme, peek().location});
                advance();
                if (!match(TokenKind::kComma)) {
                    break;
                }
                if (!check(TokenKind::kIdentifier)) {
                    return syntax_error("expected column identifier after ','");
                }
            }
        }

        if (!match(TokenKind::kFrom)) {
            return syntax_error("expected FROM after SELECT list");
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier after FROM");
        }

        std::string table_name = peek().lexeme;
        const SourceLocation table_location = peek().location;
        advance();

        AstExprPtr predicate;
        if (match(TokenKind::kWhere)) {
            predicate = parse_expression();
            if (predicate == nullptr) {
                return take_expression_error();
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            return syntax_error("expected ';' after SELECT statement");
        }
        if (!check(TokenKind::kEnd)) {
            return syntax_error("expected end of statement after ';'");
        }

        SelectAst select{
            std::move(table_name),
            table_location,
            select_all,
            std::move(columns),
            std::move(predicate)
        };
        return ParseResult{StatementAst{
            std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst>{std::move(select)}
        }};
    }

    ParseResult parse_delete() {
        if (!match(TokenKind::kFrom)) {
            return syntax_error("expected FROM after DELETE");
        }
        if (!check(TokenKind::kIdentifier)) {
            return syntax_error("expected table identifier after FROM");
        }

        std::string table_name = peek().lexeme;
        const SourceLocation table_location = peek().location;
        advance();

        AstExprPtr predicate;
        if (match(TokenKind::kWhere)) {
            predicate = parse_expression();
            if (predicate == nullptr) {
                return take_expression_error();
            }
        }

        if (!match(TokenKind::kSemicolon)) {
            return syntax_error("expected ';' after DELETE statement");
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
            std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst>{std::move(deletion)}
        }};
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
            const SourceLocation location = peek().location;
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
            const SourceLocation location = peek().location;
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

        const SourceLocation location = peek().location;
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

        const auto op = comparison_operator(peek().kind);
        if (!op.has_value()) {
            return lhs;
        }
        if (!consume_expression_budget()) {
            return nullptr;
        }

        const SourceLocation location = peek().location;
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
            auto expression = std::make_unique<AstExpr>(AstIdentifierExpr{
                token.lexeme,
                token.location
            });
            advance();
            return expression;
        }

        if (token.kind == TokenKind::kIntegerLiteral) {
            if (!consume_expression_budget()) {
                return nullptr;
            }
            std::int32_t value = 0;
            const char* const begin = token.lexeme.data();
            const char* const end = begin + token.lexeme.size();
            const auto [parsed_end, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed_end != end) {
                return expression_error("invalid integer literal in expression");
            }
            auto expression = std::make_unique<AstExpr>(AstLiteralExpr{
                Value{value},
                token.location
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
                token.location
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
                CompileErrorKind::kSyntax,
                peek().location,
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

    static SourceLocation fallback_location(const std::vector<Token>& tokens) {
        if (tokens.empty()) {
            return SourceLocation{1, 1};
        }
        return tokens.back().location;
    }

    [[nodiscard]] const Token& peek() const {
        return index_ < tokens_.size() ? tokens_[index_] : fallback_end_;
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
            CompileErrorKind::kSyntax,
            peek().location,
            std::move(message)
        }};
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
