#include "file_manager.h"
#include "storage_test_access.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace tinydbms::storage::internal {
struct FileManagerTestAccess {
    static void wait_table_closing(FileManager& manager, TableId table_id) {
        std::unique_lock lock(manager.mutex_);
        manager.state_changed_.wait(lock, [&] { return manager.closing_tables_.contains(table_id); });
    }
    static void wait_all_closing(FileManager& manager) {
        std::unique_lock lock(manager.mutex_);
        manager.state_changed_.wait(lock, [&] { return manager.close_all_in_progress_; });
    }
    static std::size_t in_flight(FileManager& manager, TableId table_id) {
        std::lock_guard lock(manager.mutex_);
        const auto found = manager.in_flight_.find(table_id);
        return found == manager.in_flight_.end() ? 0 : found->second;
    }
};
} // namespace tinydbms::storage::internal

namespace {
using namespace tinydbms;
using namespace tinydbms::storage::internal;

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) throw std::runtime_error("FileManager concurrency contract at line " + std::to_string(at.line()));
}

class Fixture {
public:
    Fixture() : path_(std::filesystem::temp_directory_path() /
        ("tinydbms-file-manager-concurrency-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        check(std::filesystem::create_directory(path_));
        auto opened = FileManager::open(path_); check(opened.value.has_value()); manager_ = std::move(*opened.value);
        for (TableId table : {0U, 1U}) {
            auto created = manager_->create_table_file(table); check(created.value.has_value());
            check((*created.value)->allocate_page().value == std::optional<PageId>{1});
        }
    }
    ~Fixture() {
        manager_.reset(); std::error_code ignored; std::filesystem::remove_all(path_, ignored);
    }
    FileManager& manager() { return *manager_; }
private:
    std::filesystem::path path_;
    std::unique_ptr<FileManager> manager_;
};

void table_close_waits_and_rejects_new_leases() {
    Fixture f;
    auto held = f.manager().acquire_file(0); check(held.value.has_value());
    std::atomic<bool> close_done = false, close_ok = false;
    std::thread closer([&] { close_ok = !f.manager().close_table_file(0); close_done = true; });
    FileManagerTestAccess::wait_table_closing(f.manager(), 0);
    auto rejected = f.manager().acquire_file(0);
    check(rejected.error && rejected.error->kind == PageFileErrorKind::kInvalidArgument);
    check(!close_done.load());
    held.value->release();
    closer.join();
    check(close_ok.load() && close_done.load());
    check(f.manager().find_table_file(0) == nullptr);
}

void close_all_drains_and_preserves_independent_table_leases() {
    Fixture f;
    auto held_a = f.manager().acquire_file(0); auto held_b = f.manager().acquire_file(1);
    check(held_a.value.has_value() && held_b.value.has_value());
    std::atomic<bool> close_done = false, close_ok = false;
    std::thread closer([&] { close_ok = !f.manager().close_all(); close_done = true; });
    FileManagerTestAccess::wait_all_closing(f.manager());
    check(f.manager().acquire_file(0).error.has_value() && f.manager().acquire_file(1).error.has_value());
    held_a.value->release();
    check(!close_done.load());
    held_b.value->release();
    closer.join();
    check(close_ok.load() && close_done.load());
    check(f.manager().find_table_file(0) == nullptr && f.manager().find_table_file(1) == nullptr);

    Fixture independent;
    auto held_b_only = independent.manager().acquire_file(1); check(held_b_only.value.has_value());
    check(!independent.manager().close_table_file(0));
    check(independent.manager().find_table_file(0) == nullptr);
    check((**held_b_only.value).read_page(1).value.has_value());
    held_b_only.value->release();
}

void close_failure_reopens_leases_and_move_releases_once() {
    Fixture f;
    StorageTestAccess::fail_next_file_close();
    check(f.manager().close_table_file(0).has_value());
    check(f.manager().find_table_file(0) != nullptr);
    auto first = f.manager().acquire_file(0); auto second = f.manager().acquire_file(0);
    check(first.value.has_value() && second.value.has_value());
    *second.value = std::move(*first.value);
    check(!first.value->operator bool());
    check(FileManagerTestAccess::in_flight(f.manager(), 0) == 1);
    second.value->release();
    check(FileManagerTestAccess::in_flight(f.manager(), 0) == 0);
    check(!f.manager().close_table_file(0));
}

void concurrent_acquire_release_and_close_all() {
    Fixture f;
    auto held = f.manager().acquire_file(0); check(held.value.has_value());
    std::atomic<bool> start = false, stop = false, failed = false;
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 6; ++index) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            while (!stop.load(std::memory_order_acquire)) {
                auto lease = f.manager().acquire_file(1);
                if (!lease.value) {
                    if (lease.error && lease.error->kind == PageFileErrorKind::kInvalidArgument) break;
                    failed = true; break;
                }
                if (!(**lease.value).read_page(1).value) { failed = true; break; }
            }
        });
    }
    start.store(true, std::memory_order_release);
    std::atomic<bool> close_ok = false;
    std::thread closer([&] { close_ok = !f.manager().close_all(); });
    FileManagerTestAccess::wait_all_closing(f.manager());
    held.value->release();
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    closer.join();
    check(!failed.load() && close_ok.load());
}
} // namespace

int main() {
    try {
        table_close_waits_and_rejects_new_leases();
        close_all_drains_and_preserves_independent_table_leases();
        close_failure_reopens_leases_and_move_releases_once();
        concurrent_acquire_release_and_close_all();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
