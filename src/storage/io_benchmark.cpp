#include "buffer_pool.h"
#include "file_manager.h"

#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace tinydbms::storage::internal;
using tinydbms::TableId;
using Clock = std::chrono::steady_clock;

enum class Workload {
    kSequentialRead,
    kRandomRead,
    kRandomWrite,
    kMixed,
    kDirtyBatchFlush,
    kFullPoolDirtyRead,
};
enum class FlushStrategy { kCurrent, kAscendingPageId };
enum class AccessPattern { kIndependent, kContention };
struct Options {
    Workload workload = Workload::kSequentialRead;
    ReplacementPolicy policy = ReplacementPolicy::kFifo;
    std::size_t pages = 128;
    std::size_t capacity = 32;
    std::size_t operations = 0;
    std::uint64_t seed = 0x5444424d535f494fULL;
    std::size_t repetitions = 5;
    std::size_t warmups = 0;
    FlushStrategy strategy = FlushStrategy::kCurrent;
    bool both_strategies = false;
    std::size_t threads = 1;
    std::size_t page_files = 2;
    AccessPattern access_pattern = AccessPattern::kIndependent;
    std::string output;
};
struct Result {
    std::uint64_t elapsed_us = 0;
    std::uint64_t mutation_us = 0;
    std::uint64_t flush_us = 0;
    std::size_t reads = 0;
    std::size_t writes = 0;
    BufferPoolStats buffer;
    PageFileStats page;
};
struct DirtyBatchResult {
    std::uint64_t ordering_us = 0;
    std::uint64_t flush_io_us = 0;
    std::uint64_t total_us = 0;
    std::size_t dirty_pages = 0;
    std::uint64_t evictions = 0;
    PageFileStats page;
};
struct FullPoolDirtyResult {
    Result result;
    std::size_t dirty_resident_pages_at_start = 0;
    std::size_t verified_pages = 0;
};

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
const char* name(Workload value) {
    switch (value) {
        case Workload::kSequentialRead: return "sequential-read";
        case Workload::kRandomRead: return "random-read";
        case Workload::kRandomWrite: return "random-write";
        case Workload::kMixed: return "mixed";
        case Workload::kDirtyBatchFlush: return "dirty-batch-flush";
        case Workload::kFullPoolDirtyRead: return "full-pool-dirty-read";
    }
    return "unknown";
}
const char* name(FlushStrategy value) {
    return value == FlushStrategy::kCurrent ? "current" : "ascending-page-id";
}
const char* name(ReplacementPolicy value) {
    return value == ReplacementPolicy::kFifo ? "fifo" : "lru";
}
const char* name(AccessPattern value) {
    return value == AccessPattern::kIndependent ? "independent" : "contention";
}

std::size_t parse_size(std::string_view text, const char* argument) {
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        fail(std::string("invalid ") + argument);
    }
    return value;
}
Options parse(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            std::cout << "--workload sequential-read|random-read|random-write|mixed|dirty-batch-flush|full-pool-dirty-read "
                         "--policy fifo|lru --pages N --buffer-capacity N --operations N "
                         "--seed N --repetitions N --warmups N --threads N --page-files N "
                         "--access-pattern independent|contention "
                         "--strategy current|ascending-page-id|both --output FILE\n";
            std::exit(0);
        }
        if (index + 1 >= argc) fail("missing value for " + std::string(argument));
        const std::string_view value = argv[++index];
        if (argument == "--workload") {
            if (value == "sequential-read") result.workload = Workload::kSequentialRead;
            else if (value == "random-read") result.workload = Workload::kRandomRead;
            else if (value == "random-write") result.workload = Workload::kRandomWrite;
            else if (value == "mixed") result.workload = Workload::kMixed;
            else if (value == "dirty-batch-flush") result.workload = Workload::kDirtyBatchFlush;
            else if (value == "full-pool-dirty-read") result.workload = Workload::kFullPoolDirtyRead;
            else fail("invalid --workload");
        } else if (argument == "--policy") {
            if (value == "fifo") result.policy = ReplacementPolicy::kFifo;
            else if (value == "lru") result.policy = ReplacementPolicy::kLru;
            else fail("invalid --policy");
        } else if (argument == "--pages") result.pages = parse_size(value, "--pages");
        else if (argument == "--buffer-capacity") result.capacity = parse_size(value, "--buffer-capacity");
        else if (argument == "--operations") result.operations = parse_size(value, "--operations");
        else if (argument == "--seed") result.seed = parse_size(value, "--seed");
        else if (argument == "--repetitions") result.repetitions = parse_size(value, "--repetitions");
        else if (argument == "--warmups") result.warmups = parse_size(value, "--warmups");
        else if (argument == "--strategy") {
            if (value == "current") result.strategy = FlushStrategy::kCurrent;
            else if (value == "ascending-page-id") result.strategy = FlushStrategy::kAscendingPageId;
            else if (value == "both") result.both_strategies = true;
            else fail("invalid --strategy");
        } else if (argument == "--threads") result.threads = parse_size(value, "--threads");
        else if (argument == "--page-files") result.page_files = parse_size(value, "--page-files");
        else if (argument == "--access-pattern") {
            if (value == "independent") result.access_pattern = AccessPattern::kIndependent;
            else if (value == "contention") result.access_pattern = AccessPattern::kContention;
            else fail("invalid --access-pattern");
        } else if (argument == "--output") result.output = std::string(value);
        else fail("unknown argument " + std::string(argument));
    }
    if (result.operations == 0) result.operations = result.pages;
    return result;
}

