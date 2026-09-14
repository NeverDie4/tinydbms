#include "record_codec.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tinydbms::storage::internal {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

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
        case Type::kBigInt:
            return 8;
        case Type::kDouble:
            return 8;
        case Type::kBoolean:
            return 1;
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
    if (offset > payload.size() || payload.size() - offset < sizeof(Unsigned)) {
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

std::size_t null_bitmap_size(std::size_t column_count) {
    return column_count / 8U + (column_count % 8U == 0U ? 0U : 1U);
}

bool type_supported(RowFormat format, Type type) {
    if (type == Type::kInt || type == Type::kVarchar) {
        return true;
    }
    return format == RowFormat::kV2 &&
        (type == Type::kBigInt || type == Type::kDouble || type == Type::kBoolean);
}

RecordCodecResult<MeasuredSize> measure(RowFormat format, const TableMeta& table,
                                        const std::vector<Value>& values) {
    if (values.size() != table.columns.size()) {
        return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                     "record column count does not match schema");
    }

    MeasuredSize size;
    if (format == RowFormat::kV2) {
        size.encoded = null_bitmap_size(table.columns.size());
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        const ColumnMeta& column = table.columns[index];
        const Type type = column.type;
        const Value& value = values[index];
        if ((format == RowFormat::kV1 && column.nullable) ||
            !type_supported(format, type)) {
            return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                         format == RowFormat::kV1
                                             ? "record codec only supports SQL v1 schemas"
                                             : "record codec does not support this SQL v2 schema yet");
        }
        if (std::holds_alternative<std::monostate>(value.data)) {
            if (!column.nullable) {
                return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                             "NULL value for non-nullable column");
            }
            continue;
        }
        const bool type_matches =
            (type == Type::kInt && std::holds_alternative<std::int32_t>(value.data)) ||
            (type == Type::kBigInt && std::holds_alternative<std::int64_t>(value.data)) ||
            (type == Type::kDouble && std::holds_alternative<double>(value.data)) ||
            (type == Type::kBoolean && std::holds_alternative<bool>(value.data)) ||
            (type == Type::kVarchar && std::holds_alternative<std::string>(value.data));
        if (!type_matches) {
            return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                         "record Value type does not match schema");
        }
        if (type == Type::kDouble && !std::isfinite(std::get<double>(value.data))) {
            return failure<MeasuredSize>(RecordCodecErrorKind::kInvalidArgument,
                                         "DOUBLE value must be finite");
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
    RowFormat format, const TableMeta& table, const std::vector<Value>& values) {
    auto measured = measure(format, table, values);
    if (!measured.value.has_value()) {
        return failure<std::size_t>(measured.error->kind, measured.error->message);
    }
    return success(measured.value->encoded);
}

RecordCodecResult<std::size_t> RecordCodec::encoded_size(
    const TableMeta& table, const std::vector<Value>& values) {
    return encoded_size(RowFormat::kV1, table, values);
}

