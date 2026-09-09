#ifndef TINYDBMS_TEST_STORAGE_FAKE_HPP
#define TINYDBMS_TEST_STORAGE_FAKE_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include "tinydbms/storage.hpp"

namespace tinydbms::testing::fake_storage {

struct State {
    bool opened = false;
    std::size_t open_calls = 0;
    std::size_t close_calls = 0;
    std::size_t list_tables_calls = 0;
    std::size_t create_table_calls = 0;

    std::vector<TableMeta> tables;
    std::optional<storage::CreateTableRequest> last_create_request;
    std::optional<storage::StorageError> open_error;
    std::optional<storage::StorageError> close_error;
    std::optional<storage::StorageError> list_tables_error;
    std::optional<storage::StorageError> create_table_error;
    bool throw_on_open = false;
    bool throw_after_open = false;
    bool throw_on_close = false;
    bool throw_on_list_tables = false;
    bool throw_on_create_table = false;
};

void reset();
State& state();
void set_tables(std::vector<TableMeta> tables);
void set_open_error(storage::StorageError error);
void set_close_error(storage::StorageError error);
void set_list_tables_error(storage::StorageError error);
void set_create_table_error(storage::StorageError error);
void set_throw_on_open(bool enabled);
void set_throw_after_open(bool enabled);
void set_throw_on_close(bool enabled);
void set_throw_on_list_tables(bool enabled);
void set_throw_on_create_table(bool enabled);
void clear_close_error();
void clear_create_table_error();

}  // namespace tinydbms::testing::fake_storage

#endif  // TINYDBMS_TEST_STORAGE_FAKE_HPP
