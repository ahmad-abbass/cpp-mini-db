#pragma once

#include "minidb/database.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace minidb::wal {
inline constexpr char file_header[] = "MDBWAL2\n";
inline constexpr char legacy_file_header[] = "MDBWAL1\n";
inline constexpr std::size_t header_size = 8;

std::uint32_t crc32(const std::string& bytes) noexcept;
std::uint32_t read_u32(const char* data) noexcept;
void append_u32(std::string& out, std::uint32_t value);
std::string encode(const std::vector<Mutation>& batch);
std::vector<Mutation> decode(const std::string& payload);
std::string frame(const std::vector<Mutation>& batch, bool legacy = false);
} // namespace minidb::wal