RecordCodecResult<std::vector<std::byte>> RecordCodec::encode(
    RowFormat format, const TableMeta& table, const std::vector<Value>& values) {
    auto measured = measure(format, table, values);
    if (!measured.value.has_value()) {
        return failure<std::vector<std::byte>>(measured.error->kind, measured.error->message);
    }

    std::vector<std::byte> output;
    output.reserve(measured.value->encoded);
    if (format == RowFormat::kV2) {
        output.resize(null_bitmap_size(table.columns.size()), std::byte{0});
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        const Type type = table.columns[index].type;
        const Value& value = values[index];
        if (std::holds_alternative<std::monostate>(value.data)) {
            const std::size_t bitmap_index = index / 8U;
            const std::uint8_t mask = static_cast<std::uint8_t>(1U << (index % 8U));
            output[bitmap_index] |= std::byte{mask};
            continue;
        }
        switch (type) {
            case Type::kInt:
                append_little_endian(output,
                    std::bit_cast<std::uint32_t>(std::get<std::int32_t>(value.data)));
                break;
            case Type::kBigInt:
                append_little_endian(output,
                    std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value.data)));
                break;
            case Type::kDouble:
                append_little_endian(output,
                    std::bit_cast<std::uint64_t>(std::get<double>(value.data)));
                break;
            case Type::kBoolean:
                output.push_back(std::get<bool>(value.data) ? std::byte{1} : std::byte{0});
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

RecordCodecResult<std::vector<std::byte>> RecordCodec::encode(
    const TableMeta& table, const std::vector<Value>& values) {
    return encode(RowFormat::kV1, table, values);
}

RecordCodecResult<std::vector<Value>> RecordCodec::decode(
    RowFormat format, const TableMeta& table, std::span<const std::byte> payload) {
    for (const ColumnMeta& column : table.columns) {
        if ((format == RowFormat::kV1 && column.nullable) ||
            !type_supported(format, column.type)) {
            return failure<std::vector<Value>>(
                RecordCodecErrorKind::kInvalidArgument,
                format == RowFormat::kV1
                    ? "record codec only supports SQL v1 schemas"
                    : "record codec does not support this SQL v2 schema yet");
        }
    }
    if (payload.size() > kMaxRecordPayloadBytes) {
        return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                           "encoded record exceeds one-page payload limit");
    }

    std::vector<Value> values;
    values.reserve(table.columns.size());
    std::size_t offset = 0;
    if (format == RowFormat::kV2) {
        const std::size_t bitmap_size = null_bitmap_size(table.columns.size());
        if (payload.size() < bitmap_size) {
            return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                               "short null bitmap");
        }
        for (std::size_t index = 0; index < table.columns.size(); ++index) {
            const std::uint8_t bitmap = std::to_integer<std::uint8_t>(payload[index / 8U]);
            if ((bitmap & static_cast<std::uint8_t>(1U << (index % 8U))) != 0U &&
                !table.columns[index].nullable) {
                return failure<std::vector<Value>>(
                    RecordCodecErrorKind::kCorrupt,
                    "NULL bit is set for a non-nullable column");
            }
        }
        if (!table.columns.empty() && table.columns.size() % 8U != 0U) {
            const std::uint8_t bitmap =
                std::to_integer<std::uint8_t>(payload[bitmap_size - 1U]);
            const std::uint8_t used_mask = static_cast<std::uint8_t>(
                (1U << (table.columns.size() % 8U)) - 1U);
            if ((bitmap & static_cast<std::uint8_t>(~used_mask)) != 0U) {
                return failure<std::vector<Value>>(
                    RecordCodecErrorKind::kCorrupt,
                    "unused null bitmap bits are not zero");
            }
        }
        offset = bitmap_size;
    }
    std::size_t logical_size = 0;
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        const ColumnMeta& column = table.columns[index];
        if (format == RowFormat::kV2) {
            const std::uint8_t bitmap = std::to_integer<std::uint8_t>(payload[index / 8U]);
            if ((bitmap & static_cast<std::uint8_t>(1U << (index % 8U))) != 0U) {
                values.push_back(Value{std::monostate{}});
                continue;
            }
        }
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
            case Type::kBigInt: {
                std::uint64_t bits = 0;
                if (!read_little_endian(payload, offset, bits)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "short INT64 payload");
                }
                values.push_back(Value{std::bit_cast<std::int64_t>(bits)});
                logical_size += 8;
                break;
            }
            case Type::kDouble: {
                std::uint64_t bits = 0;
                if (!read_little_endian(payload, offset, bits)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "short DOUBLE payload");
                }
                const double value = std::bit_cast<double>(bits);
                if (!std::isfinite(value)) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "non-finite DOUBLE payload");
                }
                values.push_back(Value{value});
                logical_size += 8;
                break;
            }
            case Type::kBoolean: {
                if (offset >= payload.size()) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "short BOOLEAN payload");
                }
                const std::uint8_t encoded =
                    std::to_integer<std::uint8_t>(payload[offset]);
                ++offset;
                if (encoded > 1U) {
                    return failure<std::vector<Value>>(RecordCodecErrorKind::kCorrupt,
                                                       "invalid BOOLEAN payload");
                }
                values.push_back(Value{encoded == 1U});
                logical_size += 1;
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

RecordCodecResult<std::vector<Value>> RecordCodec::decode(
    const TableMeta& table, std::span<const std::byte> payload) {
    return decode(RowFormat::kV1, table, payload);
}

}  // namespace tinydbms::storage::internal
