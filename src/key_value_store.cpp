#include "minidb/key_value_store.hpp"

#include <utility>

namespace minidb {

bool KeyValueStore::set(std::string key, std::string value) {
    return entries_.insert_or_assign(std::move(key), std::move(value)).second;
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return std::nullopt;
    }
    return entry->second;
}

bool KeyValueStore::contains(const std::string& key) const {
    return entries_.find(key) != entries_.end();
}

bool KeyValueStore::erase(const std::string& key) {
    return entries_.erase(key) != 0;
}

void KeyValueStore::clear() noexcept {
    entries_.clear();
}

std::size_t KeyValueStore::size() const noexcept {
    return entries_.size();
}

bool KeyValueStore::empty() const noexcept {
    return entries_.empty();
}

} // namespace minidb
