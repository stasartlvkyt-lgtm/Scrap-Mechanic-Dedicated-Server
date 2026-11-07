#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scrapds {
struct SavedLuaValue {
    enum class Kind { nil, boolean, number, string, uuid, table } kind{Kind::nil};
    bool boolean{};
    double number{};
    std::string string;
    std::array<std::uint8_t,16> uuid{};
    std::vector<std::pair<SavedLuaValue,SavedLuaValue>> entries;
};

// Decodes a bounded savegame LUA value stream (LUA\0\0\0\1), not Lua bytecode.
SavedLuaValue decode_saved_lua_value(std::span<const std::uint8_t> bytes);
const SavedLuaValue* saved_lua_field(const SavedLuaValue& table, std::string_view name);
}
