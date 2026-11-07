#include "data/state.hpp"
#include "data/shape_geometry.hpp"
#include "data/terrain.hpp"
#include "net/world_sync.hpp"
#include <algorithm>
#include <sqlite3.h>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F&& f, const char* message) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("scrapds-state-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void sql(sqlite3* db, const char* query) { char* error = nullptr; if (sqlite3_exec(db, query, nullptr, nullptr, &error) != SQLITE_OK) { const std::string value = error ? error : "SQLite failure"; sqlite3_free(error); throw std::runtime_error(value); } }
void be32(scrapds::Blob& bytes, std::size_t at, std::uint32_t n) { for (int i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(n >> (24 - 8*i)); }
void number(scrapds::Blob& bytes, std::size_t at, float n) { be32(bytes, at, std::bit_cast<std::uint32_t>(n)); }
scrapds::Blob body() {
    scrapds::Blob bytes(60); bytes[1] = 1; bytes[2] = 1; be32(bytes, 3, 7); bytes[8] = 0;
    number(bytes, 9, 2); number(bytes, 13, -2); number(bytes, 17, 3); number(bytes, 21, -3); number(bytes, 27, 1);
    return bytes;
}
scrapds::Blob shape() { scrapds::Blob b(48); b[0]=1; b[1]=31; b[2]=1; be32(b,3,11); be32(b,7,7); be32(b,27,7); return b; }
scrapds::Blob harvestable() {
    scrapds::Blob b(72);b[0]=5;b[1]=0;b[2]=1;be32(b,3,13);b[7]=3;
    be32(b,8,std::bit_cast<std::uint32_t>(std::int32_t(-40)));
    be32(b,12,std::bit_cast<std::uint32_t>(std::int32_t(-38)));b[17]=1;b[19]=1;
    return b;
}
void insert_blob(sqlite3* db, const char* query, const scrapds::Blob& b) {
    sqlite3_stmt* stmt = nullptr;
    require(sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK, "fixture prepare failed");
    sqlite3_bind_blob64(stmt, 1, b.data(), b.size(), SQLITE_TRANSIENT);
    const auto result = sqlite3_step(stmt); sqlite3_finalize(stmt);
    require(result == SQLITE_DONE, "fixture insert failed");
}
void fixture(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    require(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK, "fixture open failed");
    try {
        sql(db, "CREATE TABLE Game(savegameversion INTEGER,flags INTEGER,seed INTEGER,gametick INTEGER,mods BLOB,uniqueIds BLOB); INSERT INTO Game VALUES(30,1,42,100,x'010203',x'010203040506');CREATE TABLE RigidBody(id INTEGER PRIMARY KEY,worldId INTEGER,data BLOB);CREATE TABLE ChildShape(id INTEGER PRIMARY KEY,bodyId INTEGER,data BLOB);CREATE TABLE Joint(id INTEGER PRIMARY KEY,childShapeIdA INTEGER,childShapeIdB INTEGER,data BLOB);CREATE TABLE Harvestable(id INTEGER PRIMARY KEY,worldId INTEGER,x INTEGER,y INTEGER,size INTEGER,data BLOB);CREATE VIRTUAL TABLE RigidBodyBounds USING rtree(id,minX,maxX,minY,maxY);INSERT INTO RigidBodyBounds VALUES(7,-3,3,-2,2);CREATE TABLE ScriptData(uid INTEGER,key INTEGER,worldId INTEGER,flags INTEGER,data BLOB);INSERT INTO ScriptData VALUES(999,5,0,1,zeroblob(1048576));");
        insert_blob(db, "INSERT INTO RigidBody VALUES(7,0,?1)", body());
        insert_blob(db, "INSERT INTO ChildShape VALUES(11,7,?1)", shape());
        insert_blob(db, "INSERT INTO Harvestable VALUES(13,1,-38,-40,3,?1)", harvestable());
        sql(db,"INSERT INTO Joint VALUES(3,11,-1,x'0102')");
        scrapds::net::BlobDataEntry terrain;terrain.uid={0x61,0xaa,0x13,0xd7,0xe7,0x15,0x41,0x53,0xa2,0x69,0x4d,0x33,0x8c,0x0c,0x5b,0xd4};terrain.key={1,0,0,0};terrain.world_id=1;terrain.flags=3;
        terrain.payload={0x80,'L','U','A',0,0,0,1,5};const auto saved=scrapds::net::make_saved_blob_data(terrain);
        sqlite3_stmt* generator=nullptr;require(sqlite3_prepare_v2(db,"INSERT INTO ScriptData VALUES(?1,?2,1,3,?3)",-1,&generator,nullptr)==SQLITE_OK,"terrain fixture prepare failed");
        sqlite3_bind_blob(generator,1,terrain.uid.data(),16,SQLITE_TRANSIENT);sqlite3_bind_blob(generator,2,terrain.key.data(),4,SQLITE_TRANSIENT);sqlite3_bind_blob64(generator,3,saved.data(),saved.size(),SQLITE_TRANSIENT);
        const auto inserted=sqlite3_step(generator);sqlite3_finalize(generator);require(inserted==SQLITE_DONE,"terrain fixture insert failed");
        sqlite3_close(db);
    } catch (...) { sqlite3_close(db); throw; }
}
std::string read(const std::filesystem::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()}; }
}
int main(int argc,char** argv) {
    try {
        Temp temp;
        const auto source = temp.path / "source.db", dest = temp.path / "saved.db";
        fixture(source);
        const auto original = read(source);
        auto state = scrapds::WorldState::load(source);
        require(state.metadata().tick == 100 && state.bodies().size() == 1 && state.shapes().size() == 1, "load lost metadata or entities");
        require(state.bodies().at(7).prefix.has_value(), "known body prefix not decoded");
        require(state.harvestables().size()==1&&state.harvestables().at(13).identity_verified&&state.harvestables().at(13).x==-38&&state.harvestables().at(13).y==-40,"saved Harvestable identity not decoded");
        require(state.joints().at(3).shape_b == UINT32_MAX, "world-anchor joint sentinel lost");
        require(state.table_counts().at("ScriptData") == 2, "unknown tables not retained");
        require(state.terrain_generator_data(1)==scrapds::Blob({'L','U','A',0,0,0,1,5})&&!state.terrain_generator_data(2),"terrain generator state extraction failed");
        state.advance_ticks(17);
        auto changed = body(); number(changed, 17, 9); number(changed, 43, 1.25f);
        state.replace_body_blob(7, changed);
        auto colored = shape(); be32(colored, 37, 0xFF123456);
        state.replace_shape_blob(11, colored);
        auto invalid = changed; be32(invalid, 3, 8);
        rejects([&]{ state.replace_body_blob(7, invalid); }, "identity-changing mutation accepted");
        require(state.bodies().at(7).data == changed, "failed mutation changed state");
        rejects([&]{ state.advance_ticks(UINT64_MAX); }, "tick overflow accepted");
        require(state.metadata().tick == 117, "failed tick advance changed state");
        require(!state.atomic([&]{state.remove_harvestable(13);return false;})&&state.harvestables().contains(13),"failed compound transaction did not restore Harvestable cache");
        state.remove_harvestable(13);
        require(!state.harvestables().contains(13)&&state.table_counts().at("Harvestable")==0,"Harvestable removal did not update native save state");
        state.save_as(dest);
        rejects([&]{ state.save_as(dest); }, "existing save overwritten");
        rejects([&]{ state.save_as(source); }, "source save overwritten");
        auto restored = scrapds::WorldState::load(dest);
        require(restored.metadata().tick == 117 && restored.bodies().at(7).data == changed && restored.shapes().at(11).data == colored, "semantic save roundtrip failed");
        require(restored.table_counts() == state.table_counts(), "save roundtrip lost tables");
        require(restored.harvestables().empty(),"removed Harvestable survived save/reload");
        sqlite3* db = nullptr; require(sqlite3_open(dest.string().c_str(), &db) == SQLITE_OK, "saved world open failed");
        sqlite3_stmt* stmt = nullptr;
        require(sqlite3_prepare_v2(db,"SELECT maxX FROM RigidBodyBounds WHERE id=7",-1,&stmt,nullptr)==SQLITE_OK,"rtree query failed");
        require(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_double(stmt,0)==9,"rtree did not follow body mutation"); sqlite3_finalize(stmt);
        require(sqlite3_prepare_v2(db,"SELECT length(data),hex(substr(data,1,16)) FROM ScriptData",-1,&stmt,nullptr)==SQLITE_OK,"opaque query failed");
        require(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==1048576,"opaque payload lost"); sqlite3_finalize(stmt);sqlite3_close(db);
        require(read(source) == original, "original save changed");
        // A later SQLite error in a physics batch must roll back earlier body
        // and RTree writes, including the in-memory cache.
        const auto batch_source=temp.path/"batch.db";fixture(batch_source);
        require(sqlite3_open(batch_source.string().c_str(),&db)==SQLITE_OK,"batch fixture open failed");
        auto second=body();be32(second,3,8);
        insert_blob(db,"INSERT INTO RigidBody VALUES(8,0,?1)",second);
        sql(db,"INSERT INTO RigidBodyBounds VALUES(8,-3,3,-2,2); CREATE TRIGGER reject_second BEFORE UPDATE ON RigidBody WHEN NEW.id=8 BEGIN SELECT RAISE(ABORT,'injected failure'); END");sqlite3_close(db);
        auto batch=scrapds::WorldState::load(batch_source);
        auto first_update=body();number(first_update,17,12);
        auto second_update=second;number(second_update,17,13);
        rejects([&]{batch.replace_body_blobs({{7,first_update},{8,second_update}});},"failed body batch accepted");
        require(batch.bodies().at(7).data==body()&&batch.bodies().at(8).data==second,"failed body batch changed cache");
        const auto batch_saved=temp.path/"batch-saved.db";batch.save_as(batch_saved);
        auto batch_restored=scrapds::WorldState::load(batch_saved);
        require(batch_restored.bodies().at(7).data==body()&&batch_restored.bodies().at(8).data==second,"failed body batch changed saved bodies");
        require(sqlite3_open(batch_saved.string().c_str(),&db)==SQLITE_OK,"batch saved open failed");
        require(sqlite3_prepare_v2(db,"SELECT maxX FROM RigidBodyBounds WHERE id=7",-1,&stmt,nullptr)==SQLITE_OK,"batch bounds query failed");
        require(sqlite3_step(stmt)==SQLITE_ROW&&sqlite3_column_double(stmt,0)==3,"failed body batch changed bounds");sqlite3_finalize(stmt);sqlite3_close(db);
        state.advance_ticks(5);
        state.save_checkpoint(dest);
        auto checkpoint= scrapds::WorldState::load(dest);
        require(checkpoint.metadata().tick==122,"atomic checkpoint did not replace the previous snapshot");
        rejects([&]{state.save_checkpoint(source);},"checkpoint overwrote original source save");
        require(read(source)==original,"checkpoint refusal changed source save");
        const auto bad = temp.path / "bad.db"; fixture(bad);
        require(sqlite3_open(bad.string().c_str(), &db) == SQLITE_OK, "bad fixture open failed");sql(db,"UPDATE ChildShape SET bodyId=99");sqlite3_close(db);
        rejects([&]{ (void)scrapds::WorldState::load(bad); }, "orphan shape accepted");
        require(sqlite3_open(bad.string().c_str(), &db) == SQLITE_OK,"unknown fixture open failed");sql(db,"UPDATE ChildShape SET bodyId=7,data=x'0102'");sqlite3_close(db);
        auto unknown = scrapds::WorldState::load(bad);
        require(!unknown.shapes().at(11).prefix && unknown.shapes().at(11).data == scrapds::Blob({1,2}),"unknown variant not preserved");
        // An INSERT failure must roll back the new body, bounds, and both
        // counters as one transaction, including the in-memory caches.
        const auto atomic_source=temp.path/"atomic-source.db";fixture(atomic_source);
        scrapds::Blob counters(72);be32(counters,0,17);
        for(std::size_t i=1;i<=17;++i)be32(counters,4*i,1000+std::uint32_t(i));
        be32(counters,4,100);be32(counters,12,200);
        require(sqlite3_open(atomic_source.string().c_str(),&db)==SQLITE_OK,"atomic fixture open");
        insert_blob(db,"UPDATE Game SET uniqueIds=?1",counters);
        sql(db,"CREATE TRIGGER reject_new_shape BEFORE INSERT ON ChildShape WHEN NEW.id>=200 BEGIN SELECT RAISE(ABORT,'injected insertion failure'); END");sqlite3_close(db);
        const auto atomic_original=read(atomic_source);
        auto atomic=scrapds::WorldState::load(atomic_source);
        scrapds::BlockCreation creation;creation.cell_position={1000,1000,0};creation.dimensions={1,1,1};creation.color_abgr=0xff123456;
        const auto initial_counts=atomic.table_counts();
        rejects([&]{atomic.create_block(creation);},"injected SQLite failure accepted");
        require(atomic.bodies().size()==1&&atomic.shapes().size()==1&&atomic.table_counts()==initial_counts,"failed SQL insert changed caches");
        const auto atomic_saved=temp.path/"atomic-saved.db";atomic.save_as(atomic_saved);
        auto saved_counters=[&](const std::filesystem::path& file) {
            sqlite3* database=nullptr;require(sqlite3_open(file.string().c_str(),&database)==SQLITE_OK,"counter test open");
            sqlite3_stmt* query=nullptr;require(sqlite3_prepare_v2(database,"SELECT uniqueIds FROM Game",-1,&query,nullptr)==SQLITE_OK,"counter test prepare");
            require(sqlite3_step(query)==SQLITE_ROW,"counter test row");
            const auto* raw=static_cast<const std::uint8_t*>(sqlite3_column_blob(query,0));
            const auto n=sqlite3_column_bytes(query,0);scrapds::Blob value(raw,raw+n);sqlite3_finalize(query);sqlite3_close(database);return value;
        };
        require(saved_counters(atomic_saved)==counters,"failed SQL insert consumed entity IDs");
        require(read(atomic_source)==atomic_original,"failed SQL insert changed source");
        const auto exhausted_source=temp.path/"exhausted-source.db";fixture(exhausted_source);
        auto exhausted_counters=counters;be32(exhausted_counters,4,UINT32_MAX);
        require(sqlite3_open(exhausted_source.string().c_str(),&db)==SQLITE_OK,"exhaustion fixture open");insert_blob(db,"UPDATE Game SET uniqueIds=?1",exhausted_counters);sqlite3_close(db);
        auto exhausted=scrapds::WorldState::load(exhausted_source);
        rejects([&]{exhausted.create_block(creation);},"body counter wrapped");
        const auto exhausted_saved=temp.path/"exhausted-saved.db";exhausted.save_as(exhausted_saved);
        require(saved_counters(exhausted_saved)==exhausted_counters&&exhausted.bodies().size()==1,"ID exhaustion was not atomic");
        // Signed remainder matches the native constructor even below zero.
        const auto negative_source=temp.path/"negative-source.db";fixture(negative_source);
        require(sqlite3_open(negative_source.string().c_str(),&db)==SQLITE_OK,"negative fixture open");insert_blob(db,"UPDATE Game SET uniqueIds=?1",counters);sqlite3_close(db);
        auto negative=scrapds::WorldState::load(negative_source);creation.cell_position={-17,-1,0};
        const auto created=negative.create_block(creation);
        require(scrapds::decode_rigid_body_pose(negative.bodies().at(created.body_id).data)->position==std::array<float,3>{-4,0,0},"negative body origin used floor division");
        require(scrapds::decode_child_geometry(negative.shapes().at(created.shape_id).data)->grid_position==std::array<std::int32_t,3>{-1,-1,0},"negative child position lost signed remainder");
        creation.world_id=1;creation.cell_position={0,0,0};
        const auto other_world=negative.create_block(creation);
        const auto other_pose=scrapds::decode_rigid_body_pose(negative.bodies().at(other_world.body_id).data);
        require(other_pose->world_id==1&&other_pose->repeated_world_id==1&&negative.bodies().at(other_world.body_id).data.at(55)==0,"repeated world ID was confused with flags");
        if(argc>=2){auto actual=scrapds::WorldState::load(argv[1]);const auto terrain=actual.terrain_generator_data(1);require(terrain&&terrain->size()>1000,"real survival terrain generator state missing");const auto cells=scrapds::decode_overworld_cells(*terrain);require(cells.size()==16128,"real survival tile grid mismatch");std::cout<<"survival terrain state: "<<terrain->size()<<" bytes, "<<cells.size()<<" cells\n";
            if(argc>=3){const auto catalog=scrapds::index_game_terrain_tiles(argv[2]);std::size_t matched=0;for(const auto& cell:cells)if(catalog.contains(cell.tile_uuid))++matched;require(matched>10000,"real survival tile UUID mapping mismatch");const auto& cell=cells.front();(void)scrapds::read_overworld_tile_voxels(cell,catalog);const auto heights=scrapds::read_terrain_tile_heights(catalog.at(cell.tile_uuid),cell.tile_x,cell.tile_y);require(heights.side==33&&heights.samples.size()==1089,"real survival tile heights missing");
                const auto generated=scrapds::decode_overworld_terrain(*terrain);const auto spawn=std::find_if(generated.cells.begin(),generated.cells.end(),[](const auto& c){return c.x==0&&c.y==0;});require(spawn!=generated.cells.end(),"survival origin cell missing");const auto surface=scrapds::mesh_saved_overworld_cell(generated,catalog,*spawn);require(surface.size()==6144,"survival generated surface missing");
                const auto adjacent=std::find_if(generated.cells.begin(),generated.cells.end(),[](const auto& c){return c.x==1&&c.y==0;});require(adjacent!=generated.cells.end(),"survival adjacent cell missing");const auto other=scrapds::mesh_saved_overworld_cell(generated,catalog,*adjacent);
                std::map<int,float> border_a,border_b;for(const auto& p:surface)if(p.x==64)border_a[int(p.y)]=p.z;for(const auto& p:other)if(p.x==64)border_b[int(p.y)]=p.z;
                require(border_a.size()==33&&border_b.size()==33,"survival boundary sample count mismatch");for(const auto& [y,z]:border_a)require(border_b.contains(y)&&std::abs(z-border_b.at(y))<0.0001,"survival heightfield seam mismatch");
                std::cout<<"matched "<<matched<<" tile cells, assembled "<<surface.size()/3<<" triangles and verified 33 boundary samples\n";}}
        std::cout << "native state checks passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
