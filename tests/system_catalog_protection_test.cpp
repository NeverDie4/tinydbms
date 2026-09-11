#include "tinydbms/core.hpp"
#include "tinydbms/storage.hpp"

#include <chrono>
#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using tinydbms::Type;
using tinydbms::Value;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::QueryResult;
using namespace tinydbms::storage;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                            \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("tinydbms-system-catalog-protection-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error{"cannot create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::optional<QueryResult> query(Database& database, const std::string& sql) {
    const auto result = database.execute_script(ExecuteScriptRequest{sql});
    if (result.outcomes.size() != 1) {
        return std::nullopt;
    }
    if (const auto* query_result = std::get_if<QueryResult>(&result.outcomes.front().outcome)) {
        return *query_result;
    }
    return std::nullopt;
}

bool command_failed(Database& database, const std::string& sql) {
    const auto result = database.execute_script(ExecuteScriptRequest{sql});
    if (result.outcomes.size() != 1) {
        return false;
    }
    const auto* command = std::get_if<CommandResult>(&result.outcomes.front().outcome);
    return std::holds_alternative<tinydbms::core::Error>(result.outcomes.front().outcome) ||
        (command != nullptr && command->error.has_value());
}

bool command_succeeded(Database& database, const std::string& sql) {
    const auto result = database.execute_script(ExecuteScriptRequest{sql});
    if (result.outcomes.size() != 1) {
        return false;
    }
    const auto* command = std::get_if<CommandResult>(&result.outcomes.front().outcome);
    return command != nullptr && !command->error.has_value();
}

std::vector<Record> scan_storage_table(tinydbms::TableId table_id) {
    const auto opened = open_table({table_id});
    if (opened.error.has_value() || !opened.cursor.has_value()) {
        throw std::runtime_error{"cannot open system catalog table"};
    }
    std::vector<Record> records;
    while (true) {
        const auto next = scan_next({*opened.cursor});
        if (next.error.has_value()) {
            throw std::runtime_error{"cannot scan system catalog table"};
        }
        if (!next.record.has_value()) {
            break;
        }
        records.push_back(*next.record);
    }
    if (close_cursor({*opened.cursor}).error.has_value()) {
        throw std::runtime_error{"cannot close system catalog cursor"};
    }
    return records;
}

const std::array<tinydbms::core::ColumnHeader, 3> kSystemTablesHeaders{{
    {"table_id", Type::kVarchar}, {"table_name", Type::kVarchar}, {"column_count", Type::kInt}}};
const std::array<tinydbms::core::ColumnHeader, 4> kSystemColumnsHeaders{{
    {"table_id", Type::kVarchar}, {"column_ordinal", Type::kInt},
    {"column_name", Type::kVarchar}, {"column_type", Type::kVarchar}}};
const std::array<tinydbms::core::ColumnHeader, 1> kTableNameHeader{
    tinydbms::core::ColumnHeader{"table_name", Type::kVarchar}};

bool has_headers(const QueryResult& query, std::span<const tinydbms::core::ColumnHeader> expected) {
    if (query.columns.size() != expected.size()) {
        return false;
    }
    std::size_t index = 0;
    for (const tinydbms::core::ColumnHeader& header : expected) {
        if (query.columns[index].name != header.name ||
            query.columns[index].type != header.type) {
            return false;
        }
        ++index;
    }
    return true;
}

bool has_only_student_schema(const QueryResult& tables, const QueryResult& columns) {
    if (!has_headers(tables, kSystemTablesHeaders) ||
        tables.rows.size() != 1 || tables.rows[0].size() != 3 ||
        !has_headers(columns, kSystemColumnsHeaders) ||
        columns.rows.size() != 2) {
        return false;
    }
    return tables.rows[0][0].data == Value{std::string{"2"}}.data &&
        tables.rows[0][1].data == Value{std::string{"student"}}.data &&
        tables.rows[0][2].data == Value{std::int32_t{2}}.data &&
        columns.rows[0][0].data == Value{std::string{"2"}}.data &&
        columns.rows[0][1].data == Value{std::int32_t{0}}.data &&
        columns.rows[0][2].data == Value{std::string{"id"}}.data &&
        columns.rows[0][3].data == Value{std::string{"INT32"}}.data &&
        columns.rows[1][0].data == Value{std::string{"2"}}.data &&
        columns.rows[1][1].data == Value{std::int32_t{1}}.data &&
        columns.rows[1][2].data == Value{std::string{"name"}}.data &&
        columns.rows[1][3].data == Value{std::string{"VARCHAR"}}.data;
}

bool test_system_catalog_read_and_write_protection() {
    TemporaryDirectory directory;
    Database database;
    CHECK(!database.open({directory.path().string()}).error.has_value());
    CHECK(command_succeeded(database, "CREATE TABLE student (id INT, name VARCHAR);"));

    auto tables = query(database, "SELECT * FROM tdb_sys_tables;");
    auto columns = query(database, "SELECT * FROM tdb_sys_columns;");
    CHECK(tables.has_value() && columns.has_value());
    CHECK(has_only_student_schema(*tables, *columns));

    const auto projected = query(
        database, "SELECT table_name FROM tdb_sys_tables WHERE table_id = '2';");
    CHECK(projected.has_value() && has_headers(*projected, kTableNameHeader) &&
        projected->rows.size() == 1 && projected->rows[0].size() == 1 &&
        projected->rows[0][0].data == Value{std::string{"student"}}.data);

    CHECK(command_failed(database, "CREATE TABLE tdb_sys_tables (id INT);"));
    CHECK(command_failed(database, "CREATE TABLE tdb_sys_columns (id INT);"));
    CHECK(command_failed(database, "INSERT INTO tdb_sys_tables VALUES ('99', 'forbidden', 1);"));
    CHECK(command_failed(database, "INSERT INTO tdb_sys_columns VALUES ('99', 0, 'id', 'INT32');"));
    CHECK(command_failed(database, "DELETE FROM tdb_sys_tables;"));
    CHECK(command_failed(database, "DELETE FROM tdb_sys_columns;"));

    const std::vector<Record> table_records = scan_storage_table(0);
    const std::vector<Record> column_records = scan_storage_table(1);
    CHECK(table_records.size() == 1 && column_records.size() == 2);
    CHECK(insert({0, {{Value{std::string{"99"}}, Value{std::string{"forbidden"}}, Value{std::int32_t{1}}}}}).error.has_value());
    CHECK(insert({1, {{Value{std::string{"99"}}, Value{std::int32_t{0}}, Value{std::string{"id"}}, Value{std::string{"INT32"}}}}}).error.has_value());
    CHECK(delete_records({0, {table_records.front().rid}}).error.has_value());
    CHECK(delete_records({1, {column_records.front().rid}}).error.has_value());
    CHECK(scan_storage_table(0).size() == 1 && scan_storage_table(1).size() == 2);

    tables = query(database, "SELECT * FROM tdb_sys_tables;");
    columns = query(database, "SELECT * FROM tdb_sys_columns;");
    CHECK(tables.has_value() && columns.has_value() && has_only_student_schema(*tables, *columns));
    CHECK(!database.close().error.has_value());

    Database reopened;
    CHECK(!reopened.open({directory.path().string()}).error.has_value());
    tables = query(reopened, "SELECT * FROM tdb_sys_tables;");
    columns = query(reopened, "SELECT * FROM tdb_sys_columns;");
    CHECK(tables.has_value() && columns.has_value() && has_only_student_schema(*tables, *columns));
    CHECK(!reopened.close().error.has_value());
    return true;
}

}  // namespace

int main() {
    try {
        return test_system_catalog_read_and_write_protection() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
