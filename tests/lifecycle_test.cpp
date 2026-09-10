#include "tinydbms/compiler.h"
#include "tinydbms/core.h"

#include <cassert>
#include <chrono>
#include <filesystem>

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path data_dir =
        std::filesystem::temp_directory_path() / ("tinydbms-lifecycle-" + std::to_string(suffix));

    const auto opened = tinydbms::core::open_database({data_dir.string()});
    assert(!opened.error.has_value());

    const auto statements = tinydbms::compiler::split_statements(
        "-- comment\n SELECT * FROM student; /* another comment */ INSERT INTO student VALUES (1);");
    assert(statements.size() == 2);
    assert(statements[0].start.line == 2);

    const auto empty = tinydbms::core::execute_script({"  -- only a comment\n"});
    assert(empty.outcomes.empty());

    const auto closed = tinydbms::core::close_database();
    assert(!closed.error.has_value());

    const auto reopened = tinydbms::core::open_database({data_dir.string()});
    assert(!reopened.error.has_value());
    const auto closed_again = tinydbms::core::close_database();
    assert(!closed_again.error.has_value());

    std::filesystem::remove_all(data_dir);
    return 0;
}
