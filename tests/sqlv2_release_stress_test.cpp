#include "tinydbms/core.hpp"
#include "tinydbms/storage.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementResult;
using tinydbms::core::StatementStatus;

constexpr std::uint32_t kSeed = 20260929U;
constexpr std::size_t kScriptCount = 500U;

void check(
    bool condition,
    std::source_location at = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error(
            "sqlv2-release-stress check failed at line " +
            std::to_string(at.line()));
    }
}

struct TemporaryDatabase {
    std::filesystem::path path;

    explicit TemporaryDatabase(std::size_t suffix)
        : path(std::filesystem::temp_directory_path() /
               ("tinydbms-release-stress-" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                '-' + std::to_string(suffix))) {}

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string join_script(const std::vector<std::string>& statements) {
    std::string script;
    for (const std::string& statement : statements) {
        script += statement;
        script += ";\n";
    }
    return script;
}

const QueryResult& query_of(const StatementResult& statement) {
    check(statement.outcome.has_value());
    return std::get<QueryResult>(statement.outcome->outcome);
}

const Error& error_of(const StatementResult& statement) {
    check(statement.outcome.has_value());
    return std::get<Error>(statement.outcome->outcome);
}

std::vector<std::string> workflow_prefix() {
    return {
        "CREATE TABLE users (id INT NOT NULL,name VARCHAR NOT NULL,active BOOLEAN NULL)",
        "CREATE TABLE orders (id BIGINT NOT NULL,user_id INT NOT NULL,amount DOUBLE NULL)",
        "CREATE TABLE items (id INT NOT NULL,order_id BIGINT NOT NULL,note VARCHAR NULL)",
        "CREATE TABLE metrics (user_id INT NOT NULL,score INT NULL)",
        "INSERT INTO users VALUES (1,'alice',TRUE),(2,'bob',FALSE),(3,'carol',NULL)",
        "INSERT INTO orders VALUES (101,1,12.5),(102,1,7.5),(103,2,NULL)",
        "INSERT INTO items VALUES (1001,101,'first'),(1002,102,NULL),(1003,103,'third')",
        "INSERT INTO metrics VALUES (1,10),(1,20),(2,NULL),(3,30)",
        "UPDATE users SET active=FALSE WHERE id=2",
        "UPDATE orders SET amount=NULL WHERE id=102",
        "DELETE FROM items WHERE id=1002",
        "SELECT name,active FROM users ORDER BY id",
        "SELECT id FROM orders WHERE amount IS NULL ORDER BY id",
        "SELECT users.name,orders.amount FROM users JOIN orders ON users.id=orders.user_id ORDER BY orders.id",
        "SELECT users.name,items.note FROM users JOIN orders ON users.id=orders.user_id JOIN items ON orders.id=items.order_id ORDER BY users.id",
        "SELECT COUNT(*),COUNT(amount),SUM(amount),AVG(amount),MIN(amount),MAX(amount) FROM orders",
        "SELECT user_id,COUNT(*),SUM(score),AVG(score),MIN(score),MAX(score) FROM metrics GROUP BY user_id ORDER BY user_id",
        "SELECT active,COUNT(*) FROM users GROUP BY active ORDER BY active",
        "SELECT COUNT(*) FROM users",
        "SELECT COUNT(*) FROM orders",
        "SELECT COUNT(*) FROM items",
        "SELECT score FROM metrics ORDER BY score DESC"};
}

std::vector<std::string> recovery_suffix() {
    return {
        "SELETC id FROM users",
        "CREATE TABLE shadow_rows (id INT NOT NULL,score DOUBLE NULL)",
        "INSERT INTO shadow_rows VALUES (1,1.5),(2,NULL)",
        "UPDATE shadow_rows SET score=2.5 WHERE id=1",
        "SELECT id,COUNT(score),SUM(score),AVG(score),MIN(score),MAX(score) FROM shadow_rows GROUP BY id ORDER BY id",
        "SELECT users.name,shadow_rows.score FROM users JOIN shadow_rows ON users.id=shadow_rows.id ORDER BY users.id",
        "SELCT id FROM shadow_rows",
        "SELECT id FROM shadow_rows ORDER BY id"};
}

void verify_executed_prefix(const ExecuteScriptResult& result, std::size_t count) {
    check(result.statements.size() == count);
    check(result.executed_count == count);
    check(!result.first_error_index.has_value());
    check(!result.script_error.has_value());
    for (std::size_t index = 0; index < count; ++index) {
        check(result.statements[index].statement_index == index);
        check(result.statements[index].status == StatementStatus::kExecuted);
        check(result.statements[index].outcome.has_value());
    }

    const QueryResult& aggregate = query_of(result.statements[15]);
    check(aggregate.rows.size() == 1U);
    check(std::get<std::int64_t>(aggregate.rows[0][0].data) == 3);
    check(std::get<std::int64_t>(aggregate.rows[0][1].data) == 1);
    check(std::get<double>(aggregate.rows[0][2].data) == 12.5);
    check(std::get<double>(aggregate.rows[0][3].data) == 12.5);

    const QueryResult& grouped = query_of(result.statements[16]);
    check(grouped.rows.size() == 3U);
    check(std::get<std::int64_t>(grouped.rows[0][1].data) == 2);
    check(std::get<std::int64_t>(grouped.rows[0][2].data) == 30);

    const QueryResult& item_count = query_of(result.statements[20]);
    check(std::get<std::int64_t>(item_count.rows[0][0].data) == 2);
}

void verify_recovery(
    const ExecuteScriptResult& result,
    std::size_t prefix_count,
    std::size_t later_error_index) {
    check(result.first_error_index == prefix_count);
    check(result.executed_count == prefix_count);
    check(!result.script_error.has_value());
    check(result.statements.size() > later_error_index);
    for (std::size_t index = 0; index < result.statements.size(); ++index) {
        const StatementResult& statement = result.statements[index];
        check(statement.statement_index == index);
        if (index < prefix_count) {
            check(statement.status == StatementStatus::kExecuted);
        } else if (index == prefix_count || index == later_error_index) {
            check(statement.status == StatementStatus::kCompileError);
            const Error& error = error_of(statement);
            check(error.suggestion.has_value());
            check(error.fix_it.has_value());
        } else {
            check(statement.status == StatementStatus::kAnalysisOnly);
            check(!statement.outcome.has_value());
        }
    }
}

void verify_real_state_and_shadow_isolation(Database& database) {
    const ExecuteScriptResult real_state = database.execute_script({
        "SELECT COUNT(*) FROM users;SELECT COUNT(*) FROM orders;SELECT COUNT(*) FROM items;"});
    check(real_state.statements.size() == 3U);
    check(real_state.executed_count == 3U);
    check(std::get<std::int64_t>(query_of(real_state.statements[0]).rows[0][0].data) == 3);
    check(std::get<std::int64_t>(query_of(real_state.statements[1]).rows[0][0].data) == 3);
    check(std::get<std::int64_t>(query_of(real_state.statements[2]).rows[0][0].data) == 2);

    const ExecuteScriptResult shadow_probe =
        database.execute_script({"SELECT * FROM shadow_rows;"});
    check(shadow_probe.statements.size() == 1U);
    check(shadow_probe.statements[0].status == StatementStatus::kCompileError);
}

void run_fixed_golden() {
    TemporaryDatabase temporary{0U};
    Database database;
    check(!database.open({temporary.path.string()}).error.has_value());

    std::vector<std::string> statements = workflow_prefix();
    for (std::size_t index = 0; index < 8U; ++index) {
        statements.push_back("SELECT name FROM users ORDER BY id");
    }
    const std::size_t first_error_index = statements.size();
    statements.push_back("SELETC id FROM users");
    statements.push_back("CREATE TABLE golden_shadow (id INT NOT NULL,value DOUBLE NULL)");
    statements.push_back("INSERT INTO golden_shadow VALUES (1,1.0),(2,NULL)");
    statements.push_back("UPDATE golden_shadow SET value=2.0 WHERE id=1");
    statements.push_back("SELECT users.name,COUNT(golden_shadow.id) FROM users JOIN golden_shadow ON users.id=golden_shadow.id GROUP BY users.name ORDER BY users.name");
    for (std::size_t index = 0; index < 20U; ++index) {
        statements.push_back("SELECT id,COUNT(value) FROM golden_shadow GROUP BY id ORDER BY id");
    }
    const std::size_t later_error_index = statements.size();
    statements.push_back("SELCT id FROM golden_shadow");
    statements.push_back("CREATE TABLE golden_tail (id INT)");

    check(statements.size() == 57U);
    const ExecuteScriptResult result = database.execute_script({join_script(statements)});
    verify_recovery(result, first_error_index, later_error_index);
    verify_real_state_and_shadow_isolation(database);
    check(!database.close().error.has_value());
}

void run_mixed_v1_v2_persistence() {
    using namespace tinydbms;
    namespace storage = tinydbms::storage;

    TemporaryDatabase temporary{1001U};
    check(!storage::open_storage({temporary.path.string()}).error.has_value());
    check(!storage::create_table({
        0U,
        "legacy",
        {{"id", Type::kInt, false}, {"name", Type::kVarchar, false}}}).error.has_value());
    check(!storage::close_storage({}).error.has_value());

    {
        std::ofstream metadata{temporary.path / "storage.meta", std::ios::trunc};
        metadata << "TINYDBMS_STORAGE_V1\n"
                    "TABLE 0 legacy\n"
                    "COLUMN INT32 id\n"
                    "COLUMN VARCHAR name\n"
                    "ENDTABLE\nEND\n";
        metadata.close();
        check(!metadata.fail());
    }

    check(!storage::open_storage({temporary.path.string()}).error.has_value());
    check(!storage::insert({
        0U,
        {{Value{std::int32_t{1}}, Value{std::string{"alice"}}},
         {Value{std::int32_t{2}}, Value{std::string{"bob"}}}}}).error.has_value());
    check(!storage::close_storage({}).error.has_value());

    Database database;
    check(!database.open({temporary.path.string()}).error.has_value());
    const ExecuteScriptResult mixed = database.execute_script({
        "CREATE TABLE modern (id INT NOT NULL,legacy_id INT NOT NULL,big BIGINT NOT NULL,"
        "amount DOUBLE NULL,active BOOLEAN NULL,note VARCHAR NULL);"
        "INSERT INTO modern VALUES "
        "(10,1,2147483648,12.5,TRUE,'short'),(11,2,9223372036854775807,NULL,FALSE,'neighbor');"
        "SELECT legacy.name,modern.big,modern.amount FROM legacy JOIN modern "
        "ON legacy.id=modern.legacy_id ORDER BY legacy.id;"
        "SELECT legacy.name,COUNT(modern.id),SUM(modern.amount),AVG(modern.amount) "
        "FROM legacy JOIN modern ON legacy.id=modern.legacy_id "
        "GROUP BY legacy.name ORDER BY legacy.name;"
        "UPDATE legacy SET name='alice-updated-with-a-longer-value' WHERE id=1;"
        "UPDATE modern SET amount=25.5,active=NULL WHERE id=10;"});
    check(mixed.statements.size() == 6U);
    check(mixed.executed_count == 6U);
    check(query_of(mixed.statements[2]).rows.size() == 2U);
    check(query_of(mixed.statements[3]).rows.size() == 2U);

    for (std::size_t cycle = 0; cycle < 102U; ++cycle) {
        std::string value;
        switch (cycle % 6U) {
            case 0U: value = "'s'"; break;
            case 1U: value = "'" + std::string(700U, 'g') + "'"; break;
            case 2U: value = "NULL"; break;
            case 3U: value = "'" + std::string(80U, 'm') + "'"; break;
            case 4U: value = "'" + std::string(1000U, 'l') + "'"; break;
            default: value = "''"; break;
        }
        const ExecuteScriptResult updated = database.execute_script({
            "UPDATE modern SET note=" + value + " WHERE id=10;"});
        check(updated.statements.size() == 1U);
        check(updated.executed_count == 1U);
    }
    check(!database.close().error.has_value());

    std::ifstream metadata_input{temporary.path / "storage.meta"};
    const std::string metadata{
        std::istreambuf_iterator<char>{metadata_input},
        std::istreambuf_iterator<char>{}};
    metadata_input.close();
    check(metadata.find("TABLE 0 legacy V1\n") != std::string::npos);
    check(metadata.find("TABLE 1 modern V2\n") != std::string::npos);

    Database reopened;
    check(!reopened.open({temporary.path.string()}).error.has_value());
    const ExecuteScriptResult persisted = reopened.execute_script({
        "SELECT name FROM legacy ORDER BY id;"
        "SELECT id,note FROM modern ORDER BY id;"
        "DELETE FROM legacy WHERE id=2;"
        "DELETE FROM modern WHERE id=11;"});
    check(persisted.statements.size() == 4U);
    check(std::get<std::string>(query_of(persisted.statements[0]).rows[0][0].data) ==
          "alice-updated-with-a-longer-value");
    const QueryResult& modern_rows = query_of(persisted.statements[1]);
    check(modern_rows.rows.size() == 2U);
    check(std::get<std::string>(modern_rows.rows[0][1].data).empty());
    check(std::get<std::string>(modern_rows.rows[1][1].data) == "neighbor");
    check(!reopened.close().error.has_value());

    Database verified;
    check(!verified.open({temporary.path.string()}).error.has_value());
    const ExecuteScriptResult after_delete = verified.execute_script({
        "SELECT COUNT(*) FROM legacy;SELECT COUNT(*) FROM modern;"});
    check(std::get<std::int64_t>(query_of(after_delete.statements[0]).rows[0][0].data) == 1);
    check(std::get<std::int64_t>(query_of(after_delete.statements[1]).rows[0][0].data) == 1);
    check(!verified.close().error.has_value());
}

}  // namespace

