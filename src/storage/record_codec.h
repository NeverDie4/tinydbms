#pragma once

#include "storage_constants.h"
#include "tinydbms/common.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tinydbms::storage::internal {

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
        const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<std::byte>> encode(
        const TableMeta& table, const std::vector<Value>& values);
    static RecordCodecResult<std::vector<Value>> decode(
        const TableMeta& table, std::span<const std::byte> payload);
};

}  // namespace tinydbms::storage::internal
