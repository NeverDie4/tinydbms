// GUI 真实后端（src/gui/core_backend.cpp）的自动化覆盖。
//
// `tinydbms.gui_ui` 只使用 FakeBackend 验证界面与结果模型；真实 data path
// （打开库 → 执行脚本 → 修复建议 → 关闭）此前只有 tinydbms-gui 二进制走过。
// 本用例补齐这条链路：它不依赖 Qt，只用真实 core 驱动与 GUI 同一个 CoreBackend，
// 断言 GUI 展示所依赖的结构化字段（status/kind/compile_stage/source/suggestion/fix_it），
// 不做任何 message 文本匹配。

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "backend.hpp"
#include "tinydbms/core.hpp"

namespace {

using tinydbms::CompileStage;
using tinydbms::core::CommandResult;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::OpenDatabaseRequest;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementStatus;
namespace gui = tinydbms::gui;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view prefix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            (std::string{prefix} + "-" + std::to_string(stamp));

        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        if (error || !created) {
            throw std::runtime_error{"cannot create gui backend test directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool open(::gui::Backend& backend, const std::filesystem::path& data_dir) {
    return !backend.open(OpenDatabaseRequest{data_dir.string()}).error.has_value();
}

const Error* statement_error(const ExecuteScriptResult& script, std::size_t index = 0) {
    if (script.statements.size() <= index || !script.statements[index].outcome().has_value()) {
        return nullptr;
    }
    return std::get_if<Error>(&script.statements[index].outcome()->outcome);
}

const QueryResult* statement_query(const ExecuteScriptResult& script, std::size_t index = 0) {
    if (script.statements.size() <= index || !script.statements[index].outcome().has_value()) {
        return nullptr;
    }
    return std::get_if<QueryResult>(&script.statements[index].outcome()->outcome);
}

// 打开 → 建表/写入 → 查询 → close → 重开：GUI 的"打开数据库/执行/重新打开"三个按钮
// 都走这一条路径，且结果必须能直接喂给结果表格模型（列名 + 行）。
bool test_round_trip_and_persistence() {
    TemporaryDirectory directory{"tinydbms-gui-core-backend"};
    std::unique_ptr<::gui::Backend> backend = ::gui::make_backend();
    CHECK(backend != nullptr);

    // 未 open 就执行：GUI 必须拿到明确的执行错误，而不是未定义行为。
    const ExecuteScriptResult before_open =
        backend->execute_script(ExecuteScriptRequest{"SELECT 1;"});
    CHECK(before_open.script_error.has_value());
    CHECK(before_open.script_error->kind == ErrorKind::kExecute);
    CHECK(before_open.statements.empty());

    CHECK(open(*backend, directory.path()));

    const ExecuteScriptResult created = backend->execute_script(ExecuteScriptRequest{
        "CREATE TABLE t (id INT, name VARCHAR NULL);\n"
        "INSERT INTO t VALUES (1, 'ada'), (2, NULL);\n"});
    CHECK(!created.script_error.has_value());
    CHECK(created.statements.size() == 2);
    CHECK(created.statements[0].status() == StatementStatus::kExecuted);
    const auto* inserted = created.statements[1].outcome().has_value()
        ? std::get_if<CommandResult>(&created.statements[1].outcome()->outcome)
        : nullptr;
    CHECK(inserted != nullptr);
    CHECK(inserted->affected_rows == 2);
    CHECK(!inserted->error.has_value());

    const ExecuteScriptResult selected =
        backend->execute_script(ExecuteScriptRequest{"SELECT * FROM t;"});
    const QueryResult* query = statement_query(selected);
    CHECK(query != nullptr);
    CHECK(query->columns.size() == 2);
    CHECK(query->columns[0].name == "id");
    CHECK(query->columns[1].name == "name");
    CHECK(query->rows.size() == 2);
    CHECK(std::holds_alternative<std::monostate>(query->rows[1][1].data));
    CHECK(std::get<std::int32_t>(query->rows[0][0].data) == 1);
    CHECK(std::get<std::string>(query->rows[0][1].data) == "ada");

    CHECK(!backend->close().error.has_value());
    // close 之后再次 close：这一次 open 已重报，core 的"database is not open"
    // 表示没有需要清理的状态，close_policy 必须把它当成成功。
    CHECK(!backend->close().error.has_value());

    // 同一个 backend 可以在关闭后重新打开并读到落盘数据。
    CHECK(open(*backend, directory.path()));
    const ExecuteScriptResult reopened =
        backend->execute_script(ExecuteScriptRequest{"SELECT COUNT(*) FROM t;"});
    const QueryResult* counted = statement_query(reopened);
    CHECK(counted != nullptr);
    CHECK(counted->rows.size() == 1);
    CHECK(!backend->close().error.has_value());
    return true;
}

// GUI 诊断列表与修复建议完全依赖结构化字段：语义错误带 stage/source，
// 拼写错误带 suggestion/fix_it，且错误不会破坏会话（GUI 可以继续执行）。
bool test_structured_diagnostics_reach_the_gui() {
    TemporaryDirectory directory{"tinydbms-gui-core-backend-diagnostics"};
    std::unique_ptr<::gui::Backend> backend = ::gui::make_backend();
    CHECK(backend != nullptr);
    CHECK(open(*backend, directory.path()));
    CHECK(!backend->execute_script(ExecuteScriptRequest{"CREATE TABLE t (id INT);"})
               .script_error.has_value());

    const ExecuteScriptResult missing_table =
        backend->execute_script(ExecuteScriptRequest{"SELECT * FROM missing;\n"});
    CHECK(missing_table.statements.size() == 1);
    CHECK(missing_table.statements[0].status() == StatementStatus::kCompileError);
    const Error* semantic = statement_error(missing_table);
    CHECK(semantic != nullptr);
    CHECK(semantic->kind == ErrorKind::kCompile);
    CHECK(semantic->compile_stage.has_value());
    CHECK(*semantic->compile_stage == CompileStage::kSemantic);
    CHECK(semantic->source.has_value());

    const ExecuteScriptResult misspelled =
        backend->execute_script(ExecuteScriptRequest{"SELECT id FROM t WHRE id = 1;\n"});
    const Error* fixable = statement_error(misspelled);
    CHECK(fixable != nullptr);
    CHECK(fixable->kind == ErrorKind::kCompile);
    CHECK(fixable->suggestion.has_value());
    CHECK(fixable->fix_it.has_value());
    CHECK(!fixable->fix_it->replacement.empty());
    CHECK(fixable->fix_it->range.begin.byte_offset <=
          fixable->fix_it->range.end.byte_offset);

    // 结构化错误之后会话仍然可用。
    const ExecuteScriptResult recovered =
        backend->execute_script(ExecuteScriptRequest{"INSERT INTO t VALUES (7);\n"});
    CHECK(!recovered.script_error.has_value());
    CHECK(recovered.statements.size() == 1);
    CHECK(recovered.statements[0].status() == StatementStatus::kExecuted);
    CHECK(!backend->close().error.has_value());
    return true;
}

// open 失败路径：GUI 在 open 失败后会无条件先 close 再 open，
// 这里锁住 close_policy 的判定——"没有需要清理的状态"不算失败。
bool test_open_failure_then_cleanup_is_quiet() {
    TemporaryDirectory directory{"tinydbms-gui-core-backend-open-failure"};
    const std::filesystem::path file = directory.path() / "not-a-directory";
    {
        std::ofstream out{file};
        out << 'x';
    }

    std::unique_ptr<::gui::Backend> backend = ::gui::make_backend();
    CHECK(backend != nullptr);

    const auto failed = backend->open(OpenDatabaseRequest{file.string()});
    CHECK(failed.error.has_value());
    CHECK(!backend->close().error.has_value());

    // 失败之后同一个 backend 仍能打开合法目录。
    CHECK(open(*backend, directory.path() / "db"));
    CHECK(!backend->close().error.has_value());
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_round_trip_and_persistence() &&
        test_structured_diagnostics_reach_the_gui() &&
        test_open_failure_then_cleanup_is_quiet();

    if (!passed) {
        return 1;
    }

    std::cout << "gui core backend tests passed\n";
    return 0;
}
