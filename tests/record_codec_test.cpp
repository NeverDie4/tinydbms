#include "record_codec.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::storage::internal::RecordCodec;
using tinydbms::storage::internal::RecordCodecErrorKind;
using tinydbms::storage::internal::RecordCodecResult;
using tinydbms::storage::internal::kMaxRecordPayloadBytes;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "expectation failed: " << message << '\n';
    }
    return condition;
}

template <typename T>
bool has_error(const RecordCodecResult<T>& result, RecordCodecErrorKind kind) {
    return !result.value.has_value() && result.error.has_value() && result.error->kind == kind;
}

TableMeta meta(std::vector<Type> types) {
    TableMeta result;
    result.table_name = "codec";
    for (std::size_t index = 0; index < types.size(); ++index) {
        result.columns.push_back(ColumnMeta{"c" + std::to_string(index), types[index]});
    }
    return result;
}

bool same_value_bits(const Value& left, const Value& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    return std::visit(
        [&right](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const T* other = std::get_if<T>(&right.data);
            if (other == nullptr) {
                return false;
            }
            if constexpr (std::is_same_v<T, float>) {
                return std::bit_cast<std::uint32_t>(value) ==
                       std::bit_cast<std::uint32_t>(*other);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::bit_cast<std::uint64_t>(value) ==
                       std::bit_cast<std::uint64_t>(*other);
            } else {
                return value == *other;
            }
        },
        left.data);
}

bool round_trip(const TableMeta& table, const std::vector<Value>& values) {
    const auto encoded = RecordCodec::encode(table, values);
    if (!expect(encoded.value.has_value() && !encoded.error.has_value(),
                "valid record must encode")) {
        return false;
    }
    const auto decoded = RecordCodec::decode(table, *encoded.value);
    if (!expect(decoded.value.has_value() && decoded.value->size() == values.size(),
                "encoded record must decode")) {
        return false;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!expect(same_value_bits(values[index], (*decoded.value)[index]),
                    "decoded Value must preserve the original bit pattern")) {
            return false;
        }
    }
    return true;
}

bool test_integer_round_trips_and_little_endian() {
    const auto int32_meta = meta({Type::kInt32, Type::kInt32, Type::kInt32});
    bool passed = round_trip(int32_meta,
                             {Value{std::numeric_limits<std::int32_t>::min()},
                              Value{std::int32_t{0}},
                              Value{std::numeric_limits<std::int32_t>::max()}});
    const auto encoded32 = RecordCodec::encode(meta({Type::kInt32}),
                                                {Value{std::int32_t{0x01020304}}});
    passed = expect(encoded32.value.has_value() && encoded32.value->size() == 4 &&
                        (*encoded32.value)[0] == std::byte{0x04} &&
                        (*encoded32.value)[1] == std::byte{0x03} &&
                        (*encoded32.value)[2] == std::byte{0x02} &&
                        (*encoded32.value)[3] == std::byte{0x01},
                    "INT32 must use little-endian encoding") && passed;

    const auto int64_meta = meta({Type::kInt64, Type::kInt64, Type::kInt64});
    passed = round_trip(int64_meta,
                        {Value{std::numeric_limits<std::int64_t>::min()},
                         Value{std::int64_t{0}},
                         Value{std::numeric_limits<std::int64_t>::max()}}) && passed;
    return passed;
}

bool test_float_bool_and_varchar_round_trips() {
    bool passed = round_trip(meta({Type::kFloat, Type::kFloat, Type::kFloat}),
                             {Value{1.25F}, Value{-19.5F}, Value{-0.0F}});
    passed = round_trip(meta({Type::kDouble, Type::kDouble, Type::kDouble}),
                        {Value{1.25}, Value{-19.5}, Value{-0.0}}) && passed;
    passed = round_trip(meta({Type::kBool, Type::kBool}),
                        {Value{false}, Value{true}}) && passed;
    passed = round_trip(meta({Type::kVarchar, Type::kVarchar, Type::kVarchar,
                              Type::kVarchar}),
                        {Value{std::string{}}, Value{std::string{"ascii"}},
                         Value{std::string{"中文"}}, Value{std::string{"🙂"}}}) && passed;
    passed = round_trip(meta({Type::kVarchar}),
                        std::vector<Value>{Value{std::string(1024, 'x')}}) && passed;
    return passed;
}

bool test_mixed_record_and_size_boundaries() {
    const auto table = meta({Type::kInt32, Type::kInt64, Type::kFloat, Type::kDouble,
                             Type::kBool, Type::kVarchar});
    bool passed = round_trip(table, {Value{std::int32_t{-7}}, Value{std::int64_t{9}},
                                     Value{3.5F}, Value{-8.25}, Value{true},
                                     Value{std::string{"混合🙂"}}});

    const auto exact = meta({Type::kVarchar, Type::kVarchar, Type::kVarchar, Type::kVarchar});
    const std::vector<Value> exact_values(4, Value{std::string(1010, 'a')});
    const auto exact_size = RecordCodec::encoded_size(exact, exact_values);
    passed = expect(exact_size.value == std::optional<std::size_t>{kMaxRecordPayloadBytes},
                    "encoded payload of exactly 4056 bytes must be accepted") && passed;
    passed = round_trip(exact, exact_values) && passed;
    return passed;
}

