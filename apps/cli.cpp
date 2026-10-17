#include "minidb/command.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--buffered")) {
        std::cerr << "Usage: minidb_cli DIRECTORY [--buffered]\n"
                  << "Commands: SET GET DEL EXISTS SIZE SCAN CLEAR BEGIN COMMIT ROLLBACK SYNC COMPACT STATS PING QUIT\n";
        return 2;
    }
    try {
        minidb::Database database(argv[1], {argc != 3});
        minidb::CommandSession session(database);
        std::string line;
        bool failed = false;
        for (;;) {
            line.clear();
            bool oversized = false;
            char c;
            bool consumed = false;
            while (std::cin.get(c)) {
                consumed = true;
                if (c == '\n') break;
                if (line.size() < minidb::max_command_bytes) line.push_back(c);
                else oversized = true;
            }
            if (!consumed) break;
            const auto result = session.execute(oversized ? std::string(minidb::max_command_bytes + 1, 'x') : line);
            std::cout << result.json << std::endl;
            if (result.json.find("\"ok\":false") == 1) failed = true;
            if (result.quit) break;
        }
        database.sync();
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_cli: " << error.what() << '\n';
        return 1;
    }
}