class TemporaryDirectory {
public:
    TemporaryDirectory(std::uint64_t seed, std::size_t ordinal) {
        path_ = std::filesystem::temp_directory_path() /
            ("tinydbms-io-" + std::to_string(seed) + "-" + std::to_string(ordinal) + "-" +
             std::to_string(Clock::now().time_since_epoch().count()));
        std::error_code error;
        if (!std::filesystem::create_directories(path_, error) || error) fail("cannot create benchmark directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

RawPage pattern(PageId id) {
    RawPage page;
    for (std::size_t index = 0; index < page.bytes.size(); ++index) {
        page.bytes[index] = std::byte{static_cast<std::uint8_t>((std::uint64_t{id} * 31U + index) & 0xffU)};
    }
    return page;
}
PageFileStats delta(const PageFileStats& before, const PageFileStats& after) {
    PageFileStats value;
    value.physical_read_attempts = after.physical_read_attempts - before.physical_read_attempts;
    value.physical_read_successes = after.physical_read_successes - before.physical_read_successes;
    value.physical_read_failures = after.physical_read_failures - before.physical_read_failures;
    value.physical_write_attempts = after.physical_write_attempts - before.physical_write_attempts;
    value.physical_write_successes = after.physical_write_successes - before.physical_write_successes;
    value.physical_write_failures = after.physical_write_failures - before.physical_write_failures;
    value.bytes_read = after.bytes_read - before.bytes_read;
    value.bytes_written = after.bytes_written - before.bytes_written;
    value.read_time_total = after.read_time_total - before.read_time_total;
    value.write_time_total = after.write_time_total - before.write_time_total;
    value.flush_count = after.flush_count - before.flush_count;
    value.flush_time_total = after.flush_time_total - before.flush_time_total;
    value.sequential_read_count = after.sequential_read_count - before.sequential_read_count;
    value.sequential_write_count = after.sequential_write_count - before.sequential_write_count;
    value.read_logical_page_distance = after.read_logical_page_distance - before.read_logical_page_distance;
    value.write_logical_page_distance = after.write_logical_page_distance - before.write_logical_page_distance;
    return value;
}
BufferPoolStats delta(const BufferPoolStats& before, const BufferPoolStats& after) {
    BufferPoolStats value;
    value.fetch_count = after.fetch_count - before.fetch_count;
    value.hit_count = after.hit_count - before.hit_count;
    value.miss_count = after.miss_count - before.miss_count;
    value.eviction_count = after.eviction_count - before.eviction_count;
    value.dirty_flush_count = after.dirty_flush_count - before.dirty_flush_count;
    value.free_frame_miss_count = after.free_frame_miss_count - before.free_frame_miss_count;
    value.clean_victim_selection_count = after.clean_victim_selection_count - before.clean_victim_selection_count;
    value.clean_replacement_commit_count = after.clean_replacement_commit_count - before.clean_replacement_commit_count;
    value.clean_replacement_cancel_count = after.clean_replacement_cancel_count - before.clean_replacement_cancel_count;
    value.dirty_victim_selection_count = after.dirty_victim_selection_count - before.dirty_victim_selection_count;
    value.dirty_requested_read_count = after.dirty_requested_read_count - before.dirty_requested_read_count;
    value.dirty_write_attempt_count = after.dirty_write_attempt_count - before.dirty_write_attempt_count;
    value.dirty_write_success_count = after.dirty_write_success_count - before.dirty_write_success_count;
    value.dirty_replacement_commit_count = after.dirty_replacement_commit_count - before.dirty_replacement_commit_count;
    value.dirty_replacement_cancel_count = after.dirty_replacement_cancel_count - before.dirty_replacement_cancel_count;
    return value;
}

Result run_case(const Options& options, std::size_t ordinal) {
    TemporaryDirectory temporary(options.seed, ordinal);
    auto manager_result = FileManager::open(temporary.path());
    if (!manager_result.value) fail("cannot open benchmark FileManager");
    auto manager = std::move(*manager_result.value);
    auto file_result = manager->create_table_file(2);
    if (!file_result.value) fail("cannot create benchmark PageFile");
    auto file_lease = manager->acquire_file(2);
    if (!file_lease.value) fail("cannot lease benchmark PageFile");
    PageFile& file = **file_lease.value;
    std::vector<RawPage> expected(options.pages + 1);
    for (std::size_t index = 1; index <= options.pages; ++index) {
        const PageId id = static_cast<PageId>(index);
        if (file.allocate_page().value != std::optional<PageId>{id}) fail("cannot allocate benchmark page");
        expected[index] = pattern(id);
        if (file.write_page(id, expected[index])) fail("cannot initialize benchmark page");
    }
    auto pool_result = BufferPool::create(*manager, options.capacity, {}, {}, options.policy);
    if (!pool_result.value) fail("cannot create benchmark BufferPool");
    auto pool = std::move(*pool_result.value);

    std::vector<PageId> sequence(options.pages);
    for (std::size_t index = 0; index < options.pages; ++index) sequence[index] = static_cast<PageId>(index + 1);
    // Repetitions use the identical workload; ordinal only makes the temporary
    // directory unique and must never perturb the benchmark access sequence.
    std::mt19937_64 generator(options.seed);
    if (options.workload == Workload::kRandomRead) std::shuffle(sequence.begin(), sequence.end(), generator);
    std::uniform_int_distribution<std::size_t> pick(1, options.pages);
    const BufferPoolStats buffer_before = pool->stats();
    const PageFileStats page_before = file.stats();
    Result result;
    const auto started = Clock::now();
    const auto mutation_started = started;
    for (std::size_t operation = 0; operation < options.operations; ++operation) {
        const bool is_read = options.workload == Workload::kSequentialRead ||
            options.workload == Workload::kRandomRead ||
            (options.workload == Workload::kMixed &&
             ((operation + 1) * 7 / 10 != operation * 7 / 10));
        const PageId id = options.workload == Workload::kSequentialRead ||
                                options.workload == Workload::kRandomRead
            ? sequence[operation % sequence.size()]
            : static_cast<PageId>(pick(generator));
        auto guard = pool->fetch_page({2, id});
        if (!guard.value) fail("benchmark fetch failed");
        if (is_read) {
            if (guard.value->page().bytes != expected[id].bytes) {
                fail("benchmark read verification failed");
            }
            ++result.reads;
        } else {
            RawPage& page = guard.value->mutable_page();
            page.bytes[0] = std::byte{static_cast<std::uint8_t>(operation & 0xffU)};
            expected[id].bytes[0] = page.bytes[0];
            guard.value->mark_dirty();
            ++result.writes;
        }
    }
    const auto mutation_finished = Clock::now();
    if (result.writes != 0) {
        const auto flush_started = Clock::now();
        if (auto error = pool->flush_all()) fail("benchmark flush_all failed: " + error->message);
        result.flush_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - flush_started).count());
    }
    const auto finished = Clock::now();
    result.mutation_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(mutation_finished - mutation_started).count());
    result.elapsed_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());
    result.buffer = delta(buffer_before, pool->stats());
    result.page = delta(page_before, file.stats());

    if (auto error = pool->close()) fail("benchmark pool close failed: " + error->message);
    file_lease.value->release();
    if (auto error = manager->close_all()) fail("benchmark file close failed: " + error->message);
    auto reopened = PageFile::open(temporary.path() / "tables" / "table_2.dat");
    if (!reopened.value) fail("benchmark reopen failed");
    for (std::size_t index = 1; index <= options.pages; ++index) {
        const auto page = (*reopened.value)->read_page(static_cast<PageId>(index));
        if (!page.value || page.value->bytes != expected[index].bytes) fail("benchmark persistence verification failed");
    }
    if (auto error = (*reopened.value)->close()) fail("benchmark reopened close failed: " + error->message);
    return result;
}

