#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CorruptionError : public StorageError {
public:
    using StorageError::StorageError;
};

struct Mutation {
    enum class Kind : std::uint8_t { Set = 1, Erase = 2, Clear = 3 };
    Kind kind;
    std::string key;
    std::string value;

    static Mutation set(std::string key, std::string value);
    static Mutation erase(std::string key);
    static Mutation clear();
};

struct DatabaseOptions {
    // false acknowledges writes before fsync; call sync() for an explicit barrier.
    bool sync_on_write = true;
};

struct DatabaseStats {
    std::size_t keys;
    std::uint64_t log_bytes;
    std::uint64_t recovered_tail_bytes;
    bool sync_on_write;
};

// POSIX persistent store. One owning process, concurrent threads, copied reads.
// write() is an atomic ordered batch, not an interactive transaction.
class Database {
public:
    static constexpr std::size_t max_field_bytes = 1024 * 1024;
    static constexpr std::size_t max_frame_bytes = 16 * 1024 * 1024;
    static constexpr std::size_t max_batch_operations = 100000;

    explicit Database(const std::filesystem::path& directory,
                      DatabaseOptions options = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    void set(std::string key, std::string value);
    void erase(std::string key);
    void clear();
    void write(const std::vector<Mutation>& batch);
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] std::size_t size() const;
    // Ordered prefix scan returns a consistent, bounded copy under a read lock.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> scan(
        const std::string& prefix = {}, std::size_t limit = 100) const;
    [[nodiscard]] DatabaseStats stats() const;

    void sync();
    void compact();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minidb
