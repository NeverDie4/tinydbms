#include "runner.hpp"
#include "session.hpp"

#include "tinydbms/common.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using tinydbms::app::CliEnvironment;
using tinydbms::app::CoreSession;

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

InvocationResult invoke_cli(
    const std::filesystem::path& data_dir,
    std::string input,
    bool interactive) {
    std::vector<std::string> arguments{
        "tinydbms",
        "--data-dir",
        data_dir.string()};
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
    CHECK(failed.error.rfind("ERROR compile 1:8 ", 0) == 0);

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
    CHECK(duplicated.error.rfind("ERROR compile 1:14 ", 0) == 0);

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

bool test_repl_recovers_after_oversized_input() {
    TemporaryDirectory data_dir{"tinydbms-integration-repl"};
    std::string input(tinydbms::kMaxSqlBytes + 1, ' ');
    input += "\n";
    input += "CREATE TABLE recovered (id INT);\n";
    input += "INSERT INTO recovered VALUES (7);\n";
    input += "SELECT * FROM recovered;\n";

    const InvocationResult result = invoke_cli(data_dir.path(), std::move(input), true);
    CHECK(result.exit_code == 1);
    CHECK(result.output == "OK 0\nOK 1\nid\n7\n");
    CHECK(
        result.error.find("ERROR compile 1:1 SQL text exceeds maximum length\n") !=
        std::string::npos);
    CHECK(result.error.find("tinydbms> ") != std::string::npos);

    const InvocationResult reopened =
        invoke_cli(data_dir.path(), "SELECT * FROM recovered;\n", false);
    CHECK(reopened.exit_code == 0);
    CHECK(reopened.error.empty());
    CHECK(reopened.output == "id\n7\n");
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

}  // namespace

int main() {
    try {
        return test_persistent_cli_lifecycle() &&
                test_batch_stops_on_compile_error() &&
                test_utf8_data_directory_lifecycle() &&
                test_batch_reports_semantic_error() &&
                test_batch_reports_storage_error_and_stops() &&
                test_repl_recovers_after_oversized_input() &&
                test_open_error_has_storage_exit_status()
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
