#ifndef TINYDBMS_TESTING_JSON_CHECK_HPP
#define TINYDBMS_TESTING_JSON_CHECK_HPP

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydbms::testing {

// 极简 JSON 解析器，只用于测试断言：不引入第三方依赖，覆盖 RFC 8259 的完整语法，
// 用来确认 CLI 的每一行输出都能被标准解析器接受，并取回字段做类型断言。
struct JsonNode {
    enum class Kind {
        kNull,
        kBoolean,
        kNumber,
        kString,
        kArray,
        kObject
    };

    Kind kind = Kind::kNull;
    bool boolean = false;
    bool number_is_integer = false;
    std::int64_t integer = 0;
    double number = 0.0;
    std::string text;  // 字符串解码结果
    std::vector<JsonNode> items;
    std::vector<std::pair<std::string, JsonNode>> members;
};

namespace json_detail {

constexpr std::size_t kMaxDepth = 64;

inline bool is_digit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

inline bool parse_hex4(
    const std::string_view text,
    const std::size_t index,
    std::uint32_t& value) noexcept {
    if (index + 4U > text.size()) {
        return false;
    }
    std::uint32_t result = 0;
    for (std::size_t offset = 0; offset < 4U; ++offset) {
        const char character = text[index + offset];
        std::uint32_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint32_t>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<std::uint32_t>(character - 'A' + 10);
        } else {
            return false;
        }
        result = (result << 4U) | digit;
    }
    value = result;
    return true;
}

inline void append_utf8(std::string& output, const std::uint32_t code_point) {
    if (code_point < 0x80U) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

struct Parser {
    std::string_view text;
    std::size_t index = 0;
    std::size_t depth = 0;
    std::size_t error_offset = 0;

    bool fail() {
        error_offset = index;
        return false;
    }

    void skip_whitespace() {
        while (index < text.size()) {
            const char character = text[index];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
                return;
            }
            ++index;
        }
    }

    bool parse_value(JsonNode& node) {
        if (depth >= kMaxDepth) {
            return fail();
        }
        skip_whitespace();
        if (index >= text.size()) {
            return fail();
        }
        switch (text[index]) {
        case '{':
            return parse_object(node);
        case '[':
            return parse_array(node);
        case '"':
            node.kind = JsonNode::Kind::kString;
            return parse_string(node.text);
        case 't':
            node.kind = JsonNode::Kind::kBoolean;
            node.boolean = true;
            return parse_literal("true");
        case 'f':
            node.kind = JsonNode::Kind::kBoolean;
            node.boolean = false;
            return parse_literal("false");
        case 'n':
            node.kind = JsonNode::Kind::kNull;
            return parse_literal("null");
        default:
            return parse_number(node);
        }
    }

    bool parse_literal(const std::string_view literal) {
        if (text.compare(index, literal.size(), literal) != 0) {
            return fail();
        }
        index += literal.size();
        return true;
    }

    bool parse_object(JsonNode& node) {
        ++depth;
        node.kind = JsonNode::Kind::kObject;
        ++index;  // '{'
        skip_whitespace();
        if (index < text.size() && text[index] == '}') {
            ++index;
            --depth;
            return true;
        }
        for (;;) {
            skip_whitespace();
            std::string key;
            if (!parse_string(key)) {
                return fail();
            }
            skip_whitespace();
            if (index >= text.size() || text[index] != ':') {
                return fail();
            }
            ++index;
            JsonNode value;
            if (!parse_value(value)) {
                return false;
            }
            node.members.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (index >= text.size()) {
                return fail();
            }
            if (text[index] == ',') {
                ++index;
                continue;
            }
            if (text[index] == '}') {
                ++index;
                --depth;
                return true;
            }
            return fail();
        }
    }

    bool parse_array(JsonNode& node) {
        ++depth;
        node.kind = JsonNode::Kind::kArray;
        ++index;  // '['
        skip_whitespace();
        if (index < text.size() && text[index] == ']') {
            ++index;
            --depth;
            return true;
        }
        for (;;) {
            JsonNode value;
            if (!parse_value(value)) {
                return false;
            }
            node.items.push_back(std::move(value));
            skip_whitespace();
            if (index >= text.size()) {
                return fail();
            }
            if (text[index] == ',') {
                ++index;
                continue;
            }
            if (text[index] == ']') {
                ++index;
                --depth;
                return true;
            }
            return fail();
        }
    }

    bool parse_string(std::string& output) {
        if (index >= text.size() || text[index] != '"') {
            return fail();
        }
        ++index;
        output.clear();
        while (index < text.size()) {
            const char character = text[index];
            if (character == '"') {
                ++index;
                return true;
            }
            if (character != '\\') {
                if (static_cast<unsigned char>(character) < 0x20U) {
                    return fail();
                }
                output.push_back(character);
                ++index;
                continue;
            }
            ++index;
            if (index >= text.size()) {
                return fail();
            }
            const char escape = text[index];
            ++index;
            switch (escape) {
            case '"':
                output.push_back('"');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '/':
                output.push_back('/');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!parse_hex4(text, index, code_point)) {
                    return fail();
                }
                index += 4U;
                if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                    if (index + 2U > text.size() || text[index] != '\\' ||
                        text[index + 1U] != 'u') {
                        return fail();
                    }
                    index += 2U;
                    std::uint32_t low = 0;
                    if (!parse_hex4(text, index, low)) {
                        return fail();
                    }
                    index += 4U;
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        return fail();
                    }
                    code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    return fail();
                }
                append_utf8(output, code_point);
                break;
            }
            default:
                return fail();
            }
        }
        return fail();
    }

    bool parse_number(JsonNode& node) {
        const std::size_t start = index;
        if (index < text.size() && text[index] == '-') {
            ++index;
        }
        if (index >= text.size()) {
            return fail();
        }
        if (text[index] == '0') {
            ++index;
        } else if (text[index] >= '1' && text[index] <= '9') {
            while (index < text.size() && is_digit(text[index])) {
                ++index;
            }
        } else {
            return fail();
        }

        bool is_integer = true;
        if (index < text.size() && text[index] == '.') {
            is_integer = false;
            ++index;
            if (index >= text.size() || !is_digit(text[index])) {
                return fail();
            }
            while (index < text.size() && is_digit(text[index])) {
                ++index;
            }
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            is_integer = false;
            ++index;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                ++index;
            }
            if (index >= text.size() || !is_digit(text[index])) {
                return fail();
            }
            while (index < text.size() && is_digit(text[index])) {
                ++index;
            }
        }

        const std::string_view token = text.substr(start, index - start);
        node.kind = JsonNode::Kind::kNumber;
        node.number_is_integer = false;
        if (is_integer) {
            std::int64_t value = 0;
            const auto [end, error] =
                std::from_chars(token.data(), token.data() + token.size(), value);
            if (error == std::errc{} && end == token.data() + token.size()) {
                node.integer = value;
                node.number_is_integer = true;
                node.number = static_cast<double>(value);
                return true;
            }
        }
        double value = 0.0;
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size()) {
            return fail();
        }
        node.number = value;
        return true;
    }
};

}  // namespace json_detail

