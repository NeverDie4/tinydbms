#include "page_file.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tinydbms::storage::internal {
struct PageFileTestAccess {
    static void set_before_raw_read(PageFile& file, std::function<void()> hook) {
        std::lock_guard lock(file.mutex_);
        file.before_raw_read_for_testing_ = std::move(hook);
    }
};
} // namespace tinydbms::storage::internal

namespace {
using namespace tinydbms::storage::internal;

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) throw std::runtime_error("PageFile concurrency contract at line " + std::to_string(at.line()));
}

RawPage patterned(PageId page_id, std::uint8_t salt = 0) {
    RawPage page;
    for (std::size_t index = 0; index < page.bytes.size(); ++index)
        page.bytes[index] = std::byte{static_cast<std::uint8_t>(page_id + index + salt)};
    return page;
}

class Fixture {
public:
    Fixture(std::string label, PageId pages) : path_(std::filesystem::temp_directory_path() /
        ("tinydbms-page-file-concurrency-" + std::move(label) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        check(std::filesystem::create_directory(path_));
        auto created = PageFile::create(path_ / "table.dat"); check(created.value.has_value());
        file_ = std::move(*created.value);
        for (PageId page = 1; page <= pages; ++page) {
            check(file_->allocate_page().value == std::optional<PageId>{page});
            check(!file_->write_page(page, patterned(page)).has_value());
        }
    }
    ~Fixture() {
        file_.reset(); std::error_code ignored; std::filesystem::remove_all(path_, ignored);
    }
    PageFile& file() { return *file_; }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
    std::unique_ptr<PageFile> file_;
};

template <typename Work>
void concurrently(unsigned count, Work work) {
    std::barrier start(static_cast<std::ptrdiff_t>(count) + 1);
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            work(index);
        });
    }
    start.arrive_and_wait();
    for (auto& thread : threads) thread.join();
}

void concurrent_reads_different_pages() {
    Fixture fixture("different-reads", 8);
    const auto before = fixture.file().stats();
    constexpr unsigned kThreads = 8, kReads = 100;
    std::atomic<bool> failed = false;
    concurrently(kThreads, [&](unsigned thread) {
        for (unsigned iteration = 0; iteration < kReads; ++iteration) {
            const PageId page = static_cast<PageId>((thread + iteration) % 8U + 1U);
            auto read = fixture.file().read_page(page);
            if (!read.value || read.value->bytes != patterned(page).bytes) { failed = true; return; }
        }
    });
    check(!failed.load());
    const auto after = fixture.file().stats();
    check(after.physical_read_attempts == before.physical_read_attempts + kThreads * kReads);
    check(after.physical_read_successes == before.physical_read_successes + kThreads * kReads);
}

void concurrent_reads_same_page() {
    Fixture fixture("same-read", 1);
    const auto before = fixture.file().stats();
    constexpr unsigned kThreads = 8, kReads = 100;
    std::atomic<bool> failed = false;
    concurrently(kThreads, [&](unsigned) {
        for (unsigned iteration = 0; iteration < kReads; ++iteration) {
            auto read = fixture.file().read_page(1);
            if (!read.value || read.value->bytes != patterned(1).bytes) { failed = true; return; }
        }
    });
    check(!failed.load());
    const auto after = fixture.file().stats();
    check(after.physical_read_attempts == before.physical_read_attempts + kThreads * kReads);
    check(after.physical_read_successes == before.physical_read_successes + kThreads * kReads);
}

void concurrent_writes_and_readback() {
    Fixture fixture("writes", 4);
    std::atomic<bool> failed = false;
    concurrently(4, [&](unsigned thread) {
        const PageId page = thread + 1U;
        for (unsigned iteration = 0; iteration < 50; ++iteration)
            if (fixture.file().write_page(page, patterned(page, static_cast<std::uint8_t>(thread + 20U))).has_value()) {
                failed = true; return;
            }
    });
    check(!failed.load());
    check(!fixture.file().close());
    auto reopened = PageFile::open(fixture.path() / "table.dat"); check(reopened.value.has_value());
    for (PageId page = 1; page <= 4; ++page) {
        auto read = (*reopened.value)->read_page(page);
        check(read.value && read.value->bytes == patterned(page, static_cast<std::uint8_t>(page + 19U)).bytes);
    }
    check(!(*reopened.value)->close());
}