DirtyBatchResult run_dirty_batch_case(const Options& options, FlushStrategy strategy,
                                      std::size_t ordinal) {
    if (options.capacity < options.pages)
        fail("dirty-batch-flush requires --buffer-capacity >= --pages");
    TemporaryDirectory temporary(options.seed, ordinal);
    auto manager_result = FileManager::open(temporary.path());
    if (!manager_result.value) fail("cannot open benchmark FileManager");
    auto manager = std::move(*manager_result.value);
    auto file_result = manager->create_table_file(2);
    if (!file_result.value) fail("cannot create benchmark PageFile");
    auto file_lease = manager->acquire_file(2);
    if (!file_lease.value) fail("cannot lease benchmark PageFile");
    PageFile& file = **file_lease.value;
    std::vector<RawPage> expected(options.pages + 1);
    for (std::size_t index = 1; index <= options.pages; ++index) {
        const PageId id = static_cast<PageId>(index);
        if (file.allocate_page().value != std::optional<PageId>{id})
            fail("cannot allocate benchmark page");
        expected[index] = pattern(id);
        if (file.write_page(id, expected[index])) fail("cannot initialize benchmark page");
    }
    auto pool_result = BufferPool::create(*manager, options.capacity, {}, {}, options.policy);
    if (!pool_result.value) fail("cannot create benchmark BufferPool");
    auto pool = std::move(*pool_result.value);
    std::vector<PageId> sequence(options.pages);
    for (std::size_t index = 0; index < options.pages; ++index)
        sequence[index] = static_cast<PageId>(index + 1);
    std::mt19937_64 generator(options.seed);
    std::shuffle(sequence.begin(), sequence.end(), generator);
    for (std::size_t operation = 0; operation < sequence.size(); ++operation) {
        const PageId id = sequence[operation];
        auto guard = pool->fetch_page({2, id});
        if (!guard.value) fail("dirty-batch mutation fetch failed");
        RawPage& page = guard.value->mutable_page();
        page.bytes[0] = std::byte{static_cast<std::uint8_t>(operation & 0xffU)};
        expected[id].bytes[0] = page.bytes[0];
        guard.value->mark_dirty();
    }
    const BufferPoolStats mutation_stats = pool->stats();
    if (mutation_stats.eviction_count != 0) fail("dirty-batch-flush invalid: mutation evicted a frame");
    const auto preflight = pool->snapshot_dirty_frames_for_experiment();
    if (!preflight.value || preflight.value->size() != options.pages)
        fail("dirty-batch-flush invalid: dirty resident pages do not equal page count");

    const PageFileStats page_before = file.stats();
    const auto total_started = Clock::now();
    const auto ordering_started = total_started;
    auto snapshot_result = pool->snapshot_dirty_frames_for_experiment();
    if (!snapshot_result.value) fail("dirty-batch-flush invalid: cannot snapshot dirty frames");
    auto snapshot = std::move(*snapshot_result.value);
    if (strategy == FlushStrategy::kAscendingPageId) {
        std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
            return left.key.page_id < right.key.page_id;
        });
    }
    const auto ordering_finished = Clock::now();
    auto flush_result = pool->flush_dirty_frames_for_experiment(snapshot);
    if (!flush_result.value) fail("dirty-batch flush failed: " + flush_result.error->message);

    DirtyBatchResult result;
    result.ordering_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(ordering_finished - ordering_started).count());
    result.flush_io_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(flush_result.value->flush_io_time).count());
    result.total_us = result.ordering_us + result.flush_io_us;
    result.dirty_pages = snapshot.size();
    result.evictions = mutation_stats.eviction_count;
    result.page = delta(page_before, file.stats());
    const std::uint64_t expected_bytes = static_cast<std::uint64_t>(options.pages) * RawPage{}.bytes.size();
    if (result.page.physical_write_attempts != options.pages ||
        result.page.physical_write_successes != options.pages ||
        result.page.physical_write_failures != 0 || result.page.bytes_written != expected_bytes ||
        result.page.flush_count != options.pages)
        fail("dirty-batch-flush invalid: physical write invariants failed");

    if (auto error = pool->close()) fail("benchmark pool close failed: " + error->message);
    file_lease.value->release();
    if (auto error = manager->close_all()) fail("benchmark file close failed: " + error->message);
    auto reopened = PageFile::open(temporary.path() / "tables" / "table_2.dat");
    if (!reopened.value) fail("benchmark reopen failed");
    for (std::size_t index = 1; index <= options.pages; ++index) {
        const auto page = (*reopened.value)->read_page(static_cast<PageId>(index));
        if (!page.value || page.value->bytes != expected[index].bytes)
            fail("dirty-batch persistence verification failed");
    }
    if (auto error = (*reopened.value)->close()) fail("benchmark reopened close failed: " + error->message);
    return result;
}

