#include "state.hpp"
#include "core/json.hpp"
#include "shape_geometry.hpp"
#include "net/world_sync.hpp"
#include <lz4.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <sqlite3.h>
#include <chrono>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace scrapds {
namespace {
using Db = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
std::string utf8(const std::filesystem::path& path) { const auto u = path.u8string(); return {reinterpret_cast<const char*>(u.data()), u.size()}; }
std::string quote(std::string_view value) { std::string s = "\""; for (auto c : value) { s += c; if (c == '"') s += c; } return s + '"'; }
[[noreturn]] void fail(sqlite3* db, const char* context) { throw std::runtime_error(std::string(context) + ": " + sqlite3_errmsg(db)); }
Db open(const std::string& name, int flags) {
    sqlite3* raw = nullptr;
    const auto result = sqlite3_open_v2(name.c_str(), &raw, flags, nullptr);
    Db db(raw, sqlite3_close);
    if (result != SQLITE_OK) throw std::runtime_error(std::string("Cannot open world: ") + (raw ? sqlite3_errmsg(raw) : "out of memory"));
    sqlite3_busy_timeout(db.get(), 5000);
    return db;
}
Stmt prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    const auto result = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
    Stmt stmt(raw, sqlite3_finalize);
    if (result != SQLITE_OK) fail(db, "Preparing world state query");
    return stmt;
}
int step(sqlite3* db, sqlite3_stmt* s) { const auto r = sqlite3_step(s); if (r != SQLITE_ROW && r != SQLITE_DONE) fail(db, "Reading world state"); return r; }
void execute(sqlite3* db, const std::string& sql) { auto s = prepare(db, sql); while (step(db, s.get()) == SQLITE_ROW) {} }
std::string text(sqlite3_stmt* s, int col) { const auto* p = sqlite3_column_text(s, col); return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, col)) : std::string{}; }
Blob blob(sqlite3_stmt* s, int col) {
    if (sqlite3_column_type(s, col) != SQLITE_BLOB) throw std::runtime_error("Entity data must be a BLOB");
    const auto n = sqlite3_column_bytes(s, col);
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(s, col));
    if (n > 0 && !p) throw std::runtime_error("Cannot read entity BLOB");
    return n ? Blob(p, p + n) : Blob{};
}
std::uint32_t id(sqlite3_stmt* s, int col) {
    if (sqlite3_column_type(s, col) != SQLITE_INTEGER) throw std::runtime_error("Entity ID must be an integer");
    const auto n = sqlite3_column_int64(s, col);
    if (n < 0 || n > UINT32_MAX) throw std::runtime_error("Entity ID outside uint32 range");
    return static_cast<std::uint32_t>(n);
}
std::uint32_t reference_id(sqlite3_stmt* s, int col) {
    if (sqlite3_column_type(s, col) == SQLITE_INTEGER && sqlite3_column_int64(s, col) == -1) return UINT32_MAX;
    return id(s, col);
}
StoredContainer decode_stored_container(std::uint32_t row_id, Blob data) {
    StoredContainer result;result.id=row_id;result.data=std::move(data);
    const auto& bytes=result.data;
    if(bytes.size()<9||bytes[0]!=4||bytes[1]!=0||bytes[2]!=1)return result;
    const auto saved_id=(std::uint32_t(bytes[3])<<24)|(std::uint32_t(bytes[4])<<16)|
        (std::uint32_t(bytes[5])<<8)|bytes[6];
    const auto entries=std::uint16_t((std::uint16_t(bytes[7])<<8)|bytes[8]);
    constexpr std::size_t entry_size=22;
    if(saved_id!=row_id||entries>4096||std::size_t(entries)>(bytes.size()-9)/entry_size)return result;
    result.entry_count=entries;result.serialized_entries.reserve(entries);
    std::size_t offset=9;
    for(std::uint16_t i=0;i<entries;++i) {
        std::array<std::uint8_t,entry_size> entry{};
        std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(offset),entry_size,entry.begin());
        result.serialized_entries.push_back(entry);offset+=entry_size;
    }
    result.opaque_suffix.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.end());
    result.supported_layout=true;return result;
}
std::uint32_t container_quantity(const std::array<std::uint8_t,22>& entry) {
    return (std::uint32_t(entry[18])<<24)|(std::uint32_t(entry[19])<<16)|
        (std::uint32_t(entry[20])<<8)|entry[21];
}
void write_container_quantity(Blob& data,std::size_t offset,std::uint32_t quantity) {
    for(int i=0;i<4;++i)data.at(offset+i)=std::uint8_t(quantity>>(24-8*i));
}
void backup(sqlite3* source, sqlite3* dest) {
    auto* raw = sqlite3_backup_init(dest, "main", source, "main");
    if (!raw) fail(dest, "Initializing native world snapshot");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int result;
    do {
        result = sqlite3_backup_step(raw, 512);
        if ((result == SQLITE_BUSY || result == SQLITE_LOCKED) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        else if (result != SQLITE_OK) break;
    } while (std::chrono::steady_clock::now() < deadline);
    const auto finish = sqlite3_backup_finish(raw);
    if (result != SQLITE_DONE || finish != SQLITE_OK) throw std::runtime_error("Cannot complete native world snapshot: " + std::string(sqlite3_errstr(result)));
}
void check(sqlite3* db) {
    auto s = prepare(db, "PRAGMA quick_check");
    if (step(db, s.get()) != SQLITE_ROW || text(s.get(), 0) != "ok" || step(db, s.get()) != SQLITE_DONE) throw std::runtime_error("World database integrity check failed");
}
struct Transaction {
    sqlite3* db;
    bool committed{};
    explicit Transaction(sqlite3* value) : db(value) { execute(db, "SAVEPOINT scrapds_operation"); }
    void commit() { execute(db, "RELEASE SAVEPOINT scrapds_operation"); committed = true; }
    ~Transaction() { if (!committed) { sqlite3_exec(db, "ROLLBACK TO SAVEPOINT scrapds_operation", nullptr, nullptr, nullptr); sqlite3_exec(db, "RELEASE SAVEPOINT scrapds_operation", nullptr, nullptr, nullptr); } }
};
struct TemporaryDirectory {
    std::filesystem::path path;
    explicit TemporaryDirectory(const std::filesystem::path& parent) {
        std::random_device random;
        for (int i = 0; i < 32; ++i) {
            const auto p = parent / (".scrapds-save-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(p)) { path = p; return; }
        }
        throw std::runtime_error("Cannot create private snapshot directory");
    }
    ~TemporaryDirectory() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
};
}

struct WorldState::Impl {
    Db db{nullptr, sqlite3_close};
    std::filesystem::path source;
    GameMetadata metadata;
    std::map<std::uint32_t, StoredBody> bodies;
    std::map<std::uint32_t, StoredShape> shapes;
    std::map<std::uint32_t, StoredJoint> joints;
    std::map<std::uint32_t, StoredController> controllers;
    std::map<std::uint32_t, StoredContainer> container_records;
    std::set<std::uint32_t> container_ids;
    std::map<std::string, std::uint64_t> counts;
};
WorldState::WorldState(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorldState::WorldState(WorldState&&) noexcept = default;
WorldState& WorldState::operator=(WorldState&&) noexcept = default;
WorldState::~WorldState() = default;

bool WorldState::atomic(const std::function<bool()>& operation) {
    auto bodies=impl_->bodies; auto shapes=impl_->shapes; auto joints=impl_->joints;
    auto controllers=impl_->controllers; auto container_records=impl_->container_records; auto container_ids=impl_->container_ids;
    auto counts=impl_->counts; auto metadata=impl_->metadata;
    auto restore=[&] { impl_->bodies.swap(bodies); impl_->shapes.swap(shapes); impl_->joints.swap(joints); impl_->controllers.swap(controllers); impl_->container_records.swap(container_records); impl_->container_ids.swap(container_ids); impl_->counts.swap(counts); std::swap(impl_->metadata,metadata); };
    try {
        bool accepted;
        { Transaction transaction(impl_->db.get()); accepted=operation(); if(accepted) transaction.commit(); }
        if(!accepted)restore();
        return accepted;
    } catch(...) { restore(); throw; }
}
std::uint32_t WorldState::reserve_player_id() {
    auto* db=impl_->db.get();Transaction transaction(db);
    auto query=prepare(db,"SELECT uniqueIds FROM Game");
    if(step(db,query.get())!=SQLITE_ROW)throw std::runtime_error("missing-unique-id-counters");
    auto counters=blob(query.get(),0);query.reset();
    auto read32=[&](std::size_t at){if(at>counters.size()||counters.size()-at<4)throw std::runtime_error("invalid-unique-id-counters");return (std::uint32_t(counters[at])<<24)|(std::uint32_t(counters[at+1])<<16)|(std::uint32_t(counters[at+2])<<8)|counters[at+3];};
    const auto count=read32(0);
    if(count<9||count>17||counters.size()!=4ull*(count+1))throw std::runtime_error("invalid-unique-id-counters");
    constexpr std::size_t offset=36;const auto id=read32(offset);
    if(!id||id==UINT32_MAX)throw std::runtime_error("player-id-exhausted");
    for(int i=0;i<4;++i)counters[offset+i]=std::uint8_t((id+1)>>(24-8*i));
    auto update=prepare(db,"UPDATE Game SET uniqueIds=?1");
    sqlite3_bind_blob64(update.get(),1,counters.data(),counters.size(),SQLITE_TRANSIENT);
    if(step(db,update.get())!=SQLITE_DONE||sqlite3_changes(db)!=1)throw std::runtime_error("cannot-persist-player-id-counter");
    transaction.commit();return id;
}

std::map<std::uint64_t,StoredPlayerSnapshot> WorldState::player_snapshots() const {
    std::map<std::uint64_t,StoredPlayerSnapshot> players;
    if(!impl_->counts.contains("scrapds_player_v1"))return players;
    auto* db=impl_->db.get();
    std::set<std::uint64_t> verified_inventory_owners;
    if(impl_->counts.contains("scrapds_verified_player_inventory_v1")) {
        auto verified=prepare(db,"SELECT steam_id FROM scrapds_verified_player_inventory_v1 ORDER BY steam_id");
        while(step(db,verified.get())==SQLITE_ROW) {
            if(sqlite3_column_type(verified.get(),0)!=SQLITE_INTEGER||sqlite3_column_int64(verified.get(),0)<=0)
                throw std::runtime_error("invalid-verified-player-inventory-owner");
            verified_inventory_owners.insert(static_cast<std::uint64_t>(sqlite3_column_int64(verified.get(),0)));
        }
    }
    auto query=prepare(db,"SELECT steam_id, player_id, unlinked_packet FROM scrapds_player_v1 ORDER BY steam_id");
    while(step(db,query.get())==SQLITE_ROW) {
        if(sqlite3_column_type(query.get(),0)!=SQLITE_INTEGER||sqlite3_column_int64(query.get(),0)<=0)
            throw std::runtime_error("invalid-stored-player-owner");
        const auto owner=static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0));
        auto player=StoredPlayerSnapshot{id(query.get(),1),blob(query.get(),2),verified_inventory_owners.contains(owner)};
        if(player.player_id==UINT32_MAX||player.unlinked_packet.empty()||player.unlinked_packet.size()>2*1024*1024)
            throw std::runtime_error("invalid-stored-player-snapshot");
        if(!players.emplace(owner,std::move(player)).second)throw std::runtime_error("duplicate-stored-player-owner");
    }
    for(const auto owner:verified_inventory_owners)if(!players.contains(owner))throw std::runtime_error("verified-player-inventory-has-no-player-snapshot");
    return players;
}
void WorldState::store_player_snapshot(std::uint64_t steam_id,std::uint32_t player_id,const Blob& packet) {
    store_player_snapshot(steam_id,player_id,packet,false);
}
void WorldState::store_player_snapshot(std::uint64_t steam_id,std::uint32_t player_id,const Blob& packet,bool inventory_verified) {
    if(!steam_id||steam_id>INT64_MAX||player_id==UINT32_MAX||packet.empty()||packet.size()>2*1024*1024)
        throw std::invalid_argument("invalid-player-snapshot");
    auto* db=impl_->db.get();Transaction transaction(db);
    execute(db,"CREATE TABLE IF NOT EXISTS scrapds_player_v1 (steam_id INTEGER PRIMARY KEY, player_id INTEGER NOT NULL UNIQUE, unlinked_packet BLOB NOT NULL)");
    auto stmt=prepare(db,"INSERT INTO scrapds_player_v1(steam_id,player_id,unlinked_packet) VALUES(?1,?2,?3) ON CONFLICT(steam_id) DO UPDATE SET player_id=excluded.player_id, unlinked_packet=excluded.unlinked_packet");
    if(sqlite3_bind_int64(stmt.get(),1,static_cast<sqlite3_int64>(steam_id))!=SQLITE_OK||sqlite3_bind_int64(stmt.get(),2,player_id)!=SQLITE_OK||sqlite3_bind_blob(stmt.get(),3,packet.data(),static_cast<int>(packet.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
        fail(db,"Binding player snapshot");
    if(step(db,stmt.get())!=SQLITE_DONE)throw std::runtime_error("cannot-store-player-snapshot");
    if(inventory_verified) {
        execute(db,"CREATE TABLE IF NOT EXISTS scrapds_verified_player_inventory_v1 (steam_id INTEGER PRIMARY KEY)");
        auto verified=prepare(db,"INSERT INTO scrapds_verified_player_inventory_v1(steam_id) VALUES(?1) ON CONFLICT(steam_id) DO NOTHING");
        if(sqlite3_bind_int64(verified.get(),1,static_cast<sqlite3_int64>(steam_id))!=SQLITE_OK||step(db,verified.get())!=SQLITE_DONE)
            fail(db,"Binding verified player inventory owner");
    } else if(impl_->counts.contains("scrapds_verified_player_inventory_v1")) {
        auto verified=prepare(db,"DELETE FROM scrapds_verified_player_inventory_v1 WHERE steam_id=?1");
        if(sqlite3_bind_int64(verified.get(),1,static_cast<sqlite3_int64>(steam_id))!=SQLITE_OK||step(db,verified.get())!=SQLITE_DONE)
            fail(db,"Removing verified player inventory owner");
    }
    auto count=prepare(db,"SELECT count(*) FROM scrapds_player_v1");
    if(step(db,count.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-player-snapshots");
    const auto n=static_cast<std::uint64_t>(sqlite3_column_int64(count.get(),0));
    std::uint64_t verified_count{};
    if(inventory_verified||impl_->counts.contains("scrapds_verified_player_inventory_v1")) {
        auto verified=prepare(db,"SELECT count(*) FROM scrapds_verified_player_inventory_v1");
        if(step(db,verified.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-verified-player-inventory-owners");
        verified_count=static_cast<std::uint64_t>(sqlite3_column_int64(verified.get(),0));
    }
    transaction.commit();impl_->counts["scrapds_player_v1"]=n;
    if(inventory_verified||impl_->counts.contains("scrapds_verified_player_inventory_v1"))impl_->counts["scrapds_verified_player_inventory_v1"]=verified_count;
}
std::map<std::uint64_t,std::uint32_t> WorldState::player_character_colors() const {
    std::map<std::uint64_t,std::uint32_t> colors;
    if(!impl_->counts.contains("scrapds_player_color_v1"))return colors;
    auto query=prepare(impl_->db.get(),"SELECT steam_id,color_rgba FROM scrapds_player_color_v1 ORDER BY steam_id");
    while(step(impl_->db.get(),query.get())==SQLITE_ROW) {
        if(sqlite3_column_type(query.get(),0)!=SQLITE_INTEGER||sqlite3_column_int64(query.get(),0)<=0||sqlite3_column_type(query.get(),1)!=SQLITE_INTEGER)
            throw std::runtime_error("invalid-stored-player-color");
        const auto owner=static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0));
        const auto color=sqlite3_column_int64(query.get(),1);
        if(color<=0||color>UINT32_MAX||!colors.emplace(owner,static_cast<std::uint32_t>(color)).second)
            throw std::runtime_error("invalid-stored-player-color");
    }
    return colors;
}
void WorldState::store_player_character_color(std::uint64_t steam_id,std::uint32_t color_rgba) {
    if(!steam_id||steam_id>INT64_MAX||!color_rgba)throw std::invalid_argument("invalid-player-character-color");
    auto* db=impl_->db.get();Transaction transaction(db);
    execute(db,"CREATE TABLE IF NOT EXISTS scrapds_player_color_v1 (steam_id INTEGER PRIMARY KEY, color_rgba INTEGER NOT NULL)");
    auto stmt=prepare(db,"INSERT INTO scrapds_player_color_v1(steam_id,color_rgba) VALUES(?1,?2) ON CONFLICT(steam_id) DO UPDATE SET color_rgba=excluded.color_rgba");
    if(sqlite3_bind_int64(stmt.get(),1,static_cast<sqlite3_int64>(steam_id))!=SQLITE_OK||sqlite3_bind_int64(stmt.get(),2,color_rgba)!=SQLITE_OK)
        fail(db,"Binding player character color");
    if(step(db,stmt.get())!=SQLITE_DONE)throw std::runtime_error("cannot-store-player-character-color");
    auto count=prepare(db,"SELECT count(*) FROM scrapds_player_color_v1");
    if(step(db,count.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-player-character-colors");
    const auto n=static_cast<std::uint64_t>(sqlite3_column_int64(count.get(),0));
    transaction.commit();impl_->counts["scrapds_player_color_v1"]=n;
}
std::map<std::uint64_t,StoredPlayerLocation> WorldState::player_locations() const {
    std::map<std::uint64_t,StoredPlayerLocation> players;
    if(!impl_->counts.contains("scrapds_player_location_v1"))return players;
    auto* db=impl_->db.get();auto query=prepare(db,"SELECT steam_id,world_id,x,y,z FROM scrapds_player_location_v1 ORDER BY steam_id");
    while(step(db,query.get())==SQLITE_ROW) {
        if(sqlite3_column_type(query.get(),0)!=SQLITE_INTEGER||sqlite3_column_int64(query.get(),0)<=0||sqlite3_column_type(query.get(),1)!=SQLITE_INTEGER)
            throw std::runtime_error("invalid-stored-player-location");
        const auto owner=static_cast<std::uint64_t>(sqlite3_column_int64(query.get(),0));const auto world=sqlite3_column_int64(query.get(),1);
        if(world<0||world>UINT16_MAX)throw std::runtime_error("invalid-stored-player-world");
        StoredPlayerLocation location;location.world_id=static_cast<std::uint16_t>(world);
        for(int i=0;i<3;++i) {
            const auto type=sqlite3_column_type(query.get(),i+2);
            if(type!=SQLITE_FLOAT&&type!=SQLITE_INTEGER)throw std::runtime_error("invalid-stored-player-position");
            location.position[i]=sqlite3_column_double(query.get(),i+2);
            if(!std::isfinite(location.position[i])||std::abs(location.position[i])>8192.0)throw std::runtime_error("invalid-stored-player-position");
        }
        if(!players.emplace(owner,location).second)throw std::runtime_error("duplicate-stored-player-location");
    }
    return players;
}
void WorldState::store_player_location(std::uint64_t steam_id,const StoredPlayerLocation& location) {
    if(!steam_id||steam_id>INT64_MAX)throw std::invalid_argument("invalid-player-owner");
    for(const auto value:location.position)if(!std::isfinite(value)||std::abs(value)>8192.0)throw std::invalid_argument("invalid-player-position");
    auto* db=impl_->db.get();Transaction transaction(db);
    execute(db,"CREATE TABLE IF NOT EXISTS scrapds_player_location_v1 (steam_id INTEGER PRIMARY KEY, world_id INTEGER NOT NULL, x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL)");
    auto stmt=prepare(db,"INSERT INTO scrapds_player_location_v1(steam_id,world_id,x,y,z) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(steam_id) DO UPDATE SET world_id=excluded.world_id,x=excluded.x,y=excluded.y,z=excluded.z");
    if(sqlite3_bind_int64(stmt.get(),1,static_cast<sqlite3_int64>(steam_id))!=SQLITE_OK||sqlite3_bind_int(stmt.get(),2,location.world_id)!=SQLITE_OK)fail(db,"Binding player location");
    for(int i=0;i<3;++i)if(sqlite3_bind_double(stmt.get(),i+3,location.position[i])!=SQLITE_OK)fail(db,"Binding player location");
    if(step(db,stmt.get())!=SQLITE_DONE)throw std::runtime_error("cannot-store-player-location");
    auto count=prepare(db,"SELECT count(*) FROM scrapds_player_location_v1");
    if(step(db,count.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-player-locations");
    const auto n=static_cast<std::uint64_t>(sqlite3_column_int64(count.get(),0));transaction.commit();impl_->counts["scrapds_player_location_v1"]=n;
}

WorldState WorldState::load(const std::filesystem::path& source) {
    auto impl = std::make_unique<Impl>();
    impl->source = std::filesystem::canonical(source);
    auto original = open(utf8(impl->source), SQLITE_OPEN_READONLY);
    execute(original.get(), "PRAGMA query_only=ON");
    impl->db = open(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    backup(original.get(), impl->db.get());
    original.reset();
    auto* db = impl->db.get();
    check(db);
    {
        auto s = prepare(db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        while (step(db, s.get()) == SQLITE_ROW) {
            auto name = text(s.get(), 0);
            auto count = prepare(db, "SELECT count(*) FROM " + quote(name));
            if (step(db, count.get()) != SQLITE_ROW) throw std::runtime_error("Cannot count world table");
            impl->counts.emplace(std::move(name), static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0)));
        }
    }
    if (!impl->counts.contains("Game") || impl->counts.at("Game") != 1) throw std::runtime_error("World must contain exactly one Game row");
    {
        auto s = prepare(db, "SELECT * FROM Game");
        if (step(db, s.get()) != SQLITE_ROW) throw std::runtime_error("Missing Game metadata");
        std::map<std::string, int> fields;
        for (int c = 0; c < sqlite3_column_count(s.get()); ++c) fields.emplace(sqlite3_column_name(s.get(), c), c);
        auto integer = [&](const char* key) {
            if (!fields.contains(key) || sqlite3_column_type(s.get(), fields.at(key)) != SQLITE_INTEGER) throw std::runtime_error(std::string("Missing or invalid Game.") + key);
            return sqlite3_column_int64(s.get(), fields.at(key));
        };
        impl->metadata = {integer("savegameversion"), integer("flags"), integer("seed"), integer("gametick"), {}, {}};
        if (impl->metadata.save_version < 27 || impl->metadata.save_version > 30 || impl->metadata.tick < 0) throw std::runtime_error("Unsupported save version or negative game tick");
        if (fields.contains("gameversion") && sqlite3_column_type(s.get(), fields.at("gameversion")) != SQLITE_NULL) impl->metadata.game_version = text(s.get(), fields.at("gameversion"));
        if (fields.contains("buildnumber") && sqlite3_column_type(s.get(), fields.at("buildnumber")) != SQLITE_NULL) impl->metadata.build_number = integer("buildnumber");
    }
    if (impl->counts.contains("RigidBody")) {
        auto s = prepare(db, "SELECT id,worldId,data FROM RigidBody ORDER BY id");
        while (step(db, s.get()) == SQLITE_ROW) {
            const auto world = id(s.get(), 1);
            if (world > UINT16_MAX) throw std::runtime_error("World ID outside uint16 range");
            StoredBody b{id(s.get(), 0), static_cast<std::uint16_t>(world), blob(s.get(), 2), {}};
            try { b.prefix = decode_rigid_body_prefix(b.data, b.data.size()); } catch (const std::runtime_error&) {}
            if (b.prefix && (b.prefix->id != b.id || b.prefix->world_id != b.world_id)) throw std::runtime_error("RigidBody BLOB identity disagrees with row");
            if (!impl->bodies.emplace(b.id, std::move(b)).second) throw std::runtime_error("Duplicate body ID");
        }
    }
    if (impl->counts.contains("ChildShape")) {
        auto s = prepare(db, "SELECT id,bodyId,data FROM ChildShape ORDER BY id");
        while (step(db, s.get()) == SQLITE_ROW) {
            StoredShape shape{id(s.get(), 0), id(s.get(), 1), blob(s.get(), 2), {}};
            try { shape.prefix = decode_child_shape_prefix(shape.data, shape.data.size()); } catch (const std::runtime_error&) {}
            if (shape.prefix && (shape.prefix->id != shape.id || shape.prefix->body_id != shape.body_id)) throw std::runtime_error("ChildShape BLOB identity disagrees with row");
            if (!impl->bodies.contains(shape.body_id)) throw std::runtime_error("ChildShape refers to missing body");
            if (!impl->shapes.emplace(shape.id, std::move(shape)).second) throw std::runtime_error("Duplicate shape ID");
        }
    }
    if (impl->counts.contains("Joint")) {
        auto s = prepare(db, "SELECT id,childShapeIdA,childShapeIdB,data FROM Joint ORDER BY id");
        while (step(db, s.get()) == SQLITE_ROW) {
            StoredJoint joint{id(s.get(), 0), reference_id(s.get(), 1), reference_id(s.get(), 2), blob(s.get(), 3)};
            // Some joints use sentinel/world anchors; retain these for the joint
            // decoder rather than inventing a referential constraint here.
            if (!impl->joints.emplace(joint.id, std::move(joint)).second) throw std::runtime_error("Duplicate joint ID");
        }
    }
    if (impl->counts.contains("Controller")) {
        auto s = prepare(db, "SELECT id,hostId,hostType,data FROM Controller ORDER BY id");
        while (step(db, s.get()) == SQLITE_ROW) {
            StoredController controller{id(s.get(), 0), reference_id(s.get(), 1), id(s.get(), 2), blob(s.get(), 3)};
            if (!impl->controllers.emplace(controller.id, std::move(controller)).second) throw std::runtime_error("Duplicate controller ID");
        }
    }
    if (impl->counts.contains("Container")) {
        auto s = prepare(db, "SELECT id,data FROM Container ORDER BY id");
        while (step(db, s.get()) == SQLITE_ROW) {
            const auto container_id = id(s.get(), 0);
            auto record=decode_stored_container(container_id,blob(s.get(),1));
            if (!impl->container_ids.insert(container_id).second||!impl->container_records.emplace(container_id,std::move(record)).second)
                throw std::runtime_error("Duplicate container ID");
        }
    }
    return WorldState(std::move(impl));
}
const GameMetadata& WorldState::metadata() const { return impl_->metadata; }
const std::map<std::uint32_t, StoredBody>& WorldState::bodies() const { return impl_->bodies; }
const std::map<std::uint32_t, StoredShape>& WorldState::shapes() const { return impl_->shapes; }
const std::map<std::uint32_t, StoredJoint>& WorldState::joints() const { return impl_->joints; }
const std::map<std::uint32_t, StoredController>& WorldState::controllers() const { return impl_->controllers; }
const std::map<std::uint32_t, StoredContainer>& WorldState::containers() const { return impl_->container_records; }
std::optional<std::uint32_t> WorldState::container_slot_quantity(std::uint32_t container_id,
    std::uint32_t slot,const std::array<std::uint8_t,16>& saved_item_uuid) const {
    const auto container=impl_->container_records.find(container_id);
    if(container==impl_->container_records.end()||!container->second.supported_layout||slot>=container->second.entry_count)
        return std::nullopt;
    const auto& entry=container->second.serialized_entries[slot];
    if(!std::equal(saved_item_uuid.begin(),saved_item_uuid.end(),entry.begin()+2))return std::nullopt;
    const auto quantity=container_quantity(entry);
    if(quantity==UINT32_MAX)return std::nullopt;
    return quantity;
}
void WorldState::spend_container_slot(std::uint32_t container_id,std::uint32_t slot,
    const std::array<std::uint8_t,16>& saved_item_uuid,std::uint32_t quantity) {
    if(quantity==0||quantity==UINT32_MAX)throw std::invalid_argument("invalid-container-spend-quantity");
    const auto container=impl_->container_records.find(container_id);
    if(container==impl_->container_records.end())throw std::runtime_error("unknown-container");
    if(!container->second.supported_layout)throw std::runtime_error("unsupported-container-layout");
    if(slot>=container->second.entry_count)throw std::runtime_error("container-slot-out-of-range");
    const auto available=container_slot_quantity(container_id,slot,saved_item_uuid);
    if(!available)throw std::runtime_error("container-slot-item-mismatch-or-empty");
    if(*available<quantity)throw std::runtime_error("insufficient-container-slot-quantity");
    auto updated=container->second.data;
    constexpr std::size_t header_size=9,entry_size=22,quantity_offset=18;
    const auto offset=header_size+std::size_t(slot)*entry_size+quantity_offset;
    write_container_quantity(updated,offset,*available==quantity?UINT32_MAX:*available-quantity);
    replace_container_blob(container_id,std::move(updated));
}
const std::set<std::uint32_t>& WorldState::container_ids() const { return impl_->container_ids; }
const std::map<std::string, std::uint64_t>& WorldState::table_counts() const { return impl_->counts; }
std::set<std::uint16_t> WorldState::flat_surface_worlds() const {
    std::set<std::uint16_t> result;if(!impl_->counts.contains("GenericData"))return result;
    auto query=prepare(impl_->db.get(),"SELECT uid,key,worldId,flags,data FROM GenericData");
    constexpr std::string_view path="$GAME_DATA/Scripts/game/worlds/CreativeFlatWorld.lua",name="CreativeFlatWorld";
    while(step(impl_->db.get(),query.get())==SQLITE_ROW) {
        try {
            const auto uid=blob(query.get(),0),key=blob(query.get(),1),data=blob(query.get(),4);
            if(uid.size()!=16||key!=Blob({0,0,0,0})||data.size()>1024*1024)continue;
            const auto entry=net::parse_saved_blob_data(data);const auto world=sqlite3_column_int64(query.get(),2),flags=sqlite3_column_int64(query.get(),3);
            if(world<0||world>65535||entry.world_id!=world||entry.flags!=flags||entry.key!=key||!std::equal(uid.begin(),uid.end(),entry.uid.begin())||entry.payload.empty())continue;
            std::array<char,4096> unpacked{};const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(entry.payload.data()),unpacked.data(),int(entry.payload.size()),int(unpacked.size()));
            if(n<8)continue;const auto size=std::size_t(n);const auto* p=reinterpret_cast<const unsigned char*>(unpacked.data());
            std::size_t at=4;const auto take_string=[&](std::string_view expected){if(at+2>size)return false;const auto length=std::size_t((p[at]<<8)|p[at+1]);at+=2;if(length!=expected.size()||length>size-at)return false;const bool ok=std::equal(expected.begin(),expected.end(),unpacked.begin()+at);at+=length;return ok;};
            if(take_string(path)&&take_string(name))result.insert(entry.world_id);
        } catch(const std::runtime_error&) { /* Unrelated or unsupported GenericData records are preserved unchanged. */ }
    }
    return result;
}
std::set<std::uint16_t> WorldState::terrain_generator_worlds() const {
    std::set<std::uint16_t> result;if(!impl_->counts.contains("ScriptData"))return result;
    constexpr std::array<std::uint8_t,16> uid{0x61,0xaa,0x13,0xd7,0xe7,0x15,0x41,0x53,0xa2,0x69,0x4d,0x33,0x8c,0x0c,0x5b,0xd4};
    auto query=prepare(impl_->db.get(),"SELECT worldId,key FROM ScriptData WHERE uid=?1 ORDER BY worldId");
    sqlite3_bind_blob(query.get(),1,uid.data(),int(uid.size()),SQLITE_TRANSIENT);
    while(step(impl_->db.get(),query.get())==SQLITE_ROW) {
        if(sqlite3_column_type(query.get(),0)!=SQLITE_INTEGER||sqlite3_column_type(query.get(),1)!=SQLITE_BLOB)continue;
        const auto world=sqlite3_column_int64(query.get(),0);if(world<0||world>UINT16_MAX)continue;
        const auto key=blob(query.get(),1);const auto id=static_cast<std::uint16_t>(world);
        const std::array<std::uint8_t,4> expected{std::uint8_t(id),std::uint8_t(id>>8),0,0};
        if(key.size()==expected.size()&&std::equal(key.begin(),key.end(),expected.begin()))result.insert(id);
    }
    return result;
}
namespace {
std::vector<StoredBlobDataEntry> read_saved_blob_entries(sqlite3* db,const char* table,const std::array<std::uint8_t,16>* filter=nullptr) {
    auto statement="SELECT uid,key,worldId,flags,data FROM "+quote(table);
    if(filter)statement+=" WHERE uid=?1";
    statement+=" ORDER BY worldId,uid,key";
    auto query=prepare(db,statement);
    if(filter&&sqlite3_bind_blob(query.get(),1,filter->data(),int(filter->size()),SQLITE_TRANSIENT)!=SQLITE_OK)fail(db,"Binding saved BlobData UUID");
    std::vector<StoredBlobDataEntry> result;
    while(step(db,query.get())==SQLITE_ROW) {
        if(sqlite3_column_type(query.get(),0)!=SQLITE_BLOB||sqlite3_column_type(query.get(),1)!=SQLITE_BLOB||
           sqlite3_column_type(query.get(),2)!=SQLITE_INTEGER||sqlite3_column_type(query.get(),3)!=SQLITE_INTEGER||
           sqlite3_column_type(query.get(),4)!=SQLITE_BLOB)throw std::runtime_error("invalid-saved-blob-data-row");
        const auto uid=blob(query.get(),0),key=blob(query.get(),1),data=blob(query.get(),4);
        const auto world=sqlite3_column_int64(query.get(),2),flags=sqlite3_column_int64(query.get(),3);
        if(uid.size()!=16||world<0||world>UINT16_MAX||flags<0||flags>UINT8_MAX)throw std::runtime_error("invalid-saved-blob-data-identity");
        const auto entry=net::parse_saved_blob_data(data);
        if(!std::equal(uid.begin(),uid.end(),entry.uid.begin())||key!=entry.key||entry.world_id!=world||entry.flags!=flags)
            throw std::runtime_error("saved-blob-data-row-disagrees-with-entry");
        StoredBlobDataEntry stored;std::copy(uid.begin(),uid.end(),stored.uid.begin());stored.key=key;stored.payload=entry.payload;
        stored.world_id=entry.world_id;stored.flags=entry.flags;result.push_back(std::move(stored));
    }
    return result;
}
}
std::vector<StoredBlobDataEntry> WorldState::terrain_generator_entries() const {
    if(!impl_->counts.contains("ScriptData"))return {};
    constexpr std::array<std::uint8_t,16> uid{0x61,0xaa,0x13,0xd7,0xe7,0x15,0x41,0x53,0xa2,0x69,0x4d,0x33,0x8c,0x0c,0x5b,0xd4};
    return read_saved_blob_entries(impl_->db.get(),"ScriptData",&uid);
}
std::vector<StoredBlobDataEntry> WorldState::script_data_entries(const std::array<std::uint8_t,16>& uid) const {
    return impl_->counts.contains("ScriptData")?read_saved_blob_entries(impl_->db.get(),"ScriptData",&uid):std::vector<StoredBlobDataEntry>{};
}
std::vector<StoredBlobDataEntry> WorldState::generic_data_entries() const {
    return impl_->counts.contains("GenericData")?read_saved_blob_entries(impl_->db.get(),"GenericData"):std::vector<StoredBlobDataEntry>{};
}
std::vector<TerrainDelta> WorldState::terrain_deltas() const {
    if(!impl_->counts.contains("VoxelTerrain"))return {};
    auto query=prepare(impl_->db.get(),"SELECT id,worldId,x,y,data FROM VoxelTerrain ORDER BY id");
    std::vector<TerrainDelta> result;
    while(step(impl_->db.get(),query.get())==SQLITE_ROW) {
        for(int column=0;column<4;++column)if(sqlite3_column_type(query.get(),column)!=SQLITE_INTEGER)throw std::runtime_error("invalid-terrain-delta-identity");
        if(sqlite3_column_type(query.get(),4)!=SQLITE_BLOB)throw std::runtime_error("invalid-terrain-delta-payload");
        const auto row_id=sqlite3_column_int64(query.get(),0),row_world=sqlite3_column_int64(query.get(),1);
        const auto row_x=sqlite3_column_int64(query.get(),2),row_y=sqlite3_column_int64(query.get(),3);
        if(row_id<0||row_id>UINT32_MAX||row_world<0||row_world>UINT16_MAX||row_x<INT32_MIN||row_x>INT32_MAX||row_y<INT32_MIN||row_y>INT32_MAX)
            throw std::runtime_error("terrain-delta-identity-out-of-range");
        const auto data=blob(query.get(),4);auto delta=decode_terrain_delta(data);
        if(delta.id!=row_id||delta.world_id!=row_world||delta.region_x!=row_x||delta.region_y!=row_y)
            throw std::runtime_error("terrain-delta-row-disagrees-with-record");
        result.push_back(std::move(delta));
    }
    return result;
}
std::optional<Blob> WorldState::terrain_generator_data(std::uint16_t world_id) const {
    if(!impl_->counts.contains("ScriptData"))return std::nullopt;
    constexpr std::array<std::uint8_t,16> uid{0x61,0xaa,0x13,0xd7,0xe7,0x15,0x41,0x53,0xa2,0x69,0x4d,0x33,0x8c,0x0c,0x5b,0xd4};
    const std::array<std::uint8_t,4> key{std::uint8_t(world_id),std::uint8_t(world_id>>8),0,0};
    auto query=prepare(impl_->db.get(),"SELECT flags,data FROM ScriptData WHERE uid=?1 AND key=?2 AND worldId=?3");
    sqlite3_bind_blob(query.get(),1,uid.data(),int(uid.size()),SQLITE_TRANSIENT);
    sqlite3_bind_blob(query.get(),2,key.data(),int(key.size()),SQLITE_TRANSIENT);
    sqlite3_bind_int(query.get(),3,world_id);
    if(step(impl_->db.get(),query.get())!=SQLITE_ROW)return std::nullopt;
    const auto flags=sqlite3_column_int64(query.get(),0);const auto entry=net::parse_saved_blob_data(blob(query.get(),1));
    if(entry.uid!=uid||entry.key!=Blob(key.begin(),key.end())||entry.world_id!=world_id||entry.flags!=flags||entry.payload.empty()||entry.payload.size()>16u*1024u*1024u)throw std::runtime_error("invalid terrain generator entry");
    Blob decoded(16u*1024u*1024u);
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(entry.payload.data()),reinterpret_cast<char*>(decoded.data()),int(entry.payload.size()),int(decoded.size()));
    constexpr std::array<std::uint8_t,7> magic{'L','U','A',0,0,0,1};
    if(n<int(magic.size())||!std::equal(magic.begin(),magic.end(),decoded.begin()))throw std::runtime_error("unsupported terrain generator payload");
    decoded.resize(std::size_t(n));return decoded;
}

void WorldState::advance_ticks(std::uint64_t count) {
    if (count > static_cast<std::uint64_t>(INT64_MAX - impl_->metadata.tick)) throw std::runtime_error("Game tick overflow");
    const auto tick = impl_->metadata.tick + static_cast<std::int64_t>(count);
    auto s = prepare(impl_->db.get(), "UPDATE Game SET gametick=?1");
    sqlite3_bind_int64(s.get(), 1, tick);
    if (step(impl_->db.get(), s.get()) != SQLITE_DONE || sqlite3_changes(impl_->db.get()) != 1) throw std::runtime_error("Cannot update game tick");
    impl_->metadata.tick = tick;
}
void WorldState::replace_body_blob(std::uint32_t body_id, Blob data) {
    const auto old = impl_->bodies.find(body_id);
    if (old == impl_->bodies.end()) throw std::runtime_error("Cannot replace unknown body");
    const auto decoded = decode_rigid_body_prefix(data, data.size());
    if (decoded.id != body_id || decoded.world_id != old->second.world_id) throw std::runtime_error("Cannot change body identity or world in BLOB replacement");
    Transaction transaction(impl_->db.get());
    auto s = prepare(impl_->db.get(), "UPDATE RigidBody SET data=?1 WHERE id=?2");
    sqlite3_bind_blob64(s.get(), 1, data.data(), data.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.get(), 2, body_id);
    if (step(impl_->db.get(), s.get()) != SQLITE_DONE || sqlite3_changes(impl_->db.get()) != 1) throw std::runtime_error("Cannot replace body BLOB");
    if (impl_->counts.contains("RigidBodyBounds")) {
        auto bounds = prepare(impl_->db.get(), "UPDATE RigidBodyBounds SET minX=?1,maxX=?2,minY=?3,maxY=?4 WHERE id=?5");
        for (int i = 0; i < 4; ++i) sqlite3_bind_double(bounds.get(), i + 1, decoded.bounds_min_x_max_x_min_y_max_y[i]);
        sqlite3_bind_int64(bounds.get(), 5, body_id);
        if (step(impl_->db.get(), bounds.get()) != SQLITE_DONE || sqlite3_changes(impl_->db.get()) != 1) throw std::runtime_error("Cannot update body RTree bounds");
    }
    transaction.commit();
    old->second.data = std::move(data);
    old->second.prefix = decoded;
}
void WorldState::replace_shape_blob(std::uint32_t shape_id, Blob data) {
    std::map<std::uint32_t,Blob> mutation;
    mutation.emplace(shape_id,std::move(data));
    replace_shape_blobs(std::move(mutation));
}
void WorldState::replace_shape_blobs(std::map<std::uint32_t, Blob> data) {
    if(data.empty())return;
    std::map<std::uint32_t,ChildShapePrefix> decoded;
    for(const auto& [id,bytes]:data) {
        const auto old=impl_->shapes.find(id);
        if(old==impl_->shapes.end())throw std::runtime_error("Cannot replace unknown shape");
        auto prefix=decode_child_shape_prefix(bytes,bytes.size());
        if(prefix.id!=id||prefix.body_id!=old->second.body_id)throw std::runtime_error("Cannot change shape identity or body in BLOB replacement");
        decoded.emplace(id,std::move(prefix));
    }
    Transaction transaction(impl_->db.get());
    auto s=prepare(impl_->db.get(),"UPDATE ChildShape SET data=?1 WHERE id=?2");
    for(const auto& [id,bytes]:data) {
        sqlite3_reset(s.get());sqlite3_clear_bindings(s.get());
        sqlite3_bind_blob64(s.get(),1,bytes.data(),bytes.size(),SQLITE_TRANSIENT);
        sqlite3_bind_int64(s.get(),2,id);
        if(step(impl_->db.get(),s.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("Cannot replace shape BLOB");
    }
    transaction.commit();
    for(auto& [id,bytes]:data) { auto& shape=impl_->shapes.at(id);shape.data=std::move(bytes);shape.prefix=std::move(decoded.at(id)); }
}
void WorldState::replace_joint_blob(std::uint32_t joint_id, Blob data) {
    const auto old=impl_->joints.find(joint_id);
    if(old==impl_->joints.end())throw std::runtime_error("Cannot replace unknown joint");
    const auto read32=[](const Blob& bytes,std::size_t at) {
        return (std::uint32_t(bytes.at(at))<<24)|(std::uint32_t(bytes.at(at+1))<<16)|(std::uint32_t(bytes.at(at+2))<<8)|bytes.at(at+3);
    };
    if(data.size()!=old->second.data.size()||data.size()<15||data[0]!=2||data[2]!=1||
       read32(data,3)!=joint_id||read32(data,7)!=old->second.shape_a||read32(data,11)!=old->second.shape_b)
        throw std::runtime_error("Cannot change joint identity, references, or BLOB size");
    Transaction transaction(impl_->db.get());
    auto s=prepare(impl_->db.get(),"UPDATE Joint SET data=?1 WHERE id=?2");
    sqlite3_bind_blob64(s.get(),1,data.data(),data.size(),SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.get(),2,joint_id);
    if(step(impl_->db.get(),s.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("Cannot replace joint BLOB");
    transaction.commit();
    old->second.data=std::move(data);
}
void WorldState::replace_container_blob(std::uint32_t container_id,Blob data) {
    const auto old=impl_->container_records.find(container_id);
    if(old==impl_->container_records.end())throw std::runtime_error("Cannot replace unknown container");
    if(data.empty()||data.size()>16*1024*1024)throw std::invalid_argument("invalid-container-blob-size");
    auto decoded=decode_stored_container(container_id,std::move(data));
    if(!decoded.supported_layout)throw std::runtime_error("unsupported-container-blob-layout");
    Transaction transaction(impl_->db.get());
    auto update=prepare(impl_->db.get(),"UPDATE Container SET data=?1 WHERE id=?2");
    if(sqlite3_bind_blob64(update.get(),1,decoded.data.data(),decoded.data.size(),SQLITE_TRANSIENT)!=SQLITE_OK||
       sqlite3_bind_int64(update.get(),2,container_id)!=SQLITE_OK)
        fail(impl_->db.get(),"Binding container BLOB");
    if(step(impl_->db.get(),update.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)
        throw std::runtime_error("Cannot replace container BLOB");
    transaction.commit();old->second=std::move(decoded);
}
CreatedBlock WorldState::create_block(const BlockCreation& request) {
    auto* db=impl_->db.get();
    if(request.world_id==UINT16_MAX)throw std::runtime_error("invalid-world-id");
    if(!impl_->counts.contains("RigidBody")||!impl_->counts.contains("ChildShape"))throw std::runtime_error("missing-creation-tables");
    auto read32=[](const Blob& b,std::size_t at) { return (std::uint32_t(b.at(at))<<24)|(std::uint32_t(b.at(at+1))<<16)|(std::uint32_t(b.at(at+2))<<8)|b.at(at+3); };
    auto put32=[](Blob& b,std::size_t at,std::uint32_t n) { for(int i=0;i<4;++i)b.at(at+i)=std::uint8_t(n>>(24-8*i)); };
    auto put16=[](Blob& b,std::size_t at,std::int32_t n) { b.at(at)=std::uint8_t(std::uint32_t(n)>>8);b.at(at+1)=std::uint8_t(n); };
    auto putfloat=[&](Blob& b,std::size_t at,float n) { put32(b,at,std::bit_cast<std::uint32_t>(n)); };
    auto identity=[](const RigidBodyPose& pose) { const auto& q=pose.rotation_xyzw;return pose.is_static&&q[0]==0&&q[1]==0&&q[2]==0&&(q[3]==1||q[3]==-1); };
    std::array<std::int32_t,3> local=request.cell_position;
    std::array<float,3> origin{};
    const StoredBody* existing=nullptr;
    if(request.body_id) {
        const auto it=impl_->bodies.find(*request.body_id);
        if(it==impl_->bodies.end())throw std::runtime_error("unknown-body");
        existing=&it->second;
        if(existing->world_id!=request.world_id)throw std::runtime_error("body-in-another-world");
        const auto pose=decode_rigid_body_pose(existing->data);
        if(!pose||!identity(*pose)||existing->data.at(55)!=0)throw std::runtime_error("unsupported-target-body-transform-or-flags");
        origin=pose->position;
    } else {
        for(std::size_t i=0;i<3;++i) {
            local[i]=request.cell_position[i]%16;
            const auto exact=double(request.cell_position[i]-local[i])*0.25;
            origin[i]=float(exact);
            if(double(origin[i])!=exact)throw std::runtime_error("body-position-float-overflow");
        }
    }
    std::array<double,3> lower{},upper{};
    std::uint64_t volume=1;
    for(std::size_t i=0;i<3;++i) {
        const auto extent=request.dimensions[i];
        if(local[i]<INT16_MIN||local[i]>INT16_MAX||extent<=0||extent>INT16_MAX)throw std::runtime_error("block-geometry-out-of-range");
        volume*=std::uint32_t(extent);
        lower[i]=double(origin[i])+double(local[i])*0.25;
        upper[i]=lower[i]+double(extent)*0.25;
        if(double(float(lower[i]))!=lower[i]||double(float(upper[i]))!=upper[i])throw std::runtime_error("block-bounds-float-overflow");
    }
    if(volume>UINT32_MAX)throw std::runtime_error("block-volume-overflow");
    // We only authorize geometry for which an exact box test is available.
    // Unknown/rotated/dynamic objects whose broad bounds overlap are rejected.
    for(const auto& [body_id,body]:impl_->bodies) {
        (void)body_id;
        if(body.world_id!=request.world_id)continue;
        auto broad_overlap=[&] {
            if(!body.prefix)return true;
            const auto& b=body.prefix->bounds_min_x_max_x_min_y_max_y;
            return lower[0]<b[1]&&upper[0]>b[0]&&lower[1]<b[3]&&upper[1]>b[2];
        };
        std::optional<RigidBodyPose> pose;
        try {pose=decode_rigid_body_pose(body.data);}catch(const std::runtime_error&){}
        if(!pose||!identity(*pose)) {if(broad_overlap())throw std::runtime_error("unsupported-collision-geometry");continue;}
        for(const auto& [shape_id,shape]:impl_->shapes) {
            (void)shape_id;
            if(shape.body_id!=body.id)continue;
            std::optional<ShapeGeometry> geometry;
            try {geometry=decode_child_geometry(shape.data);}catch(const std::runtime_error&){}
            if(!geometry||geometry->kind!=SavedShapeKind::block||!geometry->grid_dimensions) {if(broad_overlap())throw std::runtime_error("unsupported-collision-geometry");continue;}
            bool intersects=true;
            for(std::size_t i=0;i<3;++i) {
                const auto extent=geometry->grid_dimensions->at(i);
                if(extent<=0) {if(broad_overlap())throw std::runtime_error("unsupported-collision-geometry");intersects=false;break;}
                const auto a=double(pose->position[i])+double(geometry->grid_position[i])*0.25;
                const auto b=a+double(extent)*0.25;
                if(lower[i]>=b||upper[i]<=a)intersects=false;
            }
            if(intersects)throw std::runtime_error("block-overlaps-existing-shape");
        }
    }
    Blob counters;
    {
        auto s=prepare(db,"SELECT uniqueIds FROM Game");
        if(step(db,s.get())!=SQLITE_ROW)throw std::runtime_error("missing-unique-id-counters");
        counters=blob(s.get(),0);
    }
    if(counters.size()<16)throw std::runtime_error("invalid-unique-id-counters");
    const auto count=read32(counters,0);
    if(count<3||count>17||counters.size()!=4ull*(count+1))throw std::runtime_error("invalid-unique-id-counters");
    const auto shape_id=read32(counters,12);
    const auto body_id=request.body_id.value_or(read32(counters,4));
    if(shape_id==0||shape_id==UINT32_MAX||(!impl_->shapes.empty()&&shape_id<=impl_->shapes.rbegin()->first))throw std::runtime_error("shape-id-exhausted-or-collision");
    if(!existing&&(body_id==0||body_id==UINT32_MAX||(!impl_->bodies.empty()&&body_id<=impl_->bodies.rbegin()->first)))throw std::runtime_error("body-id-exhausted-or-collision");
    put32(counters,12,shape_id+1);
    if(!existing)put32(counters,4,body_id+1);
    Blob body_data=existing?existing->data:Blob(60);
    if(!existing) {
        body_data[1]=1;body_data[2]=1;
        put32(body_data,3,body_id);put16(body_data,7,request.world_id);put16(body_data,25,request.world_id);
        putfloat(body_data,27,1); // StaticBody serializes W,Z,Y,X.
        for(std::size_t i=0;i<3;++i)putfloat(body_data,43+4*i,origin[i]);
        put32(body_data,56,UINT32_MAX); // StaticBody attached-character reference defaults -1.
    }
    std::array<float,4> bounds{float(lower[0]),float(upper[0]),float(lower[1]),float(upper[1])};
    if(existing) {
        const auto& previous=existing->prefix->bounds_min_x_max_x_min_y_max_y;
        bounds={std::min(bounds[0],previous[0]),std::max(bounds[1],previous[1]),std::min(bounds[2],previous[2]),std::max(bounds[3],previous[3])};
    }
    putfloat(body_data,21,bounds[0]);putfloat(body_data,17,bounds[1]);putfloat(body_data,13,bounds[2]);putfloat(body_data,9,bounds[3]);
    Blob shape_data(48);shape_data[0]=1;shape_data[1]=31;shape_data[2]=1;
    put32(shape_data,3,shape_id);put32(shape_data,7,body_id);put32(shape_data,27,body_id);
    std::reverse_copy(request.uuid.begin(),request.uuid.end(),shape_data.begin()+11);
    for(std::size_t i=0;i<3;++i) {put16(shape_data,31+2*i,local[i]);put16(shape_data,41+2*i,request.dimensions[i]);}
    put32(shape_data,37,request.color_abgr);
    const auto body_prefix=decode_rigid_body_prefix(body_data,body_data.size());
    const auto shape_prefix=decode_child_shape_prefix(shape_data,shape_data.size());
    // Allocate all cache nodes before changing SQLite; merging these nodes and
    // moving their vectors after COMMIT cannot allocate or leave half a cache.
    std::map<std::uint32_t,StoredBody> added_bodies;
    std::map<std::uint32_t,StoredShape> added_shapes;
    if(!existing)added_bodies.emplace(body_id,StoredBody{body_id,request.world_id,body_data,body_prefix});
    added_shapes.emplace(shape_id,StoredShape{shape_id,body_id,std::move(shape_data),shape_prefix});
    auto counts=impl_->counts;
    Transaction transaction(db);
    {
        auto s=prepare(db,existing?"UPDATE RigidBody SET data=?1 WHERE id=?2":"INSERT INTO RigidBody(data,id,worldId) VALUES(?1,?2,?3)");
        sqlite3_bind_blob64(s.get(),1,body_data.data(),body_data.size(),SQLITE_TRANSIENT);sqlite3_bind_int64(s.get(),2,body_id);
        if(!existing)sqlite3_bind_int(s.get(),3,request.world_id);
        if(step(db,s.get())!=SQLITE_DONE||sqlite3_changes(db)!=1)throw std::runtime_error("cannot-persist-created-body");
    }
    {
        const auto& bytes=added_shapes.at(shape_id).data;
        auto s=prepare(db,"INSERT INTO ChildShape(id,bodyId,data) VALUES(?1,?2,?3)");
        sqlite3_bind_int64(s.get(),1,shape_id);sqlite3_bind_int64(s.get(),2,body_id);sqlite3_bind_blob64(s.get(),3,bytes.data(),bytes.size(),SQLITE_TRANSIENT);
        if(step(db,s.get())!=SQLITE_DONE)throw std::runtime_error("cannot-persist-created-shape");
    }
    if(counts.contains("RigidBodyBounds")) {
        auto s=prepare(db,"INSERT OR REPLACE INTO RigidBodyBounds(id,minX,maxX,minY,maxY) VALUES(?1,?2,?3,?4,?5)");
        sqlite3_bind_int64(s.get(),1,body_id);
        for(std::size_t i=0;i<4;++i)sqlite3_bind_double(s.get(),int(i+2),bounds[i]);
        if(step(db,s.get())!=SQLITE_DONE)throw std::runtime_error("cannot-persist-created-body-bounds");
    }
    {
        auto s=prepare(db,"UPDATE Game SET uniqueIds=?1");
        sqlite3_bind_blob64(s.get(),1,counters.data(),counters.size(),SQLITE_TRANSIENT);
        if(step(db,s.get())!=SQLITE_DONE||sqlite3_changes(db)!=1)throw std::runtime_error("cannot-persist-unique-id-counters");
    }
    ++counts.at("ChildShape");if(!existing)++counts.at("RigidBody");
    for(auto& [table,n]:counts)if(table.starts_with("RigidBodyBounds")) {
        auto s=prepare(db,"SELECT count(*) FROM "+quote(table));
        if(step(db,s.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-created-bounds");
        n=std::uint64_t(sqlite3_column_int64(s.get(),0));
    }
    transaction.commit();
    if(existing) {auto& body=impl_->bodies.at(body_id);body.data=std::move(body_data);body.prefix=body_prefix;}
    impl_->bodies.merge(added_bodies);impl_->shapes.merge(added_shapes);impl_->counts.swap(counts);
    return {body_id,shape_id,!existing};
}
RemovedShapes WorldState::remove_shapes(const std::vector<std::uint32_t>& ids) {
    if(ids.empty())throw std::runtime_error("empty-removal");
    std::set<std::uint32_t> unique(ids.begin(),ids.end());
    if(unique.size()!=ids.size())throw std::runtime_error("duplicate-shape-removal");
    std::map<std::uint32_t,std::size_t> removing_per_body;
    for(const auto shape_id:unique) {
        const auto it=impl_->shapes.find(shape_id);
        if(it==impl_->shapes.end())throw std::runtime_error("unknown-shape");
        const auto geometry=decode_child_geometry(it->second.data);
        if(!geometry||geometry->kind!=SavedShapeKind::block)throw std::runtime_error("unsupported-removal-shape");
        for(const auto& [joint_id,joint]:impl_->joints) {
            (void)joint_id;
            if(joint.shape_a==shape_id||joint.shape_b==shape_id)throw std::runtime_error("shape-has-joint-reference");
        }
        for(const auto& [controller_id,controller]:impl_->controllers) {
            (void)controller_id;
            if(controller.host_id==shape_id)throw std::runtime_error("shape-has-controller-reference");
        }
        ++removing_per_body[it->second.body_id];
    }
    RemovedShapes result;result.shape_ids.assign(unique.begin(),unique.end());
    for(const auto& [body_id,count]:removing_per_body) {
        std::size_t body_shape_count=0;
        for(const auto& [shape_id,shape]:impl_->shapes)if(shape.body_id==body_id){(void)shape_id;++body_shape_count;}
        if(body_shape_count==count) {
            for(const auto& [controller_id,controller]:impl_->controllers) {
                (void)controller_id;
                if(controller.host_id==body_id)throw std::runtime_error("empty-body-has-controller-reference");
            }
            result.empty_body_ids.push_back(body_id);
        }
    }
    auto counts=impl_->counts;
    Transaction transaction(impl_->db.get());
    auto remove_shape=prepare(impl_->db.get(),"DELETE FROM ChildShape WHERE id=?1");
    for(const auto shape_id:result.shape_ids) {
        sqlite3_reset(remove_shape.get());sqlite3_clear_bindings(remove_shape.get());sqlite3_bind_int64(remove_shape.get(),1,shape_id);
        if(step(impl_->db.get(),remove_shape.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("cannot-delete-shape");
    }
    for(const auto body_id:result.empty_body_ids) {
        if(counts.contains("RigidBodyBounds")) {
            auto bounds=prepare(impl_->db.get(),"DELETE FROM RigidBodyBounds WHERE id=?1");sqlite3_bind_int64(bounds.get(),1,body_id);
            if(step(impl_->db.get(),bounds.get())!=SQLITE_DONE)throw std::runtime_error("cannot-delete-empty-body-bounds");
        }
        auto body=prepare(impl_->db.get(),"DELETE FROM RigidBody WHERE id=?1");sqlite3_bind_int64(body.get(),1,body_id);
        if(step(impl_->db.get(),body.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("cannot-delete-empty-body");
    }
    if(counts.contains("ChildShape"))counts.at("ChildShape")-=result.shape_ids.size();
    if(counts.contains("RigidBody"))counts.at("RigidBody")-=result.empty_body_ids.size();
    for(auto& [table,n]:counts)if(table.starts_with("RigidBodyBounds")) {
        auto count=prepare(impl_->db.get(),"SELECT count(*) FROM "+quote(table));
        if(step(impl_->db.get(),count.get())!=SQLITE_ROW)throw std::runtime_error("cannot-count-removed-bounds");
        n=std::uint64_t(sqlite3_column_int64(count.get(),0));
    }
    transaction.commit();
    for(const auto shape_id:result.shape_ids)impl_->shapes.erase(shape_id);
    for(const auto body_id:result.empty_body_ids)impl_->bodies.erase(body_id);
    impl_->counts.swap(counts);
    return result;
}
void WorldState::remove_joint(std::uint32_t id) {
    const auto found=impl_->joints.find(id);
    if(found==impl_->joints.end())throw std::runtime_error("unknown-joint");
    auto counts=impl_->counts;
    Transaction transaction(impl_->db.get());
    auto remove=prepare(impl_->db.get(),"DELETE FROM Joint WHERE id=?1");
    if(sqlite3_bind_int64(remove.get(),1,id)!=SQLITE_OK)fail(impl_->db.get(),"Binding joint ID for deletion");
    if(step(impl_->db.get(),remove.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)
        throw std::runtime_error("cannot-delete-joint");
    if(counts.contains("Joint"))--counts.at("Joint");
    transaction.commit();
    impl_->joints.erase(found);
    impl_->counts.swap(counts);
}
SplitShape WorldState::split_block_shape(std::uint32_t id,
    const std::array<std::int32_t,3>& selected_position,
    const std::array<std::int32_t,3>& selected_dimensions,
    std::uint32_t selected_color_abgr) {
    const auto found=impl_->shapes.find(id);
    if(found==impl_->shapes.end())throw std::runtime_error("unknown-shape");
    const auto geometry=decode_child_geometry(found->second.data);
    if(!geometry||geometry->kind!=SavedShapeKind::block||!geometry->grid_dimensions)throw std::runtime_error("split-requires-block-shape");
    std::array<std::int32_t,3> begin=geometry->grid_position,end{};
    std::array<std::int32_t,3> cut_begin=selected_position,cut_end{};
    for(std::size_t i=0;i<3;++i) {
        const auto extent=geometry->grid_dimensions->at(i);
        if(extent<=0||selected_dimensions[i]<=0||selected_dimensions[i]>INT16_MAX||selected_position[i]<INT16_MIN||selected_position[i]>INT16_MAX)
            throw std::runtime_error("split-geometry-out-of-range");
        end[i]=begin[i]+extent;cut_end[i]=cut_begin[i]+selected_dimensions[i];
        if(cut_begin[i]<begin[i]||cut_end[i]>end[i]||cut_end[i]<=cut_begin[i])throw std::runtime_error("split-selection-outside-shape");
    }
    using Box=std::pair<std::array<std::int32_t,3>,std::array<std::int32_t,3>>;
    std::vector<Box> fragments;
    auto append=[&](std::array<std::int32_t,3> lo,std::array<std::int32_t,3> hi) {
        if(lo[0]>=hi[0]||lo[1]>=hi[1]||lo[2]>=hi[2])return;
        std::array<std::int32_t,3> dimensions{};
        for(std::size_t i=0;i<3;++i)dimensions[i]=hi[i]-lo[i];
        fragments.emplace_back(lo,dimensions);
    };
    auto lo=begin,hi=end;
    hi[0]=cut_begin[0];append(lo,hi);
    lo=begin;hi=end;lo[0]=cut_end[0];append(lo,hi);
    lo=begin;hi=end;lo[0]=cut_begin[0];hi[0]=cut_end[0];
    auto xlo=lo,xhi=hi;
    hi[1]=cut_begin[1];append(lo,hi);
    lo=xlo;hi=xhi;lo[1]=cut_end[1];append(lo,hi);
    lo=xlo;hi=xhi;lo[1]=cut_begin[1];hi[1]=cut_end[1];
    auto ylo=lo,yhi=hi;
    hi[2]=cut_begin[2];append(lo,hi);
    lo=ylo;hi=yhi;lo[2]=cut_end[2];append(lo,hi);

    auto read32=[](const Blob& b,std::size_t at){return (std::uint32_t(b.at(at))<<24)|(std::uint32_t(b.at(at+1))<<16)|(std::uint32_t(b.at(at+2))<<8)|b.at(at+3);};
    auto put32=[](Blob& b,std::size_t at,std::uint32_t n){for(int i=0;i<4;++i)b.at(at+i)=std::uint8_t(n>>(24-8*i));};
    auto put16=[](Blob& b,std::size_t at,std::int32_t n){b.at(at)=std::uint8_t(std::uint32_t(n)>>8);b.at(at+1)=std::uint8_t(n);};
    auto write_geometry=[&](Blob& data,const std::array<std::int32_t,3>& position,const std::array<std::int32_t,3>& dimensions,std::uint32_t color) {
        for(std::size_t i=0;i<3;++i){if(position[i]<INT16_MIN||position[i]>INT16_MAX||dimensions[i]<=0||dimensions[i]>INT16_MAX)throw std::runtime_error("split-geometry-out-of-range");put16(data,31+2*i,position[i]);put16(data,41+2*i,dimensions[i]);}
        put32(data,37,color);
    };
    auto updated=found->second.data;
    write_geometry(updated,cut_begin,selected_dimensions,selected_color_abgr);
    if(fragments.empty()) {
        replace_shape_blob(id,std::move(updated));
        return {id,{}};
    }
    Blob counters;
    {auto query=prepare(impl_->db.get(),"SELECT uniqueIds FROM Game");if(step(impl_->db.get(),query.get())!=SQLITE_ROW)throw std::runtime_error("missing-unique-id-counters");counters=blob(query.get(),0);}
    if(counters.size()<16)throw std::runtime_error("invalid-unique-id-counters");
    const auto slots=read32(counters,0);
    if(slots<3||slots>17||counters.size()!=4ull*(slots+1)||slots<3)throw std::runtime_error("invalid-unique-id-counters");
    auto next=read32(counters,12);
    if(next==0||next==UINT32_MAX||(!impl_->shapes.empty()&&next<=impl_->shapes.rbegin()->first))throw std::runtime_error("shape-id-exhausted-or-collision");
    std::map<std::uint32_t,StoredShape> added;
    SplitShape result;result.selected_shape_id=id;
    for(const auto& fragment:fragments) {
        if(next==UINT32_MAX)throw std::runtime_error("shape-id-exhausted-or-collision");
        auto data=found->second.data;put32(data,3,next);write_geometry(data,fragment.first,fragment.second,geometry->color_abgr);
        const auto prefix=decode_child_shape_prefix(data,data.size());
        added.emplace(next,StoredShape{next,found->second.body_id,std::move(data),prefix});result.fragment_shape_ids.push_back(next);++next;
    }
    put32(counters,12,next);
    auto counts=impl_->counts;
    Transaction transaction(impl_->db.get());
    {auto update=prepare(impl_->db.get(),"UPDATE ChildShape SET data=?1 WHERE id=?2");sqlite3_bind_blob64(update.get(),1,updated.data(),updated.size(),SQLITE_TRANSIENT);sqlite3_bind_int64(update.get(),2,id);if(step(impl_->db.get(),update.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("cannot-update-split-shape");}
    for(const auto& [new_id,shape]:added) {auto insert=prepare(impl_->db.get(),"INSERT INTO ChildShape(id,bodyId,data) VALUES(?1,?2,?3)");sqlite3_bind_int64(insert.get(),1,new_id);sqlite3_bind_int64(insert.get(),2,shape.body_id);sqlite3_bind_blob64(insert.get(),3,shape.data.data(),shape.data.size(),SQLITE_TRANSIENT);if(step(impl_->db.get(),insert.get())!=SQLITE_DONE)throw std::runtime_error("cannot-insert-split-fragment");}
    {auto update=prepare(impl_->db.get(),"UPDATE Game SET uniqueIds=?1");sqlite3_bind_blob64(update.get(),1,counters.data(),counters.size(),SQLITE_TRANSIENT);if(step(impl_->db.get(),update.get())!=SQLITE_DONE||sqlite3_changes(impl_->db.get())!=1)throw std::runtime_error("cannot-update-split-counters");}
    if(counts.contains("ChildShape"))counts.at("ChildShape")+=added.size();
    transaction.commit();
    const auto selected_prefix=decode_child_shape_prefix(updated,updated.size());auto& selected=impl_->shapes.at(id);selected.data=std::move(updated);selected.prefix=selected_prefix;
    impl_->shapes.merge(added);impl_->counts.swap(counts);
    return result;
}
void WorldState::save_as(const std::filesystem::path& output) const {
    const auto absolute = std::filesystem::absolute(output);
    if (std::filesystem::exists(absolute)) throw std::runtime_error("Native save output must be a new file");
    if (!std::filesystem::is_directory(absolute.parent_path())) throw std::runtime_error("Native save output parent does not exist");
    TemporaryDirectory temporary(absolute.parent_path());
    const auto file = temporary.path / "world.db";
    {
        auto dest = open(utf8(file), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        execute(dest.get(), "PRAGMA synchronous=FULL");
        backup(impl_->db.get(), dest.get());
        execute(dest.get(), "PRAGMA journal_mode=DELETE");
        check(dest.get());
    }
    // Atomic publication without replacement on both NTFS and POSIX. If the
    // output filesystem cannot hard-link, fail without changing any target.
    std::filesystem::create_hard_link(file, absolute);
}
void WorldState::save_checkpoint(const std::filesystem::path& output) const {
    const auto absolute=std::filesystem::absolute(output);
    if(!std::filesystem::is_directory(absolute.parent_path()))throw std::runtime_error("Checkpoint parent directory does not exist");
    if(std::filesystem::exists(absolute)&&std::filesystem::equivalent(absolute,impl_->source))
        throw std::runtime_error("Refusing to replace the original world save");
    TemporaryDirectory temporary(absolute.parent_path());
    const auto file=temporary.path/"world.db";
    {
        auto dest=open(utf8(file),SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
        execute(dest.get(),"PRAGMA synchronous=FULL");
        backup(impl_->db.get(),dest.get());
        execute(dest.get(),"PRAGMA journal_mode=DELETE");
        check(dest.get());
    }
#ifdef _WIN32
    if(!MoveFileExW(file.c_str(),absolute.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot atomically publish native world checkpoint");
#else
    std::filesystem::rename(file,absolute);
#endif
}
std::string WorldState::summary_json() const {
    std::size_t decoded_bodies = 0, decoded_shapes = 0;
    for (const auto& [key, body] : impl_->bodies) { (void)key; if (body.prefix) ++decoded_bodies; }
    for (const auto& [key, shape] : impl_->shapes) { (void)key; if (shape.prefix) ++decoded_shapes; }
    std::ostringstream out;
    out << "{\"source\":" << json_quote(utf8(impl_->source)) << ",\"storage\":\"native-in-memory-sqlite\",\"source_read_only\":true,\"save_version\":" << impl_->metadata.save_version
        << ",\"flags\":" << impl_->metadata.flags << ",\"seed\":" << impl_->metadata.seed << ",\"game_tick\":" << impl_->metadata.tick
        << ",\"bodies\":" << impl_->bodies.size() << ",\"shapes\":" << impl_->shapes.size() << ",\"joints\":" << impl_->joints.size()
        << ",\"decoded_body_prefixes\":" << decoded_bodies << ",\"decoded_shape_prefixes\":" << decoded_shapes
        << ",\"unknown_blob_fields_preserved\":true,\"physics_simulated\":false,\"tables\":{";
    bool first = true;
    for (const auto& [name, count] : impl_->counts) { if (!first) out << ','; first = false; out << json_quote(name) << ':' << count; }
    return out.str() + "}}";
}
}
