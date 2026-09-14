#include "record_codec.h"

#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace tinydbms;
using tinydbms::storage::internal::RecordCodec;
using tinydbms::storage::internal::RecordCodecErrorKind;
using tinydbms::storage::internal::RowFormat;

bool expect(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

TableMeta schema(std::vector<ColumnMeta> columns) {
    return TableMeta{0, "codec", std::move(columns)};
}

bool has_error(auto result, RecordCodecErrorKind kind) {
    return result.error.has_value() && result.error->kind == kind;
}

bool test_int32_little_endian_and_varchar_round_trip() {
    const auto table = schema({{"id", Type::kInt}, {"name", Type::kVarchar}});
    const std::vector<Value> input{Value{std::int32_t{-7}}, Value{std::string{"\xE4\xB8\xAD"}}};
    const auto encoded = RecordCodec::encode(table, input);
    bool ok = expect(encoded.value.has_value(), "encode canonical row");
    ok = expect(encoded.value->size() == 11, "INT32 plus VARCHAR layout size") && ok;
    const std::vector<std::byte> expected{
        std::byte{0xF9},std::byte{0xFF},std::byte{0xFF},std::byte{0xFF},
        std::byte{0x03},std::byte{0},std::byte{0},std::byte{0},
        std::byte{0xE4},std::byte{0xB8},std::byte{0xAD}};
    ok = expect(*encoded.value == expected,
                "V1 INT32/VARCHAR bytes remain exactly compatible") && ok;
    const auto decoded = RecordCodec::decode(table, *encoded.value);
    return expect(decoded.value.has_value() && decoded.value->at(0).data == input[0].data &&
                      decoded.value->at(1).data == input[1].data,
                  "round trip canonical values") && ok;
}

bool test_v2_bigint_little_endian_and_boundaries() {
    const auto table = schema({{"value", Type::kBigInt}});
    const std::vector<std::int64_t> values{
        std::numeric_limits<std::int64_t>::min(), -1, 0, 1,
        std::numeric_limits<std::int64_t>::max()};
    bool ok = true;
    for (const std::int64_t value : values) {
        const auto encoded = RecordCodec::encode(RowFormat::kV2, table, {Value{value}});
        ok = expect(encoded.value.has_value() && encoded.value->size() == 9,
                    "V2 BIGINT includes one-byte null bitmap") && ok;
        ok = expect((*encoded.value)[0] == std::byte{0},
                    "V2 non-null row has a zero null bitmap") && ok;
        const auto decoded = RecordCodec::decode(RowFormat::kV2, table, *encoded.value);
        ok = expect(decoded.value.has_value() &&
                        std::get<std::int64_t>(decoded.value->at(0).data) == value,
                    "V2 BIGINT boundary round trip") && ok;
    }

    const std::int64_t marker = 0x0102030405060708LL;
    const auto encoded = RecordCodec::encode(RowFormat::kV2, table, {Value{marker}});
    const std::vector<std::byte> expected{
        std::byte{0}, std::byte{0x08}, std::byte{0x07}, std::byte{0x06},
        std::byte{0x05}, std::byte{0x04}, std::byte{0x03}, std::byte{0x02},
        std::byte{0x01}};
    return expect(encoded.value.has_value() && *encoded.value == expected,
                  "V2 BIGINT has deterministic signed little-endian bytes") && ok;
}

bool test_v2_validation_and_corruption() {
    const auto bigint = schema({{"value", Type::kBigInt}});
    bool ok = expect(has_error(
                         RecordCodec::encode(
                             RowFormat::kV1, bigint, {Value{std::int64_t{1}}}),
                         RecordCodecErrorKind::kInvalidArgument),
                     "V1 rejects BIGINT schema");
    ok = expect(has_error(
                    RecordCodec::encode(
                        RowFormat::kV2, bigint, {Value{std::int32_t{1}}}),
                    RecordCodecErrorKind::kInvalidArgument),
                "V2 BIGINT rejects INT32 Value") && ok;
    ok = expect(has_error(
                    RecordCodec::encode(
                        RowFormat::kV2, bigint, {Value{std::monostate{}}}),
                    RecordCodecErrorKind::kInvalidArgument),
                "Phase 1B V2 rejects NULL Value") && ok;
    ok = expect(has_error(
                    RecordCodec::decode(
                        RowFormat::kV2, bigint,
                        std::vector<std::byte>{std::byte{0}, std::byte{1}}),
                    RecordCodecErrorKind::kCorrupt),
                "V2 rejects short BIGINT payload") && ok;
    return expect(has_error(
                      RecordCodec::decode(
                          RowFormat::kV2, bigint,
                          std::vector<std::byte>{std::byte{1}}),
                      RecordCodecErrorKind::kCorrupt),
                  "V2 rejects a NULL bit for non-nullable BIGINT") && ok;
}

bool test_v2_double_binary64_round_trip() {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);
    const auto table = schema({{"value", Type::kDouble}});
    const std::vector<double> values{
        0.0,
        -0.0,
        1.0,
        -1.5,
        12.5,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min()};
    bool ok = true;
    ok = expect(has_error(
                    RecordCodec::encode(RowFormat::kV1, table, {Value{1.0}}),
                    RecordCodecErrorKind::kInvalidArgument),
                "V1 rejects DOUBLE schema") && ok;
    for (const double value : values) {
        const auto encoded = RecordCodec::encode(RowFormat::kV2, table, {Value{value}});
        ok = expect(encoded.value.has_value() && encoded.value->size() == 9,
                    "V2 DOUBLE includes bitmap and binary64 payload") && ok;
        if (!encoded.value.has_value()) {
            continue;
        }
        const auto decoded = RecordCodec::decode(RowFormat::kV2, table, *encoded.value);
        const double* round_trip = decoded.value.has_value()
            ? std::get_if<double>(&decoded.value->at(0).data)
            : nullptr;
        ok = expect(round_trip != nullptr &&
                        std::bit_cast<std::uint64_t>(*round_trip) ==
                            std::bit_cast<std::uint64_t>(value),
                    "V2 DOUBLE preserves every binary64 bit") && ok;
    }

    const auto one = RecordCodec::encode(RowFormat::kV2, table, {Value{1.0}});
    const std::vector<std::byte> expected{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xF0},
        std::byte{0x3F}};
    ok = expect(one.value.has_value() && *one.value == expected,
                "V2 DOUBLE 1.0 has deterministic little-endian bytes") && ok;
    ok = expect(has_error(
                    RecordCodec::encode(
                        RowFormat::kV2,
                        table,
                        {Value{std::numeric_limits<double>::infinity()}}),
                    RecordCodecErrorKind::kInvalidArgument),
                "V2 DOUBLE rejects non-finite encode") && ok;
    ok = expect(has_error(
                    RecordCodec::decode(
                        RowFormat::kV2,
                        table,
                        std::vector<std::byte>{std::byte{0x00}, std::byte{0x00}}),
                    RecordCodecErrorKind::kCorrupt),
                "V2 DOUBLE rejects short payload") && ok;
    const std::vector<std::byte> infinity_payload{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xF0},
        std::byte{0x7F}};
    return expect(has_error(
                      RecordCodec::decode(RowFormat::kV2, table, infinity_payload),
                      RecordCodecErrorKind::kCorrupt),
                  "V2 DOUBLE rejects non-finite decode") && ok;
}

