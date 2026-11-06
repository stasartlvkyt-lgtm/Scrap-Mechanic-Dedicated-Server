#include "physics/world_physics.hpp"
#include "data/state.hpp"
#include "data/shape_geometry.hpp"
#include "data/assets.hpp"
#include "data/terrain.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void sql(sqlite3* db,const char* query){char* error=nullptr;if(sqlite3_exec(db,query,nullptr,nullptr,&error)!=SQLITE_OK){std::string message=error?error:"sqlite";sqlite3_free(error);throw std::runtime_error(message);}}
void word(scrapds::Blob& b,std::size_t offset,std::uint32_t value){for(int i=0;i<4;++i)b.at(offset+i)=std::uint8_t(value>>(24-8*i));}
void scalar(scrapds::Blob& b,std::size_t offset,float value){word(b,offset,std::bit_cast<std::uint32_t>(value));}
void shortword(scrapds::Blob& b,std::size_t offset,std::int16_t value){const auto u=static_cast<std::uint16_t>(value);b.at(offset)=std::uint8_t(u>>8);b.at(offset+1)=std::uint8_t(u);}
void insert(sqlite3* db,const char* query,const scrapds::Blob& b){sqlite3_stmt* raw=nullptr;require(sqlite3_prepare_v2(db,query,-1,&raw,nullptr)==SQLITE_OK,"prepare");sqlite3_bind_blob64(raw,1,b.data(),b.size(),SQLITE_TRANSIENT);const auto code=sqlite3_step(raw);sqlite3_finalize(raw);require(code==SQLITE_DONE,"insert");}
struct Temp {std::filesystem::path p=std::filesystem::temp_directory_path()/("scrapds-world-physics-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Temp(){std::filesystem::create_directory(p);}~Temp(){std::error_code ec;std::filesystem::remove_all(p,ec);}};
scrapds::Blob body(std::uint32_t id,bool dynamic,float z,float bound) {
    scrapds::Blob b(dynamic?80:60);b[1]=dynamic?2:1;b[2]=1;word(b,3,id);
    scalar(b,9,bound);scalar(b,13,-bound);scalar(b,17,bound);scalar(b,21,-bound);
    scalar(b,dynamic?39:27,1);scalar(b,51,z);return b;
}
scrapds::Blob block(std::uint32_t id,std::uint32_t owner,std::int16_t pos,std::int16_t size) {
    scrapds::Blob b(48);b[0]=1;b[1]=31;b[2]=1;word(b,3,id);word(b,7,owner);word(b,27,owner);
    for(int i=0;i<3;++i){shortword(b,31+i*2,pos);shortword(b,41+i*2,size);}return b;
}
}
int main(int argc,char** argv){try {
    Temp temp;const auto path=temp.p/"source.db";sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"open");
    sql(db,"CREATE TABLE Game(savegameversion INTEGER,flags INTEGER,seed INTEGER,gametick INTEGER);INSERT INTO Game VALUES(30,1,2,3);CREATE TABLE RigidBody(id INTEGER PRIMARY KEY,worldId INTEGER,data BLOB);CREATE TABLE ChildShape(id INTEGER PRIMARY KEY,bodyId INTEGER,data BLOB);CREATE TABLE Joint(id INTEGER PRIMARY KEY,childShapeIdA INTEGER,childShapeIdB INTEGER,data BLOB);CREATE VIRTUAL TABLE RigidBodyBounds USING rtree(id,minX,maxX,minY,maxY);INSERT INTO RigidBodyBounds VALUES(1,-5,5,-5,5);INSERT INTO RigidBodyBounds VALUES(2,-0.5,0.5,-0.5,0.5);INSERT INTO RigidBodyBounds VALUES(3,-0.5,0.5,-0.5,0.5);INSERT INTO RigidBodyBounds VALUES(4,10,10.25,0,0.25);INSERT INTO RigidBodyBounds VALUES(5,19.75,20,0,1);INSERT INTO RigidBodyBounds VALUES(6,30,30.5,0,1)");
    auto floor=body(1,false,-5,5);auto falling=body(2,true,5,0.5f);auto jointed=body(3,true,8,0.5f);
    insert(db,"INSERT INTO RigidBody VALUES(1,0,?1)",floor);insert(db,"INSERT INTO RigidBody VALUES(2,0,?1)",falling);insert(db,"INSERT INTO RigidBody VALUES(3,0,?1)",jointed);
    auto box_part_body=body(4,false,0,0.5f);scalar(box_part_body,9,0.25f);scalar(box_part_body,13,0);scalar(box_part_body,17,10.25f);scalar(box_part_body,21,10);scalar(box_part_body,43,10);insert(db,"INSERT INTO RigidBody VALUES(4,0,?1)",box_part_body);
    auto rotated_body=body(5,false,0,0.5f);scalar(rotated_body,9,1);scalar(rotated_body,13,0);scalar(rotated_body,17,20);scalar(rotated_body,21,19.75f);scalar(rotated_body,43,20);insert(db,"INSERT INTO RigidBody VALUES(5,0,?1)",rotated_body);
    auto cylinder_body=body(6,false,0,0.5f);scalar(cylinder_body,9,1);scalar(cylinder_body,13,0);scalar(cylinder_body,17,30.5f);scalar(cylinder_body,21,30);scalar(cylinder_body,43,30);insert(db,"INSERT INTO RigidBody VALUES(6,0,?1)",cylinder_body);
    auto floor_shape=block(11,1,-20,40);auto box=block(12,2,0,4);auto linked=block(13,3,-2,4);
    insert(db,"INSERT INTO ChildShape VALUES(11,1,?1)",floor_shape);insert(db,"INSERT INTO ChildShape VALUES(12,2,?1)",box);insert(db,"INSERT INTO ChildShape VALUES(13,3,?1)",linked);
    scrapds::Blob part(43);part[0]=1;part[1]=32;part[2]=1;word(part,3,14);word(part,7,4);word(part,27,4);part[41]=0x75;insert(db,"INSERT INTO ChildShape VALUES(14,4,?1)",part);
    auto rotated_part=part;word(rotated_part,3,15);word(rotated_part,7,5);word(rotated_part,27,5);rotated_part[41]=0x61;insert(db,"INSERT INTO ChildShape VALUES(15,5,?1)",rotated_part);
    auto cylinder_part=part;word(cylinder_part,3,16);word(cylinder_part,7,6);word(cylinder_part,27,6);std::fill(cylinder_part.begin()+11,cylinder_part.begin()+27,0xbb);insert(db,"INSERT INTO ChildShape VALUES(16,6,?1)",cylinder_part);
    scrapds::Blob joint(69);joint[0]=2;joint[1]=41;joint[2]=1;word(joint,3,5);word(joint,7,13);word(joint,11,13);insert(db,"INSERT INTO Joint VALUES(5,13,13,?1)",joint);sqlite3_close(db);
    const auto defs=temp.p/"Data/Objects/Database/ShapeSets";std::filesystem::create_directories(defs);
    {std::ofstream file(defs/"parts.shapeset");file<<R"({"partList":[{"uuid":"00000000-0000-0000-0000-000000000000","box":{"x":1,"y":1,"z":4}},{"uuid":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","cylinder":{"axis":"Y","diameter":2,"depth":4}},{"uuid":"dddddddd-dddd-dddd-dddd-dddddddddddd","hull":{"x":1,"y":1,"z":1,"col":"$GAME_DATA/Objects/Collision/test.obj"}},{"uuid":"eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee","hull":{"x":1,"y":1,"z":1,"margin":0.25,"col":"$GAME_DATA/Objects/Collision/line.obj"}},{"uuid":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","box":{"x":1,"y":1,"z":1},"hull":{"asset":"mesh.obj"}},{"uuid":"ed27f5e2-cac5-4a32-a5d9-49f116acc6af","box":{"x":1,"y":1,"z":1}},{"uuid":"67da25c9-3825-41f6-9724-4546a11cb2a5","survivalSpring":{"length":2}}]})";}
    const auto collision_dir=temp.p/"Data/Objects/Collision";std::filesystem::create_directories(collision_dir);
    {std::ofstream file(collision_dir/"test.obj");for(int x:{-1,1})for(int y:{-1,1})for(int z:{-1,1})file<<"v "<<x*0.5<<' '<<y*0.5<<' '<<z*0.5<<'\n';}
    {std::ofstream file(collision_dir/"line.obj");file<<"v -0.5 0 0\nv 0.5 0 0\n";}
    const auto assets=scrapds::AssetDefinitions::load(temp.p);
    require(!assets.find("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")->box_dimensions,"hull asset was misidentified as a pure box");
    require(assets.find("dddddddd-dddd-dddd-dddd-dddddddddddd")->hull_geometry.has_value(),"convex hull asset did not load");
    const auto* rounded=assets.find("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee");require(rounded&&rounded->hull_geometry&&rounded->hull_geometry->degenerate&&std::abs(rounded->hull_geometry->margin-0.0625)<1e-8,"rounded line hull did not load in grid units");
    auto world=scrapds::WorldState::load(path);auto physics=scrapds::WorldPhysics::from_world(world,1000.0,nullptr,&assets);
    require(physics.coverage().static_bodies==4&&physics.coverage().dynamic_bodies==1&&physics.coverage().box_parts==2&&physics.coverage().cylinder_parts==1&&physics.coverage().skipped_joint==1,"coverage classification");
    const auto ground_motion=physics.move_character_capsule(0,{-2,-2,3},{0,0,-4},0.3,0.8);
    require(ground_motion.contacts>0&&ground_motion.position.z>0.68&&ground_motion.position.z<0.73,"character capsule passed through saved static floor");
    const auto body_motion=physics.move_character_capsule(0,{0.5,0.5,8},{0,0,-5},0.3,0.8);
    require(body_motion.contacts>0&&body_motion.position.z>6.65&&body_motion.position.z<6.8,"character capsule passed through simulated body");
    const auto part_bounds=physics.bounds(4);require(part_bounds&&std::abs(part_bounds->minimum.x-10)<0.0001&&std::abs(part_bounds->maximum.x-10.25)<0.0001&&std::abs(part_bounds->maximum.z-1.0)<0.0001,"box-part bounds are wrong");
    const auto rotated_bounds=physics.bounds(5);require(rotated_bounds&&std::abs(rotated_bounds->minimum.x-19.75)<0.0001&&std::abs(rotated_bounds->maximum.x-20)<0.0001&&std::abs(rotated_bounds->maximum.y-1.0)<0.0001&&std::abs(rotated_bounds->minimum.z+0.25)<0.0001,"rotated box-part bounds are wrong");
    const auto cylinder_bounds=physics.bounds(6);require(cylinder_bounds&&std::abs(cylinder_bounds->minimum.x-30)<0.0001&&std::abs(cylinder_bounds->maximum.x-30.5)<0.0001&&std::abs(cylinder_bounds->maximum.y-1.0)<0.0001&&std::abs(cylinder_bounds->maximum.z-0.5)<0.0001,"cylinder-part bounds are wrong");
    auto cylinder_world=scrapds::WorldState::load(path);auto above_cylinder=falling;scalar(above_cylinder,43,29.75f);scalar(above_cylinder,47,0);cylinder_world.replace_body_blob(2,above_cylinder);
    auto cylinder_contact=scrapds::WorldPhysics::from_world(cylinder_world,1000.0,nullptr,&assets);cylinder_contact.step(40);
    auto cylinder_freefall=scrapds::WorldPhysics::from_world(cylinder_world);cylinder_freefall.step(40);
    require(cylinder_contact.bodies().at(0).position.z>cylinder_freefall.bodies().at(0).position.z+0.25,"dynamic block did not collide with cylindrical part");
    auto terrain_world=scrapds::WorldState::load(path);auto above_terrain=falling;scalar(above_terrain,43,49.5f);terrain_world.replace_body_blob(2,above_terrain);
    auto terrain_physics=scrapds::WorldPhysics::from_world(terrain_world);
    const std::vector<scrapds::TerrainVertex> terrain{{45,-5,0},{55,-5,0},{55,5,0},{45,-5,0},{55,5,0},{45,5,0}};
    terrain_physics.add_terrain_mesh(0,terrain);terrain_physics.step(400);
    const auto terrain_player=terrain_physics.move_character_capsule(0,{54,0,3},{0,0,-4},0.3,0.8);
    require(terrain_player.contacts>0&&terrain_player.position.z>0.68&&terrain_player.position.z<0.73,"character capsule passed through resident terrain");
    require(terrain_physics.coverage().terrain_triangles==2&&std::abs(terrain_physics.bodies().at(0).position.z)<0.05,"dynamic block did not settle on terrain mesh");
    require(std::abs(physics.bodies().at(0).position.z-5.0)<0.0001,"compound centre of mass shifted saved body origin");
    physics.step(0);physics.writeback(world);require(world.bodies().at(2).data==falling,"zero physics ticks changed a body");
    auto foreign_world=scrapds::WorldState::load(path);
    bool foreign_rejected=false;try{physics.writeback(foreign_world);}catch(const std::runtime_error&){foreign_rejected=true;}
    require(foreign_rejected,"physics accepted a different world instance");
    const auto version=world.physics_version();
    require(!world.atomic([&]{world.replace_body_blob(2,falling);return false;}),"rollback fixture unexpectedly committed");
    require(world.physics_version()==version,"rollback changed physics version");
    physics.writeback(world);
    const std::set<std::uint32_t> unmapped;
    auto filtered=scrapds::WorldPhysics::from_world(world,1000.0,&unmapped,&assets);
    require(filtered.coverage().dynamic_bodies==0&&filtered.coverage().skipped_unmapped==1&&filtered.coverage().static_bodies==4,"network-unmapped dynamic body was simulated");
    auto impulse_test=scrapds::WorldPhysics::from_world(world);
    impulse_test.apply_impulse(2,{1000,0,0});
    require(std::abs(impulse_test.bodies().at(0).velocity.x-1.0)<0.001,"impulse did not use body mass");
    impulse_test.apply_force(2,{1000,0,0});impulse_test.step(1);
    require(std::abs(impulse_test.bodies().at(0).velocity.x-1.025)<0.002,"force did not affect the next fixed tick");
    bool static_rejected=false;try{impulse_test.apply_force(1,{1,0,0});}catch(const std::invalid_argument&){static_rejected=true;}
    require(static_rejected,"force accepted for a static body");
    physics.step(400);const auto simulated=physics.bodies();require(simulated.size()==1&&simulated[0].id==2&&std::abs(simulated[0].position.z)<0.04,"dynamic body did not settle on saved floor");
    physics.writeback(world);require(world.bodies().at(1).data==floor&&world.bodies().at(3).data==jointed&&world.bodies().at(4).data==box_part_body,"static or jointed body was changed");
    const auto written=scrapds::decode_rigid_body_pose(world.bodies().at(2).data);require(written&&std::abs(written->position[2])<0.04&&std::abs((*written->linear_velocity())[2])<0.05,"dynamic pose or velocity not written");
    const auto save=temp.p/"result.db";world.save_as(save);auto restored=scrapds::WorldState::load(save);require(restored.bodies().at(2).data==world.bodies().at(2).data,"dynamic pose lost on reload");
    const auto hull_path=temp.p/"hull.db";std::filesystem::copy_file(path,hull_path);sqlite3* hull_db=nullptr;require(sqlite3_open(hull_path.string().c_str(),&hull_db)==SQLITE_OK,"open hull fixture");
    auto hull_part=part;std::fill(hull_part.begin()+11,hull_part.begin()+27,0xdd);insert(hull_db,"UPDATE ChildShape SET data=?1 WHERE id=14",hull_part);
    auto above_hull=falling;scalar(above_hull,43,9.625f);scalar(above_hull,47,-0.375f);insert(hull_db,"UPDATE RigidBody SET data=?1 WHERE id=2",above_hull);sqlite3_close(hull_db);
    auto hull_world=scrapds::WorldState::load(hull_path);auto hull_physics=scrapds::WorldPhysics::from_world(hull_world,1000,nullptr,&assets);const auto hull_bounds=hull_physics.bounds(4);
    require(hull_physics.coverage().hull_parts==1&&hull_bounds&&std::abs(hull_bounds->minimum.x-9.96)<0.001&&std::abs(hull_bounds->maximum.x-10.29)<0.001,"convex hull placement or margin incorrect");
    hull_physics.step(120);require(hull_physics.bodies().at(0).position.z>0.2,"dynamic block passed through convex hull");
    const auto rounded_path=temp.p/"rounded.db";std::filesystem::copy_file(path,rounded_path);sqlite3* rounded_db=nullptr;require(sqlite3_open(rounded_path.string().c_str(),&rounded_db)==SQLITE_OK,"open rounded hull fixture");
    auto rounded_part=part;std::fill(rounded_part.begin()+11,rounded_part.begin()+27,0xee);insert(rounded_db,"UPDATE ChildShape SET data=?1 WHERE id=14",rounded_part);
    auto rounded_body=box_part_body;scalar(rounded_body,43,50);insert(rounded_db,"UPDATE RigidBody SET data=?1 WHERE id=4",rounded_body);
    auto above_rounded=falling;scalar(above_rounded,43,49.625f);scalar(above_rounded,47,-0.375f);insert(rounded_db,"UPDATE RigidBody SET data=?1 WHERE id=2",above_rounded);sqlite3_close(rounded_db);
    auto rounded_world=scrapds::WorldState::load(rounded_path);auto rounded_physics=scrapds::WorldPhysics::from_world(rounded_world,1000,nullptr,&assets);const auto rounded_bounds=rounded_physics.bounds(4);
    require(rounded_bounds&&std::abs(rounded_bounds->minimum.x-49.875)<0.001&&std::abs(rounded_bounds->maximum.x-50.375)<0.001,"rounded hull broadphase bounds incorrect");
    auto rounded_freefall=scrapds::WorldPhysics::from_world(rounded_world);rounded_physics.step(40);rounded_freefall.step(40);
    require(rounded_physics.bodies().at(0).position.z>rounded_freefall.bodies().at(0).position.z+0.08,"dynamic block passed through rounded line hull");
    auto changed=world.shapes().at(11).data;changed[40]^=1;world.replace_shape_blob(11,changed);
    bool rejected=false;try{physics.writeback(world);}catch(const std::runtime_error&){rejected=true;}require(rejected,"stale physics snapshot overwrote a changed world");
    const auto bearing_path=temp.p/"bearing.db";std::filesystem::copy_file(path,bearing_path);
    sqlite3* bearing_db=nullptr;require(sqlite3_open(bearing_path.string().c_str(),&bearing_db)==SQLITE_OK,"open bearing fixture");
    sql(bearing_db,"DELETE FROM Joint WHERE id=5");auto paired=jointed;scalar(paired,51,5);insert(bearing_db,"UPDATE RigidBody SET data=?1 WHERE id=3",paired);
    scrapds::Blob bearing(69);bearing[0]=2;bearing[1]=3;bearing[2]=1;word(bearing,3,6);word(bearing,7,12);word(bearing,11,13);word(bearing,31,12);word(bearing,35,13);
    const std::array<std::uint8_t,16> bearing_uuid{0xa6,0x23,0xdb,0xb0,0x41,0x6e,0xb6,0xb5,0xad,0x4a,0x3e,0x91,0x6b,0x88,0x1b,0x4a};std::copy(bearing_uuid.begin(),bearing_uuid.end(),bearing.begin()+15);bearing[63]=0x55;bearing[64]=0x77;
    insert(bearing_db,"INSERT INTO Joint VALUES(6,12,13,?1)",bearing);sqlite3_close(bearing_db);
    auto bearing_world=scrapds::WorldState::load(bearing_path);auto linked_physics=scrapds::WorldPhysics::from_world(bearing_world);
    require(linked_physics.coverage().bearing_joints==1&&linked_physics.coverage().dynamic_bodies==2,"supported bearing pair not created");
    linked_physics.apply_impulse(2,{1000,0,0});linked_physics.step(25);const auto linked_bodies=linked_physics.bodies();
    require(linked_bodies.size()==2&&linked_bodies[0].position.x>0.05&&std::abs(linked_bodies[0].position.x-linked_bodies[1].position.x)<0.1&&std::abs(linked_bodies[0].position.z-linked_bodies[1].position.z)<0.1,"bearing failed to constrain connected moving bodies");
    auto rotating=scrapds::WorldPhysics::from_world(bearing_world);rotating.apply_impulse(3,{0,100,0},{0,0,0.5});rotating.step(10);const auto rotating_bodies=rotating.bodies();
    require(std::abs(rotating_bodies[1].angular_velocity.x-rotating_bodies[0].angular_velocity.x)>0.05,"bearing incorrectly locked rotation around its axis");
    const std::set<std::uint32_t> one_mapped{2};auto partly_mapped=scrapds::WorldPhysics::from_world(bearing_world,1000.0,&one_mapped);
    require(partly_mapped.coverage().bearing_joints==0&&partly_mapped.coverage().dynamic_bodies==0,"partially mapped bearing group was simulated");
    const auto lights_path=temp.p/"lights-bearing.db";std::filesystem::copy_file(bearing_path,lights_path);sqlite3* lights_db=nullptr;require(sqlite3_open(lights_path.string().c_str(),&lights_db)==SQLITE_OK,"open lights fixture");
    auto light_part=part;word(light_part,3,17);word(light_part,7,2);word(light_part,27,2);const std::array<std::uint8_t,16> light_uuid{0xaf,0xc6,0xac,0x16,0xf1,0x49,0xd9,0xa5,0x32,0x4a,0xc5,0xca,0xe2,0xf5,0x27,0xed};std::copy(light_uuid.begin(),light_uuid.end(),light_part.begin()+11);insert(lights_db,"INSERT INTO ChildShape VALUES(17,2,?1)",light_part);
    sql(lights_db,"CREATE TABLE Controller(id INTEGER PRIMARY KEY,hostId INTEGER,hostType INTEGER,data BLOB)");scrapds::Blob light_controller(24);light_controller[0]=3;light_controller[1]=25;light_controller[2]=1;word(light_controller,3,101);word(light_controller,7,17);light_controller[11]=1;word(light_controller,12,17);light_controller[16]=1;insert(lights_db,"INSERT INTO Controller VALUES(101,17,1,?1)",light_controller);sqlite3_close(lights_db);
    auto lights_world=scrapds::WorldState::load(lights_path);auto lit=scrapds::WorldPhysics::from_world(lights_world,1000,nullptr,&assets);
    require(lit.coverage().passive_controllers==1&&lit.coverage().bearing_joints==1&&lit.coverage().dynamic_bodies==2,"passive headlight blocked bearing physics");
    const auto linked_lights_path=temp.p/"linked-lights-bearing.db";std::filesystem::copy_file(lights_path,linked_lights_path);sqlite3* linked_lights_db=nullptr;require(sqlite3_open(linked_lights_path.string().c_str(),&linked_lights_db)==SQLITE_OK,"open linked lights fixture");light_controller[17]=1;insert(linked_lights_db,"UPDATE Controller SET data=?1 WHERE id=101",light_controller);sqlite3_close(linked_lights_db);
    auto linked_lights_world=scrapds::WorldState::load(linked_lights_path);auto unknown_light=scrapds::WorldPhysics::from_world(linked_lights_world,1000,nullptr,&assets);
    require(unknown_light.coverage().passive_controllers==0&&unknown_light.coverage().bearing_joints==0&&unknown_light.coverage().dynamic_bodies==0,"unknown linked headlight controller was treated as passive");
    const auto motor_path=temp.p/"controlled-bearing.db";std::filesystem::copy_file(bearing_path,motor_path);sqlite3* motor_db=nullptr;require(sqlite3_open(motor_path.string().c_str(),&motor_db)==SQLITE_OK,"open controlled-bearing fixture");
    sql(motor_db,"CREATE TABLE Controller(id INTEGER PRIMARY KEY,hostId INTEGER,hostType INTEGER,data BLOB);INSERT INTO Controller VALUES(100,6,2,X'00')");sqlite3_close(motor_db);
    auto controlled_world=scrapds::WorldState::load(motor_path);auto controlled=scrapds::WorldPhysics::from_world(controlled_world);
    require(controlled.coverage().bearing_joints==0&&controlled.coverage().dynamic_bodies==0,"controlled bearing was simulated without its controller");
    const auto spring_path=temp.p/"spring.db";std::filesystem::copy_file(bearing_path,spring_path);sqlite3* spring_db=nullptr;require(sqlite3_open(spring_path.string().c_str(),&spring_db)==SQLITE_OK,"open spring fixture");
    auto spring_joint=bearing;spring_joint[1]=4;const std::array<std::uint8_t,16> spring_uuid{0xa5,0xb2,0x1c,0xa1,0x46,0x45,0x24,0x97,0xf6,0x41,0x25,0x38,0xc9,0x25,0xda,0x67};std::copy(spring_uuid.begin(),spring_uuid.end(),spring_joint.begin()+15);insert(spring_db,"UPDATE Joint SET data=?1 WHERE id=6",spring_joint);
    auto spring_body=paired;scalar(spring_body,43,0.5f);insert(spring_db,"UPDATE RigidBody SET data=?1 WHERE id=3",spring_body);
    sql(spring_db,"CREATE TABLE Controller(id INTEGER PRIMARY KEY,hostId INTEGER,hostType INTEGER,data BLOB)");scrapds::Blob spring_controller(20);spring_controller[0]=3;spring_controller[1]=36;spring_controller[2]=1;word(spring_controller,3,100);word(spring_controller,7,6);spring_controller[11]=2;word(spring_controller,12,6);spring_controller[16]=2;spring_controller[19]=3;insert(spring_db,"INSERT INTO Controller VALUES(100,6,2,?1)",spring_controller);sqlite3_close(spring_db);
    auto spring_world=scrapds::WorldState::load(spring_path);auto spring_physics=scrapds::WorldPhysics::from_world(spring_world,1000,nullptr,&assets);
    require(spring_physics.coverage().spring_joints==1&&spring_physics.coverage().dynamic_bodies==2,"supported suspension group not created");
    spring_physics.apply_impulse(3,{0,100,0});spring_physics.step(10);const auto spring_bodies=spring_physics.bodies();
    require(spring_bodies.size()==2&&spring_bodies[1].position.x-spring_bodies[0].position.x>0.15&&spring_bodies[1].position.x-spring_bodies[0].position.x<0.7&&std::abs(spring_bodies[1].position.y-spring_bodies[0].position.y)<0.1,"suspension did not constrain lateral motion and travel");
    const auto soft_path=temp.p/"soft-spring.db";std::filesystem::copy_file(spring_path,soft_path);sqlite3* soft_db=nullptr;require(sqlite3_open(soft_path.string().c_str(),&soft_db)==SQLITE_OK,"open soft spring fixture");
    auto soft_controller=spring_controller;soft_controller[19]=0;insert(soft_db,"UPDATE Controller SET data=?1 WHERE id=100",soft_controller);sqlite3_close(soft_db);
    auto soft_world=scrapds::WorldState::load(soft_path);auto soft_spring=scrapds::WorldPhysics::from_world(soft_world,1000,nullptr,&assets);auto stiff_spring=scrapds::WorldPhysics::from_world(spring_world,1000,nullptr,&assets);
    soft_spring.apply_impulse(3,{-300,0,0});stiff_spring.apply_impulse(3,{-300,0,0});soft_spring.step(20);stiff_spring.step(20);
    const auto soft_bodies=soft_spring.bodies(),stiff_bodies=stiff_spring.bodies();
    require(stiff_bodies[1].position.x-stiff_bodies[0].position.x>soft_bodies[1].position.x-soft_bodies[0].position.x+0.005,"saved suspension stiffness did not affect motion");
    if(argc>=3) {
        const auto tables=scrapds::TerrainMeshTables::from_game_executable(std::filesystem::path(argv[2])/"Release/ScrapMechanic.exe");
        std::array<std::uint8_t,4913> plane{};for(int x=0;x<17;++x)for(int y=0;y<17;++y)for(int z=1;z<17;++z)plane[17*(y+17*x)+z]=31;
        const auto triangles=scrapds::mesh_terrain_chunk(plane,{3,0,0},tables);auto real_mesh_world=scrapds::WorldState::load(path);
        auto above_real_mesh=falling;scalar(above_real_mesh,43,49.5f);scalar(above_real_mesh,47,1);real_mesh_world.replace_body_blob(2,above_real_mesh);
        auto real_mesh_physics=scrapds::WorldPhysics::from_world(real_mesh_world);real_mesh_physics.add_terrain_mesh(0,triangles);real_mesh_physics.step(400);
        require(std::abs(real_mesh_physics.bodies().at(0).position.z-(0.5+16.0/31))<0.05,"dynamic block did not collide with generated terrain surface");
        std::vector<scrapds::TerrainTileChunk> tile_chunks(8);
        for(unsigned m=0;m<8;++m){auto& c=tile_chunks[m];c.position={int(m&1),int((m>>1)&1),int((m>>2)&1)};for(int x=0;x<16;++x)for(int y=0;y<16;++y)for(int z=0;z<16;++z)c.voxels[256*x+16*y+z]=((m&4)||z>=1)?31:0;}
        scrapds::OverworldCell cell;cell.rotation=1;const auto placed=scrapds::mesh_overworld_tile_voxels(cell,tile_chunks,tables);
        auto placed_world=scrapds::WorldState::load(path);auto above_placed=falling;scalar(above_placed,43,55);scalar(above_placed,47,8);scalar(above_placed,51,1);placed_world.replace_body_blob(2,above_placed);
        auto placed_physics=scrapds::WorldPhysics::from_world(placed_world);placed_physics.add_terrain_mesh(0,placed);placed_physics.step(400);
        require(!placed.empty()&&std::abs(placed_physics.bodies().at(0).position.z-(0.5+16.0/31))<0.05,"dynamic block did not collide with positioned overworld tile");
        const auto actual_world=scrapds::WorldState::load(argv[1]);const auto actual_assets=scrapds::AssetDefinitions::load(argv[2]);
        const auto actual_physics=scrapds::WorldPhysics::from_world(actual_world,1000.0,nullptr,&actual_assets);
        require(actual_world.flat_surface_worlds().contains(0)&&actual_physics.coverage().flat_surface_worlds==1&&actual_physics.coverage().terrain_triangles==2,"saved flat world terrain was not added to physics");
        auto stable_physics=scrapds::WorldPhysics::from_world(actual_world,1000.0,nullptr,&actual_assets);stable_physics.step(400);
        bool stable=false;for(const auto& body:stable_physics.bodies())if(body.id==387){const auto bounds=stable_physics.bounds(387);stable=bounds&&bounds->minimum.z>-0.1&&std::abs(body.velocity.z)<0.05;}
        require(stable,"thin saved block did not settle on flat world terrain");
        unsigned checked=0;for(const auto id:{359u,933u,1011u,1179u,1180u,1182u,1183u}) {
            if(!actual_world.bodies().contains(id))continue;
            const auto bounds=actual_physics.bounds(id);const auto& expected=actual_world.bodies().at(id).prefix->bounds_min_x_max_x_min_y_max_y;
            require(bounds&&std::abs(bounds->minimum.x-expected[0])<0.01&&std::abs(bounds->maximum.x-expected[1])<0.01&&std::abs(bounds->minimum.y-expected[2])<0.01&&std::abs(bounds->maximum.y-expected[3])<0.01,"captured part bounds do not match saved RTree");
            ++checked;
        }
        require(checked>=5,"too few captured part examples");
        std::cout<<"captured part bounds verified\n";
    }
    if(argc==4) {
        const auto real_world=scrapds::WorldState::load(argv[3]);const auto real_assets=scrapds::AssetDefinitions::load(argv[2]);
        const auto real_physics=scrapds::WorldPhysics::from_world(real_world,1000,nullptr,&real_assets);
        for(const auto id:{1u,7u,23u,24u,281u}){const auto& expected=real_world.bodies().at(id).prefix->bounds_min_x_max_x_min_y_max_y;const auto actual=real_physics.bounds(id);
            require(actual&&std::abs(actual->minimum.x-expected[0])<0.015&&std::abs(actual->maximum.x-expected[1])<0.015&&std::abs(actual->minimum.y-expected[2])<0.015&&std::abs(actual->maximum.y-expected[3])<0.015,"captured hull bounds do not match saved RTree");}
        const auto data=real_world.terrain_generator_data(1);require(data.has_value(),"saved survival terrain missing");const auto overworld=scrapds::decode_overworld_terrain(*data);const auto catalog=scrapds::index_game_terrain_tiles(argv[2]);
        const auto cell=std::find_if(overworld.cells.begin(),overworld.cells.end(),[](const auto& c){return c.x==0&&c.y==0;});require(cell!=overworld.cells.end(),"saved survival origin cell missing");
        const auto surface=scrapds::mesh_saved_overworld_cell(overworld,catalog,*cell);float highest=-1000000;for(const auto& v:surface)highest=std::max(highest,v.z);
        auto terrain_world=scrapds::WorldState::load(path);auto test_body=falling;scalar(test_body,43,32);scalar(test_body,47,32);scalar(test_body,51,highest+2);terrain_world.replace_body_blob(2,test_body);
        auto terrain_scene=scrapds::WorldPhysics::from_world(terrain_world);terrain_scene.add_saved_overworld_cell(0,overworld,catalog,*cell);auto free_scene=scrapds::WorldPhysics::from_world(terrain_world);
        terrain_scene.step(120);free_scene.step(120);require(terrain_scene.coverage().terrain_triangles==2048&&terrain_scene.bodies().at(0).position.z>free_scene.bodies().at(0).position.z+5,"saved survival surface did not affect body collision");
        std::cout<<"captured hull bounds verified\n";
    }
    std::cout<<"world physics checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
