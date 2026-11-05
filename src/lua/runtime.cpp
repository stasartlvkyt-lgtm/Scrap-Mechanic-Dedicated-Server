#include "runtime.hpp"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
namespace scrapds {
namespace {
void instruction_limit(lua_State* L, lua_Debug*) { luaL_error(L, "scenario instruction budget exceeded"); }
double number(lua_State* L, int table, const char* key, double fallback) {
    lua_getfield(L, table, key);
    double value = fallback;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) { lua_pop(L, 1); throw std::runtime_error(std::string(key) + " must be a number"); }
        value = lua_tonumber(L, -1);
        if (!std::isfinite(value)) { lua_pop(L, 1); throw std::runtime_error(std::string(key) + " must be finite"); }
    }
    lua_pop(L, 1); return value;
}
Vec3 vector(lua_State* L, int table, const char* key, Vec3 fallback) {
    lua_getfield(L, table, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return fallback; }
    if (!lua_istable(L, -1)) { lua_pop(L, 1); throw std::runtime_error(std::string(key) + " must be a table"); }
    Vec3 value{number(L, lua_gettop(L), "x", fallback.x), number(L, lua_gettop(L), "y", fallback.y), number(L, lua_gettop(L), "z", fallback.z)};
    lua_pop(L, 1); return value;
}
}
Scenario load_scenario(const std::filesystem::path& path) {
    auto close = [](lua_State* L) { if (L) lua_close(L); };
    std::unique_ptr<lua_State, decltype(close)> state(luaL_newstate(), close);
    if (!state) throw std::runtime_error("cannot allocate Lua VM");
    lua_State* L = state.get();
    // Scenario files return data. No game sm.* API is emulated here.
    lua_pushcfunction(L, luaopen_math); lua_pushstring(L, LUA_MATHLIBNAME); lua_call(L, 1, 0);
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, 1000000);
    // Lua 5.1's fopen-based loader narrows Windows paths to the active code
    // page. Read through std::filesystem so Unicode filenames stay usable.
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open scenario file");
    constexpr std::size_t max_scenario_bytes = 4u * 1024u * 1024u;
    std::string source;
    char block[16384];
    while (input.read(block, sizeof(block)) || input.gcount() != 0) {
        if (source.size() + static_cast<std::size_t>(input.gcount()) > max_scenario_bytes)
            throw std::runtime_error("scenario file exceeds 4 MiB");
        source.append(block, static_cast<std::size_t>(input.gcount()));
    }
    if (input.bad()) throw std::runtime_error("cannot read scenario file");
    // Preserve luaL_loadfile's support for a Unix interpreter line.
    if (!source.empty() && source.front() == '#') {
        const auto end = source.find('\n');
        source = end == std::string::npos ? std::string("\n") : source.substr(end);
    }
    const auto utf8_path = path.u8string();
    const std::string chunk_name = "@" + std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk_name.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        const char* error = lua_tostring(L, -1);
        throw std::runtime_error(std::string("scenario: ") + (error ? error : "non-string Lua error"));
    }
    lua_sethook(L, nullptr, 0, 0);
    if (!lua_istable(L, -1)) throw std::runtime_error("scenario must return a table");
    Scenario s;
    s.dt = number(L, 1, "dt", s.dt);
    s.ground_z = number(L, 1, "ground_z", 0);
    s.gravity = vector(L, 1, "gravity", s.gravity);
    const double iterations = number(L, 1, "solver_iterations", s.solver_iterations);
    if (iterations < 1 || iterations > 1000 || std::floor(iterations) != iterations) throw std::runtime_error("solver_iterations must be an integer between 1 and 1000");
    s.solver_iterations = int(iterations);
    lua_getfield(L, 1, "name"); if (lua_type(L, -1) == LUA_TSTRING) s.name = lua_tostring(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "bodies");
    if (!lua_istable(L, -1)) throw std::runtime_error("scenario.bodies must be an array");
    const auto count = lua_objlen(L, -1);
    if (count > 10000) throw std::runtime_error("scenario exceeds 10000 bodies");
    for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, int(i));
        if (!lua_istable(L, -1)) throw std::runtime_error("each body must be a table");
        BodySpec b;
        const double id = number(L, 3, "id", double(i));
        if (id < 1 || id > std::numeric_limits<std::uint32_t>::max() || std::floor(id) != id) throw std::runtime_error("body id must be a positive uint32");
        b.id = std::uint32_t(id);
        b.mass = number(L, 3, "mass", b.mass);
        b.friction = number(L, 3, "friction", b.friction);
        b.restitution = number(L, 3, "restitution", b.restitution);
        b.position = vector(L, 3, "position", b.position);
        b.velocity = vector(L, 3, "velocity", b.velocity);
        b.half_extents = vector(L, 3, "half_extents", b.half_extents);
        s.bodies.push_back(b); lua_pop(L, 1);
    }
    return s;
}
}