int main() {
    run_fixed_golden();
    run_mixed_v1_v2_persistence();

    std::mt19937 random{kSeed};
    std::size_t statement_count = 57U;
    std::size_t error_injections = 2U;
    std::size_t shadow_creates = 1U;

    for (std::size_t case_index = 0; case_index < kScriptCount; ++case_index) {
        TemporaryDatabase temporary{case_index + 1U};
        Database database;
        check(!database.open({temporary.path.string()}).error.has_value());

        std::vector<std::string> statements = workflow_prefix();
        const std::size_t prefix_count = statements.size();
        const bool inject_error = (random() % 4U) == 0U;
        if (inject_error) {
            const std::vector<std::string> suffix = recovery_suffix();
            statements.insert(statements.end(), suffix.begin(), suffix.end());
            ++shadow_creates;
            error_injections += 2U;
        }
        statement_count += statements.size();

        const ExecuteScriptResult result = database.execute_script({join_script(statements)});
        if (inject_error) {
            verify_recovery(result, prefix_count, prefix_count + 6U);
            verify_real_state_and_shadow_isolation(database);
        } else {
            verify_executed_prefix(result, prefix_count);
        }
        check(!database.close().error.has_value());
    }

    std::cout << "sqlv2 release stress passed: seed=" << kSeed
              << " scripts=" << kScriptCount
              << " statements=" << statement_count
              << " error-injections=" << error_injections
              << " shadow-creates=" << shadow_creates << '\n';
    return 0;
}
