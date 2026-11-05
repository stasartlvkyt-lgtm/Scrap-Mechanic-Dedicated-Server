#include "data/shape_geometry.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint8_t> from_hex(const std::string& hex) {
    std::vector<std::uint8_t> result;
    for (std::size_t i=0; i<hex.size(); i+=2) result.push_back(std::uint8_t(std::stoul(hex.substr(i,2), nullptr, 16)));
    return result;
}
void rejects(const std::vector<std::uint8_t>& bytes) {
    bool threw = false;
    try { (void)scrapds::decode_child_geometry(bytes); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "malformed geometry accepted");
}
void check_database(const std::filesystem::path& path) {
    sqlite3* db{};
    const auto u8 = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char*>(u8.c_str()), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("cannot open verification save");
    }
    struct Close { sqlite3* db; ~Close(){sqlite3_close(db);} } close{db};
    sqlite3_stmt* query{};
    require(sqlite3_prepare_v2(db, "SELECT id,bodyId,data FROM ChildShape", -1, &query, nullptr) == SQLITE_OK, "cannot query shapes");
    struct Finalize { sqlite3_stmt* p; ~Finalize(){sqlite3_finalize(p);} } finalize{query};
    std::size_t count = 0, blocks = 0;
    int status;
    while ((status=sqlite3_step(query)) == SQLITE_ROW) {
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(query, 2));
        const auto size = sqlite3_column_bytes(query, 2);
        const auto shape = scrapds::decode_child_geometry({data, std::size_t(size)});
        require(bool(shape), "unknown shape variant in observed save");
        require(shape->id == sqlite3_column_int64(query, 0), "shape ID mismatch");
        require(shape->body_id == sqlite3_column_int64(query, 1), "shape body mismatch");
        if (shape->kind == scrapds::SavedShapeKind::block) ++blocks;
        ++count;
    }
    require(status == SQLITE_DONE, "shape query failed");
    std::cout << path.filename().string() << ": " << count << " shapes, " << blocks << " blocks decoded\n";
    sqlite3_stmt* bodies{};
    require(sqlite3_prepare_v2(db, "SELECT id,worldId,data FROM RigidBody", -1, &bodies, nullptr) == SQLITE_OK, "cannot query body poses");
    Finalize finish_bodies{bodies};
    std::size_t body_count=0;
    while ((status=sqlite3_step(bodies)) == SQLITE_ROW) {
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(bodies, 2));
        const auto body = scrapds::decode_rigid_body_pose({data, std::size_t(sqlite3_column_bytes(bodies, 2))});
        require(bool(body), "unknown body pose variant");
        require(body->id == sqlite3_column_int64(bodies, 0) && body->world_id == sqlite3_column_int64(bodies,1), "body pose identifier mismatch");
        ++body_count;
    }
    require(status == SQLITE_DONE, "body pose query failed");
    std::cout << "  " << body_count << " body poses decoded\n";
}
}
int main(int argc, char** argv) {
    try {
        // Observed shape 428: independent field expectations from serializer and
        // its real SQLite row; signed coordinates and non-uniform dimensions.
        const auto block = from_hex("011f01000001ac000002b810884e44c1b26e88864c43239c5981e2000002b8004efffbfffcffdcd7c900010010000400");
        const auto geometry = scrapds::decode_child_geometry(block);
        const auto joint=scrapds::decode_joint_geometry(from_hex("020301000000030000002300000033a623dbb0416eb6b5ad4a3e916b881b4a000000230000003300000001fffffffafffffffd00000001fffffffafffffffd1155ff017fdf"));
        require(joint&&joint->id==3&&joint->shape_a==35&&joint->shape_b==51&&joint->grid_a==std::array<std::int32_t,3>{-3,-6,1}&&joint->axis_a==std::array<std::int32_t,3>{0,0,-1}&&joint->axis_b==joint->axis_a&&joint->reference_a==std::array<std::int32_t,3>{1,0,0}&&joint->uuid=="4a1b886b-913e-4aad-b5b6-6e41b0db23a6","bearing joint layout");
        require(scrapds::decode_survival_spring_setting(from_hex("0324010000002100000004020000000402000001"),33,4)==1,"saved spring setting layout");
        const auto piston=scrapds::decode_survival_piston_settings(from_hex("032301000000390000001202000000120200000909"),57,18);
        require(piston&&piston->length_index==9&&piston->speed_index==9,"saved piston settings layout");
        require(geometry && geometry->id == 428 && geometry->body_id == 696, "block identifiers");
        require(geometry->grid_position == std::array<std::int32_t,3>{78,-5,-4}, "signed grid position");
        require(geometry->grid_dimensions == std::array<std::int32_t,3>{1,16,4}, "block dimensions");
        require(geometry->color_abgr == 0xffdcd7c9u, "packed RGBA color");
        require(geometry->x_axis == std::array<std::int32_t,3>{1,0,0} && geometry->z_axis == std::array<std::int32_t,3>{0,0,1}, "canonical block axes");
        require(geometry->uuid == "e281599c-2343-4c86-886e-b2c1444e8810", "shape UUID byte order");
        for (std::size_t i=0; i<block.size(); ++i) rejects({block.begin(), block.begin()+i});
        auto bad = block; bad[41]=255;bad[42]=255;rejects(bad);
        bad=block;bad[41]=0;bad[42]=0;require((*scrapds::decode_child_geometry(bad)->grid_dimensions)[0] == 0,"degenerate observed block dimension lost");
        bad=block; bad[1]=33;require(!scrapds::decode_child_geometry(bad), "unknown subtype fabricated");
        auto part = std::vector<std::uint8_t>(block.begin(), block.begin()+43);
        part[1]=32;part[41]=0x75;part[42]=2;
        const auto p = scrapds::decode_child_geometry(part);
        require(p && !p->grid_dimensions && p->state_flags==2, "part shape fields");
        require(p->x_axis == std::array<std::int32_t,3>{1,0,0} && p->z_axis == std::array<std::int32_t,3>{0,0,1}, "packed part axes");
        part[41]=0x65;
        require(scrapds::decode_child_geometry(part)->z_axis == std::array<std::int32_t,3>{0,1,0}, "noncanonical part axes");
        part[41]=0x55;rejects(part);part[41]=0x05;rejects(part);
        part[41]=0x75;part.pop_back();require(!scrapds::decode_child_geometry(part)->state_flags,"legacy part flag invented");
        const auto static_body = from_hex("000101000002b80000444ebfbe444cf042c4671042c46c1fbe00003f800000000000000000000000000000c46c0000444e00000000000000ffffffff");
        const auto pose = scrapds::decode_rigid_body_pose(static_body);
        require(pose && pose->is_static && pose->rotation_xyzw == std::array<float,4>{0,0,0,1}, "static quaternion byte order");
        require(pose->position == std::array<float,3>{-944,824,0}, "static position");
        auto rotated = static_body;
        // Serialized W,Z,Y,X -> native X,Y,Z,W. Non-identity fixture catches
        // component-order mistakes that identity-only tests would miss.
        const auto q = from_hex("0000000000000000000000003f800000");
        for (std::size_t i=0;i<16;++i)rotated[27+i]=q[i];
        require(scrapds::decode_rigid_body_pose(rotated)->rotation_xyzw == std::array<float,4>{1,0,0,0}, "rotated static quaternion order");
        auto dynamic = static_body;dynamic[1]=2;dynamic.resize(80,0);
        for (std::size_t i=0;i<16;++i)dynamic[27+i]=q[i];
        require(scrapds::decode_rigid_body_pose(dynamic)->rotation_xyzw == std::array<float,4>{0,0,0,1}, "dynamic quaternion byte order");
        for (int i=1; i<argc; ++i) {
            const std::filesystem::path root(argv[i]);
            if (std::filesystem::is_directory(root)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if(entry.is_regular_file() && entry.path().extension()==".db")check_database(entry.path());
            } else check_database(root);
        }
        std::cout << "shape_geometry_tests: passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