bool test_v2_boolean_and_mixed_round_trip() {
    const auto boolean = schema({{"active", Type::kBoolean}});
    bool ok = expect(has_error(
                         RecordCodec::encode(RowFormat::kV1, boolean, {Value{true}}),
                         RecordCodecErrorKind::kInvalidArgument),
                     "V1 rejects BOOLEAN schema");
    for (const auto& [value, expected] : {
             std::pair{false, std::vector<std::byte>{std::byte{0}, std::byte{0}}},
             std::pair{true, std::vector<std::byte>{std::byte{0}, std::byte{1}}}}) {
        const auto encoded = RecordCodec::encode(RowFormat::kV2, boolean, {Value{value}});
        ok = expect(encoded.value.has_value() && *encoded.value == expected,
                    "V2 BOOLEAN deterministic bitmap and payload bytes") && ok;
        const auto decoded = encoded.value.has_value()
            ? RecordCodec::decode(RowFormat::kV2, boolean, *encoded.value)
            : decltype(RecordCodec::decode(RowFormat::kV2, boolean, expected)){};
        const bool* round_trip = decoded.value.has_value()
            ? std::get_if<bool>(&decoded.value->at(0).data)
            : nullptr;
        ok = expect(round_trip != nullptr && *round_trip == value,
                    "V2 BOOLEAN round trip") && ok;
    }
    ok = expect(has_error(
                    RecordCodec::decode(
                        RowFormat::kV2, boolean,
                        std::vector<std::byte>{std::byte{0}, std::byte{2}}),
                    RecordCodecErrorKind::kCorrupt),
                "V2 BOOLEAN rejects payload 0x02") && ok;
    ok = expect(has_error(
                    RecordCodec::decode(
                        RowFormat::kV2, boolean,
                        std::vector<std::byte>{std::byte{0}, std::byte{0xFF}}),
                    RecordCodecErrorKind::kCorrupt),
                "V2 BOOLEAN rejects nonzero values other than one") && ok;

    const auto mixed = schema({
        {"i", Type::kInt}, {"b", Type::kBigInt}, {"d", Type::kDouble},
        {"flag", Type::kBoolean}, {"text", Type::kVarchar}});
    const std::vector<Value> values{
        Value{std::int32_t{7}}, Value{std::int64_t{2147483648LL}}, Value{12.5},
        Value{false}, Value{std::string{"tail"}}};
    const auto encoded = RecordCodec::encode(RowFormat::kV2, mixed, values);
    const auto decoded = encoded.value.has_value()
        ? RecordCodec::decode(RowFormat::kV2, mixed, *encoded.value)
        : decltype(RecordCodec::decode(RowFormat::kV2, mixed, std::vector<std::byte>{})){};
    return expect(decoded.value.has_value() && decoded.value->size() == values.size() &&
                      decoded.value->at(0).data == values[0].data &&
                      decoded.value->at(1).data == values[1].data &&
                      decoded.value->at(2).data == values[2].data &&
                      decoded.value->at(3).data == values[3].data &&
                      decoded.value->at(4).data == values[4].data,
                  "mixed INT/BIGINT/DOUBLE/BOOLEAN/VARCHAR V2 round trip") && ok;
}

