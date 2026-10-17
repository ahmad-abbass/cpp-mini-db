#include "minidb/database.hpp"
#include "wal.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <thread>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using minidb::Database;
using minidb::Mutation;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Error, class Fn> void rejects(Fn action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    require(rejected, "expected exception was not thrown");
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minidb-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read test fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write test fixture");
}

void persistence_and_binary_values() {
    TemporaryDirectory temp;
    const std::string binary("\0\xff\n", 3);
    {
        Database db(temp.path);
        db.set("", "");
        db.set(binary, binary);
        db.set("alpha:2", "two");
        db.set("alpha:1", "one");
        db.erase("missing");
        db.sync();
        require(db.scan("alpha:", 1) == std::vector<std::pair<std::string,std::string>>{{"alpha:1", "one"}}, "bounded ordered scan");
        require(db.scan("", 0).empty(), "zero limit");
    }
    Database reopened(temp.path);
    require(reopened.get(binary) == binary, "binary strings persist");
    require(reopened.get("") == "", "empty value persists");
    require(reopened.size() == 4, "all values recover");
    require(!reopened.contains("missing"), "missing read does not insert");
    reopened.erase(binary);
    require(!reopened.contains(binary), "erase");
}

void ordered_batch_and_clear() {
    TemporaryDirectory temp;
    {
        Database db(temp.path);
        db.set("old", "value");
        db.write({Mutation::set("a", "first"), Mutation::clear(), Mutation::set("a", "second"),
                  Mutation::erase("a"), Mutation::set("b", "last"), Mutation::set("b", "final")});
        require(db.scan() == std::vector<std::pair<std::string,std::string>>{{"b", "final"}}, "ordered batch semantics");
        const auto bytes = db.stats().log_bytes;
        db.write({});
        require(bytes == db.stats().log_bytes, "empty batch is a no-op");
    }
    { Database db(temp.path); require(db.size() == 1 && db.get("b") == "final", "batch replay"); db.clear(); }
    Database db(temp.path);
    require(db.size() == 0, "clear survives reopen");
}

void invalid_batch_leaves_no_changes() {
    TemporaryDirectory temp;
    Database db(temp.path);
    db.set("old", "value");
    const auto before = read_file(temp.path / "data.log");
    rejects<std::invalid_argument>([&] {
        db.write({Mutation::set("new", "value"), Mutation::set("huge", std::string(Database::max_field_bytes + 1, 'x'))});
    });
    rejects<std::invalid_argument>([&] { db.write({{static_cast<Mutation::Kind>(99), {}, {}}}); });
    rejects<std::invalid_argument>([&] { db.write({{Mutation::Kind::Clear, "bad", {}}}); });
    require(db.size() == 1 && !db.contains("new"), "validation is atomic");
    require(read_file(temp.path / "data.log") == before, "invalid batch never reaches log");
}

void exclusive_directory_lock() {
    TemporaryDirectory temp;
    {
        Database owner(temp.path);
        rejects<minidb::StorageError>([&] { Database second(temp.path); });
        owner.compact();
        rejects<minidb::StorageError>([&] { Database second(temp.path); });
    }
    Database next_owner(temp.path);
    next_owner.set("lock", "released");
}

void every_truncated_batch_is_atomic() {
    TemporaryDirectory source;
    std::size_t committed_size;
    {
        Database db(source.path);
        db.set("stable", "yes");
        committed_size = static_cast<std::size_t>(db.stats().log_bytes);
        db.write({Mutation::set("x", "one"), Mutation::set("y", "two"), Mutation::erase("stable")});
    }
    const auto full = read_file(source.path / "data.log");
    for (std::size_t cut = committed_size; cut < full.size(); ++cut) {
        TemporaryDirectory copy;
        write_file(copy.path / "data.log", full.substr(0, cut));
        Database db(copy.path);
        require(db.get("stable") == "yes" && !db.contains("x") && !db.contains("y"), "partial batch must never appear");
        require(db.stats().recovered_tail_bytes == cut - committed_size, "exact tail recovery count");
        require(db.stats().log_bytes == committed_size, "tail is physically truncated");
        db.set("after", "recovery");
    }
}

void detects_corruption_without_rewriting_it() {
    TemporaryDirectory temp;
    { Database db(temp.path); db.set("key", "value"); }
    auto corrupt = read_file(temp.path / "data.log");
    corrupt.back() ^= 1;
    write_file(temp.path / "data.log", corrupt);
    rejects<minidb::CorruptionError>([&] { Database db(temp.path); });
    require(read_file(temp.path / "data.log") == corrupt, "checksum failure must not truncate data");
    corrupt = "not a database";
    write_file(temp.path / "data.log", corrupt);
    rejects<minidb::CorruptionError>([&] { Database db(temp.path); });
}

