#include "minidb/database.hpp"
#include "wal.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {
namespace {

[[noreturn]] void io_error(const char* operation) {
    throw StorageError(std::string(operation) + ": " + std::strerror(errno));
}

class File {
public:
    explicit File(int value = -1) noexcept : value_(value) {}
    ~File() { if (value_ >= 0) ::close(value_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const noexcept { return value_; }
    void reset(int value) noexcept {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }
    int release() noexcept { const auto value = value_; value_ = -1; return value; }
private:
    int value_;
};

void write_all(int fd, const std::string& bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + at, bytes.size() - at);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) io_error("write");
        if (written == 0) throw StorageError("write made no progress");
        at += static_cast<std::size_t>(written);
    }
}

// EOF is a short read; all other errors are fatal. The caller validates bounds.
std::string read_at(int fd, std::size_t count, off_t offset) {
    std::string bytes(count, '\0');
    std::size_t at = 0;
    while (at < count) {
        const auto read = ::pread(fd, bytes.data() + at, count - at, offset + static_cast<off_t>(at));
        if (read < 0 && errno == EINTR) continue;
        if (read < 0) io_error("pread");
        if (read == 0) break;
        at += static_cast<std::size_t>(read);
    }
    bytes.resize(at);
    return bytes;
}

void sync_file(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) io_error("fsync");
}

using Index = std::map<std::string, std::string>;

struct PreparedBatch {
    bool clear = false;
    Index puts;
    std::vector<std::string> erases;
};

PreparedBatch prepare(const std::vector<Mutation>& batch) {
    std::map<std::string, std::optional<std::string>> changes;
    PreparedBatch result;
    for (const auto& op : batch) {
        switch (op.kind) {
        case Mutation::Kind::Clear:
            result.clear = true;
            changes.clear();
            break;
        case Mutation::Kind::Set:
            changes.insert_or_assign(op.key, op.value);
            break;
        case Mutation::Kind::Erase:
            changes.insert_or_assign(op.key, std::nullopt);
            break;
        }
    }
    for (auto& [key, value] : changes) {
        if (value) result.puts.emplace(key, std::move(*value));
        else result.erases.push_back(key);
    }
    return result;
}

// All allocations occur in prepare(). Node transfer and string swap publish the
// result after WAL sync without allocating or copying the whole index.
void apply(Index& entries, PreparedBatch& batch) {
    if (batch.clear) entries.clear();
    for (const auto& key : batch.erases) entries.erase(key);
    auto it = batch.puts.begin();
    while (it != batch.puts.end()) {
        auto current = entries.find(it->first);
        if (current != entries.end()) {
            current->second.swap(it->second);
            ++it;
        } else {
            const auto node = it++;
            entries.insert(batch.puts.extract(node));
        }
    }
}

} // namespace

Mutation Mutation::set(std::string key, std::string value) {
    return {Kind::Set, std::move(key), std::move(value)};
}
Mutation Mutation::erase(std::string key) { return {Kind::Erase, std::move(key), {}}; }
Mutation Mutation::clear() { return {Kind::Clear, {}, {}}; }

struct Database::Impl {
    std::filesystem::path directory;
    DatabaseOptions options;
    File directory_fd;
    File lock_fd;
    File log_fd;
    mutable std::shared_mutex mutex;
    Index entries;
    std::uint64_t log_bytes = 0;
    std::uint64_t recovered_tail_bytes = 0;
    bool poisoned = false;
    bool legacy_format = false;