PageFileStats aggregate_stats(const std::vector<PageFileLease>& leases) {
    PageFileStats total;
    for (const PageFileLease& lease : leases) {
        const PageFileStats stats = lease->stats();
        total.physical_read_attempts += stats.physical_read_attempts;
        total.physical_read_successes += stats.physical_read_successes;
        total.physical_read_failures += stats.physical_read_failures;
        total.physical_write_attempts += stats.physical_write_attempts;
        total.physical_write_successes += stats.physical_write_successes;
        total.physical_write_failures += stats.physical_write_failures;
        total.bytes_read += stats.bytes_read;
        total.bytes_written += stats.bytes_written;
        total.read_time_total += stats.read_time_total;
        total.write_time_total += stats.write_time_total;
        total.flush_count += stats.flush_count;
        total.flush_time_total += stats.flush_time_total;
        total.sequential_read_count += stats.sequential_read_count;
        total.sequential_write_count += stats.sequential_write_count;
        total.read_logical_page_distance += stats.read_logical_page_distance;
        total.write_logical_page_distance += stats.write_logical_page_distance;
    }
    return total;
}

std::byte dirty_mutation(TableId table_id, PageId page_id, std::size_t operation) {
    return std::byte{static_cast<std::uint8_t>(
        (static_cast<std::uint64_t>(table_id) * 37U + static_cast<std::uint64_t>(page_id) * 17U +
         static_cast<std::uint64_t>(operation)) & 0xffU)};
}

