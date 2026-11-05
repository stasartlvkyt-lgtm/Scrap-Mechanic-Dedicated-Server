#include "world.hpp"
#include "blob.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace scrapds {
namespace {
constexpr int blob_prefix_bytes = 64;
constexpr int text_prefix_chars = 4096;
using Db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string quote_identifier(std::string_view input) {
    std::string output = "\"";
    for (const char c : input) { output += c; if (c == '"') output += c; }
    return output + '"';
}

std::string json(std::string_view input) {
    std::ostringstream output;
    output << '"';
    for (unsigned char c : input) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 32) output << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
            else output << char(c);
        }
    }
    output << '"';
    return output.str();
}

[[noreturn]] void fail(sqlite3* db, const std::string& action) {
    throw std::runtime_error(action + ": " + sqlite3_errmsg(db));
}

Statement prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (result != SQLITE_OK) fail(db, "Preparing world query");
    return statement;
}

int step(sqlite3* db, sqlite3_stmt* statement) {
    const int result = sqlite3_step(statement);
    if (result != SQLITE_ROW && result != SQLITE_DONE) fail(db, "Reading world");
    return result;
}

void execute(sqlite3* db, const char* sql) {
    auto statement = prepare(db, sql);
    if (step(db, statement.get()) != SQLITE_DONE) throw std::runtime_error("Unexpected query result");
}

std::string text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) return {};
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(sqlite3_column_bytes(statement, column))};
}

std::string scalar(sqlite3_stmt* statement, int column) {
    switch (sqlite3_column_type(statement, column)) {
    case SQLITE_NULL: return "null";
    case SQLITE_INTEGER: return std::to_string(sqlite3_column_int64(statement, column));
    case SQLITE_FLOAT: {
        const auto value = sqlite3_column_double(statement, column);
        if (!std::isfinite(value)) return "null";
        std::ostringstream output;
        output << std::setprecision(17) << value;
        return output.str();
    }
    case SQLITE_TEXT: return json(text(statement, column));
    default: throw std::runtime_error("Internal error: unbounded BLOB scalar");
    }
}

struct Column { std::string name; std::string type; int pk = 0; bool not_null = false; };

std::vector<Column> columns(sqlite3* db, const std::string& table) {
    auto statement = prepare(db, "PRAGMA table_info(" + quote_identifier(table) + ")");
    std::vector<Column> result;
    while (step(db, statement.get()) == SQLITE_ROW) {
        result.push_back({text(statement.get(), 1), text(statement.get(), 2), sqlite3_column_int(statement.get(), 5), sqlite3_column_int(statement.get(), 3) != 0});
    }
    return result;
}

std::string sample_json(sqlite3* db, const std::string& table, const std::vector<Column>& cols, int limit) {
    if (!limit) return "[]";
    std::string query = "SELECT ";
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (i) query += ',';
        const auto name = quote_identifier(cols[i].name);
        query += "CASE typeof(" + name + ") WHEN 'blob' THEN NULL WHEN 'text' THEN substr(" + name + ",1," + std::to_string(text_prefix_chars) + ") ELSE " + name + " END,";
        query += "typeof(" + name + "),length(" + name + "),CASE WHEN typeof(" + name + ")='blob' THEN hex(substr(" + name + ",1," + std::to_string(blob_prefix_bytes) + ")) ELSE NULL END";
    }
    query += " FROM " + quote_identifier(table);
    std::vector<Column> keys;
    for (const auto& col : cols) if (col.pk) keys.push_back(col);
    std::sort(keys.begin(), keys.end(), [](const Column& a, const Column& b) { return a.pk < b.pk; });
    if (!keys.empty()) {
        query += " ORDER BY ";
        for (std::size_t i = 0; i < keys.size(); ++i) { if (i) query += ','; query += quote_identifier(keys[i].name); }
    }
    query += " LIMIT " + std::to_string(limit);
    auto statement = prepare(db, query);
    std::string result = "[";
    bool first_row = true;
    while (step(db, statement.get()) == SQLITE_ROW) {
        if (!first_row) result += ',';
        first_row = false;
        result += '{';
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) result += ',';
            const int base = static_cast<int>(4 * i);
            const auto type = text(statement.get(), base + 1);
            result += json(cols[i].name) + ':';
            if (type == "blob") {
                const auto length = sqlite3_column_int64(statement.get(), base + 2);
                result += "{\"type\":\"blob\",\"bytes\":" + std::to_string(length) + ",\"hex_prefix\":" + json(text(statement.get(), base + 3)) + ",\"truncated\":" + (length > blob_prefix_bytes ? "true" : "false");
                if (cols[i].name == "data") {
                    const auto decoded = inspect_blob_prefix_json(table, text(statement.get(), base + 3), static_cast<std::size_t>(length));
                    if (!decoded.empty()) result += ",\"partial_decode\":" + decoded;
                }
                result += '}';
            } else if (type == "text" && sqlite3_column_int64(statement.get(), base + 2) > text_prefix_chars) {
                result += "{\"type\":\"text\",\"characters\":" + std::to_string(sqlite3_column_int64(statement.get(), base + 2)) + ",\"prefix\":" + scalar(statement.get(), base) + ",\"truncated\":true}";
            } else result += scalar(statement.get(), base);
        }
        result += '}';
    }
    return result + ']';
}

