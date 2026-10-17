#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace minidb {

// An owning, in-memory string-to-string store. Empty keys and values are valid.
// No thread safety or persistence is provided in this milestone.
class KeyValueStore {
public:
    // Inserts or replaces a value; true means a new key was inserted.
    bool set(std::string key, std::string value);

    // Returns an independent copy, or nullopt for a missing key.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    [[nodiscard]] bool contains(const std::string& key) const;

    // Returns true only if an existing entry was removed.
    bool erase(const std::string& key);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::unordered_map<std::string, std::string> entries_;
};

} // namespace minidb
