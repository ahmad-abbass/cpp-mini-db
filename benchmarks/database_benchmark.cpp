#include "minidb/database.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minidb-bench-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create benchmark directory");
        path = pattern;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::size_t number(const std::string& word, std::size_t maximum) {
    std::size_t value = 0;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), value);
    if (result.ec != std::errc{} || result.ptr != word.data() + word.size() || value == 0 || value > maximum) {
        throw std::invalid_argument("invalid numeric benchmark option");
    }
    return value;
}

void measure(const char* name, std::size_t operations,
             const std::function<void(std::size_t)>& action, bool& first) {
    std::vector<double> times;
    times.reserve(operations);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < operations; ++i) {
        const auto before = Clock::now();
        action(i);
        times.push_back(microseconds(Clock::now() - before));
    }
    const auto elapsed = microseconds(Clock::now() - start);
    std::sort(times.begin(), times.end());
    auto percentile = [&](double fraction) {
        return times[static_cast<std::size_t>(fraction * static_cast<double>(times.size() - 1))];
    };
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"workload\":\"" << name << "\",\"operations\":" << operations
              << ",\"total_ms\":" << elapsed / 1000.0
              << ",\"ops_per_second\":" << static_cast<double>(operations) * 1000000.0 / elapsed
              << ",\"p50_us\":" << percentile(0.50) << ",\"p95_us\":" << percentile(0.95)
              << ",\"p99_us\":" << percentile(0.99) << '}';
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t operations = 2000;
        std::size_t value_bytes = 128;
        bool durable = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--durable") { durable = true; continue; }
            if (i + 1 == argc) throw std::invalid_argument("missing benchmark option value");
            const std::string value = argv[++i];
            if (option == "--operations") operations = number(value, 1000000);
            else if (option == "--value-bytes") value_bytes = number(value, minidb::Database::max_field_bytes);
            else throw std::invalid_argument("unknown benchmark option");
        }
        TemporaryDirectory directory;
        auto database = std::make_unique<minidb::Database>(directory.path, minidb::DatabaseOptions{durable});
        const std::string value(value_bytes, 'v');
        std::vector<std::string> keys;
        std::vector<std::string> expected(operations, value);
        for (std::size_t i = 0; i < operations; ++i) keys.push_back("key:" + std::to_string(i));
        std::vector<std::size_t> order(operations);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 random(240218);
        std::shuffle(order.begin(), order.end(), random);
        std::cout << "{\"compiler\":\"" << __VERSION__ << "\",\"build\":\""
#ifdef NDEBUG
                  << "Release"
#else
                  << "Debug"
#endif
                  << "\",\"sync_on_write\":" << (durable ? "true" : "false")
                  << ",\"value_bytes\":" << value_bytes << ",\"seed\":240218,\"measurements\":[";
        bool first = true;
        measure("insert", operations, [&](std::size_t i) { database->set(keys[i], value); }, first);
        measure("random_get", operations, [&](std::size_t i) {
            if (database->get(keys[order[i]]) != value) throw std::runtime_error("read verification failed");
        }, first);
        const auto hot_keys = std::min<std::size_t>(operations, 100);
        measure("hot_update", operations, [&](std::size_t i) {
            expected[i % hot_keys] = std::to_string(i);
            database->set(keys[i % hot_keys], expected[i % hot_keys]);
        }, first);
        measure("prefix_scan_100", 100, [&](std::size_t) {
            if (database->scan("key:", 100).size() != std::min<std::size_t>(operations, 100)) {
                throw std::runtime_error("scan verification failed");
            }
        }, first);
        database->sync();
        const auto before = database->stats().log_bytes;
        measure("compact", 1, [&](std::size_t) { database->compact(); }, first);
        const auto after = database->stats().log_bytes;
        database.reset();
        measure("reopen", 1, [&](std::size_t) {
            database = std::make_unique<minidb::Database>(directory.path);
        }, first);
        if (database->size() != operations) throw std::runtime_error("recovered size mismatch");
        for (std::size_t i = 0; i < operations; ++i) {
            if (database->get(keys[i]) != expected[i]) throw std::runtime_error("recovered value mismatch");
        }
        std::cout << "],\"log_bytes_before_compaction\":" << before
                  << ",\"log_bytes_after_compaction\":" << after << ",\"verified\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_bench: " << error.what() << '\n';
        return 1;
    }
}
