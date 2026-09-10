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

inline constexpr std::uint32_t kMaxVarcharBytes = 1024;     // 单个 VARCHAR 值的 UTF-8 字节上限
inline constexpr std::uint32_t kMaxRowLogicalBytes = 4096;  // 单行逻辑载荷字节上限
inline constexpr std::size_t kMaxSqlBytes = std::size_t{1024} * 1024;  // SQL 文本上限 1 MiB

enum class Type {
    kInt,      // INT，32 位
    kVarchar   // VARCHAR
};

struct Value {
    std::variant<std::int32_t, std::string> data;
};

struct ColumnMeta {
    std::string name;
    Type type;
};

struct TableMeta {
    TableId table_id;
    std::string table_name;
    std::vector<ColumnMeta> columns;  // 顺序即记录布局
};

struct SourceLocation {
    int line;    // 从 1 开始
    int column;  // 从 1 开始
};

}  // namespace tinydbms

#endif  // TINYDBMS_COMMON_HPP
