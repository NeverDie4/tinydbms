#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace tinydbms {

using TableId = std::uint32_t;
using ColumnId = std::uint32_t;
using CursorId = std::uint64_t;

inline constexpr std::size_t kMaxVarcharBytes = 1024;
inline constexpr std::size_t kMaxRowLogicalBytes = 4096;

enum class Type {
    kInt32,
    kInt64,
    kFloat,
    kDouble,
    kBool,
    kVarchar,
};

struct Value {
    std::variant<std::int32_t, std::int64_t, float, double, bool, std::string> data;
};

inline bool value_matches_type(Type type, const Value& value) {
    switch (type) {
        case Type::kInt32:
            return std::holds_alternative<std::int32_t>(value.data);
        case Type::kInt64:
            return std::holds_alternative<std::int64_t>(value.data);
        case Type::kFloat:
            return std::holds_alternative<float>(value.data);
        case Type::kDouble:
            return std::holds_alternative<double>(value.data);
        case Type::kBool:
            return std::holds_alternative<bool>(value.data);
        case Type::kVarchar:
            return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

struct ColumnMeta {
    std::string name;
    Type type = Type::kInt32;
};

struct TableMeta {
    TableId table_id = 0;
    std::string table_name;
    std::vector<ColumnMeta> columns;
};

struct SourceLocation {
    std::size_t line = 1;
    std::size_t column = 1;
};

enum class StorageErrorKind {
    kTableNotFound,
    kCursorInvalid,
    kValueTooLarge,
    kIoError,
    kCorrupt,
    kInvalidRequest,
};

struct StorageError {
    StorageErrorKind kind = StorageErrorKind::kInvalidRequest;
    std::string message;
};

}  // namespace tinydbms
