#pragma once

#include "storage_constants.h"
#include "tinydbms/common.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tinydbms::storage::internal {

enum class RowFormat {
    kV1,
    kV2,
};

enum class RecordCodecErrorKind {
    kInvalidArgument,
    kValueTooLarge,
    kCorrupt,
};

struct RecordCodecError {
    RecordCodecErrorKind kind;
    std::string message;
};

template <typename T>
struct RecordCodecResult {
    std::optional<T> value;
    std::optional<RecordCodecError> error;
};

class RecordCodec {
public:
    static RecordCodecResult<std::size_t> encoded_size(
        RowFormat format, const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<std::byte>> encode(
        RowFormat format, const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<Value>> decode(
        RowFormat format, const TableMeta& table, std::span<const std::byte> payload);

    // Internal compatibility entry points for existing V1-only page/heap tests.
    static RecordCodecResult<std::size_t> encoded_size(
        const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<std::byte>> encode(
        const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<Value>> decode(
        const TableMeta& table, std::span<const std::byte> payload);
};

}  // namespace tinydbms::storage::internal