bool test_invalid_encode() {
    bool passed = expect(has_error(RecordCodec::encode(meta({Type::kInt32}), {}),
                                   RecordCodecErrorKind::kInvalidArgument),
                         "column-count mismatch must be invalid");
    passed = expect(has_error(RecordCodec::encode(meta({Type::kInt32}),
                                                   {Value{std::int64_t{1}}}),
                              RecordCodecErrorKind::kInvalidArgument),
                    "Value/Type mismatch must be invalid") && passed;
    passed = expect(has_error(RecordCodec::encode(meta({Type::kVarchar}),
                                                   {Value{std::string{"\xC0\xAF", 2}}}),
                              RecordCodecErrorKind::kInvalidArgument),
                    "invalid UTF-8 must be invalid") && passed;
    passed = expect(has_error(RecordCodec::encode(meta({Type::kVarchar}),
                                                   {Value{std::string(1025, 'x')}}),
                              RecordCodecErrorKind::kValueTooLarge),
                    "VARCHAR beyond 1024 bytes must be too large") && passed;

    const auto logical_meta = meta({Type::kVarchar, Type::kVarchar, Type::kVarchar,
                                    Type::kVarchar, Type::kVarchar});
    const std::vector<Value> logical_values = {
        Value{std::string(1024, 'a')}, Value{std::string(1024, 'b')},
        Value{std::string(1024, 'c')}, Value{std::string(1024, 'd')}, Value{std::string("e")}};
    const auto logical_result = RecordCodec::encode(logical_meta, logical_values);
    passed = expect(has_error(logical_result, RecordCodecErrorKind::kValueTooLarge) &&
                        logical_result.error->message.find("logical") != std::string::npos,
                    "logical row beyond 4096 bytes must be too large") && passed;

    const auto physical_meta = meta({Type::kVarchar, Type::kVarchar, Type::kVarchar,
                                     Type::kVarchar});
    const std::vector<Value> physical_values(4, Value{std::string(1011, 'p')});
    passed = expect(has_error(RecordCodec::encode(physical_meta, physical_values),
                              RecordCodecErrorKind::kValueTooLarge),
                    "encoded payload beyond 4056 bytes must be too large") && passed;
    return passed;
}

bool test_corrupt_fixed_width_and_bool() {
    bool passed = expect(has_error(RecordCodec::decode(meta({Type::kInt32}),
                                                       std::vector<std::byte>(3)),
                                   RecordCodecErrorKind::kCorrupt),
                         "short INT32 must be corrupt");
    passed = expect(has_error(RecordCodec::decode(meta({Type::kInt64}),
                                                   std::vector<std::byte>(7)),
                              RecordCodecErrorKind::kCorrupt),
                    "short INT64 must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(meta({Type::kFloat}),
                                                   std::vector<std::byte>(3)),
                              RecordCodecErrorKind::kCorrupt),
                    "short FLOAT must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(meta({Type::kDouble}),
                                                   std::vector<std::byte>(7)),
                              RecordCodecErrorKind::kCorrupt),
                    "short DOUBLE must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(meta({Type::kBool}),
                                                   std::vector<std::byte>{std::byte{2}}),
                              RecordCodecErrorKind::kCorrupt),
                    "BOOL byte other than 0/1 must be corrupt") && passed;
    return passed;
}

bool test_corrupt_varchar_and_record_boundaries() {
    bool passed = expect(has_error(RecordCodec::decode(meta({Type::kVarchar}),
                                                       std::vector<std::byte>(3)),
                                   RecordCodecErrorKind::kCorrupt),
                         "missing VARCHAR length must be corrupt");
    passed = expect(has_error(RecordCodec::decode(
                                  meta({Type::kVarchar}),
                                  std::vector<std::byte>{std::byte{2}, std::byte{0}, std::byte{0},
                                                         std::byte{0}, std::byte{0x61}}),
                              RecordCodecErrorKind::kCorrupt),
                    "VARCHAR length beyond remaining payload must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(
                                  meta({Type::kVarchar}),
                                  std::vector<std::byte>{std::byte{2}, std::byte{0}, std::byte{0},
                                                         std::byte{0}, std::byte{0xC0},
                                                         std::byte{0xAF}}),
                              RecordCodecErrorKind::kCorrupt),
                    "invalid UTF-8 on disk must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(
                                  meta({Type::kInt32}),
                                  std::vector<std::byte>{std::byte{1}, std::byte{0}, std::byte{0},
                                                         std::byte{0}, std::byte{9}}),
                              RecordCodecErrorKind::kCorrupt),
                    "trailing bytes must be corrupt") && passed;
    passed = expect(has_error(RecordCodec::decode(
                                  meta({Type::kInt32, Type::kVarchar, Type::kBool}),
                                  std::vector<std::byte>{std::byte{1}, std::byte{0}, std::byte{0},
                                                         std::byte{0}, std::byte{1}, std::byte{0},
                                                         std::byte{0}, std::byte{0}, std::byte{0x78}}),
                              RecordCodecErrorKind::kCorrupt),
                    "mixed record missing its final field must be corrupt") && passed;
    return passed;
}

}  // namespace

int main() {
    const bool passed = test_integer_round_trips_and_little_endian() &&
                        test_float_bool_and_varchar_round_trips() &&
                        test_mixed_record_and_size_boundaries() && test_invalid_encode() &&
                        test_corrupt_fixed_width_and_bool() &&
                        test_corrupt_varchar_and_record_boundaries();
    return passed ? 0 : 1;
}