void format_validation_and_crc_golden_value() {
    require(minidb::wal::crc32("123456789") == 0xcbf43926U, "standard CRC-32 golden vector");
    TemporaryDirectory temp;
    auto invalid_payload = std::string("\x01\0\0\0\x7f", 5);
    std::string bytes(minidb::wal::file_header, minidb::wal::header_size);
    minidb::wal::append_u32(bytes, 5);
    minidb::wal::append_u32(bytes, minidb::wal::crc32(invalid_payload));
    minidb::wal::append_u32(bytes, minidb::wal::crc32(bytes.substr(8, 8)));
    bytes += invalid_payload;
    write_file(temp.path / "data.log", bytes);
    rejects<minidb::CorruptionError>([&] { Database db(temp.path); });
    for (std::size_t length = 1; length < 8; ++length) {
        write_file(temp.path / "data.log", std::string(minidb::wal::file_header, length));
        rejects<minidb::CorruptionError>([&] { Database db(temp.path); });
    }
}

void compaction_reclaims_history() {
    TemporaryDirectory temp;
    {
        Database db(temp.path, {false});
        for (int i = 0; i < 80; ++i) db.set("key", std::to_string(i));
        db.set("delete", "me");
        db.erase("delete");
        const auto old_size = db.stats().log_bytes;
        db.compact();
        require(db.stats().log_bytes < old_size, "compaction reclaims obsolete frames");
        db.set("after", "compact");
    }
    { Database db(temp.path); require(db.get("key") == "79" && db.get("after") == "compact", "compacted data and subsequent append recover"); db.clear(); db.compact(); }
    Database empty(temp.path);
    require(empty.size() == 0 && empty.stats().log_bytes == 8, "empty compacted database reopens");
}

void randomized_reference_model() {
    TemporaryDirectory temp;
    std::map<std::string, std::string> reference;
    auto db = std::make_unique<Database>(temp.path, minidb::DatabaseOptions{false});
    std::mt19937 random(240218);
    for (int i = 0; i < 2000; ++i) {
        std::vector<Mutation> batch;
        for (int j = 0; j < 1 + i % 5; ++j) {
            const auto key = std::to_string(random() % 73);
            const auto value = std::to_string(random());
            if (random() % 3 == 0) { batch.push_back(Mutation::erase(key)); reference.erase(key); }
            else { batch.push_back(Mutation::set(key, value)); reference[key] = value; }
        }
        db->write(batch);
        if (i % 79 == 0) db->compact();
        if (i % 97 == 0) { db.reset(); db = std::make_unique<Database>(temp.path, minidb::DatabaseOptions{false}); }
        const std::vector<std::pair<std::string, std::string>> expected(reference.begin(), reference.end());
        require(db->scan("", 1000) == expected, "randomized history agrees with reference model");
    }
}

void concurrent_batches_have_consistent_snapshots() {
    TemporaryDirectory temp;
    Database db(temp.path, {false});
    db.write({Mutation::set("pair:a", "0"), Mutation::set("pair:b", "0")});
    std::atomic<bool> done{false};
    std::atomic<bool> valid{true};
    std::thread reader([&] {
        while (!done.load()) {
            const auto snapshot = db.scan("pair:");
            if (snapshot.size() != 2 || snapshot[0].second != snapshot[1].second) valid = false;
        }
    });
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 4; ++worker) {
        writers.emplace_back([&, worker] {
            for (int i = 0; i < 100; ++i) {
                const auto value = std::to_string(worker * 100 + i);
                db.write({Mutation::set("pair:a", value), Mutation::set("pair:b", value)});
            }
        });
    }
    for (auto& writer : writers) writer.join();
    done = true;
    reader.join();
    require(valid, "scan cannot observe half an atomic batch");
    db.sync();
}

void large_snapshot_compaction_and_scan_limit() {
    TemporaryDirectory temp;
    const std::string large(Database::max_field_bytes, 'v');
    {
        Database db(temp.path, {false});
        for (int i = 0; i < 17; ++i) db.set(std::to_string(i), large);
        rejects<std::invalid_argument>([&] { (void) db.scan("", 1000); });
        require(db.scan("", 1).size() == 1, "small scan remains usable");
        db.compact(); // Live state exceeds the maximum single WAL frame.
    }
    Database db(temp.path);
    require(db.size() == 17 && db.get("16") == large, "multi-frame compacted snapshot recovers");
}

void child_succeeds(const std::function<void()>& action) {
    const auto child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        try { action(); ::_exit(0); }
        catch (...) { ::_exit(1); }
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fault-injection child failed");
}

void partial_write_poison_and_recovery() {
    TemporaryDirectory temp;
    std::uint64_t committed;
    { Database db(temp.path); db.set("stable", "yes"); committed = db.stats().log_bytes; }
    child_succeeds([&] {
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit {};
        require(::getrlimit(RLIMIT_FSIZE, &limit) == 0, "getrlimit");
        limit.rlim_cur = committed + 12;
        require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "setrlimit");
        Database db(temp.path, {false});
        rejects<minidb::StorageError>([&] { db.set("partial", std::string(1000, 'x')); });
        rejects<minidb::StorageError>([&] { (void) db.get("stable"); });
        rejects<minidb::StorageError>([&] { db.set("later", "must not append"); });
    });
    Database db(temp.path);
    require(db.get("stable") == "yes" && !db.contains("partial"), "partial write recovers last complete batch");
    require(db.stats().recovered_tail_bytes == 12, "real short write tail recovered");
    db.set("after", "recovery");
}

