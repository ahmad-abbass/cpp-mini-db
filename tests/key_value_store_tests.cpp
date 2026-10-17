#include "minidb/key_value_store.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using minidb::KeyValueStore;

// Unlike assert(), this check stays active in Release builds.
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void starts_empty() {
    const KeyValueStore db;
    require(db.empty(), "new store should be empty");
    require(db.size() == 0, "new store should have size zero");
}

void inserts_and_reads() {
    KeyValueStore db;
    require(db.set("language", "C++"), "new key should report insertion");
    require(db.get("language") == "C++", "stored value should be retrievable");
    require(db.contains("language"), "inserted key should exist");
    require(!db.empty() && db.size() == 1, "size should reflect insertion");
}

void missing_reads_do_not_insert() {
    KeyValueStore db;
    require(!db.get("missing").has_value(), "missing key should return nullopt");
    require(!db.contains("missing"), "missing key should not exist");
    require(db.empty(), "lookups must not insert entries");
}

void replaces_without_growing() {
    KeyValueStore db;
    db.set("key", "first");
    require(!db.set("key", "second"), "replacement should not report insertion");
    require(db.get("key") == "second", "replacement should update value");
    require(db.size() == 1, "replacement must not change size");
}

void erases_existing_key() {
    KeyValueStore db;
    db.set("keep", "one");
    db.set("remove", "two");
    require(db.erase("remove"), "existing key should be removed");
    require(!db.get("remove"), "removed value should be absent");
    require(db.get("keep") == "one", "unrelated value should survive");
    require(db.size() == 1, "erase should decrement size");
}

void erasing_missing_key_is_harmless() {
    KeyValueStore db;
    require(!db.erase("missing"), "erasing a missing key should report false");
    db.set("key", "value");
    require(db.erase("key"), "first erase should succeed");
    require(!db.erase("key"), "repeated erase should report false");
    require(db.empty(), "store should remain empty");
}

void accepts_empty_keys_and_values() {
    KeyValueStore db;
    db.set("", "");
    const auto value = db.get("");
    require(value.has_value() && value->empty(), "empty value differs from missing");
    require(db.contains(""), "empty key should exist");
    require(db.erase(""), "empty key should be erasable");
}

void keys_are_case_sensitive() {
    KeyValueStore db;
    db.set("Key", "upper");
    db.set("key", "lower");
    require(db.size() == 2, "case-distinct keys should be independent");
    require(db.get("Key") == "upper" && db.get("key") == "lower", "case matters");
}

void handles_binary_and_special_characters() {
    KeyValueStore db;
    const std::string key("a\0b", 3);
    const std::string value("line 1\nline 2\0end", 17);
    db.set(key, value);
    db.set("a", "different");
    db.set("key with spaces\tand tabs", "value with \"quotes\"");
    require(db.get(key) == value, "embedded nulls and newlines should round trip");
    require(db.get("a") == "different", "binary keys must not be truncated");
    require(db.get("key with spaces\tand tabs") == "value with \"quotes\"", "spaces survive");
}

void owns_input_strings() {
    KeyValueStore db;
    std::string key = "key";
    std::string value = "value";
    db.set(key, value);
    key = "changed";
    value = "changed";
    require(db.get("key") == "value", "store should own its keys and values");
    require(!db.contains("changed"), "mutating input should not affect store");
}

void returns_independent_values() {
    KeyValueStore db;
    db.set("key", "original");
    auto copy = db.get("key");
    require(copy.has_value(), "value should exist");
    *copy = "caller changed this";
    require(db.get("key") == "original", "changing result must not mutate store");
    db.set("key", "updated");
    db.clear();
    require(*copy == "caller changed this", "result should outlive updates and clear");
}

void clears_and_can_be_reused() {
    KeyValueStore db;
    db.clear();
    db.set("one", "1");
    db.set("two", "2");
    db.clear();
    require(db.empty() && db.size() == 0, "clear should remove all entries");
    require(!db.contains("one") && !db.get("two"), "old keys should be absent");
    db.set("three", "3");
    require(db.size() == 1 && db.get("three") == "3", "cleared store is reusable");
}

