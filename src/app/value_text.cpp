#include "value_text.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace tinydbms::app {

std::string double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return "<unsupported DOUBLE value>";
    }
    std::string result(buffer.data(), end);
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}

std::string value_text(const tinydbms::Value& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) {
                return "NULL";
            } else if constexpr (std::is_same_v<Item, std::int32_t>) {
                return std::to_string(item);
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
                return std::to_string(item);
            } else if constexpr (std::is_same_v<Item, double>) {
                return double_text(item);
            } else if constexpr (std::is_same_v<Item, bool>) {
                return item ? "TRUE" : "FALSE";
            } else {
                return item;
            }
        },
        value.data);
}

}  // namespace tinydbms::app