void failed_compaction_preserves_live_log() {
    TemporaryDirectory temp;
    { Database db(temp.path); db.set("stable", std::string(2048, 'v')); }
    child_succeeds([&] {
        std::signal(SIGXFSZ, SIG_IGN);
        Database db(temp.path, {false});
        const auto before = db.stats().log_bytes;
        rlimit original {};
        require(::getrlimit(RLIMIT_FSIZE, &original) == 0, "getrlimit");
        auto limited = original;
        limited.rlim_cur = 128;
        require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "setrlimit");
        rejects<minidb::StorageError>([&] { db.compact(); });
        require(db.stats().log_bytes == before && db.get("stable") == std::string(2048, 'v'), "failed compaction keeps usable index and log");
        require(::setrlimit(RLIMIT_FSIZE, &original) == 0, "restore rlimit");
        db.set("after", "failed compaction");
    });
    Database db(temp.path);
    require(db.get("after") == "failed compaction", "old log remains appendable");
    for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
        require(entry.path().filename().string().find("compact-") != 0, "failed compaction removes temporary file");
    }
}

void malformed_payload_corpus() {
    std::mt19937 random(20261017);
    for (int trial = 0; trial < 20000; ++trial) {
        std::string bytes(random() % 256, '\0');
        for (char& byte : bytes) byte = static_cast<char>(random() & 255U);
        // Also exercise small, plausible counts rather than only random u32s.
        if (bytes.size() >= 4 && trial % 2 == 0) {
            const auto count = random() % 8;
            bytes[0] = static_cast<char>(count);
            bytes[1] = bytes[2] = bytes[3] = '\0';
        }
        try {
            const auto decoded = minidb::wal::decode(bytes);
            require(minidb::wal::encode(decoded) == bytes, "successful decoding is canonical");
        } catch (const minidb::CorruptionError&) { /* Expected invalid payload. */ }
    }
}

void header_corruption_cannot_masquerade_as_a_torn_tail() {
    TemporaryDirectory temp;
    { Database db(temp.path); db.set("committed", "value"); }
    auto bytes = read_file(temp.path / "data.log");
    // Inflate an otherwise valid length past EOF; v2 verifies the header before
    // deciding whether a final frame is incomplete.
    bytes[9] ^= 1;
    write_file(temp.path / "data.log", bytes);
    rejects<minidb::CorruptionError>([&] { Database db(temp.path); });
    require(read_file(temp.path / "data.log") == bytes, "damaged header must not discard committed data");
}

void legacy_logs_upgrade_on_compaction() {
    TemporaryDirectory temp;
    const auto old = std::string(minidb::wal::legacy_file_header, 8) +
        minidb::wal::frame({Mutation::set("old", "v1")}, true);
    write_file(temp.path / "data.log", old);
    {
        Database db(temp.path);
        require(db.stats().format_version == 1 && db.get("old") == "v1", "legacy log opens");
        db.set("append", "still v1");
    }
    {
        Database db(temp.path);
        require(db.get("append") == "still v1", "legacy append replays");
        db.compact();
        require(db.stats().format_version == 2, "compaction upgrades the envelope");
        db.set("new", "v2");
    }
    Database db(temp.path);
    require(db.size() == 3 && db.get("old") == "v1" && db.get("new") == "v2", "upgraded data reopens");
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"persistence and binary values", persistence_and_binary_values},
        {"ordered batch and clear", ordered_batch_and_clear},
        {"invalid batch is atomic", invalid_batch_leaves_no_changes},
        {"exclusive directory lock", exclusive_directory_lock},
        {"every byte truncation", every_truncated_batch_is_atomic},
        {"checksum corruption", detects_corruption_without_rewriting_it},
        {"format validation and golden CRC", format_validation_and_crc_golden_value},
        {"compaction", compaction_reclaims_history},
        {"2000 reference model batches", randomized_reference_model},
        {"concurrent snapshot consistency", concurrent_batches_have_consistent_snapshots},
        {"large compaction and bounded scan", large_snapshot_compaction_and_scan_limit},
        {"real partial write and poisoned handle", partial_write_poison_and_recovery},
        {"failed compaction keeps live log", failed_compaction_preserves_live_log},
        {"20000 malformed payloads", malformed_payload_corpus},
        {"checksummed frame header", header_corruption_cannot_masquerade_as_a_torn_tail},
        {"legacy format upgrade", legacy_logs_upgrade_on_compaction}
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
    return 0;
}