bool test_v2_nullable_bitmap_and_offsets() {
    std::vector<ColumnMeta> ten_columns;
    ten_columns.reserve(10);
    for (std::size_t index = 0; index < 10; ++index) {
        ten_columns.push_back(ColumnMeta{"c" + std::to_string(index), Type::kInt, true});
    }
    const auto all_nullable = schema(std::move(ten_columns));
    const std::vector<Value> all_null(10, Value{std::monostate{}});
    const auto encoded_all_null = RecordCodec::encode(RowFormat::kV2, all_nullable, all_null);
    bool ok = expect(
        encoded_all_null.value.has_value() &&
            *encoded_all_null.value ==
                std::vector<std::byte>{std::byte{0xFF}, std::byte{0x03}},
        "V2 ten-column all-NULL row is a cross-byte bitmap-only payload");
    const auto decoded_all_null = encoded_all_null.value.has_value()
        ? RecordCodec::decode(RowFormat::kV2, all_nullable, *encoded_all_null.value)
        : decltype(RecordCodec::decode(
              RowFormat::kV2, all_nullable, std::vector<std::byte>{})){};
    ok = expect(
        decoded_all_null.value.has_value() && decoded_all_null.value->size() == 10U,
        "V2 all-NULL row decodes every column") && ok;
    if (decoded_all_null.value.has_value()) {
        for (const Value& value : *decoded_all_null.value) {
            ok = expect(std::holds_alternative<std::monostate>(value.data),
                        "V2 all-NULL decoded value is monostate") && ok;
        }
    }

    const auto mixed = schema({
        {"missing_int", Type::kInt, true},
        {"text", Type::kVarchar, true},
        {"missing_big", Type::kBigInt, true},
        {"flag", Type::kBoolean, true},
        {"empty", Type::kVarchar, true},
        {"zero", Type::kInt, true}});
    const std::vector<Value> values{
        Value{std::monostate{}}, Value{std::string{"x"}}, Value{std::monostate{}},
        Value{false}, Value{std::string{}}, Value{std::int32_t{0}}};
    const auto encoded = RecordCodec::encode(RowFormat::kV2, mixed, values);
    ok = expect(
        encoded.value.has_value() && !encoded.value->empty() &&
            encoded.value->front() == std::byte{0x05},
        "V2 NULL bitmap omits payloads without shifting later values") && ok;
    const auto decoded = encoded.value.has_value()
        ? RecordCodec::decode(RowFormat::kV2, mixed, *encoded.value)
        : decltype(RecordCodec::decode(
              RowFormat::kV2, mixed, std::vector<std::byte>{})){};
    ok = expect(
        decoded.value.has_value() && decoded.value->size() == values.size(),
        "V2 nullable mixed row decodes") && ok;
    if (decoded.value.has_value()) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            ok = expect(decoded.value->at(index).data == values[index].data,
                        "V2 nullable mixed row preserves value identity") && ok;
        }
    }
    ok = expect(
        has_error(
            RecordCodec::encode(RowFormat::kV1, mixed, values),
            RecordCodecErrorKind::kInvalidArgument),
        "V1 rejects nullable schema and NULL values") && ok;
    std::vector<std::byte> invalid_high_bits{std::byte{0x04}, std::byte{0x80}};
    return expect(
        has_error(
            RecordCodec::decode(RowFormat::kV2, all_nullable, invalid_high_bits),
            RecordCodecErrorKind::kCorrupt),
        "V2 rejects nonzero unused bitmap bits") && ok;
}

