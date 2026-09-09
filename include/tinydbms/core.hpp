#ifndef TINYDBMS_CORE_HPP
#define TINYDBMS_CORE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tinydbms/common.hpp"

namespace tinydbms::core {

using Row = std::vector<Value>;  // core 执行器内部与结果中的行

struct ColumnHeader {
    std::string name;
    Type type;
};

struct QueryResult {
    std::vector<ColumnHeader> columns;
    std::vector<Row> rows;
};

enum class ErrorKind {
    kCompile,
    kExecute,
    kStorage,
    kInternal
};

struct Error {
    ErrorKind kind;
    // 仅编译错误携带；已由 core 换算为整段输入的绝对位置
    std::optional<SourceLocation> location;
    std::string message;
};

struct CommandResult {
    std::uint64_t affected_rows;  // CREATE 成功时为 0
    // 仅 INSERT/DELETE 部分行成功后 storage 出错时携带；
    // 此时 affected_rows 是错误前已成功执行的行数，core 停止后续语句
    std::optional<Error> error;
};

struct ExecuteResult {
    std::variant<QueryResult, CommandResult, Error> outcome;
};

struct OpenDatabaseRequest {
    std::string data_dir;  // 已由入口补全的非空数据库目录，UTF-8 编码
};

struct OpenDatabaseResult {
    std::optional<Error> error;
};

struct CloseDatabaseResult {
    std::optional<Error> error;
};

struct ExecuteScriptRequest {
    std::string text;  // REPL 一行，或 stdin 批处理的整段文本
};

struct ExecuteScriptResult {
    // 每条已尝试语句一个结果；遇到错误后停止，剩余语句不执行
    std::vector<ExecuteResult> outcomes;
};

// core 对入口与测试暴露的有状态对象；每个实例持有自己的 Catalog 与生命周期状态
class Database {
public:
    Database();
    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    OpenDatabaseResult open(const OpenDatabaseRequest& request);
    CloseDatabaseResult close();
    ExecuteScriptResult execute_script(const ExecuteScriptRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tinydbms::core

#endif  // TINYDBMS_CORE_HPP
