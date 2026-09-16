#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

namespace tinydbms::core {
namespace {

std::atomic_bool g_database_open{false};

bool try_acquire_database_slot() noexcept {
    bool expected = false;
    return g_database_open.compare_exchange_strong(
        expected,
        true,
        std::memory_order_acquire,
        std::memory_order_relaxed);
}

void release_database_slot() noexcept {
    g_database_open.store(false, std::memory_order_release);
}

OpenDatabaseResult make_open_error(ErrorKind kind, std::string message) {
    return OpenDatabaseResult{std::optional<Error>{internal::make_error(kind, std::move(message))}};
}

CloseDatabaseResult make_close_error(ErrorKind kind, std::string message) {
    return CloseDatabaseResult{std::optional<Error>{internal::make_error(kind, std::move(message))}};
}

}  // namespace

void Database::Impl::clear() noexcept {
    open = false;
    catalog.clear();
    next_table_id = 0;
    cleanup_retry_needed = false;
}

void Database::Impl::abort_after_storage_exception() noexcept {
    if (!open && forced_close_pending) {
        return;
    }

    bool retry_cleanup = false;
    try {
        const storage::CloseStorageResult closed =
            storage::close_storage(storage::CloseStorageRequest{});
        retry_cleanup = closed.error.has_value() &&
            closed.error->kind != storage::StorageErrorKind::kInvalidRequest;
    } catch (...) {
        // close_storage is the only available best-effort recovery for an unknown
        // cursor or partially applied storage operation.
        retry_cleanup = true;
    }

    const bool owned_database_slot = open;
    clear();
    forced_close_pending = true;
    cleanup_retry_needed = retry_cleanup;
    if (owned_database_slot && !retry_cleanup) {
        release_database_slot();
    }
}

Database::Database() : impl_{std::make_unique<Impl>()} {}

Database::~Database() noexcept {
    if (impl_ == nullptr) {
        return;
    }

    const bool owns_database_slot = impl_->open || impl_->cleanup_retry_needed;
    if (owns_database_slot) {
        try {
            (void)storage::close_storage(storage::CloseStorageRequest{});
        } catch (...) {
            // Destruction is best-effort. The process-level slot must still be released.
        }
    }

    impl_->clear();
    impl_->forced_close_pending = false;
    impl_->cleanup_retry_needed = false;
    if (owns_database_slot) {
        release_database_slot();
    }
}

Database::Database(Database&& other) noexcept = default;

Database& Database::operator=(Database&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (impl_ != nullptr && (impl_->open || impl_->cleanup_retry_needed)) {
        const bool owned_database_slot = impl_->open || impl_->cleanup_retry_needed;
        bool retry_cleanup = false;
        try {
            const storage::CloseStorageResult closed =
                storage::close_storage(storage::CloseStorageRequest{});
            retry_cleanup = closed.error.has_value() &&
                closed.error->kind != storage::StorageErrorKind::kInvalidRequest;
        } catch (...) {
            retry_cleanup = true;
        }

        if (retry_cleanup) {
            // Preserve the current Impl and its retry handle. Replacing it would
            // leave a possibly open storage lifecycle without an owner.
            impl_->clear();
            impl_->forced_close_pending = true;
            impl_->cleanup_retry_needed = true;
            return *this;
        }

        impl_->clear();
        impl_->forced_close_pending = false;
        impl_->cleanup_retry_needed = false;
        if (owned_database_slot) {
            release_database_slot();
        }
    }

    impl_ = std::move(other.impl_);
    return *this;
}

OpenDatabaseResult Database::open(const OpenDatabaseRequest& request) {
    if (impl_ == nullptr) {
        return make_open_error(ErrorKind::kExecute, "database object is moved-from");
    }
    // 调用级互斥：整个 open 期间持锁，等待在途的 execute_script/close 结束。
    // 读取 impl_ 本身仍在锁外，这属于"移动/析构与调用并发"的调用方责任边界。
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (impl_->open) {
        return make_open_error(ErrorKind::kExecute, "database is already open");
    }
    if (impl_->forced_close_pending) {
        return make_open_error(
            ErrorKind::kExecute,
            "database requires close after a storage exception");
    }
    if (request.data_dir.empty()) {
        return make_open_error(ErrorKind::kExecute, "data_dir must not be empty");
    }
    if (!try_acquire_database_slot()) {
        return make_open_error(ErrorKind::kExecute, "another database is already open in this process");
    }

    bool storage_open = false;
    auto abort_open = [&](Error error, bool storage_state_unknown = false) -> OpenDatabaseResult {
        bool retry_cleanup = false;
        if (storage_open || storage_state_unknown) {
            try {
                const storage::CloseStorageResult closed =
                    storage::close_storage(storage::CloseStorageRequest{});
                retry_cleanup = closed.error.has_value() &&
                    closed.error->kind != storage::StorageErrorKind::kInvalidRequest;
            } catch (...) {
                // The primary open/recovery error is more useful than cleanup failure.
                retry_cleanup = true;
            }
        }
        impl_->clear();
        impl_->forced_close_pending = retry_cleanup;
        impl_->cleanup_retry_needed = retry_cleanup;
        if (!retry_cleanup) {
            release_database_slot();
        }
        return OpenDatabaseResult{std::optional<Error>{std::move(error)}};
    };

    try {
        const storage::OpenStorageResult opened =
            storage::open_storage(storage::OpenStorageRequest{request.data_dir});
        if (opened.error.has_value()) {
            // A failed open_storage is required not to acquire the storage lifecycle.
            return abort_open(internal::map_storage_error(*opened.error));
        }
        storage_open = true;

        storage::ListTablesResult listed =
            storage::list_tables(storage::ListTablesRequest{});
        if (listed.error.has_value()) {
            return abort_open(internal::map_storage_error(*listed.error));
        }

        std::vector<TableMeta> restored;
        restored.reserve(listed.tables.size());

        std::uint64_t next_table_id = 0;
        for (TableMeta& table : listed.tables) {
            if (auto metadata_error = internal::validate_table_metadata(table);
                metadata_error.has_value()) {
                return abort_open(std::move(*metadata_error));
            }
            if (internal::has_duplicate_table(restored, table)) {
                return abort_open(internal::make_error(
                    ErrorKind::kInternal,
                    "storage returned duplicate table metadata"));
            }
            next_table_id = std::max(
                next_table_id,
                static_cast<std::uint64_t>(table.table_id) + 1U);
            restored.push_back(std::move(table));
        }

        impl_->catalog = std::move(restored);
        impl_->next_table_id = next_table_id;
        impl_->open = true;
        impl_->forced_close_pending = false;
        impl_->cleanup_retry_needed = false;
        return OpenDatabaseResult{std::nullopt};
    } catch (const std::exception& exception) {
        // open_storage may have changed storage state before an unexpected throw.
        return abort_open(
            internal::make_error(ErrorKind::kInternal, exception.what()),
            !storage_open);
    } catch (...) {
        return abort_open(
            internal::make_error(ErrorKind::kInternal, "unknown exception while opening database"),
            !storage_open);
    }
}

CloseDatabaseResult Database::close() {
    if (impl_ == nullptr) {
        return make_close_error(ErrorKind::kExecute, "database object is moved-from");
    }
    // 与 open 同一把锁：close 会等待在途执行结束，不与执行交错。
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->open) {
        if (impl_->forced_close_pending) {
            if (impl_->cleanup_retry_needed) {
                try {
                    const storage::CloseStorageResult retried =
                        storage::close_storage(storage::CloseStorageRequest{});
                    if (retried.error.has_value() &&
                        retried.error->kind != storage::StorageErrorKind::kInvalidRequest) {
                        return make_close_error(
                            ErrorKind::kStorage,
                            retried.error->message);
                    }
                    impl_->cleanup_retry_needed = false;
                    impl_->forced_close_pending = false;
                    release_database_slot();
                    return CloseDatabaseResult{std::nullopt};
                } catch (const std::exception& exception) {
                    return make_close_error(ErrorKind::kInternal, exception.what());
                } catch (...) {
                    return make_close_error(
                        ErrorKind::kInternal,
                        "unknown exception while retrying storage cleanup");
                }
            }
            impl_->forced_close_pending = false;
            return CloseDatabaseResult{std::nullopt};
        }
        return make_close_error(ErrorKind::kExecute, "database is not open");
    }