void copies_are_independent() {
    KeyValueStore original;
    original.set("key", "original");
    KeyValueStore copied = original;
    copied.set("key", "copy");
    copied.set("other", "value");
    require(original.get("key") == "original", "copy should not alias values");
    require(original.size() == 1 && copied.size() == 2, "copies have independent indices");
    KeyValueStore assigned;
    assigned = original;
    assigned.erase("key");
    require(original.contains("key"), "copy assignment should not alias storage");
}

void retains_entries_after_growth() {
    KeyValueStore db;
    constexpr int count = 2000;
    for (int i = 0; i < count; ++i) {
        require(db.set("key" + std::to_string(i), std::to_string(i)), "insert should succeed");
    }
    require(db.size() == static_cast<std::size_t>(count), "all distinct entries should remain");
    for (int i = 0; i < count; ++i) {
        require(db.get("key" + std::to_string(i)) == std::to_string(i), "growth must retain data");
    }
    for (int i = 0; i < count; i += 2) {
        require(db.erase("key" + std::to_string(i)), "erase should survive rehashing");
    }
    require(db.size() == static_cast<std::size_t>(count / 2), "half the entries should remain");
}

void long_strings_round_trip() {
    KeyValueStore db;
    const std::string key(8192, 'k');
    const std::string value(65536, 'v');
    db.set(key, value);
    require(db.get(key) == value, "long strings should be preserved");
    require(db.erase(key) && db.empty(), "long key should be erasable");
}

void matches_reference_model() {
    KeyValueStore db;
    std::map<std::string, std::string> reference;
    std::mt19937 random(20261017U);
    for (int step = 0; step < 5000; ++step) {
        const auto key = "key" + std::to_string(random() % 100U);
        const auto value = "value" + std::to_string(random());
        switch (random() % 5U) {
        case 0: {
            const bool inserted = reference.find(key) == reference.end();
            require(db.set(key, value) == inserted, "insertion result differs from model");
            reference[key] = value;
            break;
        }
        case 1:
            require(db.erase(key) == (reference.erase(key) != 0), "erase differs from model");
            break;
        case 2: {
            const auto expected = reference.find(key);
            const auto actual = db.get(key);
            require(actual.has_value() == (expected != reference.end()), "presence differs");
            if (actual) {
                require(*actual == expected->second, "lookup value differs from model");
            }
            break;
        }
        case 3:
            require(db.contains(key) == (reference.find(key) != reference.end()), "contains differs");
            break;
        case 4:
            // Clear occasionally, while keeping most sequences long enough to grow.
            if (step % 137 == 0) {
                db.clear();
                reference.clear();
            }
            break;
        }
        require(db.size() == reference.size(), "size differs from model");
        require(db.empty() == reference.empty(), "empty differs from model");
    }
    for (const auto& [key, value] : reference) {
        require(db.get(key) == value, "final state differs from model");
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"starts empty", starts_empty},
        {"inserts and reads", inserts_and_reads},
        {"missing reads do not insert", missing_reads_do_not_insert},
        {"replaces without growing", replaces_without_growing},
        {"erases existing key", erases_existing_key},
        {"erasing missing key is harmless", erasing_missing_key_is_harmless},
        {"accepts empty keys and values", accepts_empty_keys_and_values},
        {"keys are case-sensitive", keys_are_case_sensitive},
        {"handles binary and special characters", handles_binary_and_special_characters},
        {"owns input strings", owns_input_strings},
        {"returns independent values", returns_independent_values},
        {"clears and can be reused", clears_and_can_be_reused},
        {"copies are independent", copies_are_independent},
        {"retains entries after growth", retains_entries_after_growth},
        {"long strings round trip", long_strings_round_trip},
        {"matches reference model", matches_reference_model},
    };

    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            ++failed;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            ++failed;
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
