#include "data/world.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void sql(sqlite3* db, const char* query) {
    char* error = nullptr;
    const auto result = sqlite3_exec(db, query, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string reason = error ? error : "SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(reason);
    }
}
template <typename Function> void throws(Function operation, const char* message) {
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("scrapds-world-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}

int main() {
    try {
        TemporaryDirectory temp;
        const auto file = temp.path / "world.db";
        sqlite3* raw = nullptr;
        require(sqlite3_open(file.string().c_str(), &raw) == SQLITE_OK, "Could not create fixture");
        sql(raw, "CREATE TABLE Game(savegameversion INTEGER,flags INTEGER,seed INTEGER,gametick INTEGER,mods BLOB,uniqueIds BLOB); INSERT INTO Game VALUES(30,1,123,99,x'00000000',x'010203'); CREATE TABLE \"strange\"\"name\" (id INTEGER PRIMARY KEY,\"blob\"\"column\" BLOB,value TEXT); INSERT INTO \"strange\"\"name\" VALUES(1,zeroblob(5000000),'line1'||char(10)||'line2');");
        sqlite3_close(raw);
        const auto original_size = std::filesystem::file_size(file);
        const auto original_time = std::filesystem::last_write_time(file);
        const auto report = scrapds::inspect_world_json(file, 1);
        require(report.find("\"read_only\":true") != std::string::npos, "Read-only missing");
        require(report.find("\"total_bytes\":5000000") != std::string::npos, "Incorrect blob total");
        require(report.find("\"truncated\":true") != std::string::npos, "Large blob not marked truncated");
        require(report.find("strange\\\"name") != std::string::npos, "Identifier escaping failed");
        require(report.find("line1\\nline2") != std::string::npos, "JSON text escaping failed");
        require(report.size() < 10000, "Blob sample was not bounded");
        require(std::filesystem::file_size(file) == original_size && std::filesystem::last_write_time(file) == original_time, "Save modified");
        require(scrapds::inspect_world_json(file, 0).find("\"samples\":[]") != std::string::npos, "Zero samples ignored");
        throws([&] { scrapds::inspect_world_json(file, -1); }, "Negative sample limit accepted");
        throws([&] { scrapds::inspect_world_json(file, 101); }, "Excessive sample limit accepted");
        const auto missing = temp.path / "missing.db";
        throws([&] { scrapds::inspect_world_json(missing); }, "Missing file accepted");
        require(!std::filesystem::exists(missing), "Missing file created");
        const auto broken = temp.path / "broken.db";
        { std::ofstream output(broken); output << "not sqlite"; }
        throws([&] { scrapds::inspect_world_json(broken); }, "Invalid SQLite accepted");
        require(sqlite3_open(file.string().c_str(), &raw) == SQLITE_OK, "Fixture reopen failed");
        sql(raw, "DELETE FROM Game;"); sqlite3_close(raw);
        throws([&] { scrapds::inspect_world_json(file); }, "Missing metadata row accepted");
        require(sqlite3_open(file.string().c_str(), &raw) == SQLITE_OK, "Fixture reopen failed");
        sql(raw, "INSERT INTO Game VALUES('bad',1,2,3,NULL,NULL);"); sqlite3_close(raw);
        throws([&] { scrapds::inspect_world_json(file); }, "Invalid metadata type accepted");
        require(sqlite3_open(file.string().c_str(), &raw) == SQLITE_OK, "Fixture reopen failed");
        sql(raw, "DROP TABLE Game; CREATE TABLE Game(savegameversion INTEGER);"); sqlite3_close(raw);
        throws([&] { scrapds::inspect_world_json(file); }, "Incomplete save schema accepted");
        std::cout << "world_tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "world_tests: " << error.what() << '\n';
        return 1;
    }
}