    try {
        const storage::CloseStorageResult closed =
            storage::close_storage(storage::CloseStorageRequest{});
        if (closed.error.has_value() &&
            closed.error->kind != storage::StorageErrorKind::kInvalidRequest) {
            const Error error = internal::map_storage_error(*closed.error);
            impl_->clear();
            impl_->forced_close_pending = true;
            impl_->cleanup_retry_needed = true;
            return CloseDatabaseResult{error};
        }
    } catch (const std::exception& exception) {
        const Error error = internal::make_error(ErrorKind::kInternal, exception.what());
        impl_->clear();
        impl_->forced_close_pending = true;
        impl_->cleanup_retry_needed = true;
        return CloseDatabaseResult{std::move(error)};
    } catch (...) {
        const Error error = internal::make_error(
            ErrorKind::kInternal,
            "unknown exception while closing database");
        impl_->clear();
        impl_->forced_close_pending = true;
        impl_->cleanup_retry_needed = true;
        return CloseDatabaseResult{std::move(error)};
    }

    impl_->clear();
    impl_->forced_close_pending = false;
    impl_->cleanup_retry_needed = false;
    release_database_slot();
    return CloseDatabaseResult{std::nullopt};
}

double StorageStats::hit_rate() const noexcept {
    if (fetch_count == 0) return 0.0;
    return static_cast<double>(hit_count) / static_cast<double>(fetch_count);
}

StorageStatsResult Database::storage_stats() {
    if (impl_ == nullptr) {
        return StorageStatsResult{
            std::nullopt,
            internal::make_error(ErrorKind::kExecute, "database object is moved-from")};
    }
    // 与 open/close/execute_script 同一把锁：快照要么在在途调用之前、要么在其之后取得，
    // 不会读到执行中途的存储状态。
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->open) {
        return StorageStatsResult{
            std::nullopt,
            internal::make_error(ErrorKind::kExecute, "database is not open")};
    }

    try {
        const storage::StorageStatsResult snapshot =
            storage::storage_stats(storage::StorageStatsRequest{});
        if (snapshot.error.has_value()) {
            return StorageStatsResult{std::nullopt, internal::map_storage_error(*snapshot.error)};
        }
        if (!snapshot.stats.has_value()) {
            return StorageStatsResult{
                std::nullopt,
                internal::make_error(ErrorKind::kInternal, "storage returned no statistics")};
        }
        const storage::StorageStats& source = *snapshot.stats;
        return StorageStatsResult{
            StorageStats{
                .fetch_count = source.fetch_count,
                .hit_count = source.hit_count,
                .miss_count = source.miss_count,
                .eviction_count = source.eviction_count,
                .dirty_flush_count = source.dirty_flush_count},
            std::nullopt};
    } catch (const std::exception& exception) {
        return StorageStatsResult{
            std::nullopt,
            internal::make_error(ErrorKind::kInternal, exception.what())};
    } catch (...) {
        return StorageStatsResult{
            std::nullopt,
            internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while reading storage statistics")};
    }
}

}  // namespace tinydbms::core
