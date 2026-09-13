#ifndef TINYDBMS_COMMON_HPP
#define TINYDBMS_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace tinydbms {

// 初版两者同为 uint32_t 别名，靠命名与测试防混用；不引入强类型包装
using TableId = std::uint32_t;   // 由 core 的 Catalog 分配
using ColumnId = std::uint32_t;  // 等于建表列序 0..n-1，不单独分配
using SlotId = std::uint32_t;    // Query-local slot used by compiler plans and Core SlotRow execution

inline constexpr std::uint32_t kMaxVarcharBytes = 1024;     // 单个 VARCHAR 值的 UTF-8 字节上限
inline constexpr std::uint32_t kMaxRowLogicalBytes = 4096;  // 单行逻辑载荷字节上限
inline constexpr std::size_t kMaxSqlBytes = std::size_t{1024} * 1024;  // SQL 文本上限 1 MiB

enum class Type {
    kInt = 0,      // INT，32 位
    kBigInt = 2,   // BIGINT，64 位
    kDouble = 3,   // DOUBLE
    kBoolean = 4,  // BOOLEAN
    kVarchar = 1   // VARCHAR；保留 SQL v1 枚举值
};

struct Value {
    std::variant<
        std::monostate,
        std::int32_t,
        std::int64_t,
        double,
        bool,
        std::string
    > data;
};

struct ColumnMeta {
    std::string name;
    Type type;
    bool nullable = false;
};

struct TableMeta {
    TableId table_id;
    std::string table_name;
    std::vector<ColumnMeta> columns;  // 顺序即记录布局
};

struct SourceLocation {
    int line;                 // 从 1 开始
    int column;               // 从 1 开始，按 UTF-8 字节计数
    std::size_t byte_offset;  // 在所属源码文本内从 0 开始
};

}  // namespace tinydbms

#endif  // TINYDBMS_COMMON_HPP