void concurrent_read_write_and_stats() {
    Fixture fixture("read-write-stats", 4);
    std::atomic<bool> failed = false;
    std::atomic<bool> workers_done = false;
    std::thread stats_reader([&] {
        while (!workers_done.load(std::memory_order_acquire)) {
            const auto snapshot = fixture.file().stats();
            if (snapshot.physical_read_successes > snapshot.physical_read_attempts ||
                snapshot.physical_read_failures > snapshot.physical_read_attempts ||
                snapshot.physical_read_successes + snapshot.physical_read_failures > snapshot.physical_read_attempts ||
                snapshot.physical_write_successes > snapshot.physical_write_attempts ||
                snapshot.physical_write_failures > snapshot.physical_write_attempts ||
                snapshot.physical_write_successes + snapshot.physical_write_failures > snapshot.physical_write_attempts) {
                failed = true;
            }
        }
    });
    concurrently(4, [&](unsigned thread) {
        const PageId page = thread + 1U;
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            if (thread < 2) {
                auto read = fixture.file().read_page(page);
                if (!read.value || read.value->bytes != patterned(page).bytes) { failed = true; return; }
            } else if (fixture.file().write_page(page, patterned(page, static_cast<std::uint8_t>(thread + 30U))).has_value()) {
                failed = true; return;
            }
        }
    });
    workers_done.store(true, std::memory_order_release);
    stats_reader.join();
    check(!failed.load());
}

void concurrent_allocate_and_free() {
    Fixture fixture("allocate-free", 0);
    constexpr unsigned kPages = 32;
    std::mutex results_mutex;
    std::vector<PageId> allocated;
    std::atomic<bool> failed = false;
    concurrently(kPages, [&](unsigned) {
        auto page = fixture.file().allocate_page();
        if (!page.value) { failed = true; return; }
        std::lock_guard lock(results_mutex); allocated.push_back(*page.value);
    });
    check(!failed.load());
    std::sort(allocated.begin(), allocated.end());
    check(allocated.size() == kPages && std::adjacent_find(allocated.begin(), allocated.end()) == allocated.end());
    check(fixture.file().page_count() == kPages + 1U);
    concurrently(kPages, [&](unsigned index) {
        if (fixture.file().free_page(allocated[index]).has_value()) failed = true;
    });
    check(!failed.load());
    for (PageId page : allocated) check(fixture.file().page_allocation_state(page).value == PageAllocationState::kFree);
    std::set<PageId> reused;
    for (unsigned index = 0; index < kPages; ++index) {
        auto page = fixture.file().allocate_page(); check(page.value.has_value()); reused.insert(*page.value);
    }
    check(reused.size() == kPages);
    std::atomic<unsigned> duplicate_free_successes = 0, duplicate_free_errors = 0;
    concurrently(2, [&](unsigned) {
        const auto result = fixture.file().free_page(allocated.front());
        if (!result) ++duplicate_free_successes;
        else if (result->kind == PageFileErrorKind::kInvalidArgument) ++duplicate_free_errors;
    });
    check(duplicate_free_successes == 1 && duplicate_free_errors == 1);
    check(fixture.file().page_allocation_state(allocated.front()).value == PageAllocationState::kFree);
}

void close_waits_for_active_operation() {
    Fixture fixture("close", 1);
    std::mutex coordination_mutex;
    std::condition_variable coordination;
    bool read_inside = false, release_read = false, close_started = false;
    std::atomic<bool> hook_once = true, close_done = false, close_ok = false, read_ok = false;
    PageFileTestAccess::set_before_raw_read(fixture.file(), [&] {
        if (!hook_once.exchange(false)) return;
        std::unique_lock lock(coordination_mutex);
        read_inside = true; coordination.notify_all();
        coordination.wait(lock, [&] { return release_read; });
    });
    std::thread reader([&] { read_ok = fixture.file().read_page(1).value.has_value(); });
    {
        std::unique_lock lock(coordination_mutex);
        coordination.wait(lock, [&] { return read_inside; });
    }
    std::thread closer([&] {
        {
            std::lock_guard lock(coordination_mutex);
            close_started = true;
        }
        coordination.notify_all();
        close_ok = !fixture.file().close();
        close_done = true;
    });
    {
        std::unique_lock lock(coordination_mutex);
        coordination.wait(lock, [&] { return close_started; });
    }
    check(!close_done.load());
    {
        std::lock_guard lock(coordination_mutex);
        release_read = true;
    }
    coordination.notify_all();
    reader.join(); closer.join();
    check(read_ok.load() && close_done.load() && close_ok.load());
    check(fixture.file().read_page(1).error.has_value());
    check(fixture.file().write_page(1, patterned(1)).has_value());
    check(fixture.file().allocate_page().error.has_value());
    check(fixture.file().free_page(1).has_value());
    check(fixture.file().page_allocation_state(1).error.has_value());
}
} // namespace

int main() {
    try {
        concurrent_reads_different_pages();
        concurrent_reads_same_page();
        concurrent_writes_and_readback();
        concurrent_read_write_and_stats();
        concurrent_allocate_and_free();
        close_waits_for_active_operation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