FullPoolDirtyResult run_full_pool_dirty_case(const Options& options, std::size_t ordinal) {
    if (options.page_files > 2) fail("full-pool-dirty-read supports one or two --page-files");
    if (options.access_pattern == AccessPattern::kContention && options.operations % options.threads != 0)
        fail("full-pool-dirty-read contention requires --operations divisible by --threads");

    struct WorkItem { std::size_t file_index; PageId page_id; std::size_t operation; };
    std::vector<std::size_t> preload_counts(options.page_files);
    for (std::size_t frame = 0; frame < options.capacity; ++frame)
        ++preload_counts[frame % options.page_files];
    std::vector<std::vector<WorkItem>> streams(options.threads);
    std::vector<std::size_t> next_page_id(options.page_files);
    for (std::size_t file_index = 0; file_index < options.page_files; ++file_index)
        next_page_id[file_index] = preload_counts[file_index] + 1;
    if (options.access_pattern == AccessPattern::kIndependent) {
        for (std::size_t operation = 0; operation < options.operations; ++operation) {
            const std::size_t thread_index = operation % options.threads;
            const std::size_t file_index = thread_index % options.page_files;
            streams[thread_index].push_back(
                {file_index, static_cast<PageId>(next_page_id[file_index]++), operation});
        }
    } else {
        const std::size_t batches = options.operations / options.threads;
        for (std::size_t batch = 0; batch < batches; ++batch) {
            const std::size_t file_index = batch % options.page_files;
            const WorkItem item{file_index, static_cast<PageId>(next_page_id[file_index]++), batch};
            for (auto& stream : streams) stream.push_back(item);
        }
    }

    TemporaryDirectory temporary(options.seed, ordinal);
    auto manager_result = FileManager::open(temporary.path());
    if (!manager_result.value) fail("cannot open benchmark FileManager");
    auto manager = std::move(*manager_result.value);
    std::vector<PageFileLease> leases;
    std::vector<std::vector<RawPage>> expected(options.page_files);
    leases.reserve(options.page_files);
    for (std::size_t file_index = 0; file_index < options.page_files; ++file_index) {
        const TableId table_id = static_cast<TableId>(2 + file_index);
        if (!manager->create_table_file(table_id).value) fail("cannot create full-pool benchmark PageFile");
        auto lease = manager->acquire_file(table_id);
        if (!lease.value) fail("cannot lease full-pool benchmark PageFile");
        leases.push_back(std::move(*lease.value));
        expected[file_index].resize(next_page_id[file_index]);
        for (std::size_t page = 1; page < next_page_id[file_index]; ++page) {
            const PageId id = static_cast<PageId>(page);
            if (leases.back()->allocate_page().value != std::optional<PageId>{id})
                fail("cannot allocate full-pool benchmark page");
            expected[file_index][page] = pattern(id);
            if (auto error = leases.back()->write_page(id, expected[file_index][page]))
                fail("cannot initialize full-pool benchmark page: " + error->message);
        }
    }
    auto pool_result = BufferPool::create(*manager, options.capacity, {}, {}, options.policy);
    if (!pool_result.value) fail("cannot create full-pool benchmark BufferPool");
    auto pool = std::move(*pool_result.value);
    for (std::size_t file_index = 0; file_index < options.page_files; ++file_index) {
        const TableId table_id = static_cast<TableId>(2 + file_index);
        for (std::size_t page = 1; page <= preload_counts[file_index]; ++page) {
            const PageId id = static_cast<PageId>(page);
            auto guard = pool->fetch_page({table_id, id});
            if (!guard.value) fail("cannot preload dirty benchmark frame");
            guard.value->mutable_page().bytes[0] = dirty_mutation(table_id, id, page);
            expected[file_index][page].bytes[0] = dirty_mutation(table_id, id, page);
            guard.value->mark_dirty();
        }
    }
    const auto dirty_snapshot = pool->snapshot_dirty_frames_for_experiment();
    if (!dirty_snapshot.value || dirty_snapshot.value->size() != options.capacity)
        fail("full-pool-dirty-read invalid: measured phase did not start full of dirty READY frames");

    const BufferPoolStats buffer_before = pool->stats();
    const PageFileStats page_before = aggregate_stats(leases);
    struct ThreadCounts { std::size_t reads = 0; std::size_t writes = 0; };
    std::vector<ThreadCounts> counts(options.threads);
    std::vector<std::exception_ptr> failures(options.threads);
    std::barrier phase(static_cast<std::ptrdiff_t>(options.threads));
    const auto started = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    for (std::size_t thread_index = 0; thread_index < options.threads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            try {
                for (const WorkItem& item : streams[thread_index]) {
                    if (options.access_pattern == AccessPattern::kContention) phase.arrive_and_wait();
                    const TableId table_id = static_cast<TableId>(2 + item.file_index);
                    auto guard = pool->fetch_page({table_id, item.page_id});
                    if (!guard.value) fail("full-pool-dirty-read fetch failed");
                    if (options.access_pattern == AccessPattern::kIndependent) {
                        const std::byte value = dirty_mutation(table_id, item.page_id, item.operation);
                        guard.value->mutable_page().bytes[0] = value;
                        expected[item.file_index][item.page_id].bytes[0] = value;
                        guard.value->mark_dirty();
                        ++counts[thread_index].writes;
                    } else {
                        phase.arrive_and_wait();
                        for (std::size_t writer = 0; writer < options.threads; ++writer) {
                            if (thread_index == writer) {
                                const std::byte value = dirty_mutation(
                                    table_id, item.page_id, item.operation * options.threads + writer);
                                guard.value->mutable_page().bytes[0] = value;
                                expected[item.file_index][item.page_id].bytes[0] = value;
                                guard.value->mark_dirty();
                                ++counts[thread_index].writes;
                            }
                            phase.arrive_and_wait();
                        }
                    }
                    if (options.access_pattern == AccessPattern::kContention) phase.arrive_and_wait();
                }
            } catch (...) {
                failures[thread_index] = std::current_exception();
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    const auto finished = Clock::now();

    FullPoolDirtyResult full;
    full.dirty_resident_pages_at_start = dirty_snapshot.value->size();
    full.result.elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());
    full.result.mutation_us = full.result.elapsed_us;
    for (const ThreadCounts& value : counts) {
        full.result.reads += value.reads;
        full.result.writes += value.writes;
    }
    full.result.buffer = delta(buffer_before, pool->stats());
    full.result.page = delta(page_before, aggregate_stats(leases));

    if (auto error = pool->close()) fail("full-pool benchmark pool close failed: " + error->message);
    for (PageFileLease& lease : leases) lease.release();
    if (auto error = manager->close_all()) fail("full-pool benchmark file close failed: " + error->message);
    for (std::size_t file_index = 0; file_index < options.page_files; ++file_index) {
        const TableId table_id = static_cast<TableId>(2 + file_index);
        auto reopened = PageFile::open(temporary.path() / "tables" /
                                       ("table_" + std::to_string(table_id) + ".dat"));
        if (!reopened.value) fail("full-pool benchmark reopen failed");
        for (std::size_t page = 1; page < expected[file_index].size(); ++page) {
            const auto actual = (*reopened.value)->read_page(static_cast<PageId>(page));
            if (!actual.value || actual.value->bytes != expected[file_index][page].bytes)
                fail("full-pool benchmark persistence verification failed");
            ++full.verified_pages;
        }
        if (auto error = (*reopened.value)->close()) fail("full-pool benchmark reopened close failed: " + error->message);
    }
    return full;
}

void write_dirty_batch_header(std::ostream& output) {
    output << "workload,strategy,pages,capacity,seed,repetition,buffer_pool_initial_state,"
              "os_page_cache_state,device_cache_state,dirty_pages,evictions,ordering_time_us,"
              "flush_io_time_us,total_scheduled_flush_time_us,physical_writes,"
              "physical_write_successes,physical_write_failures,bytes_written,flush_count,"
              "write_logical_page_distance,sequential_write_count,verification\n";
}
void write_dirty_batch_row(std::ostream& output, const Options& options, FlushStrategy strategy,
                           std::size_t repetition, const DirtyBatchResult& result) {
    output << "dirty-batch-flush," << name(strategy) << ',' << options.pages << ',' << options.capacity << ','
           << options.seed << ',' << repetition << ",empty,uncontrolled,uncontrolled," << result.dirty_pages << ','
           << result.evictions << ',' << result.ordering_us << ',' << result.flush_io_us << ',' << result.total_us
           << ',' << result.page.physical_write_attempts << ',' << result.page.physical_write_successes << ','
           << result.page.physical_write_failures << ',' << result.page.bytes_written << ',' << result.page.flush_count
           << ',' << result.page.write_logical_page_distance << ',' << result.page.sequential_write_count << ",pass\n";
}

void write_full_pool_dirty_header(std::ostream& output) {
    output << "workload,policy,capacity,operations,threads,page_files,access_pattern,seed,repetition,"
              "dirty_resident_pages_at_start,elapsed_us,fetch,hit,miss,eviction,dirty_flush,"
              "free_frame_miss,clean_victim_selection,clean_replacement_commit,clean_replacement_cancel,"
              "dirty_victim_selection,dirty_requested_read,dirty_write_attempt,dirty_write_success,"
              "dirty_replacement_commit,dirty_replacement_cancel,physical_read_attempts,"
              "physical_read_successes,physical_read_failures,physical_write_attempts,"
              "physical_write_successes,physical_write_failures,bytes_read,bytes_written,flush_count,"
              "read_time_us,write_time_us,flush_time_us,logical_nonmutating_fetches,logical_dirty_mutations,"
              "physical_writes_per_dirty_commit,verified_pages,verification\n";
}
void write_full_pool_dirty_row(std::ostream& output, const Options& options, std::size_t repetition,
                               const FullPoolDirtyResult& full) {
    const Result& result = full.result;
    const auto micros = [](std::chrono::nanoseconds value) {
        return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
    };
    const double amplification = result.buffer.dirty_replacement_commit_count == 0 ? 0.0 :
        static_cast<double>(result.page.physical_write_successes) /
            static_cast<double>(result.buffer.dirty_replacement_commit_count);
    output << "full-pool-dirty-read," << name(options.policy) << ',' << options.capacity << ','
           << options.operations << ',' << options.threads << ',' << options.page_files << ','
           << name(options.access_pattern) << ',' << options.seed << ',' << repetition << ','
           << full.dirty_resident_pages_at_start << ',' << result.elapsed_us << ','
           << result.buffer.fetch_count << ',' << result.buffer.hit_count << ',' << result.buffer.miss_count << ','
           << result.buffer.eviction_count << ',' << result.buffer.dirty_flush_count << ','
           << result.buffer.free_frame_miss_count << ',' << result.buffer.clean_victim_selection_count << ','
           << result.buffer.clean_replacement_commit_count << ',' << result.buffer.clean_replacement_cancel_count << ','
           << result.buffer.dirty_victim_selection_count << ',' << result.buffer.dirty_requested_read_count << ','
           << result.buffer.dirty_write_attempt_count << ',' << result.buffer.dirty_write_success_count << ','
           << result.buffer.dirty_replacement_commit_count << ',' << result.buffer.dirty_replacement_cancel_count << ','
           << result.page.physical_read_attempts << ',' << result.page.physical_read_successes << ','
           << result.page.physical_read_failures << ',' << result.page.physical_write_attempts << ','
           << result.page.physical_write_successes << ',' << result.page.physical_write_failures << ','
           << result.page.bytes_read << ',' << result.page.bytes_written << ',' << result.page.flush_count << ','
           << micros(result.page.read_time_total) << ',' << micros(result.page.write_time_total) << ','
           << micros(result.page.flush_time_total) << ',' << result.reads << ',' << result.writes << ','
           << std::fixed << std::setprecision(3) << amplification << ',' << full.verified_pages << ",pass\n";
}

void run_full_pool_dirty_benchmark(const Options& options, std::size_t& ordinal, std::ostream& output) {
    for (std::size_t warmup = 0; warmup < options.warmups; ++warmup)
        (void)run_full_pool_dirty_case(options, ordinal++);
    std::vector<std::uint64_t> elapsed;
    elapsed.reserve(options.repetitions);
    for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
        const FullPoolDirtyResult result = run_full_pool_dirty_case(options, ordinal++);
        if (result.dirty_resident_pages_at_start != options.capacity ||
            result.result.buffer.clean_victim_selection_count != 0 ||
            result.result.buffer.clean_replacement_commit_count != 0 ||
            result.result.buffer.clean_replacement_cancel_count != 0 ||
            result.result.buffer.dirty_replacement_cancel_count != 0 ||
            result.result.page.physical_read_failures != 0 ||
            result.result.page.physical_write_failures != 0)
            fail("full-pool-dirty-read invalid: dirty replacement invariants failed");
        write_full_pool_dirty_row(output, options, repetition + 1, result);
        elapsed.push_back(result.result.elapsed_us);
    }
    std::sort(elapsed.begin(), elapsed.end());
    const auto percentile = [&](std::size_t numerator) {
        return elapsed[(elapsed.size() - 1) * numerator / 100];
    };
    std::cerr << "full-pool-dirty-read/" << name(options.policy) << " cap=" << options.capacity
              << " threads=" << options.threads << " files=" << options.page_files
              << " pattern=" << name(options.access_pattern) << " min_us=" << elapsed.front()
              << " p25_us=" << percentile(25) << " median_us=" << percentile(50)
              << " p75_us=" << percentile(75) << " max_us=" << elapsed.back() << '\n';
}

