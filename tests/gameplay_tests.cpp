#include "game/commands.hpp"
#include "game/replication.hpp"
#include "game/player_spawn.hpp"
#include "data/shape_geometry.hpp"
#include "net/world_sync.hpp"
#include "net/codec.hpp"
#include "net/delivery.hpp"
#include <sqlite3.h>
#include <bit>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void word(scrapds::Blob& b,std::size_t at,std::uint32_t n){for(int i=0;i<4;++i)b.at(at+i)=std::uint8_t(n>>(24-8*i));}
void scalar(scrapds::Blob& b,std::size_t at,float n){word(b,at,std::bit_cast<std::uint32_t>(n));}
void sql(sqlite3* db,const char* q){char* e=nullptr;if(sqlite3_exec(db,q,nullptr,nullptr,&e)!=SQLITE_OK){std::string m=e?e:"sqlite";sqlite3_free(e);throw std::runtime_error(m);}}
void insert(sqlite3* db,const char* q,const scrapds::Blob& b){sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db,q,-1,&s,nullptr)==SQLITE_OK,"fixture prepare");sqlite3_bind_blob64(s,1,b.data(),b.size(),SQLITE_TRANSIENT);const int code=sqlite3_step(s);sqlite3_finalize(s);require(code==SQLITE_DONE,"fixture insert");}
struct Temp { std::filesystem::path p=std::filesystem::temp_directory_path()/("scrapds-gameplay-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Temp(){std::filesystem::create_directory(p);}~Temp(){std::error_code ec;std::filesystem::remove_all(p,ec);}};
}
int main(){try{
    std::set<int> peers{1,2,3},closing{3},desynchronized;std::vector<int> attempted;
    scrapds::net::deliver_world_update(peers,closing,desynchronized,[&](int peer){attempted.push_back(peer);return peer!=2;});
    require(attempted==std::vector<int>({1,2})&&closing==std::set<int>({2,3})&&desynchronized==std::set<int>({2}),"failed reliable world update did not isolate its peer");
    attempted.clear();scrapds::net::deliver_world_update(peers,closing,desynchronized,[&](int peer){attempted.push_back(peer);return true;});
    require(attempted==std::vector<int>({1}),"closing peer received a later world update");
    Temp temp;
    const auto definitions=temp.p/"Data/Objects/Database/ShapeSets";std::filesystem::create_directories(definitions);
    {std::ofstream file(definitions/"blocks.shapeset");file<<R"({"blockList":[{"uuid":"00000000-0000-0000-0000-000000000000","color":"112233","name":"test","physicsMaterial":"Wood"},{"uuid":"a6c6ce30-dd47-4587-b475-085d55c6a3b4","color":"9b683a","name":"wood","physicsMaterial":"Wood"}]})";}
    {std::ofstream file(definitions/"parts.shapeset");file<<R"({"partList":[{"uuid":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","color":"ffffff","name":"unsupported part"}]})";}
    auto assets=scrapds::AssetDefinitions::load(temp.p);require(assets.size()==3,"asset definition not loaded");
    require(assets.find("00000000-0000-0000-0000-000000000000")->default_color_abgr==0xff332211,"default color channel order");
    sqlite3* db=nullptr;const auto source=temp.p/"source.db";require(sqlite3_open(source.string().c_str(),&db)==SQLITE_OK,"fixture open");
    sql(db,"CREATE TABLE Game(savegameversion INTEGER,flags INTEGER,seed INTEGER,gametick INTEGER,uniqueIds BLOB);INSERT INTO Game VALUES(30,1,2,3,NULL);CREATE TABLE RigidBody(id INTEGER PRIMARY KEY,worldId INTEGER,data BLOB);CREATE TABLE ChildShape(id INTEGER PRIMARY KEY,bodyId INTEGER,data BLOB);CREATE TABLE Joint(id INTEGER PRIMARY KEY,childShapeIdA INTEGER,childShapeIdB INTEGER,data BLOB);CREATE VIRTUAL TABLE RigidBodyBounds USING rtree(id,minX,maxX,minY,maxY);INSERT INTO RigidBodyBounds VALUES(7,-20,20,-20,20)");
    scrapds::Blob counters(72);word(counters,0,17);for(std::size_t i=1;i<=17;++i)word(counters,i*4,1000+std::uint32_t(i));word(counters,4,100);word(counters,12,200);insert(db,"UPDATE Game SET uniqueIds=?1",counters);
    scrapds::Blob body(60);body[1]=1;body[2]=1;word(body,3,7);scalar(body,9,20);scalar(body,13,-20);scalar(body,17,20);scalar(body,21,-20);scalar(body,27,1);insert(db,"INSERT INTO RigidBody VALUES(7,0,?1)",body);
    scrapds::Blob shape(48);shape[0]=1;shape[1]=31;shape[2]=1;word(shape,3,11);word(shape,7,7);word(shape,27,7);word(shape,37,0xff000000);shape[42]=2;shape[44]=2;shape[46]=2;insert(db,"INSERT INTO ChildShape VALUES(11,7,?1)",shape);
    scrapds::Blob joint(69);joint[0]=2;joint[1]=41;joint[2]=1;word(joint,3,5);word(joint,7,11);word(joint,11,11);joint[65]=0xff;joint[66]=0x01;joint[67]=0x7f;joint[68]=0xdf;insert(db,"INSERT INTO Joint VALUES(5,11,11,?1)",joint);sqlite3_close(db);
    auto world=scrapds::WorldState::load(source);
    auto temporary_shape=shape;word(temporary_shape,37,0xff998877);
    require(!world.atomic([&]{world.replace_shape_blob(11,temporary_shape);return false;}),"rejected compound command committed");
    require(world.shapes().at(11).data==shape,"rejected compound command changed cached world");
    bool rolled_back_exception=false;
    try {world.atomic([&]()->bool {world.replace_shape_blob(11,temporary_shape);throw std::runtime_error("injected failure");});}
    catch(const std::runtime_error&){rolled_back_exception=true;}
    require(rolled_back_exception&&world.shapes().at(11).data==shape,"exception left compound command in cache");
    const auto rollback_save=temp.p/"rollback.db";world.save_as(rollback_save);
    require(scrapds::WorldState::load(rollback_save).shapes().at(11).data==shape,"compound rollback left changed SQLite state");
    bool encoding_failed=false;
    try {
        scrapds::net::PaintShapeCommand bad_catalog_paint{{7,11,4,{},{}},0xffabcdef};
        scrapds::apply_replicated_command(world,assets,bad_catalog_paint,1,{});
    } catch(const std::runtime_error&) {encoding_failed=true;}
    require(encoding_failed&&world.shapes().at(11).data==shape,"replication failure committed a gameplay mutation");
    const auto encode_rollback_save=temp.p/"encode-rollback.db";world.save_as(encode_rollback_save);
    require(scrapds::WorldState::load(encode_rollback_save).shapes().at(11).data==shape,"replication failure persisted a gameplay mutation");
    scrapds::net::PaintShapeCommand paint{{7,11,4,{},{}},0xffabcdef};
    auto result=scrapds::apply_game_command(world,assets,paint);
    require(result.applied&&result.changed_shapes==std::vector<std::uint32_t>{11},"direct paint did not apply");
    require(scrapds::decode_child_geometry(world.shapes().at(11).data)->color_abgr==0xffabcdef,"paint color not persisted in native state");
    auto state=scrapds::net::make_child_shape_state(world.shapes().at(11).data,214);
    auto decoded=scrapds::net::parse_net_object_packet(scrapds::net::make_net_object_packet({22,3,{state}}));
    require(decoded.records.size()==1&&decoded.records[0].payload.at(12)==0xff&&decoded.records[0].payload.at(15)==0xef,"paint replication did not carry native color");
    scrapds::net::PaintJointCommand joint_paint{5,0xff123456};
    result=scrapds::apply_game_command(world,assets,joint_paint);
    require(result.applied&&result.changed_joints==std::vector<std::uint32_t>{5}&&world.joints().at(5).data[65]==0xff&&world.joints().at(5).data[68]==0x56,"joint paint color was not persisted");
    scrapds::net::InitialAssetCatalog catalog;catalog.joints[{}]=9;
    const auto joint_update=scrapds::make_object_update(world,result,8,catalog);
    require(joint_update.records.size()==1&&joint_update.records[0].operation==3&&joint_update.records[0].type==2&&joint_update.records[0].id==5&&joint_update.records[0].payload.size()==40&&joint_update.records[0].payload[36]==0xff&&joint_update.records[0].payload[39]==0x56,"joint paint NetObj state is malformed");
    auto no_default=joint_paint;no_default.color_rgba=0;
    result=scrapds::apply_game_command(world,assets,no_default);
    const auto& default_joint=world.joints().at(5).data;
    require(result.applied&&result.changed_joints==std::vector<std::uint32_t>{5}&&default_joint[65]==0xff&&default_joint[66]==0x33&&default_joint[67]==0x22&&default_joint[68]==0x11,"joint default color was not restored");
    result=scrapds::apply_game_command(world,assets,paint);require(result.applied&&result.changed_shapes.empty(),"idempotent paint changed state");
    paint.color_rgba=0;result=scrapds::apply_game_command(world,assets,paint);require(result.applied&&scrapds::decode_child_geometry(world.shapes().at(11).data)->color_abgr==0xff332211,"asset default color not restored");
    paint.selection.body_id=99;result=scrapds::apply_game_command(world,assets,paint);require(!result.applied,"unknown body authorized");
    // The wire oracle is a real 110-byte client BuildShape command, not a
    // serialization of the implementation's own structure.
    std::ifstream packet_file(std::filesystem::path(__FILE__).parent_path()/"fixtures/game-build-command.bin",std::ios::binary);
    require(bool(packet_file),"real build fixture unavailable");
    std::vector<std::uint8_t> packet{std::istreambuf_iterator<char>(packet_file),std::istreambuf_iterator<char>()};
    const auto command=scrapds::net::parse_game_command(packet);
    const auto original_body_count=world.bodies().size(),original_shape_count=world.shapes().size();
    result=scrapds::apply_game_command(world,assets,command);
    require(result.applied&&result.created_bodies==std::vector<std::uint32_t>{100}&&result.created_shapes==std::vector<std::uint32_t>{200},"real build was not allocated saved IDs");
    require(world.bodies().size()==original_body_count+1&&world.shapes().size()==original_shape_count+1,"build cache counts wrong");
    const auto pose=scrapds::decode_rigid_body_pose(world.bodies().at(100).data);
    const auto placed=scrapds::decode_child_geometry(world.shapes().at(200).data);
    require(pose&&pose->is_static&&pose->position==std::array<float,3>{12,12,0}&&pose->rotation_xyzw==std::array<float,4>{0,0,0,1},"native body origin/rotation wrong");
    require(world.bodies().at(100).data.at(55)==0&&placed&&placed->grid_position==std::array<std::int32_t,3>{6,15,0}&&placed->grid_dimensions==std::array<std::int32_t,3>{1,2,5}&&placed->state_flags==0,"native child geometry wrong");
    require(placed->uuid=="a6c6ce30-dd47-4587-b475-085d55c6a3b4"&&placed->color_abgr==0xff3a689b,"native asset UUID/default color wrong");
    result=scrapds::apply_game_command(world,assets,command);
    require(!result.applied&&result.reason=="block-overlaps-existing-shape"&&world.shapes().size()==original_shape_count+1,"overlap mutation was not atomic");
    auto build=std::get<scrapds::net::BuildShapeCommand>(command);
    build.axis_a={0,1,0};result=scrapds::apply_game_command(world,assets,build);require(!result.applied,"unsupported orientation accepted");
    build=std::get<scrapds::net::BuildShapeCommand>(command);build.shape_uuid.fill(0xaa);
    result=scrapds::apply_game_command(world,assets,build);require(!result.applied&&result.reason=="build-requires-known-block-asset","known non-block asset accepted");
    build=std::get<scrapds::net::BuildShapeCommand>(command);build.cell_position={100,100,0};build.block_dimensions={32767,32767,32767};
    result=scrapds::apply_game_command(world,assets,build);require(!result.applied&&result.reason=="block-volume-overflow","volume overflow accepted");
    build=std::get<scrapds::net::BuildShapeCommand>(command);build.kind=4;build.body_id=100;build.child_shape_id=200;build.cell_position={8,15,0};build.block_dimensions={1,1,1};
    result=scrapds::apply_game_command(world,assets,build);
    require(result.applied&&result.created_bodies.empty()&&result.created_shapes==std::vector<std::uint32_t>{201},"existing body block build failed");
    result=scrapds::apply_game_command(world,assets,build,1);require(!result.applied&&result.reason=="body-in-another-world","cross-world build accepted");
    // A rejected empty/inverted half-open paint region cannot select a voxel.
    paint.selection={100,0,4,{6,15,0},{6,17,5}};
    result=scrapds::apply_game_command(world,assets,paint);require(!result.applied,"empty half-open paint region accepted");
    paint.selection.corner_b={7,17,5};paint.color_rgba=0xff102030;
    result=scrapds::apply_game_command(world,assets,paint);require(result.applied&&result.changed_shapes==std::vector<std::uint32_t>{200},"half-open paint upper bound selected adjacent shape");
    paint.selection={100,0,4,{6,15,0},{7,16,5}};paint.color_rgba=0xffaabbcc;
    result=scrapds::apply_game_command(world,assets,paint);
    require(result.applied&&result.changed_shapes==std::vector<std::uint32_t>{200}&&result.created_shapes==std::vector<std::uint32_t>{202},"partial block paint did not split geometry and allocate fragment");
    const auto selected_geometry=scrapds::decode_child_geometry(world.shapes().at(200).data);
    const auto fragment_geometry=scrapds::decode_child_geometry(world.shapes().at(202).data);
    require(selected_geometry->grid_position==std::array<std::int32_t,3>{6,15,0}&&selected_geometry->grid_dimensions==std::array<std::int32_t,3>{1,1,5}&&selected_geometry->color_abgr==0xffaabbcc,"selected intersection geometry/color is wrong");
    require(fragment_geometry->grid_position==std::array<std::int32_t,3>{6,16,0}&&fragment_geometry->grid_dimensions==std::array<std::int32_t,3>{1,1,5}&&fragment_geometry->color_abgr==0xff102030,"untouched block fragment geometry/color is wrong");
    const auto saved=temp.p/"saved.db";world.save_as(saved);auto restored=scrapds::WorldState::load(saved);
    require(restored.shapes().at(11).data==world.shapes().at(11).data&&restored.shapes().at(200).data==world.shapes().at(200).data&&restored.shapes().at(201).data==world.shapes().at(201).data&&restored.shapes().at(202).data==world.shapes().at(202).data,"native build/paint lost on save/load");
    require(restored.bodies().at(100).data==world.bodies().at(100).data&&restored.table_counts()==world.table_counts(),"build body/counts lost on save/load");
    require(restored.joints().at(5).data==world.joints().at(5).data,"joint paint lost on save/load");
    require(sqlite3_open(saved.string().c_str(),&db)==SQLITE_OK,"saved fixture open");
    sqlite3_stmt* query=nullptr;require(sqlite3_prepare_v2(db,"SELECT uniqueIds FROM Game",-1,&query,nullptr)==SQLITE_OK,"counters query");
    require(sqlite3_step(query)==SQLITE_ROW,"counters row");
    auto expected=counters;word(expected,4,101);word(expected,12,203);
    const auto* raw=static_cast<const std::uint8_t*>(sqlite3_column_blob(query,0));
    require(sqlite3_column_bytes(query,0)==int(expected.size())&&std::equal(expected.begin(),expected.end(),raw),"counter endianness or untouched counters changed");sqlite3_finalize(query);
    require(sqlite3_prepare_v2(db,"SELECT minX,maxX,minY,maxY FROM RigidBodyBounds WHERE id=100",-1,&query,nullptr)==SQLITE_OK,"created RTree query");
    require(sqlite3_step(query)==SQLITE_ROW&&sqlite3_column_double(query,0)==13.5&&sqlite3_column_double(query,1)==14.25&&sqlite3_column_double(query,2)==15.75&&sqlite3_column_double(query,3)==16.25,"created body RTree bounds wrong");sqlite3_finalize(query);sqlite3_close(db);
    std::uint32_t rolled_back_player=0;
    world.atomic([&]{rolled_back_player=world.reserve_player_id();return false;});
    require(rolled_back_player==1009&&world.reserve_player_id()==1009,"player allocator did not roll back saved counter");
    const auto player_checkpoint=temp.p/"player-counter.db";world.save_as(player_checkpoint);auto player_restored=scrapds::WorldState::load(player_checkpoint);
    require(player_restored.reserve_player_id()==1010,"player allocator counter did not survive restart");
    auto spawn_world=scrapds::WorldState::load(player_checkpoint);scrapds::net::ServerCharacters characters;
    scrapds::PlayerSpawnSpec spec;spec.steam_id=1ull;spec.info.name=std::string(256,'x');spec.character.owner_steam_id=99;spec.character.position={1,2,3};spec.character.flag=true;spec.character_state.active=true;
    bool failed_spawn=false;try {(void)scrapds::prepare_player_spawn(spawn_world,characters,spec,42);}catch(const std::invalid_argument&){failed_spawn=true;}
    require(failed_spawn&&characters.size()==0,"failed spawn left a registered character");
    spec.info.name="Bob";const auto spawn=scrapds::prepare_player_spawn(spawn_world,characters,spec,42);
    require(spawn.player_id==1010&&spawn.character_id==2&&characters.size()==1,"spawn did not roll back Player ID or preserve Character ID uniqueness");
    const auto before=scrapds::net::parse_blob_data_packet(spawn.player_before),after=scrapds::net::parse_blob_data_packet(spawn.player_after);
    auto uncompress_player=[](const auto& entry){std::vector<std::uint8_t> bytes{0};bytes.insert(bytes.end(),entry.payload.begin(),entry.payload.end());bytes=scrapds::net::decompress_packet(bytes);bytes.erase(bytes.begin());return bytes;};
    const auto before_data=uncompress_player(before.entries.at(0)),after_data=uncompress_player(after.entries.at(0));
    require(before_data[0]==0xff&&before_data[1]==0xff&&before_data[2]==0xff&&before_data[3]==0xff&&after_data[3]==2,"Player spawn linkage was not encoded");
    const auto children=scrapds::net::split_world_aggregate(spawn.character_update);const auto character_packet=scrapds::net::parse_net_object_packet(children.at(0));
    require(character_packet.records.size()==2&&character_packet.records[0].type==6&&character_packet.records[0].operation==1&&character_packet.records[0].id==2&&character_packet.records[1].operation==3,"Character create/state sequence malformed");
    require(scrapds::net::parse_character_creation(character_packet.records[0].payload).owner_steam_id==spec.steam_id,"spawn trusted the replay owner instead of the connected peer");
    scrapds::net::CharacterUpdate moved;moved.has_character_state=true;moved.character_id=spawn.character_id;moved.tick=55;moved.position={17.25f,-8.0f,3.5f};
    require(characters.observe(spec.steam_id,moved).empty()&&characters.latest(spec.steam_id).has_value(),"accepted Character state was not retained");
    const auto current_spawn=scrapds::character_spawn_at_latest_position(spawn,*characters.latest(spec.steam_id));
    const auto current_record=scrapds::net::parse_net_object_packet(scrapds::net::split_world_aggregate(current_spawn).at(0)).records.at(0);
    const auto current_creation=scrapds::net::parse_character_creation(current_record.payload);
    require(current_creation.position==moved.position&&current_creation.owner_steam_id==spec.steam_id,"late join did not receive the latest Character position");
    auto foreign=moved;foreign.character_id++;bool foreign_rejected=false;
    try{(void)scrapds::character_spawn_at_latest_position(spawn,foreign);}catch(const std::invalid_argument&){foreign_rejected=true;}
    require(foreign_rejected,"late join accepted another Character's movement");
    require(!scrapds::strip_replay_players(spawn.player_before)&&!scrapds::strip_replay_players(spawn.character_update),"replay-only players were not removed");
    auto other_entry=before.entries.at(0);other_entry.uid[0]^=0x80;const auto mixed=scrapds::net::make_blob_data_packet({13,42,{before.entries.at(0),other_entry}});
    const auto filtered=scrapds::strip_replay_players(mixed);require(filtered.has_value(),"unrelated bootstrap entry was removed");
    const auto kept=scrapds::net::parse_blob_data_packet(*filtered);require(kept.entries.size()==1&&kept.entries[0].uid==other_entry.uid&&kept.entries[0].payload==other_entry.payload,"replay player filtering damaged unrelated blob data");
    auto mixed_objects=character_packet;mixed_objects.records.push_back({2,1,{},123,{}});
    const auto filtered_objects=scrapds::strip_replay_players(scrapds::net::make_world_aggregate(std::vector<std::vector<std::uint8_t>>{scrapds::net::make_net_object_packet(mixed_objects)}));
    require(filtered_objects.has_value(),"unrelated NetObj was removed with replay character");
    const auto kept_objects=scrapds::net::parse_net_object_packet(scrapds::net::split_world_aggregate(*filtered_objects).at(0));
    require(kept_objects.records.size()==1&&kept_objects.records[0].type==1&&kept_objects.records[0].id==123,"replay character filtering damaged unrelated NetObj");
    const auto spawn_checkpoint=temp.p/"spawn-counter.db";spawn_world.save_as(spawn_checkpoint);auto spawn_restored=scrapds::WorldState::load(spawn_checkpoint);
    require(spawn_restored.reserve_player_id()==1011,"prepared spawn did not persist the Player ID");
    const auto saved_players=spawn_restored.player_snapshots();
    require(saved_players.size()==1&&saved_players.at(spec.steam_id).player_id==spawn.player_id,"Player identity did not survive a checkpoint");
    const auto restored_player=scrapds::restore_player_snapshot(spec.steam_id,saved_players.at(spec.steam_id));
    require(restored_player.player_id==spawn.player_id&&restored_player.player_before==spawn.player_before,"checkpoint did not restore the offline Player snapshot");
    bool wrong_owner=false;try{(void)scrapds::restore_player_snapshot(spec.steam_id+1,saved_players.at(spec.steam_id));}catch(const std::invalid_argument&){wrong_owner=true;}
    require(wrong_owner,"checkpoint accepted a Player snapshot for a different owner");
    scrapds::net::ServerCharacters restarted_characters;auto restarted_spec=spec;restarted_spec.existing_player_id=restored_player.player_id;
    const auto restarted_spawn=scrapds::prepare_player_spawn(spawn_restored,restarted_characters,restarted_spec,45);
    require(restarted_spawn.player_id==spawn.player_id&&restarted_spawn.character_id==1&&spawn_restored.reserve_player_id()==1012,"restart did not preserve Player identity and runtime Character allocation");
    const scrapds::StoredPlayerLocation saved_location{7,{17.25,-8.0,3.5}};
    spawn_restored.store_player_location(spec.steam_id,saved_location);
    const auto location_checkpoint=temp.p/"player-location.db";spawn_restored.save_as(location_checkpoint);
    const auto restored_locations=scrapds::WorldState::load(location_checkpoint).player_locations();
    require(restored_locations.at(spec.steam_id).world_id==7&&restored_locations.at(spec.steam_id).position==saved_location.position,"Player position did not survive checkpoint");
    bool duplicate_player=false;try{spawn_restored.store_player_snapshot(spec.steam_id+1,spawn.player_id,spawn.player_before);}catch(const std::runtime_error&){duplicate_player=true;}
    require(duplicate_player&&spawn_restored.player_snapshots().size()==1,"duplicate Player ID corrupted saved identities");
    const auto original_saved_packet=spawn_restored.player_snapshots().at(spec.steam_id).unlinked_packet;
    spawn_restored.atomic([&]{spawn_restored.store_player_snapshot(spec.steam_id,spawn.player_id,spawn.player_before);return false;});
    require(spawn_restored.player_snapshots().at(spec.steam_id).unlinked_packet==original_saved_packet,"rolled-back Player snapshot changed the checkpoint state");
    const auto disconnected=scrapds::prepare_player_disconnect(spawn,77);
    const auto offline=scrapds::net::parse_blob_data_packet(disconnected[0]);
    require(offline.tick==77&&offline.entries.size()==1&&uncompress_player(offline.entries[0])==before_data,"disconnect did not unlink the original Player");
    const auto removed_character=scrapds::net::parse_net_object_packet(scrapds::net::split_world_aggregate(disconnected[1]).at(0));
    require(removed_character.tick==77&&removed_character.records.size()==1&&removed_character.records[0].operation==2&&removed_character.records[0].type==6&&removed_character.records[0].id==spawn.character_id,"disconnect removed the wrong Character");
    characters.despawn(spec.steam_id);spec.existing_player_id=spawn.player_id;
    const auto rejoined=scrapds::prepare_player_spawn(spawn_world,characters,spec,78);
    require(rejoined.player_id==spawn.player_id&&rejoined.character_id!=spawn.character_id&&spawn_world.reserve_player_id()==1011,"rejoin failed to reuse Player ID or consumed another saved ID");
    scrapds::net::RemoveShapeCommand partial{{100,0,4,{6,15,0},{7,16,1}}};
    result=scrapds::apply_game_command(world,assets,partial);
    require(result.applied&&result.removed_shapes==std::vector<std::uint32_t>{200}&&result.created_shapes==std::vector<std::uint32_t>{203}&&!world.shapes().contains(200)&&world.shapes().contains(203),"partial removal did not preserve the unselected geometry");
    scrapds::net::RemoveShapeCommand remove_small{{100,201,4,{},{}}};
    result=scrapds::apply_game_command(world,assets,remove_small);
    require(result.applied&&result.removed_shapes==std::vector<std::uint32_t>{201}&&result.removed_bodies.empty()&&!world.shapes().contains(201)&&world.bodies().contains(100),"whole-shape removal modified wrong records");
    scrapds::net::NetObjectPacket remove_update{22,4,{{2,1,{},201,{}},{1,1,31,203,{}}}};
    const auto remove_wire=scrapds::net::parse_net_object_packet(scrapds::net::make_net_object_packet(remove_update));
    require(remove_wire.records.size()==2&&remove_wire.records[0].operation==2&&remove_wire.records[0].type==1&&remove_wire.records[0].id==201&&remove_wire.records[1].operation==1&&remove_wire.records[1].type==1&&remove_wire.records[1].id==203,"shape removal NetObj update malformed");
    scrapds::net::RemoveShapeCommand remove_fragment{{100,202,4,{},{}}};
    result=scrapds::apply_game_command(world,assets,remove_fragment);
    require(result.applied&&result.removed_shapes==std::vector<std::uint32_t>{202}&&result.removed_bodies.empty()&&!world.shapes().contains(202)&&world.bodies().contains(100),"shape removal modified wrong body lifecycle");
    scrapds::net::RemoveShapeCommand remove_last{{100,203,4,{},{}}};
    result=scrapds::apply_game_command(world,assets,remove_last);
    require(result.applied&&result.removed_shapes==std::vector<std::uint32_t>{203}&&result.removed_bodies==std::vector<std::uint32_t>{100}&&!world.shapes().contains(203)&&!world.bodies().contains(100),"last shape did not remove empty body");
    const auto final_update=scrapds::make_object_update(world,result,9,catalog);
    require(final_update.records.size()==2&&final_update.records[0].operation==2&&final_update.records[0].type==1&&final_update.records[0].id==203&&final_update.records[1].operation==2&&final_update.records[1].type==0&&final_update.records[1].id==100,"shape and body removals were not replicated");
    const auto removed_save=temp.p/"removed.db";world.save_as(removed_save);auto removed_restored=scrapds::WorldState::load(removed_save);
    require(!removed_restored.shapes().contains(200)&&!removed_restored.shapes().contains(201)&&!removed_restored.shapes().contains(202)&&!removed_restored.shapes().contains(203)&&!removed_restored.bodies().contains(100)&&removed_restored.table_counts().at("RigidBodyBounds")==1,"removed records or RTree bounds survived save/load");
    require(scrapds::WorldState::load(source).shapes().at(11).data==shape,"original save modified by painting");
    std::cout<<"native gameplay checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
