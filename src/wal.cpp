#include "wal.hpp"

#include <limits>

namespace minidb::wal {

std::uint32_t crc32(const std::string& bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t read_u32(const char* data) noexcept {
    std::uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << (8U * i);
    }
    return value;
}

void append_u32(std::string& out, std::uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8U * i)) & 0xffU));
    }
}

std::string encode(const std::vector<Mutation>& batch) {
    if (batch.empty() || batch.size() > Database::max_batch_operations) {
        throw std::invalid_argument("batch must contain 1..100000 operations");
    }
    std::string out;
    append_u32(out, static_cast<std::uint32_t>(batch.size()));
    for (const auto& op : batch) {
        if (op.key.size() > Database::max_field_bytes || op.value.size() > Database::max_field_bytes) {
            throw std::invalid_argument("keys and values are limited to 1 MiB each");
        }
        out.push_back(static_cast<char>(op.kind));
        switch (op.kind) {
        case Mutation::Kind::Set:
            append_u32(out, static_cast<std::uint32_t>(op.key.size()));
            append_u32(out, static_cast<std::uint32_t>(op.value.size()));
            out += op.key;
            out += op.value;
            break;
        case Mutation::Kind::Erase:
            if (!op.value.empty()) throw std::invalid_argument("erase has no value");
            append_u32(out, static_cast<std::uint32_t>(op.key.size()));
            out += op.key;
            break;
        case Mutation::Kind::Clear:
            if (!op.key.empty() || !op.value.empty()) throw std::invalid_argument("clear has no fields");
            break;
        default:
            throw std::invalid_argument("unknown mutation kind");
        }
        if (out.size() > Database::max_frame_bytes) {
            throw std::invalid_argument("batch exceeds 16 MiB encoded limit");
        }
    }
    return out;
}

std::vector<Mutation> decode(const std::string& payload) {
    std::size_t at = 0;
    auto number = [&]() {
        if (payload.size() - at < 4) throw CorruptionError("truncated operation length");
        const auto result = read_u32(payload.data() + at);
        at += 4;
        return result;
    };
    auto field = [&](std::uint32_t length) {
        if (length > Database::max_field_bytes || length > payload.size() - at) {
            throw CorruptionError("invalid operation field length");
        }
        auto result = payload.substr(at, length);
        at += length;
        return result;
    };
    const auto count = number();
    if (count == 0 || count > Database::max_batch_operations || count > payload.size() - at) {
        throw CorruptionError("invalid operation count");
    }
    std::vector<Mutation> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at == payload.size()) throw CorruptionError("missing operation tag");
        const auto tag = static_cast<Mutation::Kind>(payload[at++]);
        if (tag == Mutation::Kind::Set) {
            const auto key_length = number();
            const auto value_length = number();
            auto key = field(key_length);
            auto value = field(value_length);
            result.push_back(Mutation::set(std::move(key), std::move(value)));
        } else if (tag == Mutation::Kind::Erase) {
            const auto key_length = number();
            result.push_back(Mutation::erase(field(key_length)));
        } else if (tag == Mutation::Kind::Clear) {
            result.push_back(Mutation::clear());
        } else {
            throw CorruptionError("unknown operation tag");
        }
    }
    if (at != payload.size()) throw CorruptionError("trailing bytes in transaction");
    return result;
}

std::string frame(const std::vector<Mutation>& batch) {
    auto payload = encode(batch);
    std::string out;
    out.reserve(payload.size() + 8);
    append_u32(out, static_cast<std::uint32_t>(payload.size()));
    append_u32(out, crc32(payload));
    out += payload;
    return out;
}
} // namespace minidb::wal