struct DirtyBatchSummary {
    FlushStrategy strategy;
    std::uint64_t ordering_us;
    std::uint64_t flush_us;
    std::uint64_t total_us;
    std::uint64_t write_distance;
    std::uint64_t sequential_writes;
};

DirtyBatchSummary run_dirty_batch_strategy(const Options& options, FlushStrategy strategy,
                                           std::size_t& ordinal, std::ostream& output) {
    for (std::size_t warmup = 0; warmup < options.warmups; ++warmup)
        (void)run_dirty_batch_case(options, strategy, ordinal++);
    std::vector<std::uint64_t> ordering;
    std::vector<std::uint64_t> flush;
    std::vector<std::uint64_t> total;
    std::vector<std::uint64_t> distance;
    std::vector<std::uint64_t> sequential;
    std::optional<DirtyBatchResult> first;
    for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
        const DirtyBatchResult result = run_dirty_batch_case(options, strategy, ordinal++);
        if (!first) first = result;
        else if (result.dirty_pages != first->dirty_pages || result.evictions != first->evictions ||
                 result.page.physical_write_attempts != first->page.physical_write_attempts ||
                 result.page.physical_write_successes != first->page.physical_write_successes ||
                 result.page.physical_write_failures != first->page.physical_write_failures ||
                 result.page.bytes_written != first->page.bytes_written ||
                 result.page.flush_count != first->page.flush_count)
            fail("dirty-batch-flush invalid: deterministic correctness counters differ across repetitions");
        write_dirty_batch_row(output, options, strategy, repetition + 1, result);
        ordering.push_back(result.ordering_us);
        flush.push_back(result.flush_io_us);
        total.push_back(result.total_us);
        distance.push_back(result.page.write_logical_page_distance);
        sequential.push_back(result.page.sequential_write_count);
    }
    const auto median = [](std::vector<std::uint64_t>& values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    DirtyBatchSummary summary{strategy, median(ordering), median(flush), median(total),
                              median(distance), median(sequential)};
    std::cerr << "dirty-batch-flush/" << name(strategy) << " pages=" << options.pages
              << " median_ordering_us=" << summary.ordering_us
              << " median_flush_io_us=" << summary.flush_us << " median_total_us=" << summary.total_us
              << " median_write_distance=" << summary.write_distance
              << " median_sequential_writes=" << summary.sequential_writes << '\n';
    return summary;
}

