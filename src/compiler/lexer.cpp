#include "lexer.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tinydbms::compiler::internal {
namespace {

using namespace std::string_view_literals;

constexpr bool is_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

constexpr bool is_ascii_letter(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

constexpr bool is_ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

constexpr bool is_identifier_continue(char value) {
    return is_ascii_letter(value) || is_ascii_digit(value) || value == '_';
}

constexpr char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

std::optional<TokenKind> keyword_kind(std::string_view value) {
    static constexpr std::array keywords{
        std::pair{"create"sv, TokenKind::kCreate},
        std::pair{"table"sv, TokenKind::kTable},
        std::pair{"insert"sv, TokenKind::kInsert},
        std::pair{"into"sv, TokenKind::kInto},
        std::pair{"values"sv, TokenKind::kValues},
        std::pair{"select"sv, TokenKind::kSelect},
        std::pair{"from"sv, TokenKind::kFrom},
        std::pair{"where"sv, TokenKind::kWhere},
        std::pair{"delete"sv, TokenKind::kDelete},
        std::pair{"and"sv, TokenKind::kAnd},
        std::pair{"or"sv, TokenKind::kOr},
        std::pair{"not"sv, TokenKind::kNot},
        std::pair{"int"sv, TokenKind::kInt},
        std::pair{"varchar"sv, TokenKind::kVarchar},
    };

    for (const auto& [keyword, kind] : keywords) {
        if (value == keyword) {
            return kind;
        }
    }
    return std::nullopt;
}

constexpr bool is_continuation(std::uint8_t byte) {
    return byte >= 0x80U && byte <= 0xBFU;
}

bool is_valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1 >= value.size() ||
                !is_continuation(static_cast<std::uint8_t>(value[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2 >= value.size()) {
                return false;
            }
            const auto second = static_cast<std::uint8_t>(value[index + 1]);
            const auto third = static_cast<std::uint8_t>(value[index + 2]);
            const bool second_valid =
                (first == 0xE0U && second >= 0xA0U && second <= 0xBFU) ||
                (first == 0xEDU && second >= 0x80U && second <= 0x9FU) ||
                (((first >= 0xE1U && first <= 0xECU) ||
                  (first >= 0xEEU && first <= 0xEFU)) &&
                 is_continuation(second));
            if (!second_valid || !is_continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3 >= value.size()) {
                return false;
            }
            const auto second = static_cast<std::uint8_t>(value[index + 1]);
            const auto third = static_cast<std::uint8_t>(value[index + 2]);
            const auto fourth = static_cast<std::uint8_t>(value[index + 3]);
            const bool second_valid =
                (first == 0xF0U && second >= 0x90U && second <= 0xBFU) ||
                (first >= 0xF1U && first <= 0xF3U && is_continuation(second)) ||
                (first == 0xF4U && second >= 0x80U && second <= 0x8FU);
            if (!second_valid || !is_continuation(third) || !is_continuation(fourth)) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }

    return true;
}

class Scanner {
public:
    explicit Scanner(std::string_view sql) : sql_{sql} {}

    LexResult run() {
        while (!at_end()) {
            if (is_whitespace(current())) {
                advance();
                continue;
            }

            if (current() == '-' && peek() == '-') {
                skip_line_comment();
                continue;
            }

            if (current() == '/' && peek() == '*') {
                const SourceLocation comment_start = location();
                if (!skip_block_comment()) {
                    return error(comment_start, "unterminated block comment");
                }
                continue;
            }

            if (is_ascii_letter(current())) {
                scan_word();
                if (tokens_.back().lexeme.size() > 64) {
                    const SourceLocation start = tokens_.back().location;
                    tokens_.pop_back();
                    return error(start, "identifier exceeds 64 bytes");
                }
                continue;
            }

            if (is_ascii_digit(current())) {
                const auto integer_error = scan_integer();
                if (integer_error.has_value()) {
                    return LexResult{std::move(*integer_error)};
                }
                continue;
            }

            if (current() == '\'') {
                const auto string_error = scan_string();
                if (string_error.has_value()) {
                    return LexResult{std::move(*string_error)};
                }
                continue;
            }

            const SourceLocation start = location();
            const char value = current();
            switch (value) {
                case '=':
                    add_single(TokenKind::kEq);
                    break;
                case '!':
                    if (peek() != '=') {
                        return error(start, "invalid character '!'");
                    }
                    add_double(TokenKind::kNe);
                    break;
                case '<':
                    if (peek() == '=') {
                        add_double(TokenKind::kLe);
                    } else {
                        add_single(TokenKind::kLt);
                    }
                    break;
                case '>':
                    if (peek() == '=') {
                        add_double(TokenKind::kGe);
                    } else {
                        add_single(TokenKind::kGt);
                    }
                    break;
                case '(':
                    add_single(TokenKind::kLeftParen);
                    break;
                case ')':
                    add_single(TokenKind::kRightParen);
                    break;
                case ',':
                    add_single(TokenKind::kComma);
                    break;
                case ';':
                    add_single(TokenKind::kSemicolon);
                    break;
                case '*':
                    add_single(TokenKind::kStar);
                    break;
                case '_':
                    return error(start, "invalid identifier start");
                default:
                    return error(start, "invalid character");
            }
        }

        tokens_.push_back(Token{TokenKind::kEnd, "", location()});
        return LexResult{std::move(tokens_)};
    }

private:
    [[nodiscard]] bool at_end() const {
        return index_ >= sql_.size();
    }

    [[nodiscard]] char current() const {
        return sql_[index_];
    }

    [[nodiscard]] char peek() const {
        return index_ + 1 < sql_.size() ? sql_[index_ + 1] : '\0';
    }

    [[nodiscard]] SourceLocation location() const {
        return SourceLocation{line_, column_};
    }

    char advance() {
        const char value = sql_[index_];
        ++index_;
        if (value == '\n') {
            ++line_;
            column_ = 1;
        } else if (value != '\r') {
            ++column_;
        }
        return value;
    }

    LexResult error(SourceLocation error_location, std::string message) const {
        return LexResult{CompileError{
            CompileErrorKind::kLex,
            error_location,
            std::move(message)
        }};
    }

    void add_single(TokenKind kind) {
        const SourceLocation start = location();
        const char value = advance();
        tokens_.push_back(Token{kind, std::string(1, value), start});
    }

    void add_double(TokenKind kind) {
        const SourceLocation start = location();
        std::string lexeme;
        lexeme.push_back(advance());
        lexeme.push_back(advance());
        tokens_.push_back(Token{kind, std::move(lexeme), start});
    }

    void skip_line_comment() {
        advance();
        advance();
        while (!at_end() && current() != '\n') {
            advance();
        }
    }

    bool skip_block_comment() {
        advance();
        advance();
        while (!at_end()) {
            if (current() == '*' && peek() == '/') {
                advance();
                advance();
                return true;
            }
            advance();
        }
        return false;
    }

    void scan_word() {
        const SourceLocation start = location();
        std::string value;
        while (!at_end() && is_identifier_continue(current())) {
            value.push_back(ascii_lower(advance()));
        }

        const auto keyword = keyword_kind(value);
        tokens_.push_back(Token{keyword.value_or(TokenKind::kIdentifier), std::move(value), start});
    }

    std::optional<CompileError> scan_integer() {
        const SourceLocation start = location();
        const std::size_t begin = index_;
        constexpr std::uint32_t maximum =
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        std::uint32_t value = 0;
        bool overflow = false;

        while (!at_end() && is_ascii_digit(current())) {
            const std::uint32_t digit = static_cast<std::uint32_t>(current() - '0');
            if (!overflow && value > (maximum - digit) / 10U) {
                overflow = true;
            } else if (!overflow) {
                value = value * 10U + digit;
            }
            advance();
        }

        if (overflow) {
            return CompileError{
                CompileErrorKind::kLex,
                start,
                "integer literal exceeds int32 range"
            };
        }

        tokens_.push_back(Token{
            TokenKind::kIntegerLiteral,
            std::string{sql_.substr(begin, index_ - begin)},
            start
        });
        return std::nullopt;
    }

    std::optional<CompileError> scan_string() {
        const SourceLocation start = location();
        advance();
        std::string value;

        while (!at_end()) {
            if (current() != '\'') {
                value.push_back(advance());
                continue;
            }

            if (peek() == '\'') {
                advance();
                advance();
                value.push_back('\'');
                continue;
            }

            advance();
            if (!is_valid_utf8(value)) {
                return CompileError{
                    CompileErrorKind::kLex,
                    start,
                    "string literal contains invalid UTF-8"
                };
            }
            if (value.size() > kMaxVarcharBytes) {
                return CompileError{
                    CompileErrorKind::kLex,
                    start,
                    "string literal exceeds maximum VARCHAR length"
                };
            }
            tokens_.push_back(Token{TokenKind::kStringLiteral, std::move(value), start});
            return std::nullopt;
        }

        return CompileError{
            CompileErrorKind::kLex,
            start,
            "unterminated string literal"
        };
    }

    std::string_view sql_;
    std::size_t index_{0};
    int line_{1};
    int column_{1};
    std::vector<Token> tokens_;
};

}  // namespace

LexResult tokenize(std::string_view sql) {
    return Scanner{sql}.run();
}

}  // namespace tinydbms::compiler::internal
