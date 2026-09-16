#include "runner.hpp"
#include "session.hpp"
#include "file_manager.h"
#include "page_file.h"
#include "storage_test_access.h"

#include "json_check.hpp"
#include "tinydbms/common.hpp"
#include "tinydbms/core.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using tinydbms::app::CliEnvironment;
using tinydbms::app::CoreSession;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::QueryResult;

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
            throw std::runtime_error{"cannot create integration test directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct InvocationResult {
    int exit_code = 1;
    std::string output;
    std::string error;
};

// --stats 的一行快照；字段缺一不可，任何一项解析失败都返回 nullopt。
struct BufferSnapshot {
    std::uint64_t fetch_count = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    std::uint64_t eviction_count = 0;
    std::uint64_t dirty_flush_count = 0;
    double miss_rate = 0.0;
};

std::optional<std::uint64_t> parse_count_field(
    std::string_view token,
    std::string_view name) {
    if (!token.starts_with(name)) {
        return std::nullopt;
    }
    token.remove_prefix(name.size());
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_miss_rate_field(std::string_view token) {
    constexpr std::string_view kName = "miss_rate=";
    if (!token.starts_with(kName) || token.size() < kName.size() + 2U ||
        !token.ends_with('%')) {
        return std::nullopt;
    }
    token.remove_prefix(kName.size());
    token.remove_suffix(1);  // 百分号
    const std::size_t dot = token.find('.');
    if (dot == std::string_view::npos || token.size() - dot != 3U) {
        return std::nullopt;
    }
    std::uint64_t whole = 0;
    std::uint64_t fraction = 0;
    const std::string_view whole_text = token.substr(0, dot);
    const std::string_view fraction_text = token.substr(dot + 1);
    const auto whole_parsed = std::from_chars(
        whole_text.data(), whole_text.data() + whole_text.size(), whole);
    const auto fraction_parsed = std::from_chars(
        fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
    if (whole_parsed.ec != std::errc{} ||
        whole_parsed.ptr != whole_text.data() + whole_text.size() ||
        fraction_parsed.ec != std::errc{} ||
        fraction_parsed.ptr != fraction_text.data() + fraction_text.size()) {
        return std::nullopt;
    }
    return static_cast<double>(whole) + static_cast<double>(fraction) / 100.0;
}

// 解析单行快照（不含换行符）；字段缺一不可，顺序与数量都固定。
std::optional<BufferSnapshot> parse_buffer_snapshot_line(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position <= line.size()) {
        const std::size_t space = line.find(' ', position);
        const std::size_t stop = space == std::string_view::npos ? line.size() : space;
        tokens.push_back(line.substr(position, stop - position));
        if (space == std::string_view::npos) {
            break;
        }
        position = space + 1;
    }
    if (tokens.size() != 7U || tokens[0] != "BUFFER") {
        return std::nullopt;
    }

    BufferSnapshot snapshot;
    const auto fetch = parse_count_field(tokens[1], "fetch=");
    const auto hit = parse_count_field(tokens[2], "hit=");
    const auto miss = parse_count_field(tokens[3], "miss=");
    const auto rate = parse_miss_rate_field(tokens[4]);
    const auto evictions = parse_count_field(tokens[5], "evictions=");
    const auto flushes = parse_count_field(tokens[6], "flushes=");
    if (!fetch.has_value() || !hit.has_value() || !miss.has_value() ||
        !rate.has_value() || !evictions.has_value() || !flushes.has_value()) {
        return std::nullopt;
    }
    snapshot.fetch_count = *fetch;
    snapshot.hit_count = *hit;
    snapshot.miss_count = *miss;
    snapshot.miss_rate = *rate;
    snapshot.eviction_count = *evictions;
    snapshot.dirty_flush_count = *flushes;
    return snapshot;
}

// 取整段文本里的第一条 BUFFER 快照。
std::optional<BufferSnapshot> parse_buffer_line(const std::string& text) {
    const std::size_t begin = text.find("BUFFER ");
    if (begin == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t end = text.find('\n', begin);
    return parse_buffer_snapshot_line(
        end == std::string::npos
            ? std::string_view{text}.substr(begin)
            : std::string_view{text}.substr(begin, end - begin));
}

std::vector<BufferSnapshot> parse_buffer_lines(const std::string& text) {
    std::vector<BufferSnapshot> snapshots;
    std::size_t position = 0;
    while ((position = text.find("BUFFER ", position)) != std::string::npos) {
        const std::size_t end = text.find('\n', position);
        const std::string_view line = end == std::string::npos
            ? std::string_view{text}.substr(position)
            : std::string_view{text}.substr(position, end - position);
        const auto snapshot = parse_buffer_snapshot_line(line);
        if (!snapshot.has_value()) {
            return {};
        }
        snapshots.push_back(*snapshot);
        position = end == std::string::npos ? text.size() : end;
    }
    return snapshots;
}

InvocationResult invoke_cli(
    const std::filesystem::path& data_dir,
    std::string input,
    bool interactive,
    const std::vector<std::string>& extra_arguments = {}) {
    std::vector<std::string> arguments{
        "tinydbms",
        "--data-dir",
        data_dir.string()};
    for (const std::string& extra : extra_arguments) {
        arguments.push_back(extra);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    std::istringstream input_stream{std::move(input)};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    CliEnvironment environment{
        input_stream,
        output_stream,
        error_stream,
        interactive};
    CoreSession session;
    const int exit_code = tinydbms::app::run_cli(
        static_cast<int>(argv.size()),
        argv.data(),
        "0.1.0",
        session,
        environment);
    return InvocationResult{exit_code, output_stream.str(), error_stream.str()};
}

// 数据目录的完整文件清单（含大小）：用于断言入口升级没有引入额外副作用。
std::vector<std::string> list_data_files(const std::filesystem::path& data_dir) {
    std::vector<std::string> files;
    std::error_code error;
    if (!std::filesystem::exists(data_dir, error)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{data_dir}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto relative = std::filesystem::relative(entry.path(), data_dir, error);
        files.push_back(
            relative.generic_string() + ":" + std::to_string(entry.file_size()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

using ExpectedRows = std::map<std::int32_t, std::string>;

std::string make_payload(std::int32_t id) {
    std::string payload = "row-" + std::to_string(id) + ":";
    payload.append(tinydbms::kMaxVarcharBytes - payload.size(),
                   static_cast<char>('a' + (id % 26)));
    return payload;
}

bool execute_command(Database& database, std::string script, std::uint64_t expected_affected_rows) {
    const auto executed = database.execute_script(ExecuteScriptRequest{std::move(script)});
    CHECK(executed.statements.size() == 1);
    const auto* command =
        std::get_if<CommandResult>(&executed.statements.front().outcome()->outcome);
    CHECK(command != nullptr);
    CHECK(!command->error.has_value());
    CHECK(command->affected_rows == expected_affected_rows);
    return true;
}

bool expect_rows(Database& database, const std::string& script, const ExpectedRows& expected) {
    const auto executed = database.execute_script(ExecuteScriptRequest{script});
    CHECK(executed.statements.size() == 1);
    const auto* query =
        std::get_if<QueryResult>(&executed.statements.front().outcome()->outcome);
    CHECK(query != nullptr);
    CHECK(query->columns.size() == 2);
    CHECK(query->rows.size() == expected.size());

    ExpectedRows actual;
    for (const auto& row : query->rows) {
        CHECK(row.size() == 2);
        const auto* id = std::get_if<std::int32_t>(&row[0].data);
        const auto* payload = std::get_if<std::string>(&row[1].data);
        CHECK(id != nullptr);
        CHECK(payload != nullptr);
        CHECK(actual.emplace(*id, *payload).second);
    }
    CHECK(actual == expected);
    return true;
}

bool expect_data_pages_for_first_user_table() {
    auto* file_manager = tinydbms::storage::internal::StorageTestAccess::file_manager();
    CHECK(file_manager != nullptr);
    auto* page_file = file_manager->find_table_file(2);
    CHECK(page_file != nullptr);
    // Page 0 is the file header. At least two data pages prove a real cross-page scan.
    CHECK(page_file->page_count() >= 3);
    return true;
}

bool test_sql_multipage_restart_acceptance() {
    TemporaryDirectory data_dir{"tinydbms-integration-multipage-restart"};
    ExpectedRows expected;
    for (std::int32_t id = 0; id < 12; ++id) {
        expected.emplace(id, make_payload(id));
    }

    {
        Database db1;
        CHECK(!db1.open({data_dir.path().string()}).error.has_value());
        CHECK(execute_command(
            db1, "CREATE TABLE multipage_test (id INT, payload VARCHAR);", 0));

        std::string insert = "INSERT INTO multipage_test VALUES ";
        for (const auto& [id, payload] : expected) {
            if (id != 0) {
                insert += ", ";
            }
            insert += "(" + std::to_string(id) + ", '" + payload + "')";
        }
        insert += ";";
        CHECK(execute_command(db1, std::move(insert), expected.size()));
        CHECK(expect_rows(db1, "SELECT * FROM multipage_test;", expected));

        ExpectedRows where_rows;
        for (const auto& [id, payload] : expected) {
            if (id >= 8) {
                where_rows.emplace(id, payload);
            }
        }
        CHECK(expect_rows(db1, "SELECT * FROM multipage_test WHERE id >= 8;", where_rows));
        CHECK(execute_command(db1, "DELETE FROM multipage_test WHERE id = 3;", 1));
        CHECK(execute_command(db1, "DELETE FROM multipage_test WHERE id = 9;", 1));
        expected.erase(3);
        expected.erase(9);
        CHECK(expect_rows(db1, "SELECT * FROM multipage_test;", expected));
        CHECK(expect_data_pages_for_first_user_table());
        CHECK(!db1.close().error.has_value());
    }

    {
        Database db2;
        CHECK(!db2.open({data_dir.path().string()}).error.has_value());
        CHECK(expect_rows(db2, "SELECT * FROM multipage_test;", expected));
        CHECK(expect_data_pages_for_first_user_table());

        const auto inserted_id = 12;
        expected.emplace(inserted_id, make_payload(inserted_id));
        CHECK(execute_command(
            db2,
            "INSERT INTO multipage_test VALUES (12, '" + expected.at(inserted_id) + "');",
            1));
        CHECK(expect_rows(db2, "SELECT * FROM multipage_test WHERE id >= 12;",
                          ExpectedRows{{inserted_id, expected.at(inserted_id)}}));
        CHECK(expect_rows(db2, "SELECT * FROM multipage_test;", expected));
        CHECK(!db2.close().error.has_value());
    }

    {
        Database db3;
        CHECK(!db3.open({data_dir.path().string()}).error.has_value());
        CHECK(expect_rows(db3, "SELECT * FROM multipage_test;", expected));
        CHECK(expect_data_pages_for_first_user_table());
        CHECK(!db3.close().error.has_value());
    }
    return true;
}

bool test_persistent_cli_lifecycle() {
    TemporaryDirectory data_dir{"tinydbms-integration-persist"};
    const std::string utf8_name = "\xE6\x9D\x8E\xE9\x9B\xB7";

    std::string script;
    script += "CREATE TABLE students (id INT, name VARCHAR);\n";
    script += "INSERT INTO students VALUES (1, 'Alice'), (2, '";
    script += utf8_name;
    script += "');\n";
    script += "SELECT name FROM students WHERE id = 2;\n";
    script += "DELETE FROM students WHERE id = 1;\n";
    script += "SELECT * FROM students;\n";

    const InvocationResult first = invoke_cli(data_dir.path(), std::move(script), false);
    CHECK(first.exit_code == 0);
    CHECK(first.error.empty());
    CHECK(
        first.output ==
        "OK 0\nOK 2\nname\n" + utf8_name + "\nOK 1\nid\tname\n2\t" + utf8_name + "\n");

    const InvocationResult reopened =
        invoke_cli(data_dir.path(), "SELECT * FROM students;\n", false);
    CHECK(reopened.exit_code == 0);
    CHECK(reopened.error.empty());
    CHECK(reopened.output == "id\tname\n2\t" + utf8_name + "\n");
    return true;
}

bool test_batch_stops_on_compile_error() {
    TemporaryDirectory data_dir{"tinydbms-integration-batch"};
    const InvocationResult prepared = invoke_cli(
        data_dir.path(),
        "CREATE TABLE items (id INT);\n"
        "INSERT INTO items VALUES (1);\n",
        false);
    CHECK(prepared.exit_code == 0);
    CHECK(prepared.output == "OK 0\nOK 1\n");
    CHECK(prepared.error.empty());

    const InvocationResult failed = invoke_cli(
        data_dir.path(),
        "SELECT missing FROM items;\n"
        "INSERT INTO items VALUES (2);\n",
        false);
    CHECK(failed.exit_code == 1);
    CHECK(failed.output.empty());
    // 诊断升级后保留语义阶段标签和脚本绝对范围，首错后的语句被跳过。
    // 分段范围含前导换行：跳过范围从上一句分号之后开始，到本语句分号为止。
    CHECK(failed.error.rfind("ERROR semantic 1:", 0) == 0);
    CHECK(failed.error.find("SKIPPED 1:27-2:30 policy") != std::string::npos);

    const InvocationResult persisted = invoke_cli(data_dir.path(), "SELECT * FROM items;\n", false);
    CHECK(persisted.exit_code == 0);
    CHECK(persisted.error.empty());
    CHECK(persisted.output == "id\n1\n");
    return true;
}

bool test_utf8_data_directory_lifecycle() {
    // 数据目录路径本身使用 UTF-8 中文，覆盖真实 storage 的非 ASCII 路径。
    const std::string directory_name =
        "tinydbms-integration-\xE6\x95\xB0\xE6\x8D\xAE\xE7\x9B\xAE\xE5\xBD\x95";
    TemporaryDirectory data_dir{directory_name};
    const std::string utf8_value = "\xE6\xB5\x8B\xE8\xAF\x95";

    const InvocationResult created = invoke_cli(
        data_dir.path(),
        "CREATE TABLE notes (id INT, body VARCHAR);\n"
        "INSERT INTO notes VALUES (1, '" + utf8_value + "');\n"
        "SELECT body FROM notes WHERE id = 1;\n",
        false);
    CHECK(created.exit_code == 0);
    CHECK(created.error.empty());
    CHECK(created.output == "OK 0\nOK 1\nbody\n" + utf8_value + "\n");

    const InvocationResult reopened = invoke_cli(
        data_dir.path(), "SELECT body FROM notes;\n", false);
    CHECK(reopened.exit_code == 0);
    CHECK(reopened.error.empty());
    CHECK(reopened.output == "body\n" + utf8_value + "\n");
    return true;
}

bool test_batch_reports_semantic_error() {
    TemporaryDirectory data_dir{"tinydbms-integration-semantic"};
    const InvocationResult prepared = invoke_cli(
        data_dir.path(), "CREATE TABLE items (id INT);\n", false);
    CHECK(prepared.exit_code == 0);
    CHECK(prepared.error.empty());
    CHECK(prepared.output == "OK 0\n");

    const InvocationResult duplicated = invoke_cli(
        data_dir.path(),
        "CREATE TABLE items (id INT);\n"
        "INSERT INTO items VALUES (1);\n",
        false);
    CHECK(duplicated.exit_code == 1);
    CHECK(duplicated.output.empty());
    // 重复建表由 compiler 语义阶段拒绝，位置由 core 换算为整段输入坐标。
    CHECK(duplicated.error.rfind("ERROR semantic 1:", 0) == 0);
    CHECK(duplicated.error.find("SKIPPED 1:29-2:30 policy") != std::string::npos);

    const InvocationResult persisted =
        invoke_cli(data_dir.path(), "SELECT * FROM items;\n", false);
    CHECK(persisted.exit_code == 0);
    CHECK(persisted.error.empty());
    CHECK(persisted.output == "id\n");
    return true;
}

bool test_batch_reports_storage_error_and_stops() {
    TemporaryDirectory data_dir{"tinydbms-integration-storage-error"};
    const InvocationResult prepared = invoke_cli(
        data_dir.path(),
        "CREATE TABLE wide (a VARCHAR, b VARCHAR, c VARCHAR, d VARCHAR, e VARCHAR);\n"
        "CREATE TABLE marker (id INT);\n",
        false);
    CHECK(prepared.exit_code == 0);
    CHECK(prepared.error.empty());
    CHECK(prepared.output == "OK 0\nOK 0\n");

    // 单列 1024 字节合法，5 列合计超过 kMaxRowLogicalBytes，只有 storage 能拒绝。
    const std::string payload(1024, 'x');
    std::string oversized = "INSERT INTO wide VALUES (";
    for (int index = 0; index < 5; ++index) {
        if (index != 0) {
            oversized += ", ";
        }
        oversized += "'" + payload + "'";
    }
    oversized += ");\n";

    const InvocationResult failed = invoke_cli(
        data_dir.path(),
        "INSERT INTO marker VALUES (1);\n" + oversized + "INSERT INTO marker VALUES (2);\n",
        false);
    CHECK(failed.exit_code == 1);
    CHECK(failed.output == "OK 1\n");
    CHECK(failed.error.rfind("ERROR storage ", 0) == 0);

    const InvocationResult persisted =
        invoke_cli(data_dir.path(), "SELECT * FROM marker;\n", false);
    CHECK(persisted.exit_code == 0);
    CHECK(persisted.error.empty());
    CHECK(persisted.output == "id\n1\n");
    return true;
}

bool test_repl_stops_after_oversized_input() {
    TemporaryDirectory data_dir{"tinydbms-integration-repl"};
    std::string input(tinydbms::kMaxSqlBytes + 1, ' ');
    input += "\n";
    input += "CREATE TABLE recovered (id INT);\n";

    const InvocationResult result = invoke_cli(data_dir.path(), std::move(input), true);
    CHECK(result.exit_code == 1);
    // script_error 是 REPL 致命错误：停止读取后续输入，也不执行任何语句。
    CHECK(result.output.empty());
    CHECK(
        result.error.find("ERROR compile 1:1-1:1 SQL text exceeds maximum length\n") !=
        std::string::npos);
    CHECK(result.error.find("tinydbms> ") != std::string::npos);

    const InvocationResult reopened =
        invoke_cli(data_dir.path(), "SELECT * FROM recovered;\n", false);
    CHECK(reopened.exit_code == 1);
    CHECK(reopened.output.empty());
    return true;
}

bool test_open_error_has_storage_exit_status() {
    TemporaryDirectory data_dir{"tinydbms-integration-open"};
    const std::filesystem::path file_path = data_dir.path() / "not-a-directory";
    {
        std::ofstream file{file_path};
        file << "not a database directory\n";
    }

    const InvocationResult result = invoke_cli(file_path, "", false);
    CHECK(result.exit_code == 1);
    CHECK(result.output.empty());
    CHECK(result.error.rfind("ERROR storage ", 0) == 0);
    return true;
}

bool test_json_batch_output_is_parseable() {
    TemporaryDirectory data_dir{"tinydbms-integration-json"};
    const InvocationResult result = invoke_cli(
        data_dir.path(),
        "create table users (id int, name varchar);\n"
        "insert into users values (1, 'a'), (2, 'b');\n"
        "select * from users;\n",
        false,
        {"--format", "json"});
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    // JSON 模式下 stdout 只有结果对象，提示符与诊断都不在这里。
    CHECK(result.output.find("tinydbms> ") == std::string::npos);

    const std::vector<std::string_view> lines = tinydbms::testing::json_lines(result.output);
    CHECK(lines.size() == 3);

    std::vector<tinydbms::testing::JsonNode> nodes;
    for (const std::string_view line : lines) {
        std::size_t offset = 0;
        const std::optional<tinydbms::testing::JsonNode> parsed =
            tinydbms::testing::json_parse(line, offset);
        CHECK(parsed.has_value());
        nodes.push_back(*parsed);
    }

    const auto* first_type = tinydbms::testing::json_member(nodes[0], "type");
    const auto* first_rows = tinydbms::testing::json_member(nodes[0], "affected_rows");
    CHECK(first_type != nullptr && first_type->text == "command");
    CHECK(first_rows != nullptr && first_rows->integer == 0);

    const auto* inserted = tinydbms::testing::json_member(nodes[1], "affected_rows");
    CHECK(inserted != nullptr && inserted->integer == 2);

    const auto* query_type = tinydbms::testing::json_member(nodes[2], "type");
    const auto* row_count = tinydbms::testing::json_member(nodes[2], "row_count");
    const auto* columns = tinydbms::testing::json_member(nodes[2], "columns");
    const auto* rows = tinydbms::testing::json_member(nodes[2], "rows");
    CHECK(query_type != nullptr && query_type->text == "query");
    CHECK(row_count != nullptr && row_count->integer == 2);
    CHECK(columns != nullptr && columns->items.size() == 2);
    CHECK(rows != nullptr && rows->items.size() == 2);
    // rows 长度必须与 row_count 一致，且每行宽度等于列数。
    CHECK(rows->items[0].items.size() == columns->items.size());
    const auto* name_value =
        &rows->items[0].items[1];
    CHECK(name_value->kind == tinydbms::testing::JsonNode::Kind::kString);
    return true;
}

bool test_plan_mode_real_chain_has_no_side_effects() {
    TemporaryDirectory data_dir{"tinydbms-integration-plan"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "create table dept (id int, name varchar);\n"
        "create table emp (id int, dept int, amount bigint, note varchar);\n"
        "insert into emp values (1, 1, 10, 'x'), (2, 2, 20, null);\n",
        false);
    CHECK(setup.exit_code == 0);

    const std::vector<std::string> before = list_data_files(data_dir.path());
    CHECK(!before.empty());

    const InvocationResult planned = invoke_cli(
        data_dir.path(),
        "create table extra (id int);\n"
        "select * from emp where id > 10;\n"
        "select dept, sum(amount) from emp where note is null group by dept;\n"
        "select dept.name, emp.note from dept join emp on dept.id = emp.dept;\n",
        false,
        {"--plan"});
    CHECK(planned.exit_code == 0);
    CHECK(planned.error.empty());
    CHECK(planned.output.find("CreateTable extra(id INT)") != std::string::npos);
    CHECK(planned.output.find(
              "QueryPlan outputs=[id:INT, dept:INT, amount:BIGINT, note:VARCHAR]") !=
          std::string::npos);
    CHECK(planned.output.find("SeqScan emp") != std::string::npos);
    CHECK(planned.output.find("Aggregate group=[slot1] calls=[SUM(amount#2) -> slot4 BIGINT]") !=
          std::string::npos);
    CHECK(planned.output.find("Join kind=INNER condition=(dept.id#0 = emp.dept#3)") !=
          std::string::npos);
    // 计划模式不写任何文件、不改动数据页。
    CHECK(list_data_files(data_dir.path()) == before);

    // 同一脚本里 CREATE 之后看不到新表：零副作用的必然结果，按编译错误报告。
    const InvocationResult self_reference = invoke_cli(
        data_dir.path(),
        "create table fresh (id int);\nselect * from fresh;\n",
        false,
        {"--plan", "--format", "json"});
    CHECK(self_reference.exit_code == 1);
    CHECK(self_reference.error.find("\"kind\":\"compile\"") != std::string::npos);
    CHECK(self_reference.error.find("does not exist") != std::string::npos);
    CHECK(list_data_files(data_dir.path()) == before);

    // --plan 与 --format json 组合：计划走普通的 query 对象，列名固定为 plan。
    const std::vector<std::string_view> plan_lines =
        tinydbms::testing::json_lines(self_reference.output);
    CHECK(plan_lines.size() == 1);
    std::size_t plan_offset = 0;
    const std::optional<tinydbms::testing::JsonNode> plan_object =
        tinydbms::testing::json_parse(plan_lines.front(), plan_offset);
    CHECK(plan_object.has_value());
    const auto* plan_columns = tinydbms::testing::json_member(*plan_object, "columns");
    CHECK(plan_columns != nullptr && plan_columns->items.size() == 1);
    CHECK(plan_columns->items.front().members.size() == 2);
    CHECK(plan_columns->items.front().members.front().second.text == "plan");

    // 数据本身没有被计划模式动过。
    const InvocationResult verify = invoke_cli(
        data_dir.path(), "select * from emp;\n", false, {"--format", "json"});
    CHECK(verify.exit_code == 0);
    const std::vector<std::string_view> lines = tinydbms::testing::json_lines(verify.output);
    CHECK(lines.size() == 1);
    std::size_t offset = 0;
    const std::optional<tinydbms::testing::JsonNode> parsed =
        tinydbms::testing::json_parse(lines.front(), offset);
    CHECK(parsed.has_value());
    const auto* row_count = tinydbms::testing::json_member(*parsed, "row_count");
    CHECK(row_count != nullptr && row_count->integer == 2);
    return true;
}

bool test_real_chain_cancellation_leaves_data_unchanged() {
    TemporaryDirectory data_dir{"tinydbms-integration-cancel"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "CREATE TABLE emp (id INT, amount BIGINT);\n"
        "INSERT INTO emp VALUES (1, 10), (2, 20), (3, 30);\n",
        false);
    CHECK(setup.exit_code == 0);
    CHECK(setup.output == "OK 0\nOK 3\n");

    const std::vector<std::string> before = list_data_files(data_dir.path());
    CHECK(!before.empty());

    // 真实 compiler + storage：请求前已置位的令牌让整段脚本在编译前结束。
    {
        Database database;
        CHECK(!database.open({data_dir.path().string()}).error.has_value());
        tinydbms::core::CancelToken cancel;
        cancel.request_cancel();
        ExecuteScriptRequest request{
            "INSERT INTO emp VALUES (4, 40);\nSELECT * FROM emp;\n"};
        request.cancel = cancel;
        const auto result = database.execute_script(request);
        CHECK(!result.script_error.has_value());
        CHECK(result.statements.size() == 2);
        for (const auto& statement : result.statements) {
            CHECK(
                statement.status() ==
                tinydbms::core::StatementStatus::kCancelled);
            CHECK(!statement.outcome().has_value());
        }
        CHECK(!database.close().error.has_value());
    }

    // 取消不产生任何副作用：文件清单与数据都保持不变。
    CHECK(list_data_files(data_dir.path()) == before);
    const InvocationResult verify =
        invoke_cli(data_dir.path(), "SELECT * FROM emp;\n", false);
    CHECK(verify.exit_code == 0);
    CHECK(verify.error.empty());
    CHECK(verify.output == "id\tamount\n1\t10\n2\t20\n3\t30\n");
    return true;
}

// U6：真实 compiler + storage 上并发执行读语句与写语句。core 串行化后，
// 两个脚本各自完整返回、结果互不污染，数据文件重新打开后仍然一致。
bool test_real_chain_concurrent_scripts_are_serialized() {
    TemporaryDirectory data_dir{"tinydbms-integration-concurrency"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "CREATE TABLE emp (id INT, amount BIGINT);\n"
        "INSERT INTO emp VALUES (1, 10), (2, 20), (3, 30);\n",
        false);
    CHECK(setup.exit_code == 0);
    CHECK(setup.output == "OK 0\nOK 3\n");

    {
        Database database;
        CHECK(!database.open({data_dir.path().string()}).error.has_value());

        ExecuteScriptResult read_result;
        ExecuteScriptResult second_read_result;
        ExecuteScriptResult write_result;
        std::thread reader{[&database, &read_result] {
            read_result = database.execute_script(
                ExecuteScriptRequest{"SELECT * FROM emp;"});
        }};
        std::thread second_reader{[&database, &second_read_result] {
            second_read_result = database.execute_script(
                ExecuteScriptRequest{"SELECT * FROM emp;"});
        }};
        std::thread writer{[&database, &write_result] {
            write_result = database.execute_script(
                ExecuteScriptRequest{"INSERT INTO emp VALUES (4, 40);"});
        }};
        reader.join();
        second_reader.join();
        writer.join();

        for (const ExecuteScriptResult* read : {&read_result, &second_read_result}) {
            CHECK(!read->script_error.has_value());
            CHECK(read->statements.size() == 1);
            const auto* query =
                std::get_if<QueryResult>(&read->statements.front().outcome()->outcome);
            CHECK(query != nullptr);
            // 每条读语句可能排在写语句之前或之后，但只可能是这两种完整结果。
            CHECK(query->rows.size() == 3 || query->rows.size() == 4);
        }

        CHECK(!write_result.script_error.has_value());
        CHECK(write_result.statements.size() == 1);
        const auto* command =
            std::get_if<CommandResult>(&write_result.statements.front().outcome()->outcome);
        CHECK(command != nullptr);
        CHECK(!command->error.has_value());
        CHECK(command->affected_rows == 1);

        CHECK(!database.close().error.has_value());
    }

    // 重新打开数据目录：写入确实落盘，没有因为并发调用而丢行或损坏。
    const InvocationResult verify =
        invoke_cli(data_dir.path(), "SELECT * FROM emp;\n", false);
    CHECK(verify.exit_code == 0);
    CHECK(verify.error.empty());
    CHECK(verify.output == "id\tamount\n1\t10\n2\t20\n3\t30\n4\t40\n");
    return true;
}

// U6：真实链路上并发 close 与执行。close 与执行互斥，执行要么完整返回，
// 要么在 close 之后返回与顺序执行一致的"database is not open"；不允许出现半截结果、
// 存储错误或清理失败，数据文件也必须保持可重新打开。
bool test_real_chain_concurrent_close_is_clean() {
    TemporaryDirectory data_dir{"tinydbms-integration-close-race"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "CREATE TABLE emp (id INT, amount BIGINT);\n"
        "INSERT INTO emp VALUES (1, 10), (2, 20), (3, 30);\n",
        false);
    CHECK(setup.exit_code == 0);
    CHECK(setup.output == "OK 0\nOK 3\n");

    constexpr std::size_t kRounds = 8;
    std::size_t executed_rounds = 0;
    std::size_t closed_rounds = 0;
    for (std::size_t round = 0; round < kRounds; ++round) {
        Database database;
        CHECK(!database.open({data_dir.path().string()}).error.has_value());

        ExecuteScriptResult execution;
        bool close_failed = false;
        std::thread executing_thread{[&database, &execution] {
            execution = database.execute_script(
                ExecuteScriptRequest{"SELECT * FROM emp;"});
        }};
        std::thread closing_thread{[&database, &close_failed] {
            close_failed = database.close().error.has_value();
        }};
        executing_thread.join();
        closing_thread.join();

        // close 在两个顺序下都必须成功，且不能留下需要重试的清理状态。
        CHECK(!close_failed);
        if (execution.script_error.has_value()) {
            CHECK(execution.script_error->kind == tinydbms::core::ErrorKind::kExecute);
            CHECK(execution.statements.empty());
            ++closed_rounds;
        } else {
            CHECK(execution.statements.size() == 1);
            const auto* query =
                std::get_if<QueryResult>(&execution.statements.front().outcome()->outcome);
            CHECK(query != nullptr);
            CHECK(query->rows.size() == 3);
            ++executed_rounds;
        }
    }
    // 每轮都必须落到两种合法结果之一。这里不断言"执行至少赢一轮"：谁先取得锁取决于
    // 调度，断言它会让用例偶发失败；"close 确实等在途执行结束"的顺序取证由 fake 侧
    // 用例确定性地完成（test_concurrent_close_waits_for_in_flight_execution 用 Gate
    // 把执行流停在 storage 内部后再发起 close）。
    CHECK(executed_rounds + closed_rounds == kRounds);

    const InvocationResult verify =
        invoke_cli(data_dir.path(), "SELECT * FROM emp;\n", false);
    CHECK(verify.exit_code == 0);
    CHECK(verify.error.empty());
    CHECK(verify.output == "id\tamount\n1\t10\n2\t20\n3\t30\n");
    return true;
}

bool test_real_chain_row_limit_is_reported_and_recoverable() {
    TemporaryDirectory data_dir{"tinydbms-integration-row-limit"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "CREATE TABLE emp (id INT, amount BIGINT);\n"
        "INSERT INTO emp VALUES (1, 10), (2, 20), (3, 30);\n"
        "CREATE TABLE marker (id INT);\n"
        "INSERT INTO marker VALUES (7);\n",
        false);
    CHECK(setup.exit_code == 0);
    CHECK(setup.output == "OK 0\nOK 3\nOK 0\nOK 1\n");
    CHECK(setup.error.empty());

    const std::vector<std::string> before = list_data_files(data_dir.path());
    CHECK(!before.empty());

    // 上限按物化节点计：emp 扫描 3 行，上限 2 必然超限。退出码 1，错误带当前上限与
    // suggestion，且不写任何文件。
    const InvocationResult limited = invoke_cli(
        data_dir.path(), "SELECT * FROM emp;\n", false, {"--max-rows", "2"});
    CHECK(limited.exit_code == 1);
    CHECK(limited.output.empty());
    CHECK(limited.error.rfind("ERROR execute 1:", 0) == 0);
    CHECK(
        limited.error.find(
            "query materialization exceeds the maximum row count (limit 2)") !=
        std::string::npos);
    CHECK(
        limited.error.find(
            "SUGGESTION raise max_query_rows or narrow the query") !=
        std::string::npos);
    CHECK(list_data_files(data_dir.path()) == before);

    // REPL：超限语句报错后会话仍可用，只扫描 1 行的语句在同一上限下正常返回；
    // 有语句失败时退出码为 1。
    const InvocationResult repl = invoke_cli(
        data_dir.path(),
        "SELECT * FROM emp;\nSELECT * FROM marker;\n",
        true,
        {"--max-rows", "2"});
    CHECK(repl.exit_code == 1);
    CHECK(repl.error.find("(limit 2)") != std::string::npos);
    CHECK(repl.output == "id\n7\n");
    CHECK(list_data_files(data_dir.path()) == before);

    // 默认上限下数据完好：三条记录都在，文件集合也没有变化。
    const InvocationResult verify = invoke_cli(
        data_dir.path(), "SELECT * FROM emp;\n", false);
    CHECK(verify.exit_code == 0);
    CHECK(verify.error.empty());
    CHECK(verify.output == "id\tamount\n1\t10\n2\t20\n3\t30\n");
    CHECK(list_data_files(data_dir.path()) == before);
    return true;
}

// P3：--stats 走真实 compiler/storage 链路。stdout 与关闭时逐字节相同，
// stderr 每次 execute_script 追加一条 BUFFER 快照；快照在 close 之前取，
// 所以会话内计数只增不减，重复查询至少能观察到新的命中。
bool test_real_chain_stats_reporting() {
    TemporaryDirectory data_dir{"tinydbms-integration-stats"};
    const InvocationResult setup = invoke_cli(
        data_dir.path(),
        "CREATE TABLE emp (id INT, amount BIGINT);\n"
        "INSERT INTO emp VALUES (1, 10), (2, 20), (3, 30);\n",
        false);
    CHECK(setup.exit_code == 0);
    CHECK(setup.output == "OK 0\nOK 3\n");

    const std::vector<std::string> before = list_data_files(data_dir.path());
    const InvocationResult plain = invoke_cli(
        data_dir.path(), "SELECT * FROM emp;\n", false);
    CHECK(plain.exit_code == 0);
    CHECK(plain.error.empty());

    // 批处理：一次调用一条快照，stdout 不受观测开关影响。
    const InvocationResult stats = invoke_cli(
        data_dir.path(), "SELECT * FROM emp;\n", false, {"--stats"});
    CHECK(stats.exit_code == 0);
    CHECK(stats.output == plain.output);
    const auto batch_snapshot = parse_buffer_line(stats.error);
    CHECK(batch_snapshot.has_value());
    CHECK(batch_snapshot->fetch_count > 0);
    CHECK(batch_snapshot->hit_count + batch_snapshot->miss_count ==
          batch_snapshot->fetch_count);
    CHECK(batch_snapshot->miss_rate >= 0.0 && batch_snapshot->miss_rate <= 100.0);
    CHECK(stats.error.find("BUFFER fetch=") == stats.error.rfind("BUFFER fetch="));
    CHECK(list_data_files(data_dir.path()) == before);

    // REPL：每行一条快照，计数在同一会话内单调累加，重复查询新增命中。
    const InvocationResult repl = invoke_cli(
        data_dir.path(),
        "SELECT * FROM emp;\nSELECT * FROM emp;\n",
        true,
        {"--stats"});
    CHECK(repl.exit_code == 0);
    CHECK(repl.output == "id\tamount\n1\t10\n2\t20\n3\t30\nid\tamount\n1\t10\n2\t20\n3\t30\n");
    const std::vector<BufferSnapshot> snapshots = parse_buffer_lines(repl.error);
    CHECK(snapshots.size() == 2);
    CHECK(snapshots[1].fetch_count > snapshots[0].fetch_count);
    CHECK(snapshots[1].hit_count > snapshots[0].hit_count);
    CHECK(snapshots[1].hit_count + snapshots[1].miss_count == snapshots[1].fetch_count);

    // 语句失败时仍然给出快照，且不改变退出码与 stdout。
    const InvocationResult failing_plain = invoke_cli(
        data_dir.path(), "SELECT * FROM missing;\n", false);
    const InvocationResult failing_stats = invoke_cli(
        data_dir.path(), "SELECT * FROM missing;\n", false, {"--stats"});
    CHECK(failing_plain.exit_code == 1);
    CHECK(failing_stats.exit_code == failing_plain.exit_code);
    CHECK(failing_stats.output == failing_plain.output);
    CHECK(parse_buffer_line(failing_stats.error).has_value());
    CHECK(list_data_files(data_dir.path()) == before);
    return true;
}

}  // namespace

int main() {
    try {
        return test_persistent_cli_lifecycle() &&
                test_batch_stops_on_compile_error() &&
                test_utf8_data_directory_lifecycle() &&
                test_batch_reports_semantic_error() &&
                test_batch_reports_storage_error_and_stops() &&
                test_repl_stops_after_oversized_input() &&
                test_open_error_has_storage_exit_status() &&
                test_sql_multipage_restart_acceptance() &&
                test_json_batch_output_is_parseable() &&
                test_plan_mode_real_chain_has_no_side_effects() &&
                test_real_chain_cancellation_leaves_data_unchanged() &&
                test_real_chain_concurrent_scripts_are_serialized() &&
                test_real_chain_concurrent_close_is_clean() &&
                test_real_chain_row_limit_is_reported_and_recoverable() &&
                test_real_chain_stats_reporting()
            ? 0
            : 1;
    } catch (const std::exception& exception) {
        std::cerr << "integration test exception: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "integration test raised an unknown exception\n";
        return 1;
    }
}