std::string blob_stats_json(sqlite3* db, const std::string& table, const std::vector<Column>& cols) {
    std::string query = "SELECT ";
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (i) query += ',';
        const auto name = quote_identifier(cols[i].name);
        const auto bytes = "CASE WHEN typeof(" + name + ")='blob' THEN length(" + name + ") END";
        query += "count(" + bytes + "),coalesce(sum(" + bytes + "),0),min(" + bytes + "),max(" + bytes + "),avg(" + bytes + ")";
    }
    query += " FROM " + quote_identifier(table);
    auto statement = prepare(db, query);
    if (step(db, statement.get()) != SQLITE_ROW) throw std::runtime_error("Missing blob statistics");
    std::string result = "{";
    bool first = true;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const int base = static_cast<int>(5 * i);
        if (!sqlite3_column_int64(statement.get(), base)) continue;
        if (!first) result += ',';
        first = false;
        result += json(cols[i].name) + ":{\"count\":" + scalar(statement.get(), base) + ",\"total_bytes\":" + scalar(statement.get(), base + 1) + ",\"min_bytes\":" + scalar(statement.get(), base + 2) + ",\"max_bytes\":" + scalar(statement.get(), base + 3) + ",\"mean_bytes\":" + scalar(statement.get(), base + 4) + '}';
    }
    return result + '}';
}
} // namespace

std::string inspect_world_json(const std::filesystem::path& path, int sample_limit) {
    if (sample_limit < 0 || sample_limit > 100) throw std::runtime_error("sample_limit must be between 0 and 100");
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("World file does not exist or is not a regular file: " + utf8(path));
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(utf8(path).c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    Db db(raw, sqlite3_close);
    if (opened != SQLITE_OK) {
        if (db) fail(db.get(), "Opening world read-only");
        throw std::runtime_error("Could not allocate SQLite connection");
    }
    sqlite3_busy_timeout(db.get(), 3000);
    sqlite3_enable_load_extension(db.get(), 0);
    sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    execute(db.get(), "PRAGMA query_only=ON");
    execute(db.get(), "BEGIN");

    const auto game_columns = columns(db.get(), "Game");
    for (const std::string_view required : {"savegameversion", "flags", "seed", "gametick", "mods", "uniqueIds"}) {
        if (std::none_of(game_columns.begin(), game_columns.end(), [&](const Column& col) { return col.name == required; })) {
            throw std::runtime_error("Not a Scrap Mechanic save: Game column missing: " + std::string(required));
        }
    }
    auto game = prepare(db.get(), "SELECT count(*),typeof(savegameversion),typeof(flags),typeof(seed),typeof(gametick) FROM Game");
    if (step(db.get(), game.get()) != SQLITE_ROW || sqlite3_column_int64(game.get(), 0) != 1) throw std::runtime_error("Invalid save: expected one Game metadata row");
    for (int col = 1; col <= 4; ++col) if (text(game.get(), col) != "integer") throw std::runtime_error("Invalid save: Game numeric metadata must contain integers");

    std::ostringstream output;
    output << "{\"format\":\"scrapds.world-inspection.v1\",\"path\":" << json(utf8(std::filesystem::absolute(path)))
           << ",\"read_only\":true,\"blob_decoding\":\"partial-prefixes-with-confidence-labels\",\"sample_limit\":" << sample_limit
           << ",\"blob_prefix_bytes\":" << blob_prefix_bytes << ",\"game\":" << sample_json(db.get(), "Game", game_columns, 1)
           << ",\"tables\":[";
    auto tables = prepare(db.get(), "SELECT name,sql FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    bool first = true;
    while (step(db.get(), tables.get()) == SQLITE_ROW) {
        const auto table = text(tables.get(), 0);
        const auto cols = columns(db.get(), table);
        if (cols.empty()) throw std::runtime_error("Unsupported table without visible columns: " + table);
        auto count = prepare(db.get(), "SELECT count(*) FROM " + quote_identifier(table));
        if (step(db.get(), count.get()) != SQLITE_ROW) throw std::runtime_error("Missing table count");
        if (!first) output << ',';
        first = false;
        output << "{\"name\":" << json(table) << ",\"schema_sql\":" << json(text(tables.get(), 1))
               << ",\"count\":" << sqlite3_column_int64(count.get(), 0) << ",\"columns\":[";
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) output << ',';
            output << "{\"name\":" << json(cols[i].name) << ",\"declared_type\":" << json(cols[i].type)
                   << ",\"primary_key_order\":" << cols[i].pk << ",\"not_null\":" << (cols[i].not_null ? "true" : "false") << '}';
        }
        output << "],\"blob_columns\":" << blob_stats_json(db.get(), table, cols)
               << ",\"samples\":" << sample_json(db.get(), table, cols, sample_limit) << '}';
    }
    output << "]}";
    // Finalize active read statements before ending the consistent snapshot.
    tables.reset();
    game.reset();
    execute(db.get(), "ROLLBACK");
    return output.str();
}
} // namespace scrapds