void write_header(std::ostream& output) {
    output << "benchmark,policy,page_count,buffer_capacity,operations,seed,repetition,buffer_pool_initial_state,"
              "os_page_cache_state,device_cache_state,elapsed_us,mutation_us,flush_all_us,read_time_us,write_time_us,"
              "flush_time_us,fetch,hit,miss,hit_rate,eviction,dirty_flush,physical_read_attempts,"
              "free_frame_miss,clean_victim_selection,clean_replacement_commit,clean_replacement_cancel,"
              "dirty_victim_selection,dirty_requested_read,dirty_write_attempt,dirty_write_success,"
              "dirty_replacement_commit,dirty_replacement_cancel,"
              "physical_read_successes,physical_read_failures,physical_write_attempts,physical_write_successes,"
              "physical_write_failures,bytes_read,bytes_written,flush_count,read_logical_page_distance,"
              "write_logical_page_distance,sequential_read_count,sequential_write_count,read_operations,write_operations\n";
}
void write_row(std::ostream& output, const Options& options, std::size_t repetition, const Result& result) {
    const auto micros = [](std::chrono::nanoseconds value) { return std::chrono::duration_cast<std::chrono::microseconds>(value).count(); };
    const double rate = result.buffer.fetch_count == 0 ? 0.0 :
        static_cast<double>(result.buffer.hit_count) / static_cast<double>(result.buffer.fetch_count);
    output << name(options.workload) << ',' << name(options.policy) << ',' << options.pages << ',' << options.capacity << ','
           << options.operations << ',' << options.seed << ',' << repetition << ",empty,uncontrolled,uncontrolled,"
           << result.elapsed_us << ',' << result.mutation_us << ',' << result.flush_us << ','
           << micros(result.page.read_time_total) << ',' << micros(result.page.write_time_total) << ','
           << micros(result.page.flush_time_total) << ',' << result.buffer.fetch_count << ',' << result.buffer.hit_count << ','
           << result.buffer.miss_count << ',' << rate << ',' << result.buffer.eviction_count << ','
           << result.buffer.dirty_flush_count << ',' << result.page.physical_read_attempts << ','
           << result.buffer.free_frame_miss_count << ',' << result.buffer.clean_victim_selection_count << ','
           << result.buffer.clean_replacement_commit_count << ',' << result.buffer.clean_replacement_cancel_count << ','
           << result.buffer.dirty_victim_selection_count << ',' << result.buffer.dirty_requested_read_count << ','
           << result.buffer.dirty_write_attempt_count << ',' << result.buffer.dirty_write_success_count << ','
           << result.buffer.dirty_replacement_commit_count << ',' << result.buffer.dirty_replacement_cancel_count << ','
           << result.page.physical_read_successes << ',' << result.page.physical_read_failures << ','
           << result.page.physical_write_attempts << ',' << result.page.physical_write_successes << ','
           << result.page.physical_write_failures << ',' << result.page.bytes_read << ',' << result.page.bytes_written << ','
           << result.page.flush_count << ',' << result.page.read_logical_page_distance << ','
           << result.page.write_logical_page_distance << ',' << result.page.sequential_read_count << ','
           << result.page.sequential_write_count << ',' << result.reads << ',' << result.writes << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        std::ofstream file_output;
        std::ostream* output = &std::cout;
        bool header = true;
        if (!options.output.empty()) {
            header = !std::filesystem::exists(options.output) || std::filesystem::file_size(options.output) == 0;
            file_output.open(options.output, std::ios::app);
            if (!file_output) fail("cannot open --output file");
            output = &file_output;
        }
        if (options.workload == Workload::kDirtyBatchFlush) {
            if (header) write_dirty_batch_header(*output);
            std::vector<DirtyBatchSummary> summaries;
            std::size_t ordinal = 0;
            summaries.push_back(run_dirty_batch_strategy(options, options.strategy, ordinal, *output));
            if (options.both_strategies) {
                summaries.push_back(run_dirty_batch_strategy(
                    options, FlushStrategy::kAscendingPageId, ordinal, *output));
                const DirtyBatchSummary& current = summaries.front();
                const DirtyBatchSummary& ascending = summaries.back();
                const auto percent_change = [](std::uint64_t baseline, std::uint64_t candidate) {
                    return baseline == 0 ? 0.0 :
                        (static_cast<double>(candidate) - static_cast<double>(baseline)) * 100.0 /
                            static_cast<double>(baseline);
                };
                std::cerr << std::fixed << std::setprecision(2)
                          << "relative ascending-vs-current distance_reduction_pct="
                          << -percent_change(current.write_distance, ascending.write_distance)
                          << " flush_time_change_pct=" << percent_change(current.flush_us, ascending.flush_us)
                          << " total_time_change_pct=" << percent_change(current.total_us, ascending.total_us) << '\n';
            }
            return 0;
        }
        if (options.workload == Workload::kFullPoolDirtyRead) {
            if (header) write_full_pool_dirty_header(*output);
            std::size_t ordinal = 0;
            run_full_pool_dirty_benchmark(options, ordinal, *output);
            return 0;
        }
        if (header) write_header(*output);
        for (std::size_t warmup = 0; warmup < options.warmups; ++warmup) (void)run_case(options, warmup);
        std::vector<std::uint64_t> elapsed;
        elapsed.reserve(options.repetitions);
        std::optional<PageFileStats> first_page_stats;
        std::optional<BufferPoolStats> first_buffer_stats;
        for (std::size_t repetition = 0; repetition < options.repetitions; ++repetition) {
            const Result result = run_case(options, options.warmups + repetition);
            write_row(*output, options, repetition + 1, result);
            elapsed.push_back(result.elapsed_us);
            if (!first_page_stats.has_value()) {
                first_page_stats = result.page;
                first_buffer_stats = result.buffer;
            } else if (result.page.physical_read_attempts != first_page_stats->physical_read_attempts ||
                       result.page.physical_write_attempts != first_page_stats->physical_write_attempts ||
                       result.buffer.fetch_count != first_buffer_stats->fetch_count ||
                       result.buffer.miss_count != first_buffer_stats->miss_count) {
                std::cerr << "warning: deterministic workload counters differ across repetitions\n";
            }
            std::cerr << name(options.workload) << '/' << name(options.policy) << " pages=" << options.pages
                      << " cap=" << options.capacity << " repetition=" << repetition + 1
                      << " elapsed_us=" << result.elapsed_us << "\n";
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::cerr << "summary min_us=" << elapsed.front() << " median_us="
                  << elapsed[elapsed.size() / 2] << " max_us=" << elapsed.back() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "tinydbms_io_benchmark: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
