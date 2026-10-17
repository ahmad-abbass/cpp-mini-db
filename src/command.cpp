#include "minidb/command.hpp"

#include <charconv>
#include <string_view>

namespace minidb {
namespace {
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("invalid hex escape");
}
void arity(const std::vector<std::string>& words, std::size_t expected) {
    if (words.size() != expected) throw std::invalid_argument("wrong number of arguments");
}
std::size_t scan_limit(const std::string& word) {
    std::size_t value = 0;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), value);
    if (result.ec != std::errc{} || result.ptr != word.data() + word.size() || value > 1000) {
        throw std::invalid_argument("SCAN limit must be an integer in 0..1000");
    }
    return value;
}
} // namespace

std::vector<std::string> tokenize(const std::string& line) {
    if (line.size() > max_command_bytes) throw std::invalid_argument("command exceeds 9 MiB limit");
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && whitespace(line[i])) ++i;
        if (i == line.size()) break;
        const bool quoted = line[i] == '"';
        if (quoted) ++i;
        bool closed = !quoted;
        std::string word;
        while (i < line.size()) {
            const char c = line[i++];
            if (quoted && c == '"') { closed = true; break; }
            if (!quoted && whitespace(c)) break;
            if (!quoted && c == '"') throw std::invalid_argument("quotes must start at a token boundary");
            if (c == '\n' || c == '\0') throw std::invalid_argument("use escapes for newline and null bytes");
            if (c != '\\') { word.push_back(c); continue; }
            if (i == line.size()) throw std::invalid_argument("incomplete escape");
            const char escape = line[i++];
            switch (escape) {
            case 'n': word.push_back('\n'); break;
            case 'r': word.push_back('\r'); break;
            case 't': word.push_back('\t'); break;
            case '0': word.push_back('\0'); break;
            case '\\': word.push_back('\\'); break;
            case '"': word.push_back('"'); break;
            case 'x': {
                if (line.size() - i < 2) throw std::invalid_argument("incomplete hex escape");
                const int high = hex(line[i]);
                const int low = hex(line[i + 1]);
                word.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                break;
            }
            default: throw std::invalid_argument("unknown escape");
            }
            if (word.size() > Database::max_field_bytes) throw std::invalid_argument("token exceeds 1 MiB");
        }
        if (!closed) throw std::invalid_argument("unterminated quoted string");
        if (quoted && i < line.size() && !whitespace(line[i])) throw std::invalid_argument("expected space after quote");
        if (word.size() > Database::max_field_bytes) throw std::invalid_argument("token exceeds 1 MiB");
        words.push_back(std::move(word));
        if (words.size() > 4) throw std::invalid_argument("too many command arguments");
    }
    return words;
}

std::string json_string(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(byte)); }
        else if (byte < 0x20 || byte >= 0x7f) {
            out += "\\u00";
            out.push_back(digits[byte >> 4U]);
            out.push_back(digits[byte & 15U]);
        } else out.push_back(static_cast<char>(byte));
    }
    out.push_back('"');
    return out;
}

void CommandSession::cancel() noexcept { pending_.clear(); batching_ = false; pending_bytes_ = 4; }

CommandResult CommandSession::execute(const std::string& line) {
    try {
        auto result = dispatch(tokenize(line));
        if (result.json.size() > max_response_bytes) throw std::invalid_argument("response exceeds 16 MiB; use a smaller SCAN limit");
        return result;
    } catch (const std::invalid_argument& error) {
        cancel();
        return {"{\"ok\":false,\"code\":\"invalid_command\",\"error\":" + json_string(error.what()) + "}"};
    } catch (const StorageError& error) {
        cancel();
        return {"{\"ok\":false,\"code\":\"storage_error\",\"error\":" + json_string(error.what()) + "}"};
    }
}

