#include "net/world_sync.hpp"
#include "data/state.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace scrapds::net;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F&& operation, const char* message) { try { operation(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
void be32(Bytes& data, std::size_t offset, std::uint32_t value) { for (int i = 0; i < 4; ++i) data.at(offset + i) = std::uint8_t(value >> (24 - i * 8)); }
void be16(Bytes& data, std::size_t offset, std::uint16_t value) { data.at(offset)=std::uint8_t(value>>8);data.at(offset+1)=std::uint8_t(value); }
Bytes read(const std::filesystem::path& path) { std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot open fixture"); return {std::istreambuf_iterator<char>(in), {}}; }
void sql(sqlite3* db, const char* statement) { char* message = nullptr; if (sqlite3_exec(db, statement, nullptr, nullptr, &message) != SQLITE_OK) { std::string error = message ? message : "SQLite error"; sqlite3_free(message); throw std::runtime_error(error); } }
void insert(sqlite3* db, const char* query, const Bytes& bytes) {
    sqlite3_stmt* s = nullptr;
    require(sqlite3_prepare_v2(db, query, -1, &s, nullptr) == SQLITE_OK, "fixture prepare failed");
    sqlite3_bind_blob64(s, 1, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
    auto result = sqlite3_step(s); sqlite3_finalize(s);
    require(result == SQLITE_DONE, "fixture insert failed");
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("scrapds-world-sync-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void builder_checks() {
    Temp temp;
    Bytes body(60); body[1] = 1; body[2] = 1; be32(body, 3, 7); be32(body, 27, std::bit_cast<std::uint32_t>(1.0f));
    std::fill(body.begin() + 56, body.end(), 255);
    Bytes shape(48); shape[0] = 1; shape[1] = 31; shape[2] = 1; be32(shape, 3, 11); be32(shape, 7, 7); be32(shape, 27, 7);
    Bytes harvestable(72); harvestable[0]=5;harvestable[2]=1;be32(harvestable,3,21);harvestable[7]=1;be32(harvestable,8,34);be32(harvestable,12,static_cast<std::uint32_t>(-12));be16(harvestable,16,2);
    be16(harvestable,18,7);for(std::size_t i=0;i<16;++i)harvestable[20+i]=static_cast<std::uint8_t>(0xA0+i);
    be32(harvestable,36,std::bit_cast<std::uint32_t>(0.25f));be32(harvestable,40,std::bit_cast<std::uint32_t>(0.5f));be32(harvestable,44,std::bit_cast<std::uint32_t>(0.75f));
    std::fill(harvestable.begin()+48,harvestable.begin()+68,0x5a);be32(harvestable,68,0xFF123456);
    sqlite3* db = nullptr;
    require(sqlite3_open((temp.path / "source.db").string().c_str(), &db) == SQLITE_OK, "fixture open failed");
    sql(db, "CREATE TABLE Game(savegameversion INTEGER,flags INTEGER,seed INTEGER,gametick INTEGER); INSERT INTO Game VALUES(30,1,42,9); CREATE TABLE RigidBody(id INTEGER PRIMARY KEY,worldId INTEGER,data BLOB); CREATE TABLE ChildShape(id INTEGER PRIMARY KEY,bodyId INTEGER,data BLOB); CREATE TABLE Harvestable(id INTEGER PRIMARY KEY,worldId INTEGER,x INTEGER,y INTEGER,size INTEGER,data BLOB);");
    insert(db, "INSERT INTO RigidBody VALUES(7,0,?1)", body);
    insert(db, "INSERT INTO ChildShape VALUES(11,7,?1)", shape);
    insert(db, "INSERT INTO Harvestable VALUES(21,2,-12,34,1,?1)", harvestable);
    sqlite3_close(db);
    auto world = scrapds::WorldState::load(temp.path / "source.db");
    NetObjectPacket captured;
    captured.tick = 9;
    Bytes captured_harvestable_state(41,0);captured_harvestable_state[0]=0;captured_harvestable_state[1]=7;captured_harvestable_state[2]=1;captured_harvestable_state[3]=35;
    captured.records = {make_body_creation(body), {1, 1, 31, 11, {}, 0, 0, false}, make_child_shape_state(shape, 214), {3, 0, {}, 7, {0,255,255,255,255}, 0, 0, false}, {1,5,0,21,{},0,0,false}, {3,5,{},21,captured_harvestable_state,0,0,false}, {1, 4, 0, 65, {10,0,0,0}, 0, 0, false}};
    auto result = rebuild_initial_world(captured, world, 10);
    const auto catalog = infer_initial_asset_catalog(captured, world);
    require(result.native_harvestable_creations==1&&result.native_harvestable_states==1,"native Harvestable reconstruction counts incorrect");
    const auto& saved_harvestable=world.harvestables().at(21);
    const auto state=make_harvestable_state(saved_harvestable,0x0123);
    require(state.type==5&&state.id==21&&state.payload.size()==41&&state.payload[0]==0&&state.payload[1]==7&&state.payload[2]==1&&state.payload[3]==0x23&&state.payload[4]==0&&std::equal(state.payload.begin()+5,state.payload.end(),harvestable.begin()+36),"Harvestable network state layout differs");
    const auto initial_wire=parse_net_object_packet(make_net_object_packet(result.packet));
    const auto wire_state=std::find_if(initial_wire.records.begin(),initial_wire.records.end(),[](const auto& record){return record.type==5&&record.operation==3;});
    require(wire_state!=initial_wire.records.end()&&wire_state->payload==state.payload,"Harvestable state did not survive NetObj packet encoding");
    rejects([&]{auto invalid=saved_harvestable;invalid.identity_verified=false;(void)make_harvestable_state(invalid,1);},"unverified Harvestable state was serialized");
    InitialAssetCatalog saved=catalog;InitialAssetCatalog::SavedUuid removed_asset{};removed_asset[0]=42;saved.shapes.emplace(removed_asset,77);
    const std::string bootstrap_hash(64,'a');
    const auto catalog_bytes=serialize_asset_catalog(saved,bootstrap_hash);
    auto restored=parse_asset_catalog(catalog_bytes,bootstrap_hash);merge_asset_catalog(restored,catalog);
    require(restored.shapes==saved.shapes&&restored.joints==saved.joints&&restored.harvestables==saved.harvestables,"catalog snapshot lost asset mapping");
    auto legacy_catalog_bytes=catalog_bytes;legacy_catalog_bytes.resize(legacy_catalog_bytes.size()-22);legacy_catalog_bytes[6]='1';
    const auto legacy_catalog=parse_asset_catalog(legacy_catalog_bytes,bootstrap_hash);
    require(legacy_catalog.shapes==saved.shapes&&legacy_catalog.joints==saved.joints&&legacy_catalog.harvestables.empty(),"legacy catalog compatibility failed");
    rejects([&]{(void)parse_asset_catalog(catalog_bytes,std::string(64,'b'));},"catalog from another bootstrap was accepted");
    auto truncated_catalog=catalog_bytes;truncated_catalog.pop_back();
    rejects([&]{(void)parse_asset_catalog(truncated_catalog,bootstrap_hash);},"truncated catalog snapshot was accepted");
    auto collision=catalog;collision.shapes.emplace(removed_asset,214);
    rejects([&]{auto target=catalog;merge_asset_catalog(target,collision);},"conflicting catalog index was accepted");
    auto stale=captured;
    stale.records.push_back({1,1,31,999,{}});
    auto stale_state=make_child_shape_state(shape,214);stale_state.id=999;stale.records.push_back(stale_state);
    const auto resumed_catalog=infer_initial_asset_catalog(stale,world);
    const auto resumed=rebuild_live_initial_world(stale,world,10,resumed_catalog);
    require(std::none_of(resumed.packet.records.begin(),resumed.packet.records.end(),[](const auto& record){return record.type==1&&record.id==999;}),"removed template shape returned after restart");
    require(result.packet.tick == 10 && result.native_body_creations == 1 && result.native_shape_states == 1 && result.native_static_body_states == 1 && result.preserved_records == 2, "native reconstruction counts incorrect");
    require(make_net_object_packet(result.packet) != make_net_object_packet(captured), "tick change missing");
    auto colored = shape; be32(colored, 37, 0xFF123456); world.replace_shape_blob(11, colored);
    auto changed = rebuild_initial_world(captured, world, 9, catalog);
    require(changed.packet.records[2].payload != captured.records[2].payload, "native shape mutation not serialized");
    require(changed.packet.records.back().payload == captured.records.back().payload, "unsupported bootstrap record changed");
    auto missing = captured; missing.records.erase(missing.records.begin() + 1);
    rejects([&] { (void)rebuild_initial_world(missing, world, 9); }, "unrepresented shape silently omitted");
    auto conflict = captured; auto duplicate = captured.records[2]; duplicate.payload[1] ^= 1; conflict.records.push_back(duplicate);
    rejects([&] { (void)rebuild_initial_world(conflict, world, 9); }, "conflicting asset catalog accepted");
    auto changed_uuid = colored; changed_uuid[11] = 1; world.replace_shape_blob(11, changed_uuid);
    rejects([&] { (void)rebuild_initial_world(captured, world, 9, catalog); }, "unmapped asset UUID silently reuses old index");
    auto new_body = body; be32(new_body, 3, 8);
    auto new_shape = shape; be32(new_shape, 3, 12); be32(new_shape, 7, 8); be32(new_shape, 27, 8);
    auto new_harvestable=harvestable;be32(new_harvestable,3,22);be32(new_harvestable,12,static_cast<std::uint32_t>(-13));
    require(sqlite3_open((temp.path / "source.db").string().c_str(), &db) == SQLITE_OK, "extended fixture open failed");
    insert(db, "INSERT INTO RigidBody VALUES(8,0,?1)", new_body);
    insert(db, "INSERT INTO ChildShape VALUES(12,8,?1)", new_shape);
    insert(db, "INSERT INTO Harvestable VALUES(22,2,-13,34,1,?1)", new_harvestable);
    sqlite3_close(db);
    auto extended = scrapds::WorldState::load(temp.path / "source.db");
    rejects([&] { (void)rebuild_initial_world(captured, extended, 9, catalog); }, "strict reconstruction accepted extra entities");
    const auto live = rebuild_live_initial_world(captured, extended, 9, catalog);
    require(live.native_body_creations == 2 && live.native_shape_states == 2 && live.native_static_body_states == 2&&live.native_harvestable_creations==2&&live.native_harvestable_states==2, "live initial synchronization omitted added entities");
    const auto decoded_live = parse_net_object_packet(make_net_object_packet(live.packet));
    const auto new_harvestable_create=std::find_if(decoded_live.records.begin(),decoded_live.records.end(),[](const auto& record){return record.type==5&&record.operation==1&&record.id==22;});
    const auto new_harvestable_state=std::find_if(decoded_live.records.begin(),decoded_live.records.end(),[](const auto& record){return record.type==5&&record.operation==3&&record.id==22;});
    require(new_harvestable_create!=decoded_live.records.end()&&new_harvestable_state!=decoded_live.records.end()&&new_harvestable_create<new_harvestable_state,"new Harvestable is missing or its state precedes create");
    std::vector<std::pair<unsigned,unsigned>> order;
    for (const auto& record : decoded_live.records) if (record.type <= 1) order.emplace_back(record.operation, record.type);
    require(order == std::vector<std::pair<unsigned,unsigned>>{{1,0},{1,0},{1,1},{1,1},{3,1},{3,1},{3,0},{3,0}}, "new entity dependency ordering is wrong");
    auto unmapped = new_shape; unmapped[11] = 1; extended.replace_shape_blob(12, unmapped);
    rejects([&] { (void)rebuild_live_initial_world(captured, extended, 9, catalog); }, "live initial synchronization accepted unknown new asset");
    extended.replace_shape_blob(12, new_shape);
    auto invalid_harvestable=extended.harvestables().at(22);invalid_harvestable.data[20]^=1;
    rejects([&]{(void)make_harvestable_state(invalid_harvestable,catalog.harvestables.begin()->second);},"Harvestable serializer accepted mismatched asset UUID");
    auto dynamic = new_body; dynamic.resize(80); dynamic[1] = 2; extended.replace_body_blob(8, dynamic);
    rejects([&] { (void)rebuild_live_initial_world(captured, extended, 9, catalog); }, "new dynamic body got fabricated runtime state");
}
}
int main(int argc, char** argv) {
    try {
        using namespace scrapds::net;
        const Bytes golden{21,0,0,0,9, 0,12, 0x24,0,0,0,0,65,10,0,0,0, 0x83,0xDF,66};
        const auto packet = parse_net_object_packet(golden);
        require(packet.tick == 9 && packet.records.size() == 2, "golden record framing failed");
        require(packet.records[0].operation == 1 && packet.records[0].type == 4 && packet.records[0].id == 65 && packet.records[0].payload == Bytes({10,0,0,0}), "golden header decode failed");
        require(packet.records[1].id == 66 && packet.records[1].used_byte_delta && packet.records[1].wire_size == 3, "byte delta decode failed");
        require(make_net_object_packet(packet) == golden, "golden native delta encoding differs");
        const auto tick_only=make_net_object_packet({22,0x12345678,{}});
        require(tick_only==Bytes({22,0x12,0x34,0x56,0x78})&&parse_net_object_packet(tick_only).records.empty(),"empty ticked NetObj update differs from client framing");
        const auto expanded = make_net_object_packet(packet, false);
        require(expanded.size() == 29 && parse_net_object_packet(expanded).records[1].payload == packet.records[1].payload, "expanded record encoding differs");
        std::vector<Bytes> children{golden, Bytes{25,0,0,0,9}};
        auto aggregate = make_world_aggregate(children);
        require(split_world_aggregate(aggregate) == children, "aggregate roundtrip failed");
        auto truncated = aggregate; truncated.pop_back();
        rejects([&] { (void)split_world_aggregate(truncated); }, "truncated aggregate accepted");
        rejects([&] { (void)split_world_aggregate(Bytes{29,0,0,0,0}); }, "empty aggregate child accepted");
        rejects([&] { (void)parse_net_object_packet(Bytes{21,0,0,0,0,128}); }, "delta without base accepted");
        rejects([&] { (void)parse_net_object_packet(Bytes{21,0,0,0,0,0,6,0,0,0,0}); }, "short record accepted");
        rejects([&] { (void)parse_net_object_packet(Bytes{21,0,0,0,0,0,7,0,0,0,0,0}); }, "invalid operation accepted");
        auto cut = golden; cut.pop_back();
        rejects([&] { (void)parse_net_object_packet(cut); }, "truncated delta accepted");
        NetObjectPacket large; large.records = {{3, 0, {}, 7, Bytes(80, 0), 0, 0, false}, {3, 0, {}, 8, Bytes(80, 0), 0, 0, false}};
        const auto large_decoded = parse_net_object_packet(make_net_object_packet(large));
        require(!large_decoded.records[1].used_byte_delta, "oversized previous record used as delta base");
        BlobDataPacket blob_packet;
        blob_packet.tick = 123;
        BlobDataEntry entry; entry.uid[0] = 7; entry.key = {0,1,2,3}; entry.world_id = 65534; entry.flags = 13; entry.payload = {4,5,6,7};
        blob_packet.entries = {entry, entry};
        const auto blob_bytes = make_blob_data_packet(blob_packet);
        const auto blobs = parse_blob_data_packet(blob_bytes);
        require(blobs.tick == 123 && blobs.entries.size() == 2 && blobs.entries[0].world_id == 65534 && blobs.entries[1].payload == entry.payload, "BlobData framing failed");
        require(make_blob_data_packet(blobs) == blob_bytes, "BlobData roundtrip differs");
        require(make_saved_blob_data(parse_saved_blob_data(make_saved_blob_data(entry))) == make_saved_blob_data(entry), "saved BlobData entry roundtrip differs");
        auto cut_blob = blob_bytes; cut_blob.pop_back();
        rejects([&] { (void)parse_blob_data_packet(cut_blob); }, "truncated BlobData accepted");
        entry.key.resize(65536);
        rejects([&] { (void)make_saved_blob_data(entry); }, "oversized BlobData key accepted");
        builder_checks();
        if (argc > 1) {
            const auto source = read(argv[1]);
            const auto components = split_world_aggregate(source);
            const auto captured = parse_net_object_packet(components.at(0));
            require(make_net_object_packet(captured) == components.at(0), "real packet roundtrip differs");
            for (const auto& component : components) if (component[0] == 25) require(make_blob_data_packet(parse_blob_data_packet(component)) == component, "real BlobData exact roundtrip differs");
            std::cout << "real NetObj records=" << captured.records.size() << " exact wire roundtrip=yes\n";
            if (argc > 2) {
                auto world = scrapds::WorldState::load(argv[2]);
                auto native = rebuild_initial_world(captured, world, captured.tick);
                const auto rebuilt = parse_net_object_packet(make_net_object_packet(native.packet));
                require(rebuilt.records.size() == captured.records.size(), "real native reconstruction lost records");
                std::cout << "native bodies=" << native.native_body_creations << " shape states=" << native.native_shape_states << " joint states=" << native.native_joint_states << " static body states=" << native.native_static_body_states << " preserved dynamic body states=" << native.preserved_dynamic_body_states << "\n";
            }
        }
        std::cout << "world synchronization checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