    explicit Impl(std::filesystem::path path, DatabaseOptions opts)
        : directory(std::move(path)), options(opts) {
        std::filesystem::create_directories(directory);
        directory_fd.reset(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (directory_fd.get() < 0) io_error("open database directory");
        lock_fd.reset(::open((directory / "LOCK").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (lock_fd.get() < 0) io_error("open lock");
        if (::flock(lock_fd.get(), LOCK_EX | LOCK_NB) < 0) io_error("database is already locked");
        log_fd.reset(::open((directory / "data.log").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (log_fd.get() < 0) io_error("open log");
        struct stat status {};
        if (::fstat(log_fd.get(), &status) < 0) io_error("stat log");
        if (status.st_size == 0) {
            write_all(log_fd.get(), std::string(wal::file_header, wal::header_size));
            sync_file(log_fd.get());
            sync_file(directory_fd.get());
        }
        recover();
    }

    void healthy() const {
        if (poisoned) throw StorageError("database encountered an I/O error; close and reopen it before use");
    }

    void recover() {
        const auto header = read_at(log_fd.get(), wal::header_size, 0);
        legacy_format = header == std::string(wal::legacy_file_header, wal::header_size);
        if (!legacy_format && header != std::string(wal::file_header, wal::header_size)) {
            throw CorruptionError("invalid or incomplete database header");
        }
        off_t at = static_cast<off_t>(wal::header_size);
        bool incomplete_tail = false;
        const std::size_t prefix_size = legacy_format ? 8 : 12;
        for (;;) {
            const auto prefix = read_at(log_fd.get(), prefix_size, at);
            if (prefix.empty()) break;
            if (prefix.size() != prefix_size) { incomplete_tail = true; break; }
            if (!legacy_format && wal::crc32(prefix.substr(0, 8)) != wal::read_u32(prefix.data() + 8)) {
                throw CorruptionError("WAL header checksum mismatch at byte " + std::to_string(at));
            }
            const auto length = wal::read_u32(prefix.data());
            if (length < 5 || length > Database::max_frame_bytes) {
                throw CorruptionError("invalid WAL frame length at byte " + std::to_string(at));
            }
            const auto payload = read_at(log_fd.get(), length, at + static_cast<off_t>(prefix_size));
            if (payload.size() != length) { incomplete_tail = true; break; }
            if (wal::crc32(payload) != wal::read_u32(prefix.data() + 4)) {
                throw CorruptionError("WAL checksum mismatch at byte " + std::to_string(at));
            }
            auto batch = prepare(wal::decode(payload));
            apply(entries, batch);
            at += static_cast<off_t>(prefix_size) + static_cast<off_t>(length);
        }
        if (incomplete_tail) {
            struct stat status {};
            if (::fstat(log_fd.get(), &status) < 0) io_error("stat damaged tail");
            recovered_tail_bytes = static_cast<std::uint64_t>(status.st_size - at);
            if (::ftruncate(log_fd.get(), at) < 0) io_error("truncate incomplete tail");
            sync_file(log_fd.get());
        }
        if (::lseek(log_fd.get(), at, SEEK_SET) < 0) io_error("seek log");
        log_bytes = static_cast<std::uint64_t>(at);
    }

    void commit(const std::vector<Mutation>& batch) {
        healthy();
        if (batch.empty()) return;
        const auto bytes = wal::frame(batch, legacy_format);
        auto prepared = prepare(batch);
        try {
            write_all(log_fd.get(), bytes);
            if (options.sync_on_write) sync_file(log_fd.get());
        } catch (...) {
            poisoned = true;
            // A complete frame may already be durable. Do not claim rollback.
            throw;
        }
        apply(entries, prepared);
        log_bytes += bytes.size();
    }
};

Database::Database(const std::filesystem::path& directory, DatabaseOptions options)
    : impl_(std::make_unique<Impl>(directory, options)) {}
Database::~Database() = default;

void Database::write(const std::vector<Mutation>& batch) {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->commit(batch);
}
void Database::set(std::string key, std::string value) {
    write({Mutation::set(std::move(key), std::move(value))});
}
void Database::erase(std::string key) { write({Mutation::erase(std::move(key))}); }
void Database::clear() { write({Mutation::clear()}); }

std::optional<std::string> Database::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    const auto found = impl_->entries.find(key);
    if (found == impl_->entries.end()) return std::nullopt;
    return found->second;
}
bool Database::contains(const std::string& key) const {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    return impl_->entries.find(key) != impl_->entries.end();
}
std::size_t Database::size() const {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    return impl_->entries.size();
}
std::vector<std::pair<std::string, std::string>> Database::scan(
    const std::string& prefix, std::size_t limit) const {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    std::vector<std::pair<std::string, std::string>> result;
    std::size_t copied_bytes = 0;
    auto it = impl_->entries.lower_bound(prefix);
    while (it != impl_->entries.end() && result.size() < limit &&
           it->first.compare(0, prefix.size(), prefix) == 0) {
        const auto row_bytes = it->first.size() + it->second.size();
        if (row_bytes > max_frame_bytes - copied_bytes) {
            throw std::invalid_argument("scan exceeds 16 MiB raw result limit; reduce the row limit");
        }
        copied_bytes += row_bytes;
        result.push_back(*it++);
    }
    return result;
}
DatabaseStats Database::stats() const {
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    return {impl_->entries.size(), impl_->log_bytes, impl_->recovered_tail_bytes,
            impl_->options.sync_on_write, impl_->legacy_format ? 1U : 2U};
}
void Database::sync() {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    try { sync_file(impl_->log_fd.get()); }
    catch (...) { impl_->poisoned = true; throw; }
}

void Database::compact() {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    std::string temporary = (impl_->directory / "compact-XXXXXX").string();
    const auto destination = impl_->directory / "data.log";
    File compacted(::mkstemp(temporary.data()));
    if (compacted.get() < 0) io_error("create compaction file");
    bool renamed = false;
    try {
        if (::fcntl(compacted.get(), F_SETFD, FD_CLOEXEC) < 0) io_error("set close-on-exec");
        write_all(compacted.get(), std::string(wal::file_header, wal::header_size));
        std::uint64_t size = wal::header_size;
        std::vector<Mutation> batch;
        std::size_t payload_size = 4;
        auto flush = [&]() {
            if (batch.empty()) return;
            const auto bytes = wal::frame(batch);
            write_all(compacted.get(), bytes);
            size += bytes.size();
            batch.clear();
            payload_size = 4;
        };
        for (const auto& [key, value] : impl_->entries) {
            const auto encoded_size = 9 + key.size() + value.size();
            if (payload_size + encoded_size > max_frame_bytes || batch.size() == 1000) flush();
            batch.push_back(Mutation::set(key, value));
            payload_size += encoded_size;
        }
        flush();
        sync_file(compacted.get());
        if (::rename(temporary.c_str(), destination.c_str()) < 0) io_error("rename compacted log");
        renamed = true;
        impl_->log_fd.reset(compacted.release());
        impl_->log_bytes = size;
        impl_->legacy_format = false;
        sync_file(impl_->directory_fd.get());
    } catch (...) {
        if (renamed) impl_->poisoned = true;
        else ::unlink(temporary.c_str());
        throw;
    }
}
} // namespace minidb
