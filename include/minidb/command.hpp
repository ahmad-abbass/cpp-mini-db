#pragma once

#include "minidb/database.hpp"

#include <string>
#include <vector>

namespace minidb {

inline constexpr std::size_t max_command_bytes = 9 * 1024 * 1024;
inline constexpr std::size_t max_response_bytes = 16 * 1024 * 1024;

struct CommandResult {
    std::string json;
    bool quit = false;
};

std::vector<std::string> tokenize(const std::string& line);
// Byte strings use JSON \u00XX escapes for non-ASCII bytes, losslessly.
std::string json_string(const std::string& value);

// One session per client. BEGIN queues writes privately; COMMIT is one atomic
// batch. Reads during a batch are rejected. This is not a SQL transaction.
class CommandSession {
public:
    explicit CommandSession(Database& database) : database_(database) {}
    CommandResult execute(const std::string& line);
private:
    Database& database_;
    bool batching_ = false;
    std::size_t pending_bytes_ = 4;
    std::vector<Mutation> pending_;
    CommandResult dispatch(const std::vector<std::string>& words);
    void cancel() noexcept;
    CommandResult mutate(Mutation mutation);
};
} // namespace minidb
