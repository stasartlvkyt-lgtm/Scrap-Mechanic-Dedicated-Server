#include "data/blob.hpp"
#include <sqlite3.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::uint8_t> unhex(const std::string& s) {
    std::vector<std::uint8_t> result;
    for (std::size_t i=0; i<s.size(); i+=2) result.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i,2),nullptr,16)));
    return result;
}
template <typename Function> void throws(Function f, const char* message) {
    try { f(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
void verify_save(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
    sqlite3* db=nullptr;
    require(sqlite3_open_v2(name.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Actual save open failed");
    struct Close { sqlite3* db; ~Close(){sqlite3_close(db);} } close{db};
    sqlite3_stmt* statement=nullptr;
    require(sqlite3_prepare_v2(db,"SELECT b.id,b.worldId,b.data,r.minX,r.maxX,r.minY,r.maxY FROM RigidBody b JOIN RigidBodyBounds r ON r.id=b.id",-1,&statement,nullptr)==SQLITE_OK,"Actual body query failed");
    struct Finalize { sqlite3_stmt*& s; ~Finalize(){sqlite3_finalize(s);} } finalize{statement};
    int result=SQLITE_OK;
    std::size_t bodies=0,shapes=0;
    while ((result=sqlite3_step(statement))==SQLITE_ROW) {
        const auto* bytes=static_cast<const std::uint8_t*>(sqlite3_column_blob(statement,2));
        const auto size=static_cast<std::size_t>(sqlite3_column_bytes(statement,2));
        const auto decoded=scrapds::decode_rigid_body_prefix({bytes,size},size);
        require(decoded.id==sqlite3_column_int64(statement,0),"Actual body ID mismatch");
        require(decoded.world_id==sqlite3_column_int64(statement,1),"Actual world ID mismatch");
        for (int i=0;i<4;++i) require(decoded.bounds_min_x_max_x_min_y_max_y[i]==sqlite3_column_double(statement,3+i),"Actual RTree bounds mismatch");
        ++bodies;
    }
    require(result==SQLITE_DONE,"Actual body iteration failed");
    sqlite3_finalize(statement); statement=nullptr;
    require(sqlite3_prepare_v2(db,"SELECT id,bodyId,data FROM ChildShape",-1,&statement,nullptr)==SQLITE_OK,"Actual shape query failed");
    while ((result=sqlite3_step(statement))==SQLITE_ROW) {
        const auto* bytes=static_cast<const std::uint8_t*>(sqlite3_column_blob(statement,2));
        const auto size=static_cast<std::size_t>(sqlite3_column_bytes(statement,2));
        const auto decoded=scrapds::decode_child_shape_prefix({bytes,size},size);
        require(decoded.id==sqlite3_column_int64(statement,0),"Actual shape ID mismatch");
        require(decoded.body_id==sqlite3_column_int64(statement,1),"Actual shape body mismatch");
        ++shapes;
    }
    require(result==SQLITE_DONE,"Actual shape iteration failed");
    std::cout << name << ": " << bodies << " bodies, " << shapes << " shapes verified\n";
}
}

int main(int argc,char** argv) {
    try {
        auto body = unhex("000101000002b80000444ebfbe444cf042c4671042c46c1fbe00003f800000000000000000000000000000c46c0000444e00000000000000ffffffff");
        const auto decoded = scrapds::decode_rigid_body_prefix(body,body.size());
        require(decoded.id==696 && decoded.world_id==0 && decoded.variant==1,"Body integer endian failure");
        require(decoded.bounds_min_x_max_x_min_y_max_y == std::array<float,4>{-944.4959716796875f,-924.2540283203125f,819.7540283203125f,826.9959716796875f},"Bounds float endian/order failure");
        require(decoded.candidate_position==std::array<float,3>{-944,824,0},"Candidate coordinate decode failed");
        require(decoded.candidate_quaternion==std::array<float,4>{1,0,0,0},"Candidate quaternion decode failed");
        for (std::size_t size=0;size<55;++size) throws([&] { scrapds::decode_rigid_body_prefix(std::span(body).first(size),body.size()); },"Truncated body accepted");
        throws([&] { scrapds::decode_rigid_body_prefix(body,body.size()-1); },"Stored length mismatch accepted");
        body[1]=9;
        throws([&] { scrapds::decode_rigid_body_prefix(body,body.size()); },"Unknown body variant accepted");
        body[1]=1; body[9]=0x7f; body[10]=0x80; body[11]=0; body[12]=0;
        throws([&] { scrapds::decode_rigid_body_prefix(body,body.size()); },"Nonfinite bounds accepted");
        auto shape=unhex("011f01000001ac000002b810884e44c1b26e88864c43239c5981e2000002b8004efffbfffcffdcd7c900010010000400");
        const auto child=scrapds::decode_child_shape_prefix(shape,shape.size());
        require(child.id==428 && child.body_id==696 && child.repeated_body_id==696,"Shape integer endian failure");
        require(child.candidate_uuid=="e281599c-2343-4c86-886e-b2c1444e8810","Shape UUID byte order failure");
        for (std::size_t size=0;size<31;++size) throws([&] { scrapds::decode_child_shape_prefix(std::span(shape).first(size),shape.size()); },"Truncated shape accepted");
        shape[30] ^= 1;
        throws([&] { scrapds::decode_child_shape_prefix(shape,shape.size()); },"Mismatched body IDs accepted");
        require(scrapds::inspect_blob_prefix_json("RigidBody","xx",60)=="{\"status\":\"unknown-or-invalid-variant\"}","Invalid hex not bounded");
        require(scrapds::inspect_blob_prefix_json("Other","ff",1).empty(),"Unknown table decoded");
        require(scrapds::rigid_body_prefix_json(decoded).find("inferred-not-runtime-verified")!=std::string::npos,"Missing pose confidence label");
        for (int i=1;i<argc;++i) {
            const auto path=std::filesystem::u8path(argv[i]);
            if (std::filesystem::is_directory(path)) {
                for (const auto& entry:std::filesystem::recursive_directory_iterator(path)) if (entry.is_regular_file() && entry.path().extension()==".db") verify_save(entry.path());
            } else verify_save(path);
        }
        std::cout << "blob_tests: passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "blob_tests: " << e.what() << '\n'; return 1; }
}
