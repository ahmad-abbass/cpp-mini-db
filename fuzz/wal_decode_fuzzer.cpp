#include "wal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > minidb::Database::max_frame_bytes) return 0;
    const std::string input(reinterpret_cast<const char*>(data), size);
    try {
        const auto decoded = minidb::wal::decode(input);
        if (minidb::wal::encode(decoded) != input) std::abort();
    } catch (const minidb::CorruptionError&) {
        // Malformed input is expected; memory errors and other failures are not.
    }
    return 0;
}
