#include "minidb/key_value_store.hpp"

#include <iostream>
#include <string>

int main() {
    minidb::KeyValueStore db;
    db.set("language", "C++");
    std::cout << "language = " << db.get("language").value_or("NOT FOUND") << '\n';

    db.set("language", "C++17");
    std::cout << "updated language = " << db.get("language").value_or("NOT FOUND") << '\n';
    std::cout << "entries = " << db.size() << '\n';

    db.erase("language");
    std::cout << "after erase = " << db.get("language").value_or("NOT FOUND") << '\n';
    return db.empty() ? 0 : 1;
}
