#include "record_codec.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tinydbms::storage::internal {
namespace {

RecordCodecError make_error(RecordCodecErrorKind kind, std::string message) {
    return RecordCodecError{kind, std::move(message)};
}

template <typename T>
RecordCodecResult<T> failure(RecordCodecErrorKind kind, std::string message) {
    return RecordCodecResult<T>{std::nullopt, make_error(kind, std::move(message))};
}

template <typename T>
RecordCodecResult<T> success(T value) {
    return RecordCodecResult<T>{std::move(value), std::nullopt};
}

bool is_continuation(std::uint8_t byte) {
    return byte >= 0x80U && byte <= 0xBFU;
}

bool is_valid_utf8(std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const std::uint8_t first = bytes[index];
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1 >= text.size() || !is_continuation(bytes[index + 1])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index + 1];
            const std::uint8_t third = bytes[index + 2];
            const bool second_valid =
                first == 0xE0U ? (second >= 0xA0U && second <= 0xBFU)
                               : first == 0xEDU ? (second >= 0x80U && second <= 0x9FU)
                                                : is_continuation(second);
            if (!second_valid || !is_continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const std::uint8_t second = bytes[index + 1];
            const bool second_valid =
                first == 0xF0U ? (second >= 0x90U && second <= 0xBFU)
                               : first == 0xF4U ? (second >= 0x80U && second <= 0x8FU)
                                                : is_continuation(second);
            if (!second_valid || !is_continuation(bytes[index + 2]) ||
                !is_continuation(bytes[index + 3])) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

std::size_t fixed_width(Type type) {
    switch (type) {
        case Type::kInt:
            return 4;
        case Type::kVarchar:
            return 0;
    }
    return 0;
}

template <typename Unsigned>
void append_little_endian(std::vector<std::byte>& output, Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        output.push_back(
            std::byte{static_cast<std::uint8_t>((value >> (index * 8U)) & Unsigned{0xFF})});
    }
}

template <typename Unsigned>
bool read_little_endian(std::span<const std::byte> payload, std::size_t& offset,
                        Unsigned& value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    if (payload.size() - offset < sizeof(Unsigned)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        value |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(payload[offset + index]))
                 << (index * 8U);
    }
    offset += sizeof(Unsigned);
    return true;
}

struct MeasuredSize {
    std::size_t logical = 0;
    std::size_t encoded = 0;
};

std::size_t add_with_limit(std::size_t current, std::size_t amount, std::size_t limit) {
    return current > limit || amount > limit - current ? limit + 1 : current + amount;
}

RecordCodecResult<MeasuredSize> measure(const TableMeta& table,
                                        const std::vector<Value>& values) {
    if (values.size() != table.columns.size()) {
        return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                     "record column count does not match schema");
    }

    MeasuredSize size;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const Type type = table.columns[index].type;
        const Value& value = values[index];
        const bool type_matches = type == Type::kInt
            ? std::holds_alternative<std::int32_t>(value.data)
            : std::holds_alternative<std::string>(value.data);
        if (!type_matches) {
            return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                         "record Value type does not match schema");
        }

        if (type == Type::kVarchar) {
            const std::string& text = std::get<std::string>(value.data);
            if (!is_valid_utf8(text)) {
                return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                             "VARCHAR is not valid UTF-8");
            }
            if (text.size() > kMaxVarcharBytes) {
                return failure<MeasuredSize>(RecordCodecErrorKind::kValueTooLarge,
                                             "VARCHAR exceeds its byte limit");
            }
            size.logical = add_with_limit(size.logical, text.size(), kMaxRowLogicalBytes);
            size.encoded = add_with_limit(size.encoded, sizeof(std::uint32_t) + text.size(),
                                          kMaxRecordPayloadBytes);
        } else {
            const std::size_t width = fixed_width(type);
            size.logical = add_with_limit(size.logical, width, kMaxRowLogicalBytes);
            size.encoded = add_with_limit(size.encoded, width, kMaxRecordPayloadBytes);
        }
    }
    if (size.logical > kMaxRowLogicalBytes) {
        return failure<MeasuredSize>(RecordCodecErrorKind::kValueTooLarge,
                                     "logical row exceeds its byte limit");
    }
    if (size.encoded > kMaxRecordPayloadBytes) {
        return failure<MeasuredSize>(RecordCodecErrorKind::kValueTooLarge,
                                     "encoded record exceeds one-page payload limit");
    }
    return success(size);
}

}  // namespace

RecordCodecResult<std::size_t> RecordCodec::encoded_size(
    const TableMeta& table, const std::vector<Value>& values) {
    auto measured = measure(table, values);
    if (!measured.value.has_value()) {
        return failure<std::size_t>(measured.error->kind, measured.error->message);
    }
    return success(measured.value->encoded);
}

RecordCodecResult<std::vector<std::byte>> RecordCodec::encode(
    const TableMeta& table, const std::vector<Value>& values) {
    auto measured = measure(table, values);
    if (!measured.value.has_value()) {
        return failure<std::vector<std::byte>>(measured.error->kind, measured.error->message);
    }

    std::vector<std::byte> output;
    output.reserve(measured.value->encoded);
    for (std::size_t index = 0; index < values.size(); ++index) {
        const Type type = table.columns[index].type;
        const Value& value = values[index];
        switch (type) {
            case Type::kInt:
                append_little_endian(output,
                    std::bit_cast<std::uint32_t>(std::get<std::int32_t>(value.data)));
                break;
            case Type::kVarchar: {
                const std::string& text = std::get<std::string>(value.data);
                append_little_endian(output, static_cast<std::uint32_t>(text.size()));
                const auto* first = reinterpret_cast<const std::byte*>(text.data());
                output.insert(output.end(), first, first + text.size());
                break;
            }
        }
    }
    return success(std::move(output));
}

RecordCodecResult<std::vector<Value>> RecordCodec::decode(
    const TableMeta& table, std::span<const std::byte> payload) {
    if (payload.size() > kMaxRecordPayloadBytes) {
        return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                           "encoded record exceeds one-page payload limit");
    }

    std::vector<Value> values;
    values.reserve(table.columns.size());
    std::size_t offset = 0;
    std::size_t logical_size = 0;
    for (const ColumnMeta& column : table.columns) {
        switch (column.type) {
            case Type::kInt: {
                std::uint32_t bits = 0;
                if (!read_little_endian(payload, offset, bits)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "short INT32 payload");
                }
                values.push_back(Value{std::bit_cast<std::int32_t>(bits)});
                logical_size += 4;
                break;
            }
            case Type::kVarchar: {
                std::uint32_t byte_count = 0;
                if (!read_little_endian(payload, offset, byte_count)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "short VARCHAR length prefix");
                }
                if (byte_count > kMaxVarcharBytes || payload.size() - offset < byte_count) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "invalid VARCHAR byte length");
                }
                const auto* first = reinterpret_cast<const char*>(payload.data() + offset);
                std::string text(first, byte_count);
                if (!is_valid_utf8(text)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "VARCHAR payload is not valid UTF-8");
                }
                offset += byte_count;
                logical_size += byte_count;
                values.push_back(Value{std::move(text)});
                break;
            }
        }
        if (logical_size > kMaxRowLogicalBytes) {
            return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                               "decoded logical row exceeds its byte limit");
        }
    }
    if (offset != payload.size()) {
        return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                           "record payload has trailing bytes");
    }
    return success(std::move(values));
}

}  // namespace tinydbms::storage::internal
