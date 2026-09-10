#include "record_codec.h"

#include <iostream>
#include <string>

namespace {
using namespace tinydbms;
using tinydbms::storage::internal::RecordCodec;
using tinydbms::storage::internal::RecordCodecErrorKind;

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
    ok = expect((*encoded.value)[0] == std::byte{0xF9} && (*encoded.value)[3] == std::byte{0xFF},
                "INT must remain little-endian INT32") && ok;
    const auto decoded = RecordCodec::decode(table, *encoded.value);
    return expect(decoded.value.has_value() && decoded.value->at(0).data == input[0].data &&
                      decoded.value->at(1).data == input[1].data,
                  "round trip canonical values") && ok;
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
    return test_int32_little_endian_and_varchar_round_trip() && test_validation_and_corruption() ? 0 : 1;
}
