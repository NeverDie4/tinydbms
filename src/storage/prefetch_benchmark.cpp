#include "heap_table.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
using Clock = std::chrono::steady_clock;
constexpr std::size_t pages = 4096, capacity = 64, rows_per_page = 8;
constexpr int warmups = 2, measurements = 20;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-pref1-" + std::to_string(Clock::now().time_since_epoch().count()));
    TempDirectory() { require(std::filesystem::create_directory(path), "create fixture directory"); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
struct Sample {
    double milliseconds{};
    std::uint64_t reads{}, checksum{}, records{};
    BufferPoolStats stats;
};
// Unsigned, fixed-round predicate/projection/checksum work. Every output is
// consumed and checked against the OFF run, so it cannot be optimized away.
std::uint64_t cpu_work(const std::string& payload, int rounds, std::uint64_t checksum) {
    for (int round = 0; round < rounds; ++round) {
        for (char value : payload) {
            const auto byte = static_cast<unsigned char>(value);
            const std::uint64_t projection = (byte >= 'm') ? byte * 17U : byte + 31U;
            checksum = (checksum ^ projection) * 1099511628211ULL;
            checksum ^= checksum >> 29U;
        }
    }
    return checksum;
}
Sample run(FileManager& files, PageFile& file, const TableMeta& meta, bool enabled, int rounds) {
    auto made = BufferPool::create(files, capacity, {}, {}, ReplacementPolicy::kFifo, {}, enabled);
    require(made.value.has_value(), "create pool");
    auto pool = std::move(*made.value);
    HeapTable heap(meta, files, *pool);
    const auto before = file.stats();
    Sample sample;
    sample.checksum = 1469598103934665603ULL;
    const auto start = Clock::now();
    auto scan = heap.begin_scan();
    require(scan.value.has_value(), "begin scan");
    for (;;) {
        auto record = heap.next_record(*scan.value);
        require(!record.error, "scan failure");
        if (!record.value) break;
        const auto& payload = std::get<std::string>(record.value->values.front().data);
        sample.checksum = cpu_work(payload, rounds, sample.checksum);
        ++sample.records;
    }
    // Include worker drain in elapsed and counters: no hidden tail work.
    require(!pool->close(), "close pool");
    sample.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    sample.stats = pool->stats();
    sample.reads = file.stats().physical_read_attempts - before.physical_read_attempts;
    require(sample.records == pages * rows_per_page, "record count changed");
    return sample;
}
double quantile(const std::vector<double>& sorted, double fraction) {
    const double index = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(index);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * (index - static_cast<double>(lower));
}
void summarize(const char* profile, bool enabled, const std::vector<Sample>& samples) {
    std::vector<double> elapsed;
    for (const auto& sample : samples) elapsed.push_back(sample.milliseconds);
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << profile << ',' << (enabled ? "ON" : "OFF") << ','
              << elapsed.front() << ',' << quantile(elapsed, .25) << ',' << quantile(elapsed, .5)
              << ',' << quantile(elapsed, .75) << ',' << elapsed.back() << ','
              << static_cast<double>(pages) * 1000.0 / quantile(elapsed, .5);
    auto mean = [&](auto field) {
        double sum = 0;
        for (const auto& sample : samples) sum += static_cast<double>(field(sample));
        std::cout << ',' << sum / static_cast<double>(samples.size());
    };
    mean([](const Sample& s) { return s.reads; });
    mean([](const Sample& s) { return s.stats.prefetch_requested; });
    mean([](const Sample& s) { return s.stats.prefetch_started; });
    mean([](const Sample& s) { return s.stats.prefetch_ready; });
    mean([](const Sample& s) { return s.stats.prefetch_hit; });
    mean([](const Sample& s) { return s.stats.useful_prefetch; });
    mean([](const Sample& s) { return s.stats.foreground_wait_for_prefetch; });
    mean([](const Sample& s) { return s.stats.prefetch_dropped; });
    mean([](const Sample& s) { return s.stats.prefetch_read_failure; });
    mean([](const Sample& s) { return s.stats.unused_prefetch_evicted; });
    mean([](const Sample& s) { return s.stats.prefetch_deduplicated; });
    std::cout << '\n';
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: tinydbms_prefetch_benchmark <raw-results.csv>");
        std::ofstream raw(argv[1]);
        require(raw.is_open(), "open output CSV");
        raw << "profile,prefetch,iteration,elapsed_ms,physical_reads,checksum,records,requested,started,ready,hit,useful,wait,dropped,read_failure,unused,deduplicated\n";
        raw << std::fixed << std::setprecision(6);
        TempDirectory temp;
        auto opened = FileManager::open(temp.path);
        require(opened.value.has_value(), "open FileManager");
        auto files = std::move(*opened.value);
        require(files->create_table_file(0).value.has_value(), "create table");
        auto* file = files->find_table_file(0);
        require(file != nullptr, "find table");
        const TableMeta meta{0, "pref1", {{"payload", Type::kVarchar}}};
        for (std::size_t page = 1; page <= pages; ++page) {
            auto allocated = file->allocate_page();
            require(allocated.value && *allocated.value == page, "allocate page");
            RawPage data;
            require(!RecordPage::initialize(data, *allocated.value), "initialize record page");
            for (std::size_t row = 0; row < rows_per_page; ++row) {
                std::string payload(384, 'a');
                for (std::size_t index = 0; index < payload.size(); ++index)
                    payload[index] = static_cast<char>('a' + (page + row + index) % 26);
                require(RecordPage::insert_record(data, *allocated.value, meta, {{payload}}).value.has_value(),
                        "insert fixture record");
            }
            require(!file->write_page(*allocated.value, data), "write fixture page");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "pages=4096 capacity=64 distance=1 queue=2 worker=1 policy=FIFO rows/page=8 payload=384 warmup=2 measurements=20\n"
                  << "fresh BufferPool per scan; OS cache uncontrolled/warm; elapsed includes worker join; counters are means\n"
                  << "profile,prefetch,min_ms,p25_ms,median_ms,p75_ms,max_ms,pages/sec,physical_reads,requested,started,ready,hit,useful,wait,dropped,read_failure,unused,deduplicated\n";
        for (int rounds : {1, 8}) {
            const char* profile = rounds == 1 ? "light" : "medium";
            std::array<std::vector<Sample>, 2> results;
            std::uint64_t expected = 0;
            for (int iteration = -warmups; iteration < measurements; ++iteration) {
                // Alternate paired order to reduce drift/order bias.
                for (int order = 0; order < 2; ++order) {
                    const int mode = (iteration + warmups + order) % 2;
                    auto sample = run(*files, *file, meta, mode != 0, rounds);
                    if (iteration == -warmups && order == 0) expected = sample.checksum;
                    require(sample.checksum == expected, "ON/OFF checksum mismatch");
                    if (iteration < 0) continue;
                    results[static_cast<std::size_t>(mode)].push_back(sample);
                    const auto& s = sample.stats;
                    raw << profile << ',' << (mode ? "ON" : "OFF") << ',' << iteration << ','
                        << sample.milliseconds << ',' << sample.reads << ',' << sample.checksum << ',' << sample.records
                        << ',' << s.prefetch_requested << ',' << s.prefetch_started << ',' << s.prefetch_ready
                        << ',' << s.prefetch_hit << ',' << s.useful_prefetch << ',' << s.foreground_wait_for_prefetch
                        << ',' << s.prefetch_dropped << ',' << s.prefetch_read_failure << ',' << s.unused_prefetch_evicted
                        << ',' << s.prefetch_deduplicated << '\n';
                }
            }
            summarize(profile, false, results[0]);
            summarize(profile, true, results[1]);
            std::cout.flush();
        }
        require(!files->close_all(), "close FileManager");
        raw.flush();
        require(raw.good(), "write CSV");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