bool test_validation_and_corruption() {
    const auto table = schema({{"id", Type::kInt}, {"name", Type::kVarchar}});
    bool ok = expect(has_error(RecordCodec::encode(table, {Value{std::string{"wrong"}}, Value{std::string{"x"}}}),
                               RecordCodecErrorKind::kInvalidArgument),
                     "reject type mismatch");
    ok = expect(has_error(RecordCodec::encode(table, {Value{std::int32_t{1}}, Value{std::string{"\xFF"}}}),
                          RecordCodecErrorKind::kInvalidArgument),
                "reject invalid UTF-8") && ok;
    const std::vector<std::byte> short_payload{std::byte{1}, std::byte{2}};
    ok = expect(has_error(RecordCodec::decode(table, short_payload),
                          RecordCodecErrorKind::kCorrupt),
                "reject short INT payload") && ok;
    const auto valid = RecordCodec::encode(table, {Value{std::int32_t{1}}, Value{std::string{"x"}}});
    auto trailing = *valid.value;
    trailing.push_back(std::byte{0});
    return expect(has_error(RecordCodec::decode(table, trailing), RecordCodecErrorKind::kCorrupt),
                  "reject trailing bytes") && ok;
}
}

int main() {
    return test_int32_little_endian_and_varchar_round_trip() &&
            test_v2_bigint_little_endian_and_boundaries() &&
            test_v2_double_binary64_round_trip() &&
            test_v2_boolean_and_mixed_round_trip() &&
            test_v2_nullable_bitmap_and_offsets() &&
            test_v2_validation_and_corruption() && test_validation_and_corruption()
        ? 0
        : 1;
}