// 解析完整 JSON 文本（允许前后空白）；失败时 error_offset 指向出错位置。
inline std::optional<JsonNode> json_parse(
    const std::string_view text,
    std::size_t& error_offset) {
    json_detail::Parser parser{text, 0, 0, 0};
    JsonNode node;
    parser.skip_whitespace();
    if (!parser.parse_value(node)) {
        error_offset = parser.error_offset;
        return std::nullopt;
    }
    parser.skip_whitespace();
    if (parser.index != text.size()) {
        error_offset = parser.index;
        return std::nullopt;
    }
    error_offset = 0;
    return node;
}

// 按行拆分 NDJSON 输出：忽略空行，保留每行的原始文本。
inline std::vector<std::string_view> json_lines(const std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        const std::string_view line = text.substr(start, stop - start);
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return lines;
}

inline const JsonNode* json_member(const JsonNode& node, const std::string_view key) {
    if (node.kind != JsonNode::Kind::kObject) {
        return nullptr;
    }
    for (const auto& member : node.members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

inline std::optional<std::string> json_string(const JsonNode& node) {
    if (node.kind != JsonNode::Kind::kString) {
        return std::nullopt;
    }
    return node.text;
}

inline std::optional<std::int64_t> json_integer(const JsonNode& node) {
    if (node.kind != JsonNode::Kind::kNumber || !node.number_is_integer) {
        return std::nullopt;
    }
    return node.integer;
}

}  // namespace tinydbms::testing

#endif  // TINYDBMS_TESTING_JSON_CHECK_HPP