CommandResult CommandSession::mutate(Mutation mutation) {
    if (!batching_) { database_.write({std::move(mutation)}); return {"{\"ok\":true}"}; }
    const std::size_t size = mutation.kind == Mutation::Kind::Clear ? 1 :
        mutation.kind == Mutation::Kind::Erase ? 5 + mutation.key.size() : 9 + mutation.key.size() + mutation.value.size();
    if (pending_.size() == Database::max_batch_operations || pending_bytes_ + size > Database::max_frame_bytes) {
        throw std::invalid_argument("batch exceeds operation or byte limit; batch cancelled");
    }
    pending_bytes_ += size;
    pending_.push_back(std::move(mutation));
    return {"{\"ok\":true,\"queued\":true}"};
}

CommandResult CommandSession::dispatch(const std::vector<std::string>& words) {
    if (words.empty()) throw std::invalid_argument("empty command");
    auto command = words.front();
    for (char& c : command) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (command == "QUIT") { arity(words, 1); cancel(); return {"{\"ok\":true}", true}; }
    if (command == "BEGIN") {
        arity(words, 1);
        if (batching_) throw std::invalid_argument("batch already active; batch cancelled");
        batching_ = true;
        return {"{\"ok\":true}"};
    }
    if (command == "COMMIT" || command == "ROLLBACK") {
        arity(words, 1);
        if (!batching_) throw std::invalid_argument("no active batch");
        if (command == "COMMIT") database_.write(pending_);
        cancel();
        return {"{\"ok\":true}"};
    }
    if (command == "SET") { arity(words, 3); return mutate(Mutation::set(words[1], words[2])); }
    if (command == "DEL") { arity(words, 2); return mutate(Mutation::erase(words[1])); }
    if (command == "CLEAR") { arity(words, 1); return mutate(Mutation::clear()); }
    if (batching_) throw std::invalid_argument("only writes, COMMIT, ROLLBACK or QUIT are allowed inside a batch; batch cancelled");
    if (command == "PING") { arity(words, 1); return {"{\"ok\":true,\"value\":\"PONG\"}"}; }
    if (command == "GET") {
        arity(words, 2);
        const auto value = database_.get(words[1]);
        return {"{\"ok\":true,\"value\":" + (value ? json_string(*value) : "null") + "}"};
    }
    if (command == "EXISTS") { arity(words, 2); return {std::string("{\"ok\":true,\"value\":") + (database_.contains(words[1]) ? "true}" : "false}")}; }
    if (command == "SIZE") { arity(words, 1); return {"{\"ok\":true,\"value\":" + std::to_string(database_.size()) + "}"}; }
    if (command == "SCAN") {
        if (words.size() != 2 && words.size() != 3) throw std::invalid_argument("SCAN needs prefix and optional limit");
        const auto rows = database_.scan(words[1], words.size() == 3 ? scan_limit(words[2]) : 100);
        std::string out = "{\"ok\":true,\"items\":[";
        bool first = true;
        for (const auto& [key, value] : rows) {
            if (!first) out += ',';
            first = false;
            out += '[' + json_string(key) + ',' + json_string(value) + ']';
            if (out.size() > max_response_bytes) throw std::invalid_argument("response exceeds 16 MiB; use a smaller SCAN limit");
        }
        return {out + "]}"};
    }
    if (command == "STATS") {
        arity(words, 1);
        const auto stats = database_.stats();
        return {"{\"ok\":true,\"keys\":" + std::to_string(stats.keys) + ",\"log_bytes\":" + std::to_string(stats.log_bytes) +
                ",\"recovered_tail_bytes\":" + std::to_string(stats.recovered_tail_bytes) +
                ",\"format_version\":" + std::to_string(stats.format_version) +
                ",\"sync_on_write\":" + (stats.sync_on_write ? "true}" : "false}")};
    }
    if (command == "SYNC") { arity(words, 1); database_.sync(); return {"{\"ok\":true}"}; }
    if (command == "COMPACT") { arity(words, 1); database_.compact(); return {"{\"ok\":true}"}; }
    throw std::invalid_argument("unknown command");
}
} // namespace minidb
