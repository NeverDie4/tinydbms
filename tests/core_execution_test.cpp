#include "tinydbms/core.hpp"
#include "storage_test_access.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

void check(
    bool condition,
    std::source_location at = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error(
            "core-execution check failed at line " + std::to_string(at.line()));
    }
}

#define assert(condition) check(static_cast<bool>(condition))

std::filesystem::path temporary_database_path() {
    return std::filesystem::temp_directory_path() /
        ("tinydbms-core-execution-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

const tinydbms::core::QueryResult& query_of(const tinydbms::core::ExecuteResult& result) {
    return std::get<tinydbms::core::QueryResult>(result.outcome);
}

const tinydbms::core::CommandResult& command_of(const tinydbms::core::ExecuteResult& result) {
    return std::get<tinydbms::core::CommandResult>(result.outcome);
}

const tinydbms::core::ExecuteResult& outcome_of(
    const tinydbms::core::StatementResult& statement) {
    return *statement.outcome;
}

const tinydbms::core::QueryResult& query_of(
    const tinydbms::core::StatementResult& statement) {
    return query_of(outcome_of(statement));
}

const tinydbms::core::CommandResult& command_of(
    const tinydbms::core::StatementResult& statement) {
    return command_of(outcome_of(statement));
}

const tinydbms::core::Error& error_of(
    const tinydbms::core::StatementResult& statement) {
    return std::get<tinydbms::core::Error>(outcome_of(statement).outcome);
}

std::optional<std::size_t> byte_offset_of(
    std::string_view text,
    tinydbms::SourceLocation target) {
    tinydbms::SourceLocation current{1, 1};
    for (std::size_t offset = 0; offset <= text.size(); ++offset) {
        if (current.line == target.line && current.column == target.column) {
            return offset;
        }
        if (offset == text.size()) {
            break;
        }
        if (text[offset] == '\n') {
            ++current.line;
            current.column = 1;
        } else if (text[offset] != '\r') {
            ++current.column;
        }
    }
    return std::nullopt;
}

}  // namespace

int main() {
    using namespace tinydbms;
    using namespace tinydbms::core;

    const auto data_dir = temporary_database_path();
    Database database;
    assert(!database.open({data_dir.string()}).error);

    Database contender;
    const auto contested = contender.open({data_dir.string()});
    assert(contested.error && contested.error->kind == ErrorKind::kExecute);

    const auto created = database.execute_script(
        {"CREATE TABLE student (id INT, name VARCHAR);"});
    assert(created.statements.size() == 1);
    assert(!created.first_error_index.has_value());
    assert(created.executed_count == 1);
    assert(!created.script_error.has_value());
    assert(created.statements[0].status == StatementStatus::kExecuted);
    assert(!command_of(created.statements[0]).error);

    const auto invalid_create = database.execute_script({"CREATE TABLE invalid (id UNKNOWN);"});
    assert(invalid_create.statements.size() == 1);
    assert(std::holds_alternative<Error>(outcome_of(invalid_create.statements[0]).outcome));
    const auto valid_after_failure = database.execute_script({"CREATE TABLE recovered (id INT);"});
    assert(valid_after_failure.statements.size() == 1 && !command_of(valid_after_failure.statements[0]).error);

    const auto oversized = database.execute_script({std::string(kMaxSqlBytes + 1, ' ')});
    assert(oversized.statements.empty());
    const auto* oversized_error = oversized.script_error.has_value()
        ? &*oversized.script_error
        : nullptr;
    assert(oversized_error != nullptr);
    assert(oversized_error->kind == ErrorKind::kCompile);
    assert(oversized_error->location.has_value());
    assert(oversized_error->location->line == 1 && oversized_error->location->column == 1);
    const auto valid_after_oversized = database.execute_script({"SELECT * FROM student;"});
    assert(valid_after_oversized.statements.size() == 1);
    assert(std::holds_alternative<QueryResult>(
        outcome_of(valid_after_oversized.statements[0]).outcome));

    const auto inserted = database.execute_script(
        {"INSERT INTO student VALUES (1, 'Alice'), (2, 'Bob');"});
    assert(inserted.statements.size() == 1);
    assert(command_of(inserted.statements[0]).affected_rows == 2);

    const auto reordered = database.execute_script(
        {"INSERT INTO student(name, id) VALUES ('Carol', 3);"});
    assert(reordered.statements.size() == 1 && command_of(reordered.statements[0]).affected_rows == 1);

    const auto selected = database.execute_script(
        {"SELECT name FROM student WHERE id = 1;"});
    assert(selected.statements.size() == 1);
    const auto& rows = query_of(selected.statements[0]);
    assert(rows.columns.size() == 1 && rows.columns[0].name == "name");
    assert(rows.rows.size() == 1 && std::get<std::string>(rows.rows[0][0].data) == "Alice");

    const auto logical = database.execute_script(
        {"SELECT id FROM student WHERE NOT id = 2 AND (name = 'Alice' OR name = 'Nobody');"});
    assert(logical.statements.size() == 1 && query_of(logical.statements[0]).rows.size() == 1);

    const auto join_setup = database.execute_script({
        "CREATE TABLE memberships (id BIGINT NULL, role VARCHAR, rank INT);"
        "INSERT INTO memberships VALUES "
        "(1,'reader',2),(1,'admin',1),(2,'writer',3),(NULL,'unknown',3);"
        "CREATE TABLE ranks (id INT, label VARCHAR);"
        "INSERT INTO ranks VALUES (1,'low'),(2,'medium'),(3,'high');"});
    assert(join_setup.statements.size() == 4);

    const auto joined = database.execute_script({
        "SELECT student.name,memberships.role FROM student JOIN memberships "
        "ON student.id=memberships.id WHERE memberships.role!='admin' "
        "ORDER BY memberships.rank DESC;"});
    assert(joined.statements.size() == 1);
    const auto& joined_rows = query_of(joined.statements[0]);
    assert(joined_rows.columns.size() == 2);
    assert(joined_rows.columns[0].name == "name" && joined_rows.columns[1].name == "role");
    assert(joined_rows.rows.size() == 2);
    assert(std::get<std::string>(joined_rows.rows[0][0].data) == "Bob");
    assert(std::get<std::string>(joined_rows.rows[1][0].data) == "Alice");

    const auto join_star = database.execute_script({
        "SELECT * FROM student INNER JOIN memberships ON student.id=memberships.id;"});
    assert(join_star.statements.size() == 1);
    const auto& star_rows = query_of(join_star.statements[0]);
    assert(star_rows.columns.size() == 5 && star_rows.rows.size() == 3);
    assert(star_rows.columns[0].name == "id" && star_rows.columns[2].name == "id");

    const auto join_truth = database.execute_script({
        "SELECT student.id FROM student JOIN memberships ON TRUE;"
        "SELECT student.id FROM student JOIN memberships ON FALSE;"
        "SELECT student.id FROM student JOIN memberships ON NULL;"});
    assert(join_truth.statements.size() == 3);
    assert(query_of(join_truth.statements[0]).rows.size() == 12);
    assert(query_of(join_truth.statements[1]).rows.empty());
    assert(query_of(join_truth.statements[2]).rows.empty());

    const auto chained_join = database.execute_script({
        "SELECT student.name,ranks.label FROM student "
        "JOIN memberships ON student.id=memberships.id "
        "JOIN ranks ON memberships.rank=ranks.id ORDER BY ranks.id;"});
    assert(chained_join.statements.size() == 1);
    const auto& chained_rows = query_of(chained_join.statements[0]);
    assert(chained_rows.rows.size() == 3);
    assert(std::get<std::string>(chained_rows.rows[0][1].data) == "low");
    assert(std::get<std::string>(chained_rows.rows[2][1].data) == "high");

    const auto deleted = database.execute_script(
        {"DELETE FROM student WHERE id = 2; SELECT * FROM student;"});
    assert(deleted.statements.size() == 2);
    assert(command_of(deleted.statements[0]).affected_rows == 1);
    const auto& remaining = query_of(deleted.statements[1]);
    assert(remaining.rows.size() == 2);
    assert(std::get<std::int32_t>(remaining.rows[0][0].data) == 1);

    const auto bigint_created = database.execute_script(
        {"CREATE TABLE events (event_id BIGINT, label VARCHAR);"});
    assert(bigint_created.statements.size() == 1);
    assert(!command_of(bigint_created.statements[0]).error);
    const auto bigint_inserted = database.execute_script(
        {"INSERT INTO events VALUES (1, 'small'), (2147483648, 'large'), "
         "(9223372036854775807, 'maximum');"});
    assert(bigint_inserted.statements.size() == 1);
    assert(command_of(bigint_inserted.statements[0]).affected_rows == 3);
    const auto bigint_selected = database.execute_script(
        {"SELECT label,event_id FROM events WHERE event_id > 1;"});
    assert(bigint_selected.statements.size() == 1);
    const auto& bigint_rows = query_of(bigint_selected.statements[0]);
    assert(bigint_rows.columns.size() == 2);
    assert(bigint_rows.columns[0].name == "label");
    assert(bigint_rows.columns[1].name == "event_id");
    assert(bigint_rows.columns[1].type == Type::kBigInt);
    assert(bigint_rows.rows.size() == 2);
    assert(std::get<std::int64_t>(bigint_rows.rows[0][1].data) == 2147483648LL);
    assert(std::get<std::int64_t>(bigint_rows.rows[1][1].data) ==
           std::numeric_limits<std::int64_t>::max());
    const auto boundary_comparison = database.execute_script(
        {"SELECT label FROM events WHERE 2147483647 < event_id;"});
    assert(boundary_comparison.statements.size() == 1);
    assert(query_of(boundary_comparison.statements[0]).rows.size() == 2);
    const auto int_vs_bigint = database.execute_script(
        {"SELECT id FROM student WHERE id < 2147483648;"});
    assert(int_vs_bigint.statements.size() == 1);
    assert(query_of(int_vs_bigint.statements[0]).rows.size() == 2);
    const auto bigint_deleted = database.execute_script(
        {"DELETE FROM events WHERE event_id = 2147483648;"});
    assert(bigint_deleted.statements.size() == 1);
    assert(command_of(bigint_deleted.statements[0]).affected_rows == 1);

    const auto double_created = database.execute_script(
        {"CREATE TABLE measurements (id INT, value DOUBLE, label VARCHAR);"});
    assert(double_created.statements.size() == 1);
    assert(!command_of(double_created.statements[0]).error);
    const auto double_inserted = database.execute_script(
        {"INSERT INTO measurements VALUES "
         "(1,0.0,'zero'),(2,1,'one'),(3,12.5,'fraction'),"
         "(4,2147483648,'bigint'),(5,9007199254740993,'precision');"});
    assert(double_inserted.statements.size() == 1);
    assert(command_of(double_inserted.statements[0]).affected_rows == 5);
    const auto double_selected = database.execute_script(
        {"SELECT label,value FROM measurements WHERE value > 10.5;"});
    assert(double_selected.statements.size() == 1);
    const auto& double_rows = query_of(double_selected.statements[0]);
    assert(double_rows.columns.size() == 2);
    assert(double_rows.columns[1].type == Type::kDouble);
    assert(double_rows.rows.size() == 3);
    assert(std::get<double>(double_rows.rows[0][1].data) == 12.5);
    assert(std::get<double>(double_rows.rows[1][1].data) == 2147483648.0);
    assert(std::get<double>(double_rows.rows[2][1].data) ==
           static_cast<double>(std::int64_t{9007199254740993LL}));
    const auto double_vs_int = database.execute_script(
        {"SELECT id FROM measurements WHERE value = 1;"});
    assert(double_vs_int.statements.size() == 1);
    assert(query_of(double_vs_int.statements[0]).rows.size() == 1);
    const auto int_vs_double = database.execute_script(
        {"SELECT id FROM student WHERE id < 1.5;"});
    assert(int_vs_double.statements.size() == 1);
    assert(query_of(int_vs_double.statements[0]).rows.size() == 1);
    const auto bigint_vs_double = database.execute_script(
        {"SELECT event_id FROM events WHERE event_id > 2147483648.5;"});
    assert(bigint_vs_double.statements.size() == 1);
    assert(query_of(bigint_vs_double.statements[0]).rows.size() == 1);
    const auto double_deleted = database.execute_script(
        {"DELETE FROM measurements WHERE value = 12.5;"});
    assert(double_deleted.statements.size() == 1);
    assert(command_of(double_deleted.statements[0]).affected_rows == 1);

    const auto boolean_created = database.execute_script(
        {"CREATE TABLE users (id INT, name VARCHAR, active BOOLEAN);"});
    assert(boolean_created.statements.size() == 1);
    assert(!command_of(boolean_created.statements[0]).error);
    const auto boolean_inserted = database.execute_script(
        {"INSERT INTO users VALUES "
         "(1,'alice',TRUE),(2,'bob',FALSE),(3,'carol',true);"});
    assert(boolean_inserted.statements.size() == 1);
    assert(command_of(boolean_inserted.statements[0]).affected_rows == 3);
    const auto boolean_selected = database.execute_script(
        {"SELECT active FROM users;"});
    assert(boolean_selected.statements.size() == 1);
    const auto& boolean_rows = query_of(boolean_selected.statements[0]);
    assert(boolean_rows.columns.size() == 1);
    assert(boolean_rows.columns[0].type == Type::kBoolean);
    assert(boolean_rows.rows.size() == 3);
    assert(std::get<bool>(boolean_rows.rows[0][0].data));
    assert(!std::get<bool>(boolean_rows.rows[1][0].data));
    assert(std::get<bool>(boolean_rows.rows[2][0].data));
    const auto boolean_predicates = database.execute_script(
        {"SELECT name FROM users WHERE active;"
         "SELECT name FROM users WHERE NOT active;"
         "SELECT name FROM users WHERE TRUE;"
         "SELECT name FROM users WHERE FALSE;"
         "SELECT name FROM users WHERE active = TRUE;"
         "SELECT name FROM users WHERE active != FALSE;"});
    assert(boolean_predicates.statements.size() == 6);
    assert(query_of(boolean_predicates.statements[0]).rows.size() == 2);
    assert(query_of(boolean_predicates.statements[1]).rows.size() == 1);
    assert(query_of(boolean_predicates.statements[2]).rows.size() == 3);
    assert(query_of(boolean_predicates.statements[3]).rows.empty());
    assert(query_of(boolean_predicates.statements[4]).rows.size() == 2);
    assert(query_of(boolean_predicates.statements[5]).rows.size() == 2);

    const auto nullable_created = database.execute_script(
        {"CREATE TABLE nullable_rows "
         "(id INT NOT NULL, note VARCHAR NULL, active BOOLEAN);"});
    assert(nullable_created.statements.size() == 1);
    assert(!command_of(nullable_created.statements[0]).error);
    const auto nullable_inserted = database.execute_script(
        {"INSERT INTO nullable_rows VALUES "
         "(1,NULL,NULL),(2,'',FALSE),(3,'present',TRUE);"});
    assert(nullable_inserted.statements.size() == 1);
    assert(command_of(nullable_inserted.statements[0]).affected_rows == 3);
    const auto not_null_violation = database.execute_script(
        {"INSERT INTO nullable_rows VALUES (NULL,'bad',TRUE);"});
    assert(not_null_violation.statements.size() == 1);
    const auto* not_null_error = std::get_if<Error>(
        &outcome_of(not_null_violation.statements[0]).outcome);
    assert(not_null_error != nullptr && not_null_error->kind == ErrorKind::kCompile);

    const auto null_predicates = database.execute_script(
        {"SELECT id FROM nullable_rows WHERE note IS NULL;"
         "SELECT id FROM nullable_rows WHERE note IS NOT NULL;"
         "SELECT id FROM nullable_rows WHERE note = NULL;"
         "SELECT id FROM nullable_rows WHERE NULL = NULL;"
         "SELECT id FROM nullable_rows WHERE active OR NULL;"});
    assert(null_predicates.statements.size() == 5);
    assert(query_of(null_predicates.statements[0]).rows.size() == 1);
    assert(query_of(null_predicates.statements[1]).rows.size() == 2);
    assert(query_of(null_predicates.statements[2]).rows.empty());
    assert(query_of(null_predicates.statements[3]).rows.empty());
    assert(query_of(null_predicates.statements[4]).rows.size() == 1);
    const auto nullable_boolean_predicates = database.execute_script(
        {"SELECT id FROM nullable_rows WHERE active;"
         "SELECT id FROM nullable_rows WHERE NOT active;"});
    assert(nullable_boolean_predicates.statements.size() == 2);
    assert(query_of(nullable_boolean_predicates.statements[0]).rows.size() == 1);
    assert(std::get<std::int32_t>(
               query_of(nullable_boolean_predicates.statements[0]).rows[0][0].data) == 3);
    assert(query_of(nullable_boolean_predicates.statements[1]).rows.size() == 1);
    assert(std::get<std::int32_t>(
               query_of(nullable_boolean_predicates.statements[1]).rows[0][0].data) == 2);
    const auto nullable_order = database.execute_script(
        {"SELECT id,note FROM nullable_rows ORDER BY note;"
         "SELECT id,note FROM nullable_rows ORDER BY note DESC;"
         "SELECT id FROM nullable_rows WHERE id >= 1 ORDER BY active DESC,id DESC;"});
    assert(nullable_order.statements.size() == 3);
    const auto& note_asc = query_of(nullable_order.statements[0]);
    assert(note_asc.rows.size() == 3);
    assert(std::get<std::int32_t>(note_asc.rows[0][0].data) == 2);
    assert(std::get<std::int32_t>(note_asc.rows[1][0].data) == 3);
    assert(std::get<std::int32_t>(note_asc.rows[2][0].data) == 1);
    assert(std::holds_alternative<std::monostate>(note_asc.rows[2][1].data));
    const auto& note_desc = query_of(nullable_order.statements[1]);
    assert(std::get<std::int32_t>(note_desc.rows[0][0].data) == 3);
    assert(std::get<std::int32_t>(note_desc.rows[1][0].data) == 2);
    assert(std::get<std::int32_t>(note_desc.rows[2][0].data) == 1);
    assert(std::holds_alternative<std::monostate>(note_desc.rows[2][1].data));
    const auto& boolean_multi = query_of(nullable_order.statements[2]);
    assert(boolean_multi.rows.size() == 3);
    assert(std::get<std::int32_t>(boolean_multi.rows[0][0].data) == 3);
    assert(std::get<std::int32_t>(boolean_multi.rows[1][0].data) == 2);
    assert(std::get<std::int32_t>(boolean_multi.rows[2][0].data) == 1);
    const auto null_deleted = database.execute_script(
        {"DELETE FROM nullable_rows WHERE note IS NULL;"});
    assert(null_deleted.statements.size() == 1);
    assert(command_of(null_deleted.statements[0]).affected_rows == 1);

    const auto null_matrix_created = database.execute_script(
        {"CREATE TABLE null_matrix "
         "(i INT,b BIGINT,d DOUBLE,flag BOOLEAN,text VARCHAR);"});
    assert(null_matrix_created.statements.size() == 1);
    assert(!command_of(null_matrix_created.statements[0]).error);
    const auto null_matrix_inserted = database.execute_script(
        {"INSERT INTO null_matrix VALUES "
         "(NULL,NULL,NULL,NULL,NULL),(0,0,0.0,FALSE,'');"});
    assert(null_matrix_inserted.statements.size() == 1);
    assert(command_of(null_matrix_inserted.statements[0]).affected_rows == 2);
    const auto null_matrix_predicates = database.execute_script(
        {"SELECT i FROM null_matrix WHERE i < NULL;"
         "SELECT i FROM null_matrix WHERE b >= NULL;"
         "SELECT i FROM null_matrix WHERE d = NULL;"
         "SELECT i FROM null_matrix WHERE flag != NULL;"
         "SELECT i FROM null_matrix WHERE text = NULL;"
         "SELECT i FROM null_matrix WHERE NULL IS NULL;"
         "SELECT i FROM null_matrix WHERE NULL IS NOT NULL;"});
    assert(null_matrix_predicates.statements.size() == 7);
    for (std::size_t index = 0; index < 5; ++index) {
        assert(query_of(null_matrix_predicates.statements[index]).rows.empty());
    }
    assert(query_of(null_matrix_predicates.statements[5]).rows.size() == 2);
    assert(query_of(null_matrix_predicates.statements[6]).rows.empty());
    for (const std::string_view sql : {
             "SELECT i FROM null_matrix WHERE flag < NULL;",
             "SELECT i FROM null_matrix WHERE text > NULL;"}) {
        const auto invalid_order = database.execute_script({std::string{sql}});
        assert(invalid_order.statements.size() == 1);
        const auto* error = std::get_if<Error>(
            &outcome_of(invalid_order.statements[0]).outcome);
        assert(error != nullptr && error->kind == ErrorKind::kCompile);
    }

    const auto update_created=database.execute_script({
        "CREATE TABLE update_rows (id INT,b BIGINT,d DOUBLE,active BOOLEAN,note VARCHAR);"
        "INSERT INTO update_rows VALUES (1,1,1.0,FALSE,'x'),(2,2,2.0,TRUE,'second');"});
    assert(update_created.statements.size()==2);
    assert(command_of(update_created.statements[1]).affected_rows==2);
    const auto updated=database.execute_script({
        "UPDATE update_rows SET active=FALSE WHERE active;"
        "UPDATE update_rows SET b=2147483648,d=12.5,active=TRUE,"
        "note='a much longer value' WHERE id=1;"
        "UPDATE update_rows SET active=NULL,note=NULL WHERE id=2;"
        "UPDATE update_rows SET note='all' WHERE id>0;"
        "UPDATE update_rows SET note='all' WHERE id=1;"
        "UPDATE update_rows SET note='never' WHERE id=999;"
        "SELECT id,b,d,active,note FROM update_rows;"});
    assert(updated.statements.size()==7);
    assert(command_of(updated.statements[0]).affected_rows==1);
    assert(command_of(updated.statements[1]).affected_rows==1);
    assert(command_of(updated.statements[2]).affected_rows==1);
    assert(command_of(updated.statements[3]).affected_rows==2);
    assert(command_of(updated.statements[4]).affected_rows==1);
    assert(command_of(updated.statements[5]).affected_rows==0);
    const auto& updated_rows=query_of(updated.statements[6]);
    assert(updated_rows.rows.size()==2);
    assert(std::get<std::int64_t>(updated_rows.rows[0][1].data)==2147483648LL);
    assert(std::get<double>(updated_rows.rows[0][2].data)==12.5);
    assert(std::get<bool>(updated_rows.rows[0][3].data));
    assert(std::get<std::string>(updated_rows.rows[0][4].data)=="all");
    assert(std::holds_alternative<std::monostate>(updated_rows.rows[1][3].data));
    assert(std::get<std::string>(updated_rows.rows[1][4].data)=="all");

    const auto typed_ordering = database.execute_script({
        "SELECT id FROM student ORDER BY name DESC;"
        "SELECT event_id FROM events ORDER BY event_id DESC;"
        "SELECT id,value FROM measurements ORDER BY value;"
        "SELECT name FROM users ORDER BY active DESC,name;"
        "SELECT id,b FROM update_rows ORDER BY b DESC;"});
    assert(typed_ordering.statements.size() == 5);
    assert(std::get<std::int32_t>(query_of(typed_ordering.statements[0]).rows[0][0].data) == 3);
    assert(std::get<std::int64_t>(query_of(typed_ordering.statements[1]).rows[0][0].data) ==
           std::numeric_limits<std::int64_t>::max());
    const auto& ordered_doubles = query_of(typed_ordering.statements[2]);
    assert(ordered_doubles.rows.size() == 4);
    assert(std::get<double>(ordered_doubles.rows[0][1].data) == 0.0);
    assert(std::get<double>(ordered_doubles.rows[1][1].data) == 1.0);
    assert(std::get<std::string>(query_of(typed_ordering.statements[3]).rows[0][0].data) == "alice");
    assert(std::get<std::string>(query_of(typed_ordering.statements[3]).rows[1][0].data) == "carol");
    assert(std::get<std::int32_t>(query_of(typed_ordering.statements[4]).rows[0][0].data) == 1);

    const auto stable_created = database.execute_script(
        {"CREATE TABLE stable_rows (id INT NOT NULL,k INT NULL);"
         "INSERT INTO stable_rows VALUES (3,NULL),(1,NULL),(2,NULL);"
         "SELECT id FROM stable_rows ORDER BY k DESC;"});
    assert(stable_created.statements.size() == 3);
    const auto& stable_rows = query_of(stable_created.statements[2]);
    assert(stable_rows.rows.size() == 3);
    assert(std::get<std::int32_t>(stable_rows.rows[0][0].data) == 3);
    assert(std::get<std::int32_t>(stable_rows.rows[1][0].data) == 1);
    assert(std::get<std::int32_t>(stable_rows.rows[2][0].data) == 2);

    const auto binary_text_order = database.execute_script(
        {"CREATE TABLE text_order (id INT NOT NULL,value VARCHAR NOT NULL);"
         "INSERT INTO text_order VALUES (1,'a'),(2,'A'),(3,'b');"
         "SELECT value FROM text_order ORDER BY value;"});
    assert(binary_text_order.statements.size() == 3);
    const auto& text_rows = query_of(binary_text_order.statements[2]);
    assert(text_rows.rows.size() == 3);
    assert(std::get<std::string>(text_rows.rows[0][0].data) == "A");
    assert(std::get<std::string>(text_rows.rows[1][0].data) == "a");
    assert(std::get<std::string>(text_rows.rows[2][0].data) == "b");

    const auto many_created = database.execute_script(
        {"CREATE TABLE sort_many (id INT NOT NULL);"});
    assert(many_created.statements.size() == 1);
    std::string many_insert{"INSERT INTO sort_many VALUES "};
    for (int value = 300; value >= 1; --value) {
        if (value != 300) {
            many_insert += ',';
        }
        many_insert += '(' + std::to_string(value) + ')';
    }
    many_insert += ';';
    const auto many_inserted = database.execute_script({many_insert});
    assert(many_inserted.statements.size() == 1);
    assert(command_of(many_inserted.statements[0]).affected_rows == 300);
    const auto many_ordered = database.execute_script(
        {"SELECT id FROM sort_many ORDER BY id;"});
    assert(many_ordered.statements.size() == 1);
    const auto& many_rows = query_of(many_ordered.statements[0]);
    assert(many_rows.rows.size() == 300);
    for (std::size_t index = 0; index < many_rows.rows.size(); ++index) {
        assert(std::get<std::int32_t>(many_rows.rows[index][0].data) ==
               static_cast<std::int32_t>(index + 1U));
    }
    const auto multi_page_join = database.execute_script({
        "SELECT student.name FROM student JOIN sort_many ON student.id=sort_many.id "
        "ORDER BY sort_many.id;"});
    assert(multi_page_join.statements.size() == 1);
    const auto& multi_page_rows = query_of(multi_page_join.statements[0]);
    assert(multi_page_rows.rows.size() == 2);
    assert(std::get<std::string>(multi_page_rows.rows[0][0].data) == "Alice");
    assert(std::get<std::string>(multi_page_rows.rows[1][0].data) == "Carol");

    const auto aggregate_setup = database.execute_script({
        "CREATE TABLE aggregate_rows "
        "(dept VARCHAR NULL,active BOOLEAN NULL,pay INT NULL,score DOUBLE NULL);"
        "INSERT INTO aggregate_rows VALUES "
        "('A',TRUE,10,1.5),('A',FALSE,20,2.5),('B',TRUE,NULL,3.0),"
        "(NULL,TRUE,30,NULL),(NULL,NULL,NULL,NULL),('A',TRUE,NULL,4.0);"});
    assert(aggregate_setup.statements.size() == 2);
    assert(command_of(aggregate_setup.statements[1]).affected_rows == 6);

    const auto global_aggregate = database.execute_script({
        "SELECT COUNT(*),COUNT(pay),SUM(pay),SUM(score),AVG(score),MIN(dept),MAX(pay) "
        "FROM aggregate_rows;"});
    assert(global_aggregate.statements.size() == 1);
    const auto& global = query_of(global_aggregate.statements[0]);
    assert(global.columns.size() == 7 && global.rows.size() == 1);
    assert(global.columns[0].name == "COUNT(*)" && global.columns[0].type == Type::kBigInt);
    assert(global.columns[2].name == "SUM(pay)" && global.columns[2].type == Type::kBigInt);
    assert(global.columns[3].type == Type::kDouble && global.columns[4].type == Type::kDouble);
    assert(std::get<std::int64_t>(global.rows[0][0].data) == 6);
    assert(std::get<std::int64_t>(global.rows[0][1].data) == 3);
    assert(std::get<std::int64_t>(global.rows[0][2].data) == 60);
    assert(std::get<double>(global.rows[0][3].data) == 11.0);
    assert(std::get<double>(global.rows[0][4].data) == 2.75);
    assert(std::get<std::string>(global.rows[0][5].data) == "A");
    assert(std::get<std::int32_t>(global.rows[0][6].data) == 30);

    const auto grouped_aggregate = database.execute_script({
        "SELECT dept,COUNT(*),COUNT(pay),SUM(pay),AVG(score),MIN(pay),MAX(pay) "
        "FROM aggregate_rows GROUP BY dept ORDER BY dept;"
        "SELECT COUNT(*) FROM aggregate_rows GROUP BY dept ORDER BY dept;"
        "SELECT dept,active,COUNT(*) FROM aggregate_rows "
        "GROUP BY dept,active ORDER BY dept,active;"
        "SELECT dept FROM aggregate_rows GROUP BY dept ORDER BY dept;"
        "SELECT dept,COUNT(*) FROM aggregate_rows WHERE active "
        "GROUP BY dept ORDER BY dept;"});
    assert(grouped_aggregate.statements.size() == 5);
    const auto& grouped = query_of(grouped_aggregate.statements[0]);
    assert(grouped.rows.size() == 3);
    assert(std::get<std::string>(grouped.rows[0][0].data) == "A");
    assert(std::get<std::int64_t>(grouped.rows[0][1].data) == 3);
    assert(std::get<std::int64_t>(grouped.rows[0][2].data) == 2);
    assert(std::get<std::int64_t>(grouped.rows[0][3].data) == 30);
    assert(std::get<double>(grouped.rows[0][4].data) == 8.0 / 3.0);
    assert(std::get<std::int32_t>(grouped.rows[0][5].data) == 10);
    assert(std::get<std::int32_t>(grouped.rows[0][6].data) == 20);
    assert(std::get<std::string>(grouped.rows[1][0].data) == "B");
    assert(std::holds_alternative<std::monostate>(grouped.rows[1][3].data));
    assert(std::holds_alternative<std::monostate>(grouped.rows[2][0].data));
    assert(std::get<std::int64_t>(grouped.rows[2][1].data) == 2);
    assert(std::get<std::int64_t>(grouped.rows[2][2].data) == 1);
    assert(query_of(grouped_aggregate.statements[1]).rows.size() == 3);
    assert(query_of(grouped_aggregate.statements[2]).rows.size() == 5);
    assert(query_of(grouped_aggregate.statements[3]).rows.size() == 3);
    const auto& filtered_groups = query_of(grouped_aggregate.statements[4]);
    assert(filtered_groups.rows.size() == 3);
    assert(std::get<std::int64_t>(filtered_groups.rows[0][1].data) == 2);

    const auto join_aggregate = database.execute_script({
        "SELECT student.name,COUNT(memberships.role),MIN(memberships.rank) "
        "FROM student JOIN memberships ON student.id=memberships.id "
        "GROUP BY student.name ORDER BY student.name;"});
    assert(join_aggregate.statements.size() == 1);
    const auto& joined_groups = query_of(join_aggregate.statements[0]);
    assert(joined_groups.rows.size() == 1);
    assert(std::get<std::string>(joined_groups.rows[0][0].data) == "Alice");
    assert(std::get<std::int64_t>(joined_groups.rows[0][1].data) == 2);
    assert(std::get<std::int32_t>(joined_groups.rows[0][2].data) == 1);

    const auto empty_aggregate_setup = database.execute_script({
        "CREATE TABLE empty_aggregate (value INT NULL);"
        "SELECT COUNT(*),COUNT(value),SUM(value),AVG(value),MIN(value),MAX(value) "
        "FROM empty_aggregate;"
        "SELECT value,COUNT(*) FROM empty_aggregate GROUP BY value;"});
    assert(empty_aggregate_setup.statements.size() == 3);
    const auto& empty_global = query_of(empty_aggregate_setup.statements[1]);
    assert(empty_global.rows.size() == 1 && empty_global.rows[0].size() == 6);
    assert(std::get<std::int64_t>(empty_global.rows[0][0].data) == 0);
    assert(std::get<std::int64_t>(empty_global.rows[0][1].data) == 0);
    for (std::size_t index = 2; index < empty_global.rows[0].size(); ++index) {
        assert(std::holds_alternative<std::monostate>(empty_global.rows[0][index].data));
    }
    assert(query_of(empty_aggregate_setup.statements[2]).rows.empty());

    const auto multi_page_aggregate = database.execute_script({
        "SELECT COUNT(*),SUM(id),AVG(id),MIN(id),MAX(id) FROM sort_many;"});
    assert(multi_page_aggregate.statements.size() == 1);
    const auto& many_aggregates = query_of(multi_page_aggregate.statements[0]);
    assert(many_aggregates.rows.size() == 1);
    assert(std::get<std::int64_t>(many_aggregates.rows[0][0].data) == 300);
    assert(std::get<std::int64_t>(many_aggregates.rows[0][1].data) == 45150);
    assert(std::get<double>(many_aggregates.rows[0][2].data) == 150.5);
    assert(std::get<std::int32_t>(many_aggregates.rows[0][3].data) == 1);
    assert(std::get<std::int32_t>(many_aggregates.rows[0][4].data) == 300);
    const auto multi_page_join_aggregate = database.execute_script({
        "SELECT student.name,COUNT(sort_many.id),SUM(sort_many.id) FROM student "
        "JOIN sort_many ON student.id=sort_many.id GROUP BY student.name ORDER BY student.name;"});
    assert(multi_page_join_aggregate.statements.size() == 1);
    const auto& multi_page_join_groups = query_of(multi_page_join_aggregate.statements[0]);
    assert(multi_page_join_groups.rows.size() == 2);
    assert(std::get<std::string>(multi_page_join_groups.rows[0][0].data) == "Alice");
    assert(std::get<std::int64_t>(multi_page_join_groups.rows[0][1].data) == 1);
    assert(std::get<std::int64_t>(multi_page_join_groups.rows[0][2].data) == 1);
    assert(std::get<std::string>(multi_page_join_groups.rows[1][0].data) == "Carol");
    assert(std::get<std::int64_t>(multi_page_join_groups.rows[1][2].data) == 3);

    const auto mutation_aggregate = database.execute_script({
        "SELECT COUNT(*),COUNT(active),SUM(b) FROM update_rows;"
        "UPDATE update_rows SET b=NULL WHERE id=2;"
        "SELECT COUNT(*),COUNT(b),SUM(b) FROM update_rows;"
        "DELETE FROM aggregate_rows WHERE dept='B';"
        "SELECT COUNT(*) FROM aggregate_rows;"});
    assert(mutation_aggregate.statements.size() == 5);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[0]).rows[0][0].data) == 2);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[0]).rows[0][1].data) == 1);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[0]).rows[0][2].data) ==
           2147483650LL);
    assert(command_of(mutation_aggregate.statements[1]).affected_rows == 1);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[2]).rows[0][1].data) == 1);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[2]).rows[0][2].data) ==
           2147483648LL);
    assert(command_of(mutation_aggregate.statements[3]).affected_rows == 1);
    assert(std::get<std::int64_t>(query_of(mutation_aggregate.statements[4]).rows[0][0].data) == 5);

    const auto recovered_script = database.execute_script({
        "CREATE TABLE recovery_real (id INT);\n"
        "SELEC id FROM recovery_real;\n"
        "CREATE TABLE recovery_shadow (id INT);\n"
        "INSERT INTO recovery_shadow VALUES (1);\n"
        "SELECT id FROM recovery_shadow ORDER BY id;\n"
        "SELCT id FROM recovery_shadow;\n"
        "CREATE TABLE recovery_tail (id INT);"});
    assert(recovered_script.statements.size() == 7);
    assert(recovered_script.executed_count == 1);
    assert(recovered_script.first_error_index == std::optional<std::size_t>{1});
    assert(!recovered_script.script_error.has_value());
    assert(recovered_script.statements[0].status == StatementStatus::kExecuted);
    assert(recovered_script.statements[1].status == StatementStatus::kCompileError);
    assert(recovered_script.statements[2].status == StatementStatus::kAnalysisOnly);
    assert(recovered_script.statements[3].status == StatementStatus::kAnalysisOnly);
    assert(recovered_script.statements[4].status == StatementStatus::kAnalysisOnly);
    assert(recovered_script.statements[5].status == StatementStatus::kCompileError);
    assert(recovered_script.statements[6].status == StatementStatus::kAnalysisOnly);
    for (std::size_t index = 0; index < recovered_script.statements.size(); ++index) {
        assert(recovered_script.statements[index].statement_index == index);
    }
    assert(recovered_script.statements[1].source_range.begin.line == 1);
    assert(recovered_script.statements[1].source_range.end.line == 2);
    const Error& first_compile_error = error_of(recovered_script.statements[1]);
    assert(first_compile_error.location.has_value());
    assert(first_compile_error.location->line == 2);
    assert(first_compile_error.location->column == 1);
    assert(first_compile_error.suggestion.has_value());
    assert(first_compile_error.fix_it.has_value());
    assert(first_compile_error.fix_it->range.begin.line == 2);
    assert(first_compile_error.fix_it->range.begin.column == 1);
    const Error& later_compile_error = error_of(recovered_script.statements[5]);
    assert(later_compile_error.location.has_value());
    assert(later_compile_error.location->line == 6);
    assert(later_compile_error.location->column == 1);
    assert(later_compile_error.suggestion.has_value());
    assert(later_compile_error.fix_it.has_value());
    assert(later_compile_error.fix_it->range.begin.line == 6);
    assert(later_compile_error.fix_it->range.begin.column == 1);
    assert(!recovered_script.statements[2].outcome.has_value());
    assert(!recovered_script.statements[3].outcome.has_value());
    assert(!recovered_script.statements[4].outcome.has_value());
    assert(!recovered_script.statements[6].outcome.has_value());

    const auto shadow_does_not_leak = database.execute_script(
        {"SELECT * FROM recovery_shadow;"});
    assert(shadow_does_not_leak.statements.size() == 1);
    assert(shadow_does_not_leak.statements[0].status == StatementStatus::kCompileError);
    const auto real_create_after_recovery = database.execute_script(
        {"CREATE TABLE recovery_shadow (id INT);"});
    assert(real_create_after_recovery.statements.size() == 1);
    assert(real_create_after_recovery.statements[0].status == StatementStatus::kExecuted);

    const auto absolute_ranges = database.execute_script({
        "SELECT missing FROM recovery_real; SELEC id FROM recovery_real;\r\n"
        "SELCT id FROM recovery_real;"});
    assert(absolute_ranges.statements.size() == 3);
    assert(absolute_ranges.first_error_index == std::optional<std::size_t>{0});
    assert(absolute_ranges.executed_count == 0);
    assert(absolute_ranges.statements[0].source_range.begin.line == 1);
    assert(absolute_ranges.statements[0].source_range.begin.column == 1);
    assert(absolute_ranges.statements[0].source_range.end.line == 1);
    assert(absolute_ranges.statements[0].source_range.end.column == 35);
    assert(absolute_ranges.statements[1].source_range.begin.line == 1);
    assert(absolute_ranges.statements[1].source_range.begin.column == 35);
    assert(error_of(absolute_ranges.statements[1]).location->line == 1);
    assert(error_of(absolute_ranges.statements[1]).location->column == 36);
    assert(error_of(absolute_ranges.statements[1]).fix_it->range.begin.line == 1);
    assert(error_of(absolute_ranges.statements[1]).fix_it->range.begin.column == 36);
    assert(absolute_ranges.statements[2].source_range.begin.line == 1);
    assert(absolute_ranges.statements[2].source_range.begin.column == 64);
    assert(absolute_ranges.statements[2].source_range.end.line == 2);
    assert(error_of(absolute_ranges.statements[2]).location->line == 2);
    assert(error_of(absolute_ranges.statements[2]).location->column == 1);
    assert(error_of(absolute_ranges.statements[2]).fix_it->range.begin.line == 2);
    assert(error_of(absolute_ranges.statements[2]).fix_it->range.begin.column == 1);

    std::string utf8_fix_script =
        "SELECT missing FROM recovery_real;/*测*/ SELEC id FROM recovery_real;";
    const auto utf8_diagnostics = database.execute_script({utf8_fix_script});
    assert(utf8_diagnostics.statements.size() == 2);
    assert(utf8_diagnostics.statements[1].status == StatementStatus::kCompileError);
    const Error& utf8_error = error_of(utf8_diagnostics.statements[1]);
    assert(utf8_error.location.has_value());
    assert(utf8_error.location->line == 1);
    assert(utf8_error.location->column == 43);
    assert(utf8_error.fix_it.has_value());
    assert(utf8_error.fix_it->range.begin.column == 43);
    assert(utf8_error.fix_it->range.end.column == 48);
    const std::optional<std::size_t> fix_begin = byte_offset_of(
        utf8_fix_script, utf8_error.fix_it->range.begin);
    const std::optional<std::size_t> fix_end = byte_offset_of(
        utf8_fix_script, utf8_error.fix_it->range.end);
    assert(fix_begin.has_value() && fix_end.has_value() && *fix_end >= *fix_begin);
    utf8_fix_script.replace(
        *fix_begin, *fix_end - *fix_begin, utf8_error.fix_it->replacement);
    const auto fixed_utf8_script = database.execute_script({utf8_fix_script});
    assert(fixed_utf8_script.statements.size() == 2);
    assert(fixed_utf8_script.statements[0].status == StatementStatus::kCompileError);
    assert(fixed_utf8_script.statements[1].status == StatementStatus::kAnalysisOnly);

    const auto duplicate_shadow_create = database.execute_script({
        "SELECT missing FROM recovery_real;\n"
        "CREATE TABLE shadow_dup (id INT);\n"
        "CREATE TABLE shadow_dup (id INT);\n"
        "SELECT id FROM shadow_dup;"});
    assert(duplicate_shadow_create.statements.size() == 4);
    assert(duplicate_shadow_create.statements[0].status == StatementStatus::kCompileError);
    assert(duplicate_shadow_create.statements[1].status == StatementStatus::kAnalysisOnly);
    assert(duplicate_shadow_create.statements[2].status == StatementStatus::kCompileError);
    assert(duplicate_shadow_create.statements[3].status == StatementStatus::kAnalysisOnly);
    assert(duplicate_shadow_create.executed_count == 0);

    const auto complex_shadow_analysis = database.execute_script({
        "SELECT missing FROM recovery_real;\n"
        "CREATE TABLE shadow_complex (id INT, amount INT);\n"
        "INSERT INTO shadow_complex VALUES (1,10);\n"
        "UPDATE shadow_complex SET amount=20 WHERE id=1;\n"
        "DELETE FROM shadow_complex WHERE id=2;\n"
        "SELECT recovery_real.id,COUNT(shadow_complex.amount) FROM recovery_real "
        "JOIN shadow_complex ON recovery_real.id=shadow_complex.id "
        "GROUP BY recovery_real.id ORDER BY recovery_real.id;\n"
        "SELECT * FROM shadow_complexx;\n"
        "SELECT * FROM shadow_complex;"});
    assert(complex_shadow_analysis.statements.size() == 8);
    assert(complex_shadow_analysis.statements[0].status == StatementStatus::kCompileError);
    for (std::size_t index = 1; index <= 5; ++index) {
        assert(complex_shadow_analysis.statements[index].status == StatementStatus::kAnalysisOnly);
    }
    assert(complex_shadow_analysis.statements[6].status == StatementStatus::kCompileError);
    assert(error_of(complex_shadow_analysis.statements[6]).suggestion.has_value());
    assert(error_of(complex_shadow_analysis.statements[6]).suggestion->find("shadow_complex") !=
           std::string::npos);
    assert(complex_shadow_analysis.statements[7].status == StatementStatus::kAnalysisOnly);
    assert(complex_shadow_analysis.executed_count == 0);

    assert(!database.close().error);
    assert(!contender.open({data_dir.string()}).error);
    assert(!contender.close().error);
    Database reopened;
    assert(!reopened.open({data_dir.string()}).error);
    const auto persistent = reopened.execute_script({"SELECT * FROM student;"});
    assert(persistent.statements.size() == 1 && query_of(persistent.statements[0]).rows.size() == 2);
    const auto bigint_persistent = reopened.execute_script(
        {"SELECT event_id FROM events WHERE event_id >= 1 ORDER BY event_id;"});
    assert(bigint_persistent.statements.size() == 1);
    const auto& persisted_bigints = query_of(bigint_persistent.statements[0]);
    assert(persisted_bigints.rows.size() == 2);
    assert(std::get<std::int64_t>(persisted_bigints.rows[0][0].data) == 1);
    assert(std::get<std::int64_t>(persisted_bigints.rows[1][0].data) ==
           std::numeric_limits<std::int64_t>::max());
    const auto delete_after_reopen = reopened.execute_script(
        {"DELETE FROM events WHERE event_id = 9223372036854775807;"});
    assert(delete_after_reopen.statements.size() == 1);
    assert(command_of(delete_after_reopen.statements[0]).affected_rows == 1);
    const auto double_persistent = reopened.execute_script(
        {"SELECT value FROM measurements WHERE value >= 0.0 ORDER BY value;"});
    assert(double_persistent.statements.size() == 1);
    const auto& persisted_doubles = query_of(double_persistent.statements[0]);
    assert(persisted_doubles.rows.size() == 4);
    assert(std::get<double>(persisted_doubles.rows[0][0].data) == 0.0);
    assert(std::get<double>(persisted_doubles.rows[1][0].data) == 1.0);
    assert(std::get<double>(persisted_doubles.rows[2][0].data) == 2147483648.0);
    assert(std::get<double>(persisted_doubles.rows[3][0].data) ==
           static_cast<double>(std::int64_t{9007199254740993LL}));
    const auto boolean_persistent = reopened.execute_script(
        {"SELECT name,active FROM users WHERE active;"});
    assert(boolean_persistent.statements.size() == 1);
    const auto& persisted_booleans = query_of(boolean_persistent.statements[0]);
    assert(persisted_booleans.columns.size() == 2);
    assert(persisted_booleans.columns[1].type == Type::kBoolean);
    assert(persisted_booleans.rows.size() == 2);
    assert(std::get<std::string>(persisted_booleans.rows[0][0].data) == "alice");
    assert(std::get<bool>(persisted_booleans.rows[0][1].data));
    const auto nullable_persistent = reopened.execute_script(
        {"SELECT id,note,active FROM nullable_rows;"});
    assert(nullable_persistent.statements.size() == 1);
    const auto& persisted_nullable = query_of(nullable_persistent.statements[0]);
    assert(persisted_nullable.rows.size() == 2);
    assert(std::get<std::string>(persisted_nullable.rows[0][1].data).empty());
    assert(!std::get<bool>(persisted_nullable.rows[0][2].data));
    const auto null_matrix_persistent = reopened.execute_script(
        {"SELECT i,b,d,flag,text FROM null_matrix WHERE i IS NULL;"});
    assert(null_matrix_persistent.statements.size() == 1);
    const auto& persisted_matrix = query_of(null_matrix_persistent.statements[0]);
    assert(persisted_matrix.rows.size() == 1);
    for (const Value& value : persisted_matrix.rows[0]) {
        assert(std::holds_alternative<std::monostate>(value.data));
    }
    const auto boolean_deleted = reopened.execute_script(
        {"DELETE FROM users WHERE active; SELECT name FROM users;"});
    assert(boolean_deleted.statements.size() == 2);
    assert(command_of(boolean_deleted.statements[0]).affected_rows == 2);
    assert(query_of(boolean_deleted.statements[1]).rows.size() == 1);
    assert(std::get<std::string>(
               query_of(boolean_deleted.statements[1]).rows[0][0].data) == "bob");
    const auto update_persistent=reopened.execute_script({
        "SELECT id,b,d,active,note FROM update_rows;"
        "UPDATE update_rows SET note='s' WHERE id=1;"
        "DELETE FROM update_rows WHERE active IS NULL;"
        "SELECT id,note FROM update_rows;"});
    assert(update_persistent.statements.size()==4);
    assert(query_of(update_persistent.statements[0]).rows.size()==2);
    assert(command_of(update_persistent.statements[1]).affected_rows==1);
    assert(command_of(update_persistent.statements[2]).affected_rows==1);
    assert(query_of(update_persistent.statements[3]).rows.size()==1);
    assert(std::get<std::string>(query_of(update_persistent.statements[3]).rows[0][1].data)=="s");
    assert(!reopened.close().error);

    Database boolean_verified;
    assert(!boolean_verified.open({data_dir.string()}).error);
    const auto boolean_after_delete = boolean_verified.execute_script(
        {"SELECT name,active FROM users;"});
    assert(boolean_after_delete.statements.size() == 1);
    const auto& remaining_booleans = query_of(boolean_after_delete.statements[0]);
    assert(remaining_booleans.rows.size() == 1);
    assert(std::get<std::string>(remaining_booleans.rows[0][0].data) == "bob");
    assert(!std::get<bool>(remaining_booleans.rows[0][1].data));
    const auto nullable_second_reopen = boolean_verified.execute_script(
        {"SELECT id FROM nullable_rows WHERE active IS NOT NULL;"});
    assert(nullable_second_reopen.statements.size() == 1);
    assert(query_of(nullable_second_reopen.statements[0]).rows.size() == 2);
    const auto aggregate_after_reopen = boolean_verified.execute_script(
        {"SELECT COUNT(*),SUM(pay) FROM aggregate_rows;"});
    assert(aggregate_after_reopen.statements.size() == 1);
    assert(std::get<std::int64_t>(
               query_of(aggregate_after_reopen.statements[0]).rows[0][0].data) == 5);
    assert(std::get<std::int64_t>(
               query_of(aggregate_after_reopen.statements[0]).rows[0][1].data) == 60);
    assert(!boolean_verified.close().error);

    const auto cleanup_dir = temporary_database_path();
    Database cleanup_owner;
    assert(!cleanup_owner.open({cleanup_dir.string()}).error);
    tinydbms::storage::internal::StorageTestAccess::fail_next_file_close();
    assert(cleanup_owner.close().error.has_value());
    Database cleanup_blocked;
    const auto blocked_while_cleaning = cleanup_blocked.open({cleanup_dir.string()});
    assert(blocked_while_cleaning.error && blocked_while_cleaning.error->kind == ErrorKind::kExecute);
    assert(!cleanup_owner.close().error);
    assert(!cleanup_blocked.open({cleanup_dir.string()}).error);
    assert(!cleanup_blocked.close().error);

    std::filesystem::remove_all(data_dir);
    std::filesystem::remove_all(cleanup_dir);
    return 0;
}
